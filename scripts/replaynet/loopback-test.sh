#!/bin/sh
# loopback-test.sh — validate replaynet end to end over 127.0.0.1.
#
#   step (a): PCM round-trips byte-for-byte; wire header is big-endian (network order).
#   step (b): clock sync recovers an injected offset; the drift controller tracks an
#             injected clock-rate skew.
#
# Works on the host and under qemu-mips (arg 1 = the command prefix that runs the,
# possibly cross-built, binary) — the "2-instance qemu capture" that live-confirms the
# reversed model (docs/PLAYNET-RE.md §4) against our engine:
#   scripts/replaynet/loopback-test.sh ./replaynet
#   scripts/replaynet/loopback-test.sh "qemu-mips /tmp/rn/replaynet"
#
# Needs python3 (raw wire capture + header decode) and perl (sub-second sleeps / a hard
# watchdog so a bug can't hang the run).
set -e

RUN="${1:?usage: loopback-test.sh \"<run-prefix>\"  e.g. \"qemu-mips /tmp/rn/replaynet\" or ./replaynet}"
WORK="${WORK:-/tmp/rn-test}"
P1="${P1:-54330}"; P2="${P2:-54331}"; P3="${P3:-54332}"; P4="${P4:-54333}"

mkdir -p "$WORK"
fail() { echo "FAIL: $*" >&2; exit 1; }
nap() { perl -e 'select(undef,undef,undef,$ARGV[0])' "$1"; }
# run a paced source (blocks ~secs) with a hard alarm; $3.. = extra source args
src() { port="$1"; pcm="$2"; shift 2; perl -e 'alarm 30; exec @ARGV' $RUN --source --peer 127.0.0.1:"$port" --pcm "$pcm" "$@"; }

echo "== replaynet loopback test =="
echo "   run-prefix: $RUN"

# ---- 0. selftest (BE codec, header, drift controller, NTP math) ----
$RUN --selftest 2>&1 | grep -q "SELFTEST OK" || fail "selftest failed"
echo "   selftest: OK"

# ---- 1. round-trip integrity (paced ~0.3 s) + 2. big-endian wire header ----
IN="$WORK/in.pcm"; OUT="$WORK/out.pcm"; WIRE="$WORK/wire.bin"
head -c $((44100 * 4 / 3)) /dev/urandom > "$IN"     # ~0.33 s, frame-aligned
$RUN --sink --listen "$P1" --out "$OUT" 2>"$WORK/sink1.log" &
S=$!; ( nap 15; kill -9 $S 2>/dev/null ) &
nap 0.5; src "$P1" "$IN" 2>"$WORK/src1.log" || fail "source exited non-zero"
nap 0.5; kill -9 $S 2>/dev/null || true
cmp -s "$IN" "$OUT" || fail "round-trip mismatch ($(wc -c <"$IN") vs $(wc -c <"$OUT"))"
echo "   round-trip: IDENTICAL ($(wc -c <"$IN") bytes) OK"

python3 - "$P2" "$WIRE" <<'PY' &
import socket, sys
port, out = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port)); s.listen(1)
c, _ = s.accept()
buf = b""
while len(buf) < 64:
    b = c.recv(64 - len(buf))
    if not b: break
    buf += b
open(out, "wb").write(buf); c.close(); s.close()
PY
CAP=$!; ( nap 15; kill -9 $CAP 2>/dev/null ) &
nap 0.5; src "$P2" "$IN" 2>/dev/null || true
nap 0.5; kill -9 $CAP 2>/dev/null || true
[ -s "$WIRE" ] || fail "no wire bytes captured"
python3 - "$WIRE" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert len(b) >= 16 + 28, "short capture (%d)" % len(b)
magic, ver, typ, resv, seq, blen = struct.unpack(">IBBHII", b[:16])         # common header
track, disc, src_ns, pcmlen = struct.unpack(">QQQI", b[16:16+28])            # audio body
print("   wire header (big-endian): magic=%r ver=%d type=%d seq=%d body_len=%d" % (b[:4], ver, typ, seq, blen))
print("     track_samples=%d discarded=%d source_time_ns=%d pcm_len=%d" % (track, disc, src_ns, pcmlen))
assert magic == 0x52504C59, "magic not RPLY"
assert ver == 2 and typ == 1, "version/type"
assert pcmlen == 4608 and blen == 28 + pcmlen, "lengths"
assert struct.unpack("<I", b[:4])[0] != magic, "endianness not distinguishable"
print("   wire format: BIG-ENDIAN v2 confirmed OK")
PY

# ---- 3. clock sync: inject +5 ms offset, sink must recover theta in [4,6] ms ----
head -c $((44100 * 4 * 2)) /dev/urandom > "$IN"     # ~2 s, room for several pings
$RUN --sink --listen "$P3" --out /dev/null 2>"$WORK/sink3.log" &
S=$!; ( nap 20; kill -9 $S 2>/dev/null ) &
nap 0.5; src "$P3" "$IN" --fake-clock-offset-ns 5000000 2>/dev/null || true
nap 0.5; kill -9 $S 2>/dev/null || true
THETA=$(grep -o 'theta=[+-][0-9]* us' "$WORK/sink3.log" | tail -1 | grep -o '[+-][0-9]*')
[ -n "$THETA" ] || fail "no clock-sync theta logged"
A=$(echo "$THETA" | tr -d +); [ "$A" -ge 4000 ] && [ "$A" -le 6000 ] \
	|| fail "theta ${THETA}us not within [4000,6000] of injected 5000us"
echo "   clock sync: recovered theta=${THETA}us for injected +5000us OK"

# ---- 4. drift: inject +3000 ppm, controller must track (net same sign, >deadband) ----
head -c $((44100 * 4 * 4)) /dev/urandom > "$IN"     # ~4 s to accumulate drift
$RUN --sink --listen "$P4" --out /dev/null 2>"$WORK/sink4.log" &
S=$!; ( nap 20; kill -9 $S 2>/dev/null ) &
nap 0.5; src "$P4" "$IN" --fake-clock-rate-ppm 3000 2>/dev/null || true
nap 0.5; kill -9 $S 2>/dev/null || true
LINE=$(grep 'drift:' "$WORK/sink4.log" | tail -1)
[ -n "$LINE" ] || fail "no drift measurement logged"
MEAS=$(echo "$LINE" | grep -o 'measured [+-][0-9]*' | grep -o '[+-][0-9]*')
NET=$(echo "$LINE" | grep -o 'net [+-][0-9]*' | grep -o '[+-][0-9]*')
echo "   drift: $LINE"
[ -n "$MEAS" ] && [ -n "$NET" ] || fail "could not parse drift line"
# 3000 ppm over ~3.5 s ≈ 460 frames; require a clear signal and controller tracking
MA=$(echo "$MEAS" | tr -d +-); [ "$MA" -ge 200 ] || fail "measured drift ${MEAS} too small"
case "$MEAS" in -*) ms=neg ;; *) ms=pos ;; esac
case "$NET"  in -*) ns=neg ;; *) ns=pos ;; esac
[ "$ms" = "$ns" ] || fail "controller net ${NET} not tracking measured ${MEAS} (sign)"
NA=$(echo "$NET" | tr -d +-); [ "$NA" -ge 64 ] || fail "controller net ${NET} did not correct"
echo "   drift controller: tracked ${MEAS} frames with net ${NET} correction OK"

echo "== PASS =="
