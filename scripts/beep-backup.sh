#!/bin/sh
# beep-backup.sh — full, VERIFIED, restorable backup of a Beep's SPI flash over SSH.
#
# Why this exists: the one truly unrecoverable partition is `art` (your unit's MAC +
# wifi calibration). The mtd *numbers* are NOT the same on every unit — some have an
# extra rootfs_data partition that shifts everything — so `art` may be mtd6 on one
# device and mtd7 on another. This script therefore keys on the partition *name* from
# /proc/mtd, never a hardcoded number. It streams each partition straight to your
# machine (no /tmp staging, so it can't OOM the 64 MB device) and verifies every dump
# against the chip's own md5. Keep the output dir; beep-restore.sh can push any
# partition back if a flash ever goes wrong.
#
# Prereq: a root SSH login. On a stock Beep, do the dev unlock at the U-Boot prompt
# first — `setenv dev_lRapcY7M 1; saveenv; boot` — which turns dropbear on. The 2015
# dropbear needs the legacy-algorithm flags below (handled automatically).
#
# Usage: beep-backup.sh <device-ip> [outdir] [password]
#          password defaults to "root" (the stock root password)
set -u
IP="${1:?usage: beep-backup.sh <device-ip> [outdir] [password]}"
OUT="${2:-beep-backup-$IP}"
PW="${3:-root}"
command -v sshpass >/dev/null || { echo "!! need sshpass:  brew install sshpass"; exit 1; }

CM="/tmp/bcm-%C"   # keep the control-socket path SHORT (Unix sockets cap ~104 chars)
SSH() { SSHPASS="$PW" sshpass -e ssh \
  -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o PubkeyAuthentication=no \
  -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa -o Ciphers=+aes128-ctr,aes128-cbc,3des-cbc \
  -o ControlMaster=auto -o ControlPath="$CM" -o ControlPersist=60 -o ConnectTimeout=10 \
  -n "root@$IP" "$@"; }

mkdir -p "$OUT"
echo "== unit @ $IP — partition table (source of truth) =="
SSH 'cat /proc/mtd' | tee "$OUT/proc-mtd.txt"
echo

# parse "mtdN=name" pairs from /proc/mtd (skip the header line)
PAIRS="$(SSH 'cat /proc/mtd' | awk 'NR>1 && $1 ~ /^mtd[0-9]+:/ { n=$1; sub(/:$/,"",n); nm=$4; gsub(/"/,"",nm); print n"="nm }')"
[ -n "$PAIRS" ] || { echo "!! could not read /proc/mtd"; exit 1; }

: > "$OUT/MANIFEST.tsv"
printf 'mtd\tname\tsize_hex\tmd5\tstatus\n' >> "$OUT/MANIFEST.tsv"
fail=0
for p in $PAIRS; do
  num="${p%%=*}"; nm="${p#*=}"
  printf '  %-5s %-12s ... ' "$num" "$nm"
  SSH "dd if=/dev/$num 2>/dev/null" > "$OUT/$nm.bin"
  loc="$(md5sum "$OUT/$nm.bin" | cut -d' ' -f1)"
  rem="$(SSH "md5sum /dev/$num" | cut -d' ' -f1)"
  sz="$(SSH "cat /proc/mtd" | awk -v n="$num:" '$1==n{print $2}')"
  if [ "$loc" = "$rem" ] && [ -n "$loc" ]; then st="OK"; else st="MISMATCH"; fail=1; fi
  printf '%s  md5=%s\n' "$st" "$loc"
  printf '%s\t%s\t%s\t%s\t%s\n' "$num" "$nm" "$sz" "$loc" "$st" >> "$OUT/MANIFEST.tsv"
done

# close the multiplexed connection
SSH -O exit 2>/dev/null || true
echo
echo "== MANIFEST ($OUT/MANIFEST.tsv) =="
column -t -s "$(printf '\t')" "$OUT/MANIFEST.tsv" 2>/dev/null || cat "$OUT/MANIFEST.tsv"
echo
if [ "$fail" = 0 ]; then
  echo "✅ backup complete + every partition md5-verified against the chip."
  echo "   Keep $OUT/ safe. 'art' is the irreplaceable one (MAC + wifi cal)."
else
  echo "❌ one or more partitions FAILED verification — do NOT trust this backup; re-run."
  exit 2
fi
