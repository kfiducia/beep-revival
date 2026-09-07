#!/bin/sh
# beep-restore.sh — write a saved partition image back to a Beep over SSH.
#
# Companion to beep-backup.sh. Matches the partition by NAME (looks up the live mtd
# number from /proc/mtd — numbers differ between units, so we never hardcode them),
# refuses an image larger than the partition, and REFUSES to touch `u-boot` or `art`
# unless you pass --force (u-boot is the bootloader; art is your unit's unrecoverable
# MAC + wifi calibration — neither should ever change). Streams the file straight to
# the on-device `mtd write` (which erases then writes, by name) — no device staging.
#
# Typical use: restore the stock OS after a bad experiment ->
#   scripts/beep-restore.sh 10.9.100.166 firmware  ~/…/u2-backup/firmware.bin
# (recovery / kernel / rootfs / rootfs_data / env0 / env1 are also restorable by name)
#
# Usage: beep-restore.sh <device-ip> <partition-name> <backup-file> [--force] [password]
set -u
IP="${1:?usage: beep-restore.sh <ip> <name> <file> [--force] [password]}"
NAME="${2:?partition name (e.g. firmware, kernel, rootfs, recovery)}"
FILE="${3:?backup .bin to write}"
FORCE=0; PW=root
shift 3 || true
for a in "$@"; do case "$a" in --force) FORCE=1;; *) PW="$a";; esac; done
[ -f "$FILE" ] || { echo "!! file not found: $FILE"; exit 1; }
command -v sshpass >/dev/null || { echo "!! need sshpass:  brew install sshpass"; exit 1; }

SSH() { SSHPASS="$PW" sshpass -e ssh \
  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PubkeyAuthentication=no \
  -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa -o Ciphers=+aes128-ctr,aes128-cbc,3des-cbc \
  -o ConnectTimeout=10 "root@$IP" "$@"; }

# guardrail: never silently write the irreplaceable partitions
case "$NAME" in
  u-boot|art)
    [ "$FORCE" = 1 ] || {
      echo "!! REFUSING to write '$NAME' — bootloader/calibration; it should never change."
      echo "   art especially is your unit's UNRECOVERABLE MAC + wifi cal."
      echo "   Re-run with --force only if you truly accept the brick risk."; exit 1; } ;;
esac

# find the live mtd number + size for NAME (numbers vary between units)
LINE="$(SSH 'cat /proc/mtd' | awk -v n="\"$NAME\"" '$4==n{print}')"
[ -n "$LINE" ] || { echo "!! no partition named '$NAME' on the device — check: ssh root@$IP cat /proc/mtd"; exit 1; }
NUM="$(echo "$LINE" | awk '{sub(/:$/,"",$1); print $1}')"
PSIZE=$(( 0x$(echo "$LINE" | awk '{print $2}') ))
FSIZE=$(wc -c < "$FILE")
echo "target : $NAME = /dev/$NUM   (partition $PSIZE bytes)"
echo "source : $FILE   ($FSIZE bytes)"
[ "$FSIZE" -le "$PSIZE" ] || { echo "!! file is LARGER than the partition — wrong image; aborting."; exit 1; }
[ "$FSIZE" -eq "$PSIZE" ] || echo "   (file smaller than partition — fine for a short kernel/rootfs image)"

printf 'Overwrite %s (/dev/%s) on %s? Type YES to proceed: ' "$NAME" "$NUM" "$IP"
read ans; [ "$ans" = "YES" ] || { echo "aborted."; exit 1; }
echo "[*] mtd write (erases + writes, by name)…"
cat "$FILE" | SSH "mtd write - $NAME"
rc=$?
[ "$rc" = 0 ] && echo "✅ wrote '$NAME'. Verify (md5sum /dev/$NUM vs your backup) and reboot." \
             || { echo "❌ mtd write failed (rc=$rc)"; exit "$rc"; }
