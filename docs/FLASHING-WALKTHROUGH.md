# Installing Beep Revival — the complete flashing & update guide

Turn a stock, cloud-orphaned **Beep "Dial"** into an open-firmware AirPlay speaker.
Written for someone who has never touched this board. This is the **single canonical
guide**: the one-time UART first-flash (Steps 0–11) *and* how you update from then on
(["staying updated"](#after-the-first-flash--staying-updated-no-cable) — just a signed
upload in the web UI, no cable).

**How to read this guide.** Every step has three parts, so you always know where you
stand:

- **What** — the exact command(s) to run.
- **Why** — what it does and the risk it manages (so you understand, not just paste).
- **✓ Check** — the output you should see, and the one thing that means **STOP**.

> ⚠️ **No warranty — you flash at your own risk** ([`LICENSE`](../LICENSE), GPL-2.0
> §§11–12). This procedure is built around never destroying the two unrecoverable
> things, and making a verified backup first, so you always have a way home.

## Golden rules (read once)

1. **3.3 V logic only.** A 5 V UART adapter will kill the AR9331.
2. **Never write `u-boot` or `art`.** `art` holds your unit's MAC + wifi calibration —
   there is **no replacement** if you erase it. This procedure never writes either.
3. **Identify partitions by NAME, never by mtd number.** The numbers shift between
   units and between stock/our firmware (we saw `art` at `mtd7` on stock and `mtd6` on
   ours). Always read `/proc/mtd` and match the *name*.
4. **Verify every checksum** before flashing; back up before you write.
5. **Phases 1–2a write nothing.** Only the single `sysupgrade` commits.

## Is this your device?

This procedure is **only** for the **Beep "Dial"** — the round wireless speaker with the
LED ring — built on the **8devices Carambola 2** module (**Atheros AR9331**, **64 MB** RAM,
**16 MB** SPI-NOR flash). Beep Networks folded around 2016 and shut down the cloud the app
depended on, which is why the speaker went dark. **If your board is anything else, stop** —
the flash addresses and device tree here are specific to this hardware.

## What you need

**Hardware**
- A **3.3 V** USB-to-UART (TTL) adapter — FTDI / CP2102 / CH340. **It MUST be 3.3 V logic;
  a 5 V adapter will kill the AR9331.** If it has a voltage jumper, set it to 3.3 V first.
- 3 jumper wires (female Dupont) for **TX / RX / GND**, and a way onto the **P2 6-pin
  console header** — most units are 0.1″/2.54 mm pitch; measure yours before buying a
  connector.
- A small screwdriver / spudger to open the enclosure.

**Software (on your Mac)**
- A serial terminal (`tio`, `picocom`, `screen`), plus `lrzsz` and `sshpass`
  (`brew install lrzsz sshpass`).
- The **helper `scripts/`** and the **lean RAM-boot image** — both ship in this repo, so a
  `git clone` is all you need for them (Step 0). The firmware you actually flash is a
  **signed release download** (Step 0). **You do not build anything.**

---

## Step 0 — Get the tools and the firmware image (no building required)

You need two things on your computer, and **you build nothing**. Run everything below
**from the repo root** so the `scripts/…` and `images/…` paths resolve.

**(a) This repo** — it carries the helper `scripts/` *and* the **lean RAM-boot image**
(`images/lean-initramfs-kernel.bin`), the small audio-only initramfs you boot from RAM in
Phase 1. Clone it and confirm the lean image is intact:
```
git clone https://github.com/kfiducia/beep-revival && cd beep-revival
shasum -a 256 images/lean-initramfs-kernel.bin
# expect: 87030fe4542af25df66056c2fcdf5191c40b3adba49f2504268101c0a4af60ec
```

**(b) The signed firmware image** — the **sysupgrade** you actually flash in Phase 2, from
the [Releases page](https://github.com/kfiducia/beep-revival/releases). Download it into
the repo root and verify it:
```
export VER=v1.4.2          # ← set to the latest release tag on the Releases page
gh release download "$VER" --repo kfiducia/beep-revival \
  -p "beep-revival-$VER-sysupgrade.bin" -p SHA256SUMS
#   (no gh? download those two files from the Releases page in a browser)
grep 'sysupgrade.bin$' SHA256SUMS | grep -v signed | shasum -a 256 -c -
# expect: beep-revival-$VER-sysupgrade.bin: OK
```

**Why they're different (and why the lean image exists):** the **lean image** is a stripped,
audio-only initramfs whose only job is to give you a root shell **in RAM** so you can
transfer and flash the real image — it's a versionless **tool**, so it lives in the repo.
The **sysupgrade** is the actual firmware written to flash; it's versioned and
`usign`-**signed**, so it ships as a GitHub Release. **Do not RAM-boot the release's
`…-initramfs-kernel.bin`** — the *full* initramfs (~10 MB) **OOM-panics on this 64 MB
board** (`Kernel panic - System is deadlocked on memory`); RAM-booting the **lean** one is
the whole point (Step 6). Verifying both checksums catches a corrupt download **before** it
can cause a bad flash.

**✓ Check:** the lean image's `shasum` equals `87030fe4…`, and the release check prints
`beep-revival-$VER-sysupgrade.bin: OK`. You now have, in the repo root:
`images/lean-initramfs-kernel.bin` (Phase 1 RAM-boot) and
`beep-revival-$VER-sysupgrade.bin` (Phase 2 flash).

> Set `export VER=v1.4.2` **once** in the shell you run the scripts from (or just
> substitute the real filename in the commands below).

---

## Step 1 — Serial console → U-Boot

**What:** Wire the console (Carambola 2 module pins **44=TX → your RX**, **43=RX → your
TX**, **45=GND → GND**), open a terminal at **115200 8N1, no flow control**, power on,
and **press `ESC`** during the ~2-second autoboot window.

**Why:** U-Boot is the only thing you can reach on a stock unit over UART (stock Linux
has no console until Step 2). Stopping autoboot lets us load our firmware into RAM
later without writing flash. It must be **ESC** specifically — not "any key."

**✓ Check:** you land at
```
ar7240>
```
Record the environment as your bootloader-env backup: `printenv`. **STOP if you see
garbage** — wrong baud, or your adapter is 5 V (which can damage the board).

---

## Step 2 — Developer unlock → stock root shell

**What:**
```
ar7240> setenv dev_lRapcY7M 1
ar7240> saveenv
ar7240> boot
```
Let it boot, **press Enter**, log in as **root / root**.

**Why:** stock firmware disables the serial console *and* SSH by default. Both are gated
on a single hardcoded U-Boot env flag, `dev_lRapcY7M` (the stock `/etc/init.d/defconfig`
starts the console getty, and `/etc/init.d/dropbear` starts SSH, only if it exists). The
name is the same on every unit; only its existence matters. Setting it gives us a shell
**and** turns on SSH so we can back up fast over the network. *(To undo later:
`setenv dev_lRapcY7M` with no value, then `saveenv`.)*

**✓ Check:** a root prompt like `root@beep-xxxxxx:/#`.

---

## Step 3 — Network + SSH (the fast backup path)

**What:** on the device, `ip -4 addr | grep inet`. From **your computer**:
```
ssh -o KexAlgorithms=+diffie-hellman-group14-sha1 -o HostKeyAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-ctr,aes128-cbc,3des-cbc root@<device-ip>      # password: root
```
**Why:** the unlock started dropbear, so if the unit's wifi associates you can back up
over SSH (minutes) instead of serial (much slower). The legacy algorithm flags are
required because it's a 2015 dropbear.

**✓ Check:** the device shows an IP (e.g. `192.0.2.166`) and SSH logs in. **No wifi?**
Skip to **Appendix B** (serial-only backup).

---

## Step 4 — Validate the flash layout (by name)

**What:**
```
cat /proc/mtd
```
**Why:** this is the guardrail. We confirm the partition map on *your* unit and find
`art` by name **before** anything is written, so we target the right region and never
touch `art`/`u-boot`.

**✓ Check (real stock example):**
```
mtd0 "u-boot"   mtd1 "env0"   mtd2 "recovery"   mtd3 "kernel"
mtd4 "rootfs"   mtd5 "rootfs_data"   mtd6 "env1"   mtd7 "art"   mtd8 "firmware"
```
Note where **`art`** landed (mtd7 here). **On a unit without `rootfs_data` it's `mtd6`.**
That variance is exactly why we never hardcode the number.

---

## Step 5 — Full, verified backup (your un-brick insurance)

**What:**
```
scripts/beep-backup.sh <device-ip> ~/beep-backups/<name>
```
**Why:** dumps **every partition by name**, streamed straight to your computer (no
device staging → can't run the 64 MB box out of RAM), and **md5-verifies each dump
against the chip**. If the device is ever bricked, `beep-restore.sh` writes any of these
back. `art.bin` is the one you can never regenerate.

**✓ Check (real output):**
```
  mtd7  art          ... OK  md5=149204eca3c083703fdd95b942f0e4b8
  mtd8  firmware     ... OK  md5=125ec52c361644bcb1b6636522162583
✅ backup complete + every partition md5-verified against the chip.
```
**STOP if any line is not `OK`** — re-run; do not proceed on an unverified backup.

---

## Step 6 — Phase 1: RAM-boot our firmware (no flash writes)

**What:** back at U-Boot (`reboot`, then **ESC**), quit your terminal (`pkill tio`) so
the port is free, then:
```
scripts/serial-loady.sh /dev/cu.usbserial-XXXX images/lean-initramfs-kernel.bin
```
> **macOS: use `/dev/cu.*`, never `/dev/tty.*`.** The `tty.` device blocks on open
> waiting for carrier-detect (a 3-wire console has none) and hangs the script.

**Why:** this loads our lean image into RAM and boots it — **nothing is written to
flash**, so a power-cycle returns you to stock. It proves our kernel + audio driver run
on *your* board before you commit. The script **auto-runs `bootm` for you** and streams
the boot to **`~/beep-ramboot.log`** (not your tio log — that's why tio looks frozen at
`ar7240>` during the load).

**✓ Check:** in `~/beep-ramboot.log`, our OpenWrt boots and you see
`beep-i2s … AR9331 I2S DAI ready`. Ctrl-C the script, reconnect
`tio -b 115200 /dev/cu.usbserial-XXXX`, press Enter for a root shell (`root@beep-dial`).

---

## Step 7 — Validate the critical pieces in RAM (before writing anything)

**What:**
```
uname -r
cat /proc/mtd
N=$(grep '"art"' /proc/mtd | cut -d: -f1); echo "art=$N"; md5sum /dev/$N
```
**Why:** this is your last zero-risk checkpoint. We confirm (a) we're in *our* firmware,
(b) the target layout is sane and `art` is a separate partition the flash won't touch,
and (c) `art` is byte-for-byte intact (its md5 still matches your backup).

**✓ Check:** `uname -r` → `6.6.x` (stock was `3.8.13`); `/proc/mtd` shows our layout with
`firmware` as its own partition and `art` separate; the `art` md5 equals your backup's
(`149204ec…` on this unit).

> **About the MAC:** our device tree reads eth/wifi MACs from `art` via nvmem
> (`eth0→art@0x0`, `wmac→art@0x6 + calibration`). The **lean RAM image is stripped**, so
> it may show a *random* eth0 MAC — that's an artifact of the minimal image, **not** the
> flashed firmware. The real wifi MAC (derived from `art`) is confirmed on the full image
> **after** the flash (Step 10). Because `art` is preserved and everything is reflashable,
> deferring that check costs nothing.

---

## Step 8 — Phase 2: transfer, verify, flash (the only write)

**What (a) — send the image** (quit tio first, `pkill tio`):
```
scripts/serial-send.sh /dev/cu.usbserial-XXXX beep-revival-*-sysupgrade.bin
```
> **⏱ The transfer is SILENT on the console and takes ~15–20 minutes. Do NOT
> interrupt it.** `tio` is detached while `serial-send.sh` owns the port, so the
> **TX LED on your USB-UART adapter is your progress indicator**: it stays **solid
> red** while your Mac streams the ~10 MB image over, then goes **quiet** as the
> YMODEM handshake finalizes. **This all goes into the device's RAM** — the image
> lands in `/tmp`, which is a `tmpfs`. **Nothing is written to flash yet.**
>
> **Your "done" signal is `serial-send.sh` printing its completion** (the `sz`
> summary / `[✓] transfer complete`) and returning you to your shell prompt — not
> the TX LED alone. When you see that, the image is fully in RAM. *Then* quit the
> send script if needed and reconnect `tio -b 115200 /dev/cu.usbserial-XXXX`.

**What (b) — verify, then flash** (reconnect tio):
The image is now in **RAM** (`/tmp`). We check it there, then commit — the
`sysupgrade` below is the **only** step that writes flash (it's quick and *does*
print progress to the console before it reboots):
```
sha256sum /tmp/beep-revival-*-sysupgrade.bin     # must equal its SHA256SUMS line from Step 0
sysupgrade -T /tmp/beep-revival-*-sysupgrade.bin # image sanity ("will be flashed")
sysupgrade -n /tmp/beep-revival-*-sysupgrade.bin # writes 'firmware' ONLY; art + u-boot untouched
```
**Why:** a serial transfer can corrupt bytes, so we **check the sha before writing** —
this is the difference between a clean flash and a brick. `sysupgrade` writes only the
`firmware` partition (our layout never includes `art`/`u-boot`).

**✓ Check:** the sha **matches** (⛔ **STOP and re-send if it doesn't**); `sysupgrade -n`
writes and reboots.

---

## Step 9 — Repoint the bootloader

**What:** on reboot you'll get `Bad Magic Number` → `ar7240>` (expected — see Why). Then
set **both** pointers:
```
ar7240> setenv beep_primary   0x9f050000
ar7240> setenv beep_recovery  0x9f050000
ar7240> saveenv
ar7240> boot
```
**Why:** stock `bootb` boots `beep_primary` from `0x9f550000`, but our kernel lands at flash
`0x50000` (`0x9f050000`). Repointing primary makes `bootb` boot our kernel. The `Bad Magic`
on the very first boot is normal, not a brick.

> ⚠️ **You MUST also set `beep_recovery` (this is not optional).** `bootb` is a
> **3-strikes** failsafe: after 3 consecutive boots that don't get reset, the 4th boots
> `beep_recovery`. On a stock unit `beep_recovery` still points at `0x9f550000` — which,
> once primary is our kernel, is **garbage (mid-rootfs)**: the 4th boot fails and drops you
> back to `ar7240>`, so you'd have to **reconnect UART and re-point / `saveenv`** to recover.
> Not a permanent brick (`u-boot` + `art` are intact), but it defeats going cable-free.
> There is no real recovery slot yet, so we point `beep_recovery` at the **primary** too: a
> 3-strikes trip then simply re-boots the primary (harmless). (When a real recovery slot
> exists, this becomes its address instead — see `docs/RECOVERY-DESIGN.md`.)

**✓ Check:** our firmware boots to a login / the light ring animates, **and**
`fw_printenv beep_recovery` (or `printenv` at `ar7240>`) reads back `0x9f050000`.

---

## Step 10 — First boot, and confirm the MAC/wifi

**What:** let it boot; if it has no wifi yet it starts a **`Beep-Setup-XXXX`** WPA2
network (password = the setup code derived from the MAC). Join it, and the captive
portal sets wifi + a name. Then confirm the wifi identity is the real one from `art`:
```
# on the device (serial or, once SSH is enabled, over the network)
iw dev  ||  ip link show phy0-sta0
```
**Why:** this is where we confirm the `art`→MAC nvmem read we deferred in Step 7 — on the
*full* image. The wifi MAC should derive from `art` (e.g. `c4:93:…` family), not a random
address.

**✓ Check:** it joins your network / serves `http://<its-ip>/`, and the wifi MAC matches
your unit's `art` MAC.

---

## Step 11 — UART-exit checklist (do NOT unclip UART until all pass)

Disconnecting UART and calling the unit "safe to network-update" is a real commitment:
without a working recovery slot, UART is still the only backstop for a bad boot. **Do not
remove it until every box is checked** — these are the things that turn a self-recoverable
event into one that forces you back to the UART cable if skipped.

- [ ] **`beep_recovery` is repointed** — `fw_printenv beep_recovery` → `0x9f050000` (Step 9).
      *Without this, a 3-strikes trip boots garbage — not a permanent brick, but it drops to
      `ar7240>` and you'd need UART again to re-point / `saveenv`.*
- [ ] **`fw_env.config` round-trips** — `fw_printenv beep_primary` → `0x9f050000`, and
      `fw_setenv beep_probe 1 && fw_printenv beep_probe` returns `1`. Proves userspace can
      read/write the env safely (needed before any good-boot bootcount reset is enabled).
- [ ] **Bootcount data captured** — reboot 3–4× and record the serial
      `Set bootcount 0x%02x offset 0x%08x` line each time; confirm it climbs and that the
      unit does **not** unexpectedly drop to recovery. (Feeds the bootcount graduation
      criteria in `docs/RECOVERY-DESIGN.md`.)
- [ ] **Signed web-OTA proven** — do one signed update through the web UI and confirm it
      reboots, rejoins Wi-Fi, and keeps its admin password + name (the curated-preserve
      flash path). Ideally across a variant change (AP1↔AP2).
- [ ] **Full backup exists** (Step 5) — you can restore `firmware` over the network.

Only when all of the above hold is the unit genuinely "network-update safe." Then it's a
normal AirPlay speaker; updates are signed uploads in the web UI (SSH stays off by default).

---

## After the first flash — staying updated (no cable)

The UART flash above is a **one-time** bootstrap. Once Beep Revival is on, the unit is a
normal AirPlay speaker and **every future update is a signed upload in the web UI** — no
serial, no hand-run `sysupgrade`:

1. Download the latest **signed** image — `beep-revival-<version>-sysupgrade.signed.bin` —
   from the [Releases page](https://github.com/kfiducia/beep-revival/releases) and verify
   it against `SHA256SUMS` (optionally the signature against `beep-ota.pub`).
2. Open the device's admin UI (`https://<its-ip>/`, prefer HTTPS on 443) and log in.
3. In the **firmware update** section, upload the one `.signed.bin` — the signature travels
   with the image, so there's nothing else to select.
4. The device **verifies the `usign` signature**, writes only the `firmware` partition, and
   reboots into the new version. **Your Wi-Fi, name, and admin password are preserved;**
   `art` and `u-boot` are never touched.

Two independent gates protect this path: an **authenticated admin session** *and* a **valid
signature**. An unsigned image is refused outright (installing unsigned firmware
deliberately takes a physical triple-tap on the device — not something a remote attacker
can do). If an update ever fails to boot, you still have the UART backstop above (redo
Phase 1/2) and the full backup from Step 5.

---

## Appendix A — If something goes wrong: restore

```
scripts/beep-restore.sh <device-ip> firmware ~/beep-backups/<name>/firmware.bin
```
Matches by name, refuses `u-boot`/`art` without `--force`. A full stock restore = write
back `firmware` (and, if needed, `recovery`/`kernel`/`rootfs`/`rootfs_data`). You never
restore `u-boot`/`art` because you never wrote them.

## Appendix B — Serial-only backup (no wifi for SSH)

From the RAM-booted system (Step 6), dump each partition to `/tmp` and pull it over
serial:
```
# on device — use YOUR art number from /proc/mtd
dd if=/dev/mtd7 of=/tmp/art.bin
# on your Mac
scripts/serial-recv.sh /dev/cu.usbserial-XXXX ~/beep-backups/<name> /tmp/art.bin
```
Slower than SSH, same result. `art` (64 KB) is quick even this way — do it first.
