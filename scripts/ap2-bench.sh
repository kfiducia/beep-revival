#!/bin/sh
# ap2-bench.sh — on-device AirPlay-2 CPU/XRUN verification for the Beep (AR9331).
#
# WHY: AirPlay-2 buffered audio is AAC-LC, and ffmpeg's default AAC decoder is
# floating-point. On the FPU-less 400 MHz AR9331 that pegs the core (~0% idle)
# and the PCM XRUNs within seconds (see docs/DEV-NOTES.md §1.1). The fix is the
# fixed-point decoder aac_fixed (~10x cheaper). This script answers ONE question
# on real hardware: with the current build, does a live AirPlay-2 session sustain
# playback (PCM stays RUNNING, idle stays > 0) or does it still XRUN?
#
# Runs on the device (OpenWrt/busybox ash). Copy over and run:
#   scp -O scripts/ap2-bench.sh root@<beep>:/tmp/ && ssh root@<beep> sh /tmp/ap2-bench.sh
#
# Then start an Apple Music / AirPlay-2 stream to the Beep. Ctrl-C to stop early;
# it also auto-stops after DURATION seconds. A verdict + a saved log are printed.

DURATION="${DURATION:-90}"          # seconds to sample once playback starts
IDLE_FLOOR="${IDLE_FLOOR:-5}"       # % idle below this = "saturated" (a fail signal)
LOG="/tmp/ap2-bench-$(date +%s 2>/dev/null || echo run).log"

say() { echo "$@"; echo "$@" >>"$LOG"; }

# ---------------------------------------------------------------------------
# 0. Build / environment pre-flight (does the image even contain the fix?)
# ---------------------------------------------------------------------------
say "== Beep AirPlay-2 bench =="
say "date: $(date 2>/dev/null)"
say "kernel: $(uname -a 2>/dev/null)"

SPS_BIN="$(command -v shairport-sync 2>/dev/null || echo /usr/bin/shairport-sync)"
if [ -x "$SPS_BIN" ]; then
  if strings "$SPS_BIN" 2>/dev/null | grep -qi airplay2; then
    say "[ok] shairport-sync is an AirPlay-2 build"
  else
    say "[FAIL] shairport-sync is NOT an AirPlay-2 build — this is the classic AP1 image."
    say "       Build with AIRPLAY2=1 and reflash before benching AP2."
  fi
else
  say "[warn] shairport-sync binary not found at $SPS_BIN"
fi

# aac_fixed present in libavcodec? If present, patch 020 selects it over float aac.
AVCODEC="$(ls /usr/lib/libavcodec.so* 2>/dev/null | head -1)"
if [ -n "$AVCODEC" ]; then
  if strings "$AVCODEC" 2>/dev/null | grep -qw aac_fixed; then
    say "[ok] libavcodec advertises the aac_fixed decoder (fixed-point path available)"
  else
    say "[FAIL] aac_fixed NOT in $AVCODEC — ffmpeg was built without it, so AP2 will"
    say "       fall back to the FLOAT decoder and is expected to XRUN. Rebuild with"
    say "       --enable-decoder=aac_fixed (build.sh already does this for AIRPLAY2=1)."
  fi
else
  say "[warn] libavcodec not found — is this an AP2 image?"
fi

# nqptp running (AP2 timing daemon)?
if pidof nqptp >/dev/null 2>&1; then
  say "[ok] nqptp is running (pid $(pidof nqptp))"
else
  say "[warn] nqptp not running — AP2 needs it for PTP timing"
fi

# ---------------------------------------------------------------------------
# helpers: CPU idle from /proc/stat, PCM state from /proc/asound
# ---------------------------------------------------------------------------
# find the playback PCM status file (card index varies)
PCM_STATUS=""
for f in /proc/asound/card*/pcm*p/sub*/status; do
  [ -e "$f" ] && PCM_STATUS="$f" && break
done
[ -n "$PCM_STATUS" ] && say "pcm status: $PCM_STATUS" || say "[warn] no playback PCM status node found yet"

# read total & idle jiffies from the aggregate 'cpu' line
cpu_fields() { awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8, $5}' /proc/stat; }
pcm_state()  { [ -n "$PCM_STATUS" ] && awk -F': *' '/^state/{print $2; exit}' "$PCM_STATUS" 2>/dev/null || echo "closed"; }
mem_avail()  { awk '/^MemAvailable/{print $2" kB"}' /proc/meminfo; }
sps_pid()    { pidof shairport-sync 2>/dev/null | awk '{print $1}'; }

# ---------------------------------------------------------------------------
# 1. Wait for playback to actually start (PCM enters RUNNING)
# ---------------------------------------------------------------------------
say ""
say ">> Start an AirPlay-2 (Apple Music) stream to this Beep now."
say "   Waiting up to 120s for the PCM to reach RUNNING..."
waited=0
while [ "$waited" -lt 120 ]; do
  st="$(pcm_state)"
  [ "$st" = "RUNNING" ] && break
  waited=$((waited + 1))
  sleep 1
done
if [ "$(pcm_state)" != "RUNNING" ]; then
  say "[FAIL] PCM never reached RUNNING within 120s — playback didn't start."
  say "       (Check that the sender picked this speaker and that pairing succeeded.)"
  exit 2
fi
say "[ok] playback started — sampling for ${DURATION}s (Ctrl-C to stop early)"
say ""

# ---------------------------------------------------------------------------
# 2. Sample loop @1Hz: idle%, shairport CPU%, PCM state, MemAvailable
# ---------------------------------------------------------------------------
printf "%-9s %-7s %-9s %-9s %-8s %s\n" "t(s)" "idle%" "sps_cpu%" "pcmstate" "xruns" "memavail" | tee -a "$LOG"

set -- $(cpu_fields); ptot=$1; pidle=$2
PID="$(sps_pid)"
pjiff=0
[ -n "$PID" ] && pjiff=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null || echo 0)
HZ=100  # CLK_TCK on OpenWrt/musl mips
xrun_seen=0
min_idle=100
running_all=1
t=0

trap 'say ""; say "(stopped)"; break' INT
while [ "$t" -lt "$DURATION" ]; do
  sleep 1
  t=$((t + 1))

  set -- $(cpu_fields); tot=$1; idl=$2
  dtot=$((tot - ptot)); didle=$((idl - pidle)); ptot=$tot; pidle=$idl
  idlep=0; [ "$dtot" -gt 0 ] && idlep=$((100 * didle / dtot))

  PID="$(sps_pid)"; spscpu=0
  if [ -n "$PID" ]; then
    now=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null || echo "$pjiff")
    dj=$((now - pjiff)); pjiff=$now
    [ "$dtot" -gt 0 ] && spscpu=$((100 * dj / HZ))   # approx %, jiffies/sec
  fi

  st="$(pcm_state)"
  [ "$st" != "RUNNING" ] && running_all=0
  [ "$st" = "XRUN" ] && xrun_seen=$((xrun_seen + 1))
  [ "$idlep" -lt "$min_idle" ] && min_idle=$idlep

  printf "%-9s %-7s %-9s %-9s %-8s %s\n" "$t" "$idlep" "$spscpu" "$st" "$xrun_seen" "$(mem_avail)" | tee -a "$LOG"
done
trap - INT

# ---------------------------------------------------------------------------
# 3. Verdict
# ---------------------------------------------------------------------------
say ""
say "== verdict =="
say "min idle over run: ${min_idle}%   (floor for 'saturated' = ${IDLE_FLOOR}%)"
say "XRUN samples: ${xrun_seen}"
if [ "$running_all" -eq 1 ] && [ "$xrun_seen" -eq 0 ] && [ "$min_idle" -ge "$IDLE_FLOOR" ]; then
  say "[PASS] PCM stayed RUNNING with headroom — AirPlay-2 sustains on this chip."
  say "       Fixed-point AAC (aac_fixed) is doing its job. Now listen-test for glitches"
  say "       and confirm the sender never negotiated HE-AAC/SBR (the real cliff)."
else
  say "[FAIL] playback did not sustain (XRUN or idle collapsed)."
  say "       If aac_fixed was confirmed present above, the decoder is not the whole"
  say "       cost — next levers (measure each by re-running after ONE change):"
  say "         * force S16 output in shairport-sync.conf alsa{} (half the sample width)"
  say "           — VERIFY it stays clean, wrong endianness here = white noise."
  say "         * confirm shairport has SCHED_FIFO (ps -eo comm,rtprio | grep shairport)."
  say "         * drop the LED 'party pulse' churn in beepd during playback."
fi
say ""
say "log saved: $LOG"
