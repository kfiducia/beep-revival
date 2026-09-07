# Flashing a Beep — step-by-step walkthrough

Turn a stock, cloud-orphaned **Beep "Dial"** into an open-firmware AirPlay speaker.
Written for someone who has never touched this board.

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

## What you need

- A **3.3 V** USB-to-UART adapter (FTDI/CP2102/CH340) + 3 jumper wires (TX/RX/GND).
- A serial terminal (`tio`, `picocom`, `screen`), plus `lrzsz` and `sshpass`
  (`brew install lrzsz sshpass`).
- This repo (for the images in `images/` and the scripts in `scripts/`).

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

**✓ Check:** the device shows an IP (e.g. `10.9.100.166`) and SSH logs in. **No wifi?**
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
scripts/serial-send.sh /dev/cu.usbserial-XXXX images/beep-sysupgrade.bin
```
> **⏱ This step is SILENT — no console output — and takes ~25 minutes. Do NOT
> interrupt it.** Watch the **TX LED on your USB-UART adapter**: **solid red for
> ~20 min** while the 10 MB image streams over, then **dark for ~5 min** while it's
> flushed. Both are normal. The console only returns when it's fully done.

**What (b) — verify, then flash** (reconnect tio):
```
sha256sum /tmp/beep-sysupgrade.bin        # compare to the sha printed by your build
sysupgrade -T /tmp/beep-sysupgrade.bin    # image sanity ("will be flashed")
sysupgrade -n /tmp/beep-sysupgrade.bin    # writes 'firmware' ONLY; art + u-boot untouched
```
**Why:** a serial transfer can corrupt bytes, so we **check the sha before writing** —
this is the difference between a clean flash and a brick. `sysupgrade` writes only the
`firmware` partition (our layout never includes `art`/`u-boot`).

**✓ Check:** the sha **matches** (⛔ **STOP and re-send if it doesn't**); `sysupgrade -n`
writes and reboots.

---

## Step 9 — Repoint the bootloader

**What:** on reboot you'll get `Bad Magic Number` → `ar7240>` (expected — see Why). Then:
```
ar7240> setenv beep_primary 0x9f050000
ar7240> saveenv
ar7240> boot
```
**Why:** stock `bootb` boots from `0x9f550000`, but our kernel lands at flash `0x50000`
(`0x9f050000`). Repointing the primary slot makes `bootb` boot our kernel **while keeping
its boot-count/recovery failsafe intact** — we only move the pointer, we don't bypass it.
The `Bad Magic` on the very first boot is normal, not a brick.

**✓ Check:** our firmware boots to a login / the light ring animates.

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
your unit's `art` MAC. From here it's a normal AirPlay speaker; updates are signed
uploads in the web UI (SSH stays off by default).

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
