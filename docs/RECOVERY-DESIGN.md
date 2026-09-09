# Recovery + signed OTA — "never UART again"

Goal: after one final UART-backstopped flash (the one that installs the recovery
slot), **no future software failure should require a serial console.** This
composes three layers; only the third (bad *flash*) is still missing today.

---

## ⚠️ 2026-09-08 — READ FIRST: verified sizes + a live brick post-mortem

Two things happened on 2026-09-08 that **correct assumptions baked into the rest of
this doc**. Where the older sections below conflict with this one, this one wins.

### A. A real unit bricked today — and it was NOT a bad flash

An unsigned/physical-tap web-OTA install bricked a unit into a UART-only state
(recovered at the bench via a lean-initramfs rescue boot). The chain, from serial +
the rescue system:

1. The OTA ran `sysupgrade` **with keep-config** (no `-n`), stashing the old config
   as `sysupgrade.tgz` in the overlay.
2. First boot after the flash **hung processing that config restore** — the overlay
   was left holding *only* `sysupgrade.tgz`, never extracted into `/etc/config`.
3. With **no good-boot bootcount reset**, each stuck reboot ticked the counter until
   `bootb` hit **`Bootcount exceeded booting recovery partition`**.
4. It fell to `beep_recovery = 0x9f550000` (garbage, mid-rootfs) → `## Booting image
   at 9f550000 ... Bad Magic Number` → **`Failed to boot either partitions`** → UART.
5. **The primary squashfs was 100 % intact** — mounted read-only from the rescue
   system, full read-through, 0 I/O errors. This was a bad *first-boot config
   restore*, escalated to a brick by the missing bootcount reset + the garbage
   recovery slot. A working recovery slot turns step 4 into a network reflash.

**Re-prioritized consequence — two cheap fixes would have PREVENTED this brick, and
should land alongside (or before) the slot:**

1. **Good-boot bootcount reset is NO LONGER optional.** The section below
   ("Bootcount — decoded empirically") deferred it as "astronomically far off." The
   incident disproves that: bootcount-exceeded stranded a *perfectly good* primary
   because a *hang* (not a bad image) kept rebooting and the counter never reset.
   Clear the counter from userspace once a boot is healthy (e.g. beepd up + network
   up). This is arguably the single highest-value change here.
2. **Harden the OTA flash path** (likely its own issue). The keep-config restore is
   the true root cause. Either use `sysupgrade -n` for OTA (drop config, never hang),
   or make the first-boot restore robust (sanitize config across variant changes;
   timeout → fall back to defaults). Today's `beep-ota` also backgrounds `sysupgrade`
   fire-and-forget on RAM-tight firmware — its own hazard.

### B. The recovery slot size was re-measured on the build host — ≤4.6 MB is NOT achievable

The "slim to ~10 MB to fit a ~5 MB recovery slot" premise (and the 4.625 MB slot in
the layout below) does **not** survive contact with a modern (kernel 6.6) build.
Measured on the iMac `beep-build` container, ath79/generic, `8dev_carambola2`:

| Recovery build (initramfs unless noted) | Flash size | vs stock 5 MB slot |
|---|---|---|
| **Full-featured** (wifi AP + rpcd/ubus + dnsmasq + px5g + iwinfo + usign + beepd) | **6.51 MB** | +1.5 MB over |
| **Minimal** (wifi AP + uhttpd + usign + busybox udhcpd only) | **5.57 MB** | +0.57 MB over |
| Minimal, **squashfs** sysupgrade (stock's packaging) | **5.75 MB** | +0.75 MB over |

Stripping every optional feature *and* switching to squashfs (stock's own approach)
still lands ~5.5–5.75 MB. **Packaging is not the lever** — the floor is the modern
**6.6 kernel (~1.9 MB) + ath9k/mac80211 + the mbedtls WPA2 stack**.

**Second, harder ceiling — RAM, confirmed live during today's rescue:** a *full*
initramfs (~9.6 MB → ~30 MB unpacked) **OOM-panics on 64 MB**
(`Kernel panic - System is deadlocked on memory`); the lean one (~5.1 MB → ~14 MB)
boots. So the recovery build is **doubly constrained** — flash slot *and* unpacked
tmpfs — and must stay **lean-initramfs-class** (audio stack / wifi-full / opkg all
stripped). This independently rules out the 6.51 MB full-featured variant and points
at the **~5.57 MB minimal** build.

### C. How stock fit recovery in 5 MB (teardown of unit #2's dump, mtd2 `recovery`)

Stock's 5 MB slot = a **0.92 MB LZMA uImage kernel (OpenWrt Linux-3.7.9)** at
offset 0 + a **2.21 MB xz squashfs** at 0x200000 = only 4.25 MB used. That squashfs
is **not a stripped recovery — it's a near-full OpenWrt Barrier Breaker (r35770,
2014)**: full **LuCI** (`index.html` → `/cgi-bin/luci`), `uhttpd`, full `wpad`
(WPA2), `dnsmasq`, `dropbear`, `avahi`. Recovery flashing was just LuCI's
"Flash new firmware" page → `sysupgrade`/`mtd`, accepting **any unsigned image over
plain HTTP**. It fit because 2014 parts are a fraction of today's size and it carried
**no TLS library at all** (only `libcrypt` for password hashing; its old `wpad` had
self-contained crypto). We can't replicate that: our 6.6 kernel is ~2× the 3.7.9
one, and modern `wpad-basic-mbedtls` + uhttpd-HTTPS + px5g pull in `libmbedtls`.

Also note the **stock geometry**: recovery is at the **front** (0x050000, 5 MB),
primary **after** it (0x550000), with **dual env** copies (0x040000 + 0xFE0000).
bootb's compiled defaults (`beep_primary=0x9f550000`, `beep_recovery=0x9f050000`)
are exactly this stock geometry — the "swapped defaults" noted below are just stock's
layout, not a bug.

### D. Decision (2026-09-08): shrink the primary to buy a ~5.75 MB recovery slot

The usable region between `u-boot-env` and `art` is **0x050000..0xff0000 = 15.625 MB**.
Primary (current default image ≈ **10.06–10.55 MB**) + a 5.6 MB recovery slot
overshoots that **before any primary overlay**. So the primary must lose ~0.6–1 MB.
Measured primary-trim budget (uncompressed → ~35–40 % of that compressed in xz):

- **opkg metadata** `/usr/lib/opkg` = **2.1 MB uncompressed (~0.6 MB compressed)** —
  the light lever; `build.sh` already strips it in the LEAN path
  (`rm -rf $R/usr/lib/opkg`). Recovery never installs packages, and the primary can
  keep opkg's *binary* while dropping the package lists.
- **Snapcast** cascade — dropping multi-room removes `snapserver` (2.25 MB) +
  `snapclient` (0.69 MB) + `libstdc++` (2.06 MB) + **OpenSSL** `libcrypto`+`libssl`
  (3.6 MB, pulled only by Snapcast) + libvorbis/opus + libatomic ≈ **3–4 MB
  uncompressed (~1.5–2 MB compressed)**. Heavy lever; costs the multi-room feature
  (already flagged as saturating the AR9331 — see AGENTS "lighter multi-room").

**Fit check against the 16 MB ceiling — measured from the real `RECOVERY=1`
`build.sh` (2026-09-08).** The shipped recovery variant (wifi WPA2 AP +
`dnsmasq`/DHCP reused from the primary + `uhttpd` + `usign` + the stripped
`recovery-overlay` reflash UI; NO audio/rpcd/opkg; signed-only) builds to
**5.80 MB** (6,083,832 B). The optional `beepd`/i2c triple-tap unsigned path adds
~0.25 MB (→ ~6.05 MB) and is omitted from the lean build.

| Recovery feature set | Recovery img | Slot 0x5E0000 (5.875 MB) | Primary slot 0x9C0000 (9.75 MB) | Fits 15.625 MB? |
|---|---|---|---|---|
| **Shipped: signed-only + dnsmasq** | **5.80 MB** | 75 KB headroom | primary img ≤ ~9.4 MB (after opkg strip) | ✅ **with opkg strip only — no Snapcast loss** |
| + `beepd` triple-tap (optional) | ~6.05 MB | needs 0x600000 slot | primary must drop ~0.2 MB more | ⚠️ tighter — weigh vs multi-room |

**Good outcome: the signed-only recovery fits alongside the current primary with only
the lossless opkg-metadata strip** (`rm -rf $R/usr/lib/opkg`, ~0.6 MB, already done in
LEAN) — Snapcast/AirPlay stay. The RAM ceiling (§B) also favors this lean build.
Triple-tap-in-recovery is a later upgrade that costs ~0.2 MB more of primary. *Note:*
recovery reuses the primary's `dnsmasq` setup-AP, so it inherits whatever DHCP fix
lands on PR #18 — do not give it a separate DHCP path.

**Chosen target geometry** (custom offsets; keep a proper WPA2 + `usign`-verified
recovery, minimal package set to satisfy the RAM ceiling):

Recovery is anchored to **end at `art` (0xFF0000)**, so its start = 0xFF0000 −
slot-size. Sized to the measured 5.80 MB image with ~75 KB headroom:

```
0x000000  u-boot        256K   [preserve]
0x040000  u-boot-env     64K
0x050000  firmware     9.75M   reg = <0x050000 0x9C0000>   primary (opkg-stripped img ~9.4M + overlay)
0xA10000  recovery    5.875M   reg = <0xA10000 0x5E0000>   signed-reflash initramfs (5.80M measured)
0xFF0000  art            64K    [preserve]
```

→ `setenv beep_primary 0x9f050000; setenv beep_recovery 0x9fa10000; saveenv`.

If the optional `beepd` triple-tap is added to recovery (~6.05 MB), bump the slot to
**6.0 MB** (0x600000, start 0xA00000 → `beep_recovery=0x9fa00000`, primary slot
9.625 MB) and trim the primary ~0.2 MB further. Either way, **both** `beep_primary`
and `beep_recovery` become non-default offsets, so correction #3 below (ship a
verified `/etc/fw_env.config`, treat a lost env as a brick risk) is binding.

### Revised action order (supersedes the "Sequencing" section at the bottom)

1. **Good-boot bootcount reset** in the primary firmware. *Implemented, gated OFF:*
   `/usr/libexec/beep/bootcount-reset` (called from `/etc/rc.local` after a 60 s
   settle — reaching there means the boot cleared early-init, a reasonable "healthy"
   proxy) rewrites the env sector via `fw_setenv` to clear U-Boot's wear-leveled
   bootcount. It is a **no-op until `/etc/beep-fwenv-verified` exists** and until
   `fw_printenv` reads back `beep_primary=0x9f050000` through the shipped
   `/etc/fw_env.config` — because a wrong env geometry would reinit the env to
   U-Boot's stock defaults and brick this layout. **Bench step to enable:** on a
   UART unit, prove `fw_printenv beep_primary` → `0x9f050000` and a `fw_setenv`
   round-trip, confirm on serial the bootcount region returns to `0xFF`, then
   `touch /etc/beep-fwenv-verified`. Honest scope: this is defense-in-depth for a
   *good* primary that reboots a lot — it does **not** fix a deterministic early hang
   (that never reaches rc.local); the valid recovery slot + OTA hardening cover that.
2. **OTA keep-config hardening.** *Partly done:* `beep-ota` now `setsid`-detaches the
   flash so a uhttpd CGI teardown can't kill `sysupgrade` mid-write, and logs the
   attempt. *Deferred (the real fix):* stop relying on stock keep-config — preserve
   only our identity set (beep-code, cert/key, wireless + system config) ourselves and
   flash `-n`, so a variant-mismatch can't hang the restore. Blocked on the setup-AP
   DHCP fix (PR #18): `-n` drops the wifi client config → the unit lands on the setup
   AP, which must be reachable first. Likely its own issue.
3. **Recovery slot**: build the ~5.57 MB minimal initramfs (bake `/etc/beep-ota.pub`
   + the `99-beep` MAC→code derivation + a tap detector; strip audio/wifi-full/opkg),
   repartition to the geometry above, `setenv beep_recovery 0x9fa30000; saveenv`,
   write with a **read-back checksum** (single un-mirrored copy next to `art`).
4. Sequence on the bench unit that already has UART clipped (that's how today's brick
   was recovered) — the slot-installing flash is the last one that wants a UART
   backstop.

---

| Failure mode | Self-heals without UART? | Mechanism |
|---|---|---|
| Wrong wifi pw / router moved / dead AP | 🔧 implemented, unverified on HW | netcheck watchdog → `Beep-Setup` WPA2 AP + web reconfig |
| Bad config generally | 🔧 mostly (same, unverified) | same watchdog |
| **Corrupt primary uImage** (bad CRC) | ⬜ this doc | `bootm` fails → `bootb` boots recovery on boot 1 |
| **Primary loads but PANICS/HANGS** (e.g. interrupted sysupgrade: kernel wrote, rootfs didn't) | ⚠️ **actively DANGEROUS on our layout** — see DEFINITIVE note | `bootb` is a **3-strikes** failsafe; after 3 non-reset boots it jumps to `beep_recovery`. Our reset is OFF and `beep_recovery` is garbage → **brick**. Repoint fixes it. |
| Pushing a bad *update* | ⬜ this doc | signed OTA + recovery slot as backstop |

## Bootcount — DEFINITIVE (U-Boot `bootb` disassembly, 2026-09-08 rev 2)

**This section supersedes every earlier bootcount claim in this doc.** Decoded from
`mtd_u-boot.bin`, the stock `beepupdate` binary, and the env dumps.

**`bootb` is a 3-STRIKES failsafe, not a large cumulative counter.**
- Compare is literally `slti v0, v0, 3` (@ file `0x18104`); the recovery branch
  (`beqz`, `0x18114`) is taken when the count **≥ 3**. So after **3 consecutive boots
  that are not reset, the 4th boots `beep_recovery`.**
- Counter lives in env0 at `0x8000–0xFFFF` (a 32 KB scratch area, **outside** the CRC'd
  env; `CONFIG_ENV_SIZE = 0x8000`). U-Boot **clears one bit on every primary boot**
  (`0xff→0x7f→0x3f→0x1f`); a per-nibble map (`0xf→0, 0x7→1, 0x3→2, 0x1→3`) yields the
  count. It skips fully-cleared bytes and scores only the current nibble.
- **Stock resets it every GOOD boot:** `beepupdate bootcount` (cmd 6) advances the
  nibble (a single bit clear on `/dev/mtd1`, no erase). The dumped unit had 84 bits
  cleared but still booted primary = **~21 good boots each reset** — *that* is why it
  looked like "huge headroom." It was resets, not headroom.

**Why this is dangerous on OUR firmware (fix before any unit ships):**
- We replaced `beepupdate`, and our own reset (`usr/libexec/beep/bootcount-reset`) is
  **gated OFF by default** → **nothing resets the counter.** If U-Boot increments every
  primary boot, ~3 non-reset boots reach count 3 → boot 4 jumps to `beep_recovery` →
  which on our layout is the garbage `0x9f550000` → **brick.**
- The stock dumped unit is safe only by accident: its `beep_recovery == beep_primary`
  (both `0x9f550000`), so "recovery" re-boots the *same* image.
- Correction to an earlier worry: the env **variables** ARE redundant (env0/env1 mirror,
  `0xbe` redundant-env flag), so a *variable* `saveenv` is survivable. Only the bootcount
  half is single-copy — which is fine (increment is one atomic 1→0 bit flip, no erase).

### Graduation criteria (stop re-claiming — these are the gates)
1. **Threshold = 3.** ✅ known from the binary (`slti …,3`). No longer a guess.
2. **Repoint `beep_recovery` → `0x9f050000`** (UART, `saveenv`). This is the safety net:
   it turns a count-3 trip into "re-boot primary" (harmless, like the stock unit) instead
   of a jump to garbage. **Safety-critical; gates "safe to disconnect UART."**
3. **Bench-confirm the increment** (deterministic, ~10 min): on a UART unit, reboot 3–4×
   cleanly with **no** reset, watch the serial `Set bootcount 0x%02x offset …` climb, and
   confirm whether boot 4 goes to recovery. Settles the one open question — is the
   increment strictly per-boot (⇒ our reset-off units are on borrowed time) or conditional.
4. **Re-instate a good-boot reset**, preferring stock's lightweight **nibble-advance** on
   `/dev/mtd1` over our current full-sector `fw_setenv` erase; then enable it (prove the
   `fw_env.config` round-trip, `touch /etc/beep-fwenv-verified`).

Until 2–4 are done, only *corrupt-uImage* auto-recovers; and with the repoint absent a
panicking/looping primary **bricks** (it does not merely fail to self-heal). Do not ship
claiming hang-recovery, and do not tell anyone to disconnect UART, until the repoint (2)
is in place.

## Flash layout (16 MB, w25q128)

Current (single big firmware partition):

```
0x000000  u-boot        256K   [preserve — never write]
0x040000  u-boot-env     64K
0x050000  firmware    15.625M   kernel + squashfs + jffs2 overlay
0xff0000  art            64K    [preserve — wifi cal + MACs]
```

Proposed (carve a recovery slot out of the oversized overlay) — **⚠️ the 11M/4.625M
split below is SUPERSEDED by §D above; the recovery build measures ~5.57 MB, not
≤4.6 MB. Kept for history:**

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

> 🛑 **SUPERSEDED (2026-09-08 rev 2 — binary disassembly).** This section's
> conclusion that the threshold is "astronomically far off / defer it" is **WRONG**.
> The threshold is **3** (a 3-strikes failsafe). See **"Bootcount — DEFINITIVE
> (U-Boot disassembly)"** below; that section governs.

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
- **Bootcount reset mechanism SOLVED:** ⚠️ *partly right, dangerously mis-framed — see
  the DEFINITIVE section below.* The "non-issue for any device lifetime" conclusion is
  **wrong**: the reset in that dump was the stock `beepupdate` clearing the counter on
  **every good boot** (~84 bits = ~21 good boots), not headroom under a huge threshold.
  Our firmware dropped `beepupdate` and gates its reset OFF, so nothing resets it. Original text: the old dump had ~84 bits cleared at `0x48000`
  yet still booted primary (threshold ≫ 84), and our current flash is near-fresh
  (~2 bits) because our `saveenv` (setting `beep_primary`) erased the whole env block,
  resetting the `0x8000` region to `0xFF`. So: accumulates ~1 bit/boot, **resets on
  any `saveenv`/env-block erase**, threshold enormous. Spurious recovery is a
  non-issue for any real device lifetime, and `fw_setenv`/`saveenv` is the reset lever
  if ever wanted. **Green light to repoint `beep_recovery`.**

## Recovery image contents (task #8)

> ⚠️ Measured 2026-09-08 (§B): even this minimal set is **~5.57 MB** and must stay
> lean-initramfs-class to clear the **64 MB RAM** unpack ceiling — so `beepd`/rpcd/
> dnsmasq are the first things to drop if the size or RAM budget is tight (use a
> plain CGI + busybox `udhcpd`; the signed path needs no rpcd session).

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
