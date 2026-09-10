#!/bin/sh
# shellcheck disable=SC2086,SC2016  # dev tuning: $O is an ssh-option string meant to word-split; single-quoted strings are remote commands eval-d on the Beep, not locally
# hw-tune.sh — reproducible two-Beep sync harness for tuning replaynet --resample.
#
# Loop for an agent: edit the tuning knobs in feed/replaynet/src/replaynet.c, run this,
# read the metrics, repeat. It cross-compiles the CURRENT source, deploys to both Beeps,
# fans a tone out to both sinks, and prints OBJECTIVE metrics parsed from each sink's log.
#
# OBJECTIVE: minimise COPPER snap_events (each ≈ one audible dropout) while keeping
#   - SILVER snap_events ≈ 0        (the co-located control; must not regress)
#   - ring_min > ~200 ms on both    (no buffer starvation / XRUN)
#   - XRUN = 0 on both
# Network jitter varies run-to-run, so judge a change by the MEDIAN of >=3 runs
# (pass -n 3), not a single run.
#
# WHY snap_events is the proxy: a "snap" is a drop/insert correction — an audible click/
# dropout. The operator heard "3 dropouts" on a run whose copper log showed ~3 snap events,
# so minimising snap_events minimises audible dropouts. There is no mic in the loop; this
# metric is the ears. (cross-checked once by ear per candidate before committing.)
#
# REQUIRES: SSHPASS=<copper root password> in the environment. Run from the repo root.
# Silver uses key auth; copper uses the password (its build has a default root password).
set -u

SILVER="${SILVER:-10.9.100.169}"
COPPER="${COPPER:-10.9.100.173}"
PORT="${PORT:-5064}"
TONE_LOCAL="${TONE_LOCAL:-/tmp/tone440.pcm}"     # generated below if missing (25s 440Hz S16_LE)
TONE_BEEP=/tmp/tone440.pcm                        # path on silver (the fan-out source)
IMAC="${IMAC:-kfiducia@imac.kfiducia.com}"
CONTAINER="${CONTAINER:-beep-build}"
DOCKER="${DOCKER:-/usr/local/bin/docker}"
SRC=feed/replaynet/src/replaynet.c
RUNS=1
[ "${1:-}" = "-n" ] && { RUNS="$2"; shift 2; }

: "${SSHPASS:?set SSHPASS=<copper root password>}"; export SSHPASS

O="-o ConnectTimeout=10 -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no"
sshk(){ ssh $O -o BatchMode=yes "root@$SILVER" "$@"; }        # silver: key
sshp(){ sshpass -e ssh $O "root@$COPPER" "$@"; }              # copper: password
scpk(){ scp -O $O -o BatchMode=yes "$1" "root@$SILVER:$2"; }
scpp(){ sshpass -e scp -O $O "$1" "root@$COPPER:$2"; }
NOISE='store now|openssh|vuln|upgr|Warning|Permanently|post-quantum'
filt(){ grep -vE "$NOISE" 2>/dev/null; }

# kill replaynet on a Beep by PID (busybox has no pkill); $1 = sshk|sshp
kill_beep(){ $1 'P=$(ps w | grep "[r]eplaynet-alsa-new" | awk "{print \$1}"); for x in $P; do kill $x 2>/dev/null; done; sleep 1; P=$(ps w | grep "[r]eplaynet-alsa-new" | awk "{print \$1}"); for x in $P; do kill -9 $x 2>/dev/null; done' 2>&1 | filt >/dev/null; }
cleanup(){ kill_beep sshk; kill_beep sshp; }
trap cleanup EXIT INT TERM

# parse one sink log (stdin) -> "snap_events=.. err_max_ms=.. ring_min_ms=.. XRUN=.."
metrics(){
  awk '
    /playout:/ {
      # extract snap, err, ring via field scan (portable; no gawk match-array)
      for (i=1;i<=NF;i++){
        if ($i=="snap") s=$(i+1)+0;
        if ($i=="err")  { e=$(i+1)+0; if(e<0)e=-e; if(e>emax)emax=e; }
        if ($i=="ring") { r=$(i+1)+0; if(rmin==""||r<rmin)rmin=r; }
      }
      if (n++ && s!=prev) ev++; prev=s;
    }
    END{ printf "snap_events=%d err_max_ms=%.1f ring_min_ms=%d", ev, emax*1000/44100, rmin }'
}

echo "== cross-compile current $SRC for mips_24kc (-DRN_ALSA -DRN_FLAC) =="
scp $O -o BatchMode=yes "$SRC" "$IMAC:/tmp/replaynet.c" >/dev/null 2>&1 || { echo "!! scp to imac failed"; exit 1; }
ssh $O -o BatchMode=yes "$IMAC" "$DOCKER cp /tmp/replaynet.c $CONTAINER:/tmp/replaynet.c >/dev/null && $DOCKER exec $CONTAINER sh -lc '
  export STAGING_DIR=/build/openwrt/staging_dir
  TC=\$STAGING_DIR/toolchain-mips_24kc_gcc-13.3.0_musl; SR=\$STAGING_DIR/target-mips_24kc_musl
  rm -f /tmp/replaynet-alsa
  \$TC/bin/mips-openwrt-linux-musl-gcc -Wall -Wextra -O2 -DRN_ALSA -DRN_FLAC --sysroot=\$SR -I\$SR/usr/include -o /tmp/replaynet-alsa /tmp/replaynet.c -L\$SR/usr/lib -lasound -lFLAC 2>&1 | head
  \$TC/bin/mips-openwrt-linux-musl-strip /tmp/replaynet-alsa && echo BUILT
' && $DOCKER cp $CONTAINER:/tmp/replaynet-alsa /tmp/replaynet-alsa >/dev/null && echo PULLED" 2>&1 | filt | tail -3
scp $O -o BatchMode=yes "$IMAC:/tmp/replaynet-alsa" /tmp/replaynet-alsa >/dev/null 2>&1 || { echo "!! pull binary failed"; exit 1; }

# tone on the operator box (generate once if missing), push to silver
if [ ! -f "$TONE_LOCAL" ]; then
  echo "== generating 25s 440Hz S16_LE tone =="
  python3 - "$TONE_LOCAL" <<'PY'
import sys,math,array
p=sys.argv[1]; rate=44100; n=rate*25
m=array.array('h',(int(0.25*32767*math.sin(2*math.pi*440*i/rate)) for i in range(n)))
st=array.array('h',bytes(4*n)); st[0::2]=m; st[1::2]=m
if sys.byteorder!='little': st.byteswap()
open(p,'wb').write(st.tobytes())
PY
fi

echo "== deploy binary to both Beeps + tone to silver =="
scpk /tmp/replaynet-alsa /tmp/replaynet-alsa-new >/dev/null 2>&1 && sshk 'chmod +x /tmp/replaynet-alsa-new'
scpp /tmp/replaynet-alsa /tmp/replaynet-alsa-new >/dev/null 2>&1 && sshp 'chmod +x /tmp/replaynet-alsa-new'
scpk "$TONE_LOCAL" "$TONE_BEEP" >/dev/null 2>&1

start_sink(){ $1 "rm -f /tmp/rn-rs.log; start-stop-daemon -S -b -m -p /tmp/rn.pid -x /bin/sh -- -c '/tmp/replaynet-alsa-new --sink --listen $PORT --alsa default --resample >/tmp/rn-rs.log 2>&1'" 2>&1 | filt >/dev/null; }

run=1
while [ "$run" -le "$RUNS" ]; do
  cleanup
  start_sink sshk; start_sink sshp; sleep 2
  sshk "/tmp/replaynet-alsa-new --fanout --peers 127.0.0.1,$COPPER --listen $PORT --pcm $TONE_BEEP" 2>&1 | filt | grep -qi EOF
  sleep 1
  sshk 'cat /tmp/rn-rs.log' 2>/dev/null | filt > /tmp/hwtune-silver.log
  sshp 'cat /tmp/rn-rs.log' 2>/dev/null | filt > /tmp/hwtune-copper.log
  sx=$(grep -icE 'xrun|recover' /tmp/hwtune-silver.log); cx=$(grep -icE 'xrun|recover' /tmp/hwtune-copper.log)
  printf "run %d/%d  SILVER: %s XRUN=%s\n" "$run" "$RUNS" "$(metrics </tmp/hwtune-silver.log)" "$sx"
  printf "          COPPER: %s XRUN=%s\n" "$(metrics </tmp/hwtune-copper.log)" "$cx"
  run=$((run+1))
done
cleanup
echo "== done (logs: /tmp/hwtune-{silver,copper}.log) =="
