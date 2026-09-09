#!/bin/sh
# beep-bootcount-probe — READ-ONLY decode of the U-Boot bootcount counter.
#
# Run this ON A UNIT (over serial or SSH) between reboots to watch the 3-strikes
# counter climb. It only READS /dev/mtd1 — it never writes, so it is safe to run
# anytime. Decodes per the disassembled bootb scheme (see docs/RECOVERY-DESIGN.md,
# "Bootcount — DEFINITIVE"):
#
#   region  : env0 (mtd1) offset 0x8000..0xFFFF  (32 KB, outside the CRC'd env)
#   march   : one bit cleared per primary boot, top-bit-first within a byte
#             0xff->0x7f->0x3f->0x1f (high nibble), then 0x0f->0x07->0x03->0x01 (low)
#   count   : active nibble mapped 0xf->0, 0x7->1, 0x3->2, 0x1->3  (>=3 => recovery)
#   reset   : a GOOD boot should clear the active nibble (advance to the next, count 0)
#
# Bench use: note the count, reboot, run again. On our firmware (good-boot reset
# gated OFF) you should see it climb 0->1->2->3 across reboots and NOT reset — which
# is exactly the danger. After the beep_recovery repoint, reaching 3 is harmless.

# (testability) MTD/START/LEN are overridable from the environment so the decode can be
# exercised off-device against a synthetic counter image; on a unit these are all unset,
# so the defaults below are byte-identical to the original hard-coded values.
MTD="${MTD:-/dev/mtd1}"
START="${START:-32768}"        # 0x8000
LEN="${LEN:-32768}"            # 0x8000  (scan the whole counter region)

[ -e "$MTD" ] || { echo "no $MTD (is this the stock mtd layout? check /proc/mtd)"; exit 1; }

# Pull the counter region as hex bytes (one per line).
bytes=$(dd if="$MTD" bs=1 skip="$START" count="$LEN" 2>/dev/null | od -An -v -tx1 | tr ' ' '\n' | grep -E '^[0-9a-f]{2}$')

cleared_bits=0     # total history (cleared bits across consumed region)
cur_off=-1; cur_byte=""; i=0
for b in $bytes; do
	dec=$((0x$b))
	# popcount(dec) -> set bits; cleared = 8 - set
	set=0; v=$dec
	while [ "$v" -gt 0 ]; do set=$((set + (v & 1))); v=$((v >> 1)); done
	cleared_bits=$((cleared_bits + (8 - set)))
	if [ "$dec" -ne 0 ] && [ "$cur_off" -lt 0 ]; then cur_off=$i; cur_byte=$b; fi
	i=$((i + 1))
done

if [ "$cur_off" -lt 0 ]; then
	echo "counter region fully consumed (all 0x00) — depleted; a saveenv/erase would reset it"
	echo "total cleared bits: $cleared_bits"
	exit 0
fi

byte=$((0x$cur_byte))
hi=$(( (byte & 0xf0) >> 4 ))
if [ "$hi" -ne 0 ]; then nib=$hi; which=high; else nib=$((byte & 0x0f)); which=low; fi
case "$nib" in
	15) count=0 ;;   # 0xf
	7)  count=1 ;;
	3)  count=2 ;;
	1)  count=3 ;;
	*)  count="?? (unexpected nibble 0x$(printf %x $nib) — mid-write or non-standard)" ;;
esac

printf 'bootcount region %s\n' "$MTD @ 0x8000"
printf '  current byte : 0x%02x  at region offset 0x%04x (%s nibble active)\n' "$byte" "$cur_off" "$which"
printf '  STRIKE COUNT : %s   (>=3 => next boot falls to beep_recovery)\n' "$count"
printf '  history      : %d bits cleared total (~%d good-boot advances if reset was working)\n' "$cleared_bits" $((cleared_bits / 4))
