# Recovery + signed OTA — "never UART again"

Goal: after one final UART-backstopped flash (the one that installs the recovery
slot), **no future software failure should require a serial console.** This
composes three layers; only the third (bad *flash*) is still missing today.

| Failure mode | Self-heals without UART? | Mechanism |
|---|---|---|
| Wrong wifi pw / router moved / dead AP | 🔧 implemented, unverified on HW | netcheck watchdog → `Beep-Setup` WPA2 AP + web reconfig |
| Bad config generally | 🔧 mostly (same, unverified) | same watchdog |
| **Corrupt primary uImage** (bad CRC) | ⬜ this doc | `bootm` fails → `bootb` boots recovery on boot 1 |
| **Primary loads but PANICS/HANGS** (e.g. interrupted sysupgrade: kernel wrote, rootfs didn't) | ⚠️ **NOT auto-covered** — see note | needs a good-boot bootcount reset we don't have yet |
| Pushing a bad *update* | ⬜ this doc | signed OTA + recovery slot as backstop |

> ⚠️ **Honest caveat (from the pre-flash review):** `bootb`'s two recovery triggers
> are (a) *immediate* when `bootm` of the primary uImage fails its CRC — covers a
> corrupt kernel on boot 1 — and (b) *bootcount-exceeded*. But our investigation
> found the bootcount only resets on a `saveenv` (never on a healthy boot) and the
> threshold is enormous, so it **cannot distinguish a good boot from a hanging
> one** — a kernel that loads but panics won't trip recovery for ~dozens+ of
> power-cycles, if ever. To truly cover the panic case we must add a **good-boot
> bootcount reset** in the primary firmware (clear the counter once userspace is
> healthy) and measure the real `bootlimit` from the serial `"Set bootcount…"`
> line. Until then, only *corrupt-uImage* auto-recovers; a panicking kernel still
> needs the UART. Do not ship claiming hang-recovery.

## Flash layout (16 MB, w25q128)

Current (single big firmware partition):

```
0x000000  u-boot        256K   [preserve — never write]
0x040000  u-boot-env     64K
0x050000  firmware    15.625M   kernel + squashfs + jffs2 overlay
0xff0000  art            64K    [preserve — wifi cal + MACs]
```

Proposed (carve a recovery slot out of the oversized overlay):

```
0x000000  u-boot        256K   [preserve]
0x040000  u-boot-env     64K
0x050000  firmware       11M    reg = <0x050000 0xb00000>  (primary: 9.94M image + ~1M overlay)
0xb50000  recovery     4.625M   reg = <0xb50000 0x4a0000>  (self-contained initramfs)
0xff0000  art            64K    [preserve]
```

- Primary firmware 11 MB holds the 9.94 MB squashfs image with ~1 MB of jffs2
  overlay — ample, since runtime logs live in tmpfs (RAM), not the overlay; only
  uci config + `/etc/beep-code` + host keys + TLS cert persist there (<200 KB).
- Recovery 4.625 MB holds a minimal **initramfs** image (kernel with embedded
  rootfs). It RAM-boots and depends on **nothing** in the primary partition, so a
  wiped/corrupt primary can't stop it. Target ≤4 MB; if the recovery build exceeds
  4.6 MB, rebalance to firmware 10.5 M / recovery 5.1 M.
- `sysupgrade` of the primary writes only the `firmware` mtd — recovery is
  untouched by normal updates.

## U-Boot failsafe (`bootb`)

The Beep's custom U-Boot (`1.1.4-beep2`) already ships the mechanism: `bootcmd=bootb`
boots `beep_primary` (a raw flash addr) with a bootcount, and on repeated failure
falls back to `beep_recovery`. We keep it — no U-Boot reflash (that IS the
un-brickable backstop).

- `beep_primary  = 0x9f050000`  (already set; OpenWrt kernel at 0x50000)
- `beep_recovery = 0x9fb50000`  (NEW — points at the recovery partition)

### What we learned from the device (2026-09-06)

Env (read from `mtd1` directly — `fw_printenv` has no `/etc/fw_env.config`):
```
beep_primary=0x9f050000     # our OpenWrt kernel (we set this)
beep_recovery=0x9f550000    # ⚠️ points into the MIDDLE of our rootfs — garbage!
bootcmd=bootb   bootdelay=1   board=BEEP_CM2   model=beep3.4
```
MTD: mtd0 u-boot(256K) / mtd1 u-boot-env(64K) / mtd2 firmware(15.6M) /
mtd3 kernel / mtd4 rootfs / mtd5 rootfs_data(384K overlay) / mtd6 art(64K).

`bootb` strings from mtd0:
- "Bootcount exceeded booting recovery partition"
- "Booting primary partition failed using recovery partition"
- "Set bootcount 0x%02x offset 0x%08x (%d)"
- defaults "Using default beep_primary=0x9f550000 / beep_recovery=0x9f050000"
- also a USB-recovery path ("usb_boot_file", "8dev_recovery.bin", `recovery_flash_cmd`).

So `bootb` = boot `beep_primary`, fall back to `beep_recovery` on failed boot OR
bootcount-exceeded, with the count stored at a flash offset.

### Bootcount — decoded empirically (flash diff across a reboot, 2026-09-06)

Method: dumped `mtd0`+`mtd1`, rebooted once, re-dumped, diffed. `mtd0` unchanged;
`mtd1` changed **exactly one byte** at offset `0x8000`.

- **Location:** `mtd1` (u-boot-env) offset **`0x8000`** = flash **`0x048000`**. The
  env *variables* live in the first 32 KB of the partition (padded with `0x00`);
  the bootcount lives in a separate 32 KB region `0x8000`–`0xFFFF` (padded `0xFF`).
- **Scheme:** a **wear-leveled bit-clearing counter**. One reboot cleared the top
  bit of `0x8000` (`0xFF`→`0x7F`); a neighbor byte `0x8002` was already `0x7F` from
  an earlier boot. NOR flash only clears bits without an erase, so U-Boot advances
  one bit/offset per boot across the 32 KB region (matches the *"Set bootcount
  0x%02x offset 0x%08x"* string) rather than erasing every boot.
- **No good-boot reset** in our firmware: `0x8000` stayed `0x7F` for minutes after
  boot. It **accumulates** — but across ~32 KB, so "exceeded" is astronomically far
  off (not an imminent brick).
- **`bootb` boots primary reliably** — the reboot returned straight to primary, so
  it does NOT spuriously fail over to recovery on a normal successful boot.

**Consequences for the design:**
1. The real, present risk is that **`beep_recovery` points at garbage** — so
   **making it a valid recovery image is the priority and is safe to do now** (it
   only ever converts a would-be failure into a recoverable web-reflash, never the
   reverse). Do this.
2. A per-boot bootcount **reset is NOT urgent** (huge threshold) and is awkward to
   do safely (the count shares mtd1's single 64 KB erase block with the env, so a
   naive reset means erase+rewrite env every boot — flash wear + risk). Defer it.
3. **One detail still worth grabbing from serial** (trivial, next UART session):
   the `bootb` console line *"Set bootcount 0x%02x offset 0x%08x (%d)"* prints the
   exact offset + value + decoded count each boot, which nails the precise
   threshold and whether a confirmation/reset marker exists — in case we later want
   the reset. Not a blocker for the recovery slot.

### Confirmed from the stock 16 MB dump (2026-09-06)

The stock 16 MB flash dump settled the remaining unknowns:

- **`bootb` loads a legacy U-Boot uImage** (`0x27051956`), LZMA MIPS kernel, load/entry
  `0x80060000`. Stock primary = "OpenWrt Linux-3.8.13", stock recovery = "OpenWrt
  Linux-3.7.9" (the Beep firmware was OpenWrt-based!). Stock recovery layout = kernel
  uImage + squashfs at `0x200000` within its 5 MB slot.
- **Our OpenWrt `-initramfs-kernel.bin` is the SAME format** (`0x27051956`, load
  `0x80060000`). So `bootb`'s `bootm` boots it directly, and because it's an
  *initramfs* (rootfs embedded in the kernel) it needs no rootfs partition — ideal
  for a self-contained recovery slot.
- **Bootcount reset mechanism SOLVED:** the old dump had ~84 bits cleared at `0x48000`
  yet still booted primary (threshold ≫ 84), and our current flash is near-fresh
  (~2 bits) because our `saveenv` (setting `beep_primary`) erased the whole env block,
  resetting the `0x8000` region to `0xFF`. So: accumulates ~1 bit/boot, **resets on
  any `saveenv`/env-block erase**, threshold enormous. Spurious recovery is a
  non-issue for any real device lifetime, and `fw_setenv`/`saveenv` is the reset lever
  if ever wanted. **Green light to repoint `beep_recovery`.**

## Recovery image contents (task #8)

A separate build target (`initramfs`, minimal package set):
- kernel + ath9k/mac80211 (wifi), wpad-basic-mbedtls
- uhttpd + rpcd + a **stripped recovery web UI**: brings up the `Beep-Setup` AP and
  offers a single action — **upload + verify + write a new primary image**.
- `usign` (verify OTA signature), `mtd`/`sysupgrade` (write primary).
- NO audio stack, NO ffmpeg — recovery only needs "get back online + reflash."

## Signed OTA (task #9)

The web UI gets a firmware-upload path, but **only signature-verified images ever
reach `sysupgrade`** — otherwise the admin UI becomes a full-compromise vector.

1. Generate a `usign` keypair once: private key stored **off-device** (1Password),
   public key baked into the image at `/etc/beep-ota.pub`.
2. Release images are signed with the private key (`usign -S`).
3. Upload flow (authenticated CGI, not JSON-RPC — binary is too big for base64
   over ubus on 64 MB RAM): the CGI (a) requires a valid rpcd session with
   beep-admin, (b) streams the image to tmpfs, (c) `usign -V` against the baked
   pubkey, (d) only on success calls `sysupgrade` (keep config). Reject otherwise.
4. If the new image fails to boot, `bootb` bootcount → recovery slot → web reflash.
   The update loop is closed without serial.

**Two flash paths, split by threat model (anti-remote-hack):**
- **Signed** image → **admin session + `usign` signature**. Safe to allow remotely:
  a stolen session still can't forge the signature.
- **Unsigned** image → **a fresh PHYSICAL triple-tap** on the knob (a 60 s window in
  `/var/run/beep/phys-confirm`, opened by `beepd`). No session substitutes; an admin
  session is *not* sufficient. Since no remote actor can triple-tap, a
  remotely-compromised device can never be made to flash unsigned firmware.

Rationale: an *unbreakable* signature requirement would re-create the orphaning
trap (lost key → never updatable again), so unsigned must stay possible — but
gating it on **physical possession** instead of an admin toggle means the escape
hatch can't be abused remotely. The web UI hides the unsigned-install option until
a triple-tap opens the window (it polls the OTA `GET` status), and shows it even on
the locked-out login screen, so a forgotten password never blocks recovery. The
same rule applies to the **recovery** web-reflash.

## Update posture — web OTA primary, SSH off by default

The signed web OTA is designed to be the **sole** update path an end user needs,
so we do NOT depend on SSH being open:

- **dropbear/SSH ships DISABLED by default.** An open SSH on the LAN is a
  liability: it's a common attack target, needs hardening we'd have to maintain,
  and pushes key management onto the user. With signed OTA + recovery, nothing
  about normal operation or recovery requires it.
- A **System → Advanced "Enable SSH (temporary)"** toggle lets a power user turn
  dropbear on when they explicitly want a shell; off is the default and the
  post-reboot state. (For our own dev on Kyle's units, enable it there; it's not
  the shipping posture.)
- **Force-recovery WITHOUT SSH or UART is NOT possible via a button** — the
  U-Boot strings confirm `bootb` has no GPIO/button check; recovery is entered only
  by a failed boot, bootcount-exceeded, or a USB drive (the Beep has no accessible
  USB). A "hold knob at power-on" trigger would require modifying U-Boot, which we
  won't. The escape hatch for "boots but primary web UI broken" is instead the
  **30 s knob-hold factory reset** (restores admin) and the physical **triple-tap
  unsigned unlock** — both handled by the *primary* firmware, so they need primary
  to boot far enough to run `beepd`.

Net: the un-strand-able set is **{U-Boot, ART, AND a valid recovery slot}** — U-Boot
and ART are safe because we never write them, but the recovery slot IS written once
and is a single un-mirrored copy, so it must be verified with a read-back checksum
right after writing (UART still clipped).

## Pre-flash review corrections (2026-09-06)

Folded in from the design review; these bind the *recovery-slot* work (not the
imminent config-preserving flash):

1. **Recovery image needs baked identity (task #8).** It's a separate initramfs
   with no overlay, so it must BAKE IN: `/etc/beep-ota.pub` (to verify signed
   images), the `99-beep` MAC→code derivation (so its `Beep-Recovery` AP key + admin
   code match the unit's label), and `beepd` + the STM8 i²c stack (or a minimal
   tap-detector writing `/var/run/beep/phys-confirm`) — otherwise the recovery
   web-reflash can't verify signatures, its AP has no joinable key, and the unsigned
   triple-tap path can't fire. Without these, "unsigned rule applies to recovery" is
   false and lost-key + dead-primary = UART.
2. **Set `beep_recovery` from the U-Boot console, not `fw_setenv`.** There is no
   `/etc/fw_env.config` on the device (`fw_printenv` returns nothing), so a blind
   `fw_setenv` would fail or corrupt the shared env+bootcount block. During the
   UART repartition session use `setenv beep_recovery 0x9fb50000; saveenv`, exactly
   as `beep_primary` was set. (Optionally ship a verified `/etc/fw_env.config` —
   offset 0x40000, size 0x10000, erase 0x10000 — and prove a round-trip first.)
3. **Env-default fragility:** `bootb`'s COMPILED defaults are swapped vs ours
   (default `beep_primary=0x9f550000`, `beep_recovery=0x9f050000`). If the env block
   is ever lost, U-Boot would try primary at 0x550000 (garbage) then recovery at
   0x050000 (our kernel) — non-fatal today, but any future change to slot addresses
   must account for this or a lost env becomes a brick.
4. **Verify recovery after writing:** full read-back + checksum while UART is
   clipped; consider marking the recovery mtd read-only at runtime so no later
   `sysupgrade`/`mtd` bug can touch it.

## Sequencing (important)

Installing the recovery slot requires ONE full-image flash that repartitions —
and that single flash is the *last* one that wants a UART backstop. So: land +
verify this on unit #1 with the UART still clipped on, and from then on every
update is recovery-protected and network-only.
