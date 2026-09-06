#!/bin/bash
# serial-send.sh — push a file to a RUNNING Linux shell over the serial console
# via YMODEM (device runs `rz`, Mac runs `sz`). For the fast driver loop: swap
# beep-i2s.ko without a reboot. Error-checked, so no corrupt module.
#
# The device must be sitting at a shell prompt. Fully quit tio first (pkill tio).
#   ./serial-send.sh [port] [file] [destdir]
# Then reconnect tio and reload the module.
set -u
PORT="${1:-/dev/cu.usbserial-A602SG50}"
FILE="${2:-$HOME/github/beep-firmware/images/beep-i2s.ko}"
DEST="${3:-/tmp}"

[ -e "$PORT" ] || { echo "!! port $PORT not found"; exit 1; }
[ -f "$FILE" ] || { echo "!! file $FILE not found"; exit 1; }
if lsof "$PORT" >/dev/null 2>&1; then
	echo "!! $PORT is busy (tio open?). Quit tio first:  pkill tio"; exit 1
fi
command -v sz >/dev/null || { echo "!! sz missing:  brew install lrzsz"; exit 1; }

echo "[*] opening $PORT @115200 8N1, no flow control"
exec 3<>"$PORT"
stty 115200 cs8 -cstopb -parenb raw -echo -crtscts clocal <&3

# nudge the shell, then start the receiver in $DEST
printf '\r' >&3; sleep 0.3
printf 'cd %s && rz -y\r' "$DEST" >&3
sleep 1.5

echo "[*] sending $(basename "$FILE") ($(wc -c <"$FILE") bytes) -> device:$DEST ..."
sz --ymodem -q "$FILE" <&3 >&3
rc=$?
sleep 0.5
printf '\r' >&3
exec 3>&- 3<&-

if [ "$rc" -eq 0 ]; then
	echo "[✓] transfer complete. Reconnect: tio -b 115200 $PORT   then reload the module."
else
	echo "[x] sz rc=$rc — was the device at a shell prompt with rz available?"
fi
exit "$rc"
