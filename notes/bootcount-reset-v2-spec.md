# bootcount-reset v2 — lightweight nibble-advance (design spec, ready to implement w/ HW)

**Goal:** replace the gated `usr/libexec/beep/bootcount-reset` (which does a full
`fw_setenv`/env-sector *erase* on every good boot) with the stock `beepupdate`
approach: a **single-byte bit-clear** that advances one nibble — no erase, far less
wear, and it can't disturb the CRC'd env variables (they're in the first 0x8000; the
counter is in 0x8000..0xFFFF).

## Mechanism (from the bootb + beepupdate disassembly)
- Counter region: `/dev/mtd1` (env0), bytes `0x8000..0xFFFF`.
- A "good boot" **advances one nibble**: clear the currently-active nibble's remaining
  set bits to 0. That moves the scan to the next nibble, which is fresh (`0xf` = strike 0).
- Byte progression within an epoch (top-bit-first): `0xff→0x7f→0x3f→0x1f` (high nibble
  consumed), reset clears high nibble → `0x0f`; next epoch `0x0f→0x07→0x03→0x01`, reset
  clears low nibble → `0x00`; scan then moves to the next byte (`0xff`).
- NOR flash programs 1→0 without an erase, so writing e.g. `0x1f→0x0f` (clearing the
  high nibble) is a legal in-place program. **This is the whole trick — no erase.**

## Algorithm
1. Read `0x8000..0xFFFF`; scan low→high for the first byte `!= 0x00` (the active byte).
2. `hi = (byte & 0xf0)`; if `hi != 0` the high nibble is active, else the low nibble.
3. New byte value = active byte with the active nibble's bits cleared to 0
   (`byte & 0x0f` if high active; `byte & 0xf0` → but low active means high already 0, so
   `0x00`). i.e. AND-mask off the active nibble.
4. Program that one byte back at its offset (bit-clear only — assert new == old & new,
   i.e. we only clear bits, never set; abort if that invariant fails).
5. **Depletion edge:** if no non-zero byte exists (region all `0x00`, ~65536 good boots),
   fall back to the heavy path — erase + rewrite env0 (this is what the current v1 script
   does via `fw_setenv`). Extremely rare; fine to keep v1's logic for just this case.

## Implementation notes
- **Do NOT do the byte program from shell.** Writing a single byte in-place to a NOR mtd
  char device needs `MEMWRITE`/proper programming semantics — `dd` will not do a partial
  in-place program correctly and may erase. Implement as a tiny C helper (mirror how
  `beepupdate` opens `/dev/mtd1` and writes), or add a `bootcount` subcommand to `beepd`
  (it already owns i2c; adding an mtd op is small). C helper is cleaner + testable.
- Keep the v1 SAFETY GATES: no-op unless `/etc/beep-fwenv-verified` exists AND a sanity
  read of the region looks like our layout. Never write if the active-byte read looks
  wrong (e.g. `0x00` where we expected non-zero, or a nibble that isn't `f/7/3/1`).
- Wire it exactly where v1 is: `rc.local` after the 60 s "healthy boot" settle.

## Why this is better than v1 (fw_setenv erase)
- **Wear:** v1 erases a 64K sector every good boot (~thousands of erases/yr on a rebooty
  unit); v2 clears ~1 bit and only erases once per ~65536 boots. NOR endurance is ~100k
  erase cycles — v1 is not catastrophic but v2 is obviously right.
- **Blast radius:** v1's erase rewrites the whole env0 (incl. variables) each time — one
  bad write corrupts the live env. v2 touches only the counter half; the CRC'd variables
  are never rewritten. (Note: variables ARE redundantly mirrored to env1, so even v1 is
  survivable — but v2 avoids the question entirely.)

## Test plan (bench, UART clipped)
1. Run `beep-bootcount-probe.sh` — note strike + current offset.
2. Trigger a good boot (normal boot to healthy). Confirm the probe shows the nibble
   advanced (strike back to 0, offset moved by one nibble) and NO sector erase occurred
   (env variables unchanged — `fw_printenv` still intact, CRC good).
3. Reboot 3× with the reset DISABLED (simulate hang path): confirm strike climbs 1→2→3
   and, with beep_recovery repointed, boot 4 harmlessly re-boots primary.
4. Confirm we only ever clear bits (never set) — instrument the helper to refuse a write
   that would set a bit.

## Status
Spec only. Not implemented — the flash-write helper must be written and validated on a
bench unit (it writes NOR). Pairs with RECOVERY-DESIGN.md graduation criteria items 3–4.
