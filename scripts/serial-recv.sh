#!/bin/sh
# serial-recv.sh — pull a file OFF the device over the 3-wire UART via YMODEM.
#
# The device runs `sz` (send) and the host runs `rz` (receive). Use this to copy
# flash backups (e.g. the per-unit `art` partition) off a RAM-booted initramfs when
# there is no network shell available — which is the normal case during a flash:
# stock firmware gives you only U-Boot over UART, our lean initramfs has the wifi
# stack stripped, and even the full firmware ships SSH off by default. Serial YMODEM
# is the only channel.
#
# On the device (RAM-booted initramfs shell), first dump the partition, e.g.:
#     dd if=/dev/mtd6 of=/tmp/u2-art.bin        # art = MAC + wifi calibration
# then run this on the host to fetch it.
#
# Usage: serial-recv.sh [PORT] [OUTDIR] [REMOTE_FILE]
#   PORT         serial device       (default: the usual adapter path)
#   OUTDIR       host dir to save to  (default: current directory)
#   REMOTE_FILE  path on the device   (default: /tmp/backup.bin)
set -u
PORT="${1:-/dev/cu.usbserial-A602SG50}"
OUTDIR="${2:-.}"
REMOTE="${3:-/tmp/backup.bin}"
[ -e "$PORT" ] || { echo "!! port $PORT not found"; exit 1; }
if lsof "$PORT" >/dev/null 2>&1; then
	echo "!! $PORT is busy (tio still open?). Quit it first:  pkill tio"; exit 1
fi
command -v rz >/dev/null || { echo "!! rz missing:  brew install lrzsz"; exit 1; }
mkdir -p "$OUTDIR"
echo "[*] opening $PORT @115200 8N1 raw, no flow control (3-wire console)"
exec 3<>"$PORT"
stty 115200 cs8 -cstopb -parenb raw -echo -crtscts clocal <&3
printf '\r' >&3; sleep 0.3
echo "[*] asking the device to send $REMOTE  (sz --ymodem)…"
printf 'sz -y %s\r' "$REMOTE" >&3
sleep 1.5
echo "[*] receiving into $OUTDIR/ …"
( cd "$OUTDIR" && rz -y -vv <&3 >&3 )
rc=$?
sleep 0.5
exec 3<&-
echo "[*] done (rc=$rc) — check $OUTDIR/$(basename "$REMOTE") and verify its size/sha"
