#!/bin/sh
# loopback-test.sh — validate replaynet's 2-node PCM stream + on-wire framing.
#
# Runs a source and a sink over 127.0.0.1 and checks that the PCM round-trips
# byte-for-byte, then captures the actual on-wire bytes of the first frame and
# decodes the header to prove it is big-endian (network order) — the property
# reversed from stock playnet in docs/PLAYNET-RE.md §4.
#
# Works two ways:
#   host:   scripts/replaynet/loopback-test.sh ./replaynet
#   qemu:   scripts/replaynet/loopback-test.sh "qemu-mips /tmp/rn/replaynet"
# i.e. arg 1 is the command prefix that runs the (possibly cross-built) binary.
# This is the "2-instance qemu capture" that live-confirms §4 against OUR engine.
#
# Needs: python3 (for the raw wire capture + header decode) and perl (sub-second
# sleeps / a hard watchdog, so a bug can't hang the run).
set -e

RUN="${1:?usage: loopback-test.sh \"<run-prefix> [binary]\"  e.g. \"qemu-mips /tmp/rn/replaynet\" or ./replaynet}"
WORK="${WORK:-/tmp/rn-test}"
PORT="${PORT:-54330}"
CAPPORT="${CAPPORT:-54331}"
BYTES="${BYTES:-13816}"   # frame-aligned (÷4), not a chunk multiple (÷4608) -> exercises final partial chunk

mkdir -p "$WORK"
IN="$WORK/in.pcm"; OUT="$WORK/out.pcm"; WIRE="$WORK/wire.bin"
: > "$WORK/sink.log"; : > "$WORK/src.log"

fail() { echo "FAIL: $*" >&2; exit 1; }
naptime() { perl -e 'select(undef,undef,undef,$ARGV[0])' "$1"; }

echo "== replaynet loopback test =="
echo "   run-prefix: $RUN"
head -c "$BYTES" /dev/urandom > "$IN"

# ---- 1. round-trip integrity (sink -> file, source -> sink) ----
$RUN --sink --listen "$PORT" --out "$OUT" 2>"$WORK/sink.log" &
SINK=$!
( naptime 12; kill -9 $SINK 2>/dev/null ) & WD=$!; disown $WD 2>/dev/null || true
naptime 0.5
perl -e 'alarm 10; exec @ARGV' $RUN --source --peer 127.0.0.1:"$PORT" --pcm "$IN" 2>"$WORK/src.log" \
	|| fail "source exited non-zero (see $WORK/src.log)"
naptime 0.5
kill -0 $SINK 2>/dev/null && kill -9 $SINK 2>/dev/null
kill -9 $WD 2>/dev/null || true

[ -s "$OUT" ] || fail "sink produced no output"
if cmp -s "$IN" "$OUT"; then
	echo "   round-trip: IDENTICAL ($BYTES bytes) OK"
else
	fail "round-trip mismatch ($(wc -c <"$IN") in vs $(wc -c <"$OUT") out)"
fi

# ---- 2. wire capture: decode the first 32-byte header, prove big-endian ----
python3 - "$CAPPORT" "$WIRE" <<'PY' &
import socket, sys
port, out = int(sys.argv[1]), sys.argv[2]
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port)); s.listen(1)
c, _ = s.accept()
buf = b""
while len(buf) < 64:                    # header(32) + a little PCM is plenty
    b = c.recv(64 - len(buf))
    if not b: break
    buf += b
open(out, "wb").write(buf)
c.close(); s.close()
PY
CAP=$!
( naptime 12; kill -9 $CAP 2>/dev/null ) & WD2=$!; disown $WD2 2>/dev/null || true
naptime 0.5
perl -e 'alarm 10; exec @ARGV' $RUN --source --peer 127.0.0.1:"$CAPPORT" --pcm "$IN" 2>>"$WORK/src.log" || true
naptime 0.5
kill -9 $CAP 2>/dev/null || true
kill -9 $WD2 2>/dev/null || true

[ -s "$WIRE" ] || fail "no wire bytes captured"
python3 - "$WIRE" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert len(b) >= 32, "short capture"
magic, ver, typ, resv, seq, track, disc, pcmlen = struct.unpack(">IBBHIQQI", b[:32])
print("   wire header (decoded big-endian):")
print("     magic  = 0x%08x (%r)" % (magic, b[:4]))
print("     version=%d  type=%d  reserved=%d  seq=%d" % (ver, typ, resv, seq))
print("     track_samples=%d  discarded_samples=%d  pcm_len=%d" % (track, disc, pcmlen))
ok = (magic == 0x52504C59 and ver == 1 and typ == 1 and pcmlen == 4608)
# sanity: parsing the SAME bytes little-endian must NOT yield the sane values
le = struct.unpack("<IBBHIQQI", b[:32])
print("     (little-endian misread of magic = 0x%08x -> garbage, as expected)" % le[0])
assert ok, "header did not decode as expected big-endian frame"
print("   wire format: BIG-ENDIAN confirmed OK")
PY

echo "== PASS =="
