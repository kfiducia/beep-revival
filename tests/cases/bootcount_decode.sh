#!/bin/sh
# Bootcount decode — the READ-ONLY strike-counter math in scripts/beep-bootcount-probe.sh.
# A regression here misreports how many boots away from dropping to recovery the unit is,
# so it is worth pinning. We feed the probe a synthetic /dev/mtd1 counter image (via the
# MTD/START/LEN env overrides) and assert the decoded STRIKE COUNT.
#
# March scheme (top-bit-first per byte): 0xff->0x7f->0x3f->0x1f (high nibble),
# then 0x0f->0x07->0x03->0x01 (low). Active nibble maps 0xf->0, 0x7->1, 0x3->2, 0x1->3.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="bootcount-decode"

PROBE="$REPO_ROOT/scripts/beep-bootcount-probe.sh"
t_tmpdir; tmp="$T_TMP"

# run_probe OCTAL_ESCAPES LEN  ->  probe stdout over a synthetic counter image
run_probe() {
	printf '%b' "$1" > "$tmp/mtd"
	MTD="$tmp/mtd" START=0 LEN="$2" sh "$PROBE"
}

# Octal escapes for the bytes we need (printf %b understands \0ddd):
#   0xff=\0377 0x7f=\0177 0x3f=\0077 0x1f=\0037 0x0f=\0017 0x07=\0007 0x00=\0000 0x5f=\0137

assert_contains "$(run_probe '\0377\0377\0377\0377' 4)" "STRIKE COUNT : 0"  "0xff high nibble => count 0 (fresh)"
assert_contains "$(run_probe '\0177\0377\0377\0377' 4)" "STRIKE COUNT : 1"  "0x7f => count 1"
assert_contains "$(run_probe '\0077\0377\0377\0377' 4)" "STRIKE COUNT : 2"  "0x3f => count 2"
assert_contains "$(run_probe '\0037\0377\0377\0377' 4)" "STRIKE COUNT : 3"  "0x1f => count 3 (next boot -> recovery)"
assert_contains "$(run_probe '\0017\0377\0377\0377' 4)" "STRIKE COUNT : 0"  "0x0f low nibble => count 0"
assert_contains "$(run_probe '\0007\0377\0377\0377' 4)" "STRIKE COUNT : 1"  "0x07 low nibble => count 1"

# Consumed leading bytes are skipped; the first non-zero byte is the active one.
assert_contains "$(run_probe '\0000\0000\0177\0377' 4)" "STRIKE COUNT : 1"  "leading 0x00s skipped -> 0x7f => count 1"

# Fully depleted region (all 0x00) reports depletion rather than a bogus count.
assert_contains "$(run_probe '\0000\0000\0000\0000' 4)" "depleted"           "all-zero region => depleted"

# A non-standard nibble is flagged, not silently mapped to a count.
assert_contains "$(run_probe '\0137\0377\0377\0377' 4)" "unexpected nibble"  "0x5f => flagged as unexpected"

t_done
