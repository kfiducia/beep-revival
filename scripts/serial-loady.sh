#!/bin/bash
# serial-loady.sh — arm U-Boot `loady` and stream an image over YMODEM, race-free.
#
# Run this AFTER fully quitting tio, with the Beep sitting at the `ar7240>` prompt.
# It holds the port for the whole transfer, so there's no tio conflict and no
# window for loady to time out between "arm" and "send".
#
#   ./serial-loady.sh [port] [image] [loadaddr]
#
# Then reconnect tio and `bootm <loadaddr>`.

set -u
PORT="${1:-/dev/cu.usbserial-A602SG50}"
IMG="${2:-$HOME/github/beep-firmware/images/openwrt-ath79-generic-8dev_carambola2-initramfs-kernel.bin}"
ADDR="${3:-0x82000000}"

[ -e "$PORT" ] || { echo "!! port $PORT not found"; exit 1; }
[ -f "$IMG" ]  || { echo "!! image $IMG not found"; exit 1; }
if lsof "$PORT" >/dev/null 2>&1; then
	echo "!! $PORT is busy (tio still open?). Quit tio first:  pkill tio"; exit 1
fi
command -v sz >/dev/null || { echo "!! sz missing:  brew install lrzsz"; exit 1; }

# Open the port ONCE on fd 3, THEN apply termios to that fd. macOS resets termios
# on open, so `stty -f <path>` before opening doesn't stick — and the default
# leaves RTS/CTS flow control on, which blocks all TX on a 3-wire console.
echo "[*] opening $PORT (fd3) @ 115200 8N1 raw, NO flow control (3-wire console)"
exec 3<>"$PORT"
# -crtscts: no RTS/CTS (no CTS wired -> would block TX); clocal: ignore carrier
stty 115200 cs8 -cstopb -parenb raw -echo -crtscts clocal <&3

echo "[*] arming: loady $ADDR"
printf 'loady %s\r' "$ADDR" >&3
sleep 2                       # let U-Boot enter loady and start sending 'C'

echo "[*] sending $(basename "$IMG")  (~10 min at 115200)…"
sz --ymodem -vv "$IMG" <&3 >&3
rc=$?

echo
if [ "$rc" -ne 0 ]; then
	exec 3>&- 3<&-
	echo "[x] sz exit=$rc. If 'timeout on pathname': device wasn't at ar7240> (reboot+ESC), or port was shared."
	exit "$rc"
fi

echo "[✓] transfer complete."
LOG="${LOG:-$HOME/beep-revival/dump/phase0-ramboot.log}"
echo "[*] booting: bootm $ADDR"
echo "[*] streaming + logging the boot to: $LOG"
echo "[*] --> watch for the login prompt, then Ctrl-C and reconnect: tio -b 115200 $PORT"
: > "$LOG"
printf 'bootm %s\r' "$ADDR" >&3
# stream the OpenWrt boot to screen + logfile until you Ctrl-C
cat <&3 | tee -a "$LOG"
exec 3>&- 3<&-
