# Rescuing a Beep — flashing the open firmware

This guide takes a **stock, cloud-orphaned Beep "Dial"** speaker and puts the open
OpenWrt-based firmware on it, so it speaks AirPlay/etc. again with no dependence on
the dead Beep servers. It's written for someone who has never touched this board.

> ⚠️ **Read this whole page before starting.** Flashing embedded devices can brick
> them. The one truly unrecoverable mistake is destroying the bootloader or the
> radio calibration — this guide is built around *never* doing that, and around
> making a full backup first so you always have a way home.

---

## 0. Is this your device?

- **Beep "Dial"** wireless speaker (the round one with the light ring), based on the
  **8devices Carambola 2** module — an **Atheros AR9331** SoC, **64 MB** RAM,
  **16 MB** SPI-NOR flash. The company folded ~2016 and the cloud it depended on is
  gone, which is why the app/streaming no longer works.
- If your board is different, **stop** — the flash addresses and device tree here
  are specific to this hardware and will not fit anything else.

## 1. What you need

**Hardware**
- A **3.3 V USB-to-UART (TTL) adapter** — FTDI, CP2102, CH340, etc.
  **⚠️ It MUST be 3.3 V logic. A 5 V adapter will kill the AR9331.** If your adapter
  has a voltage jumper, set it to 3.3 V.
- 3 jumper wires (female Dupont if the header has pins) for **TX / RX / GND**.
- A way to connect to the **P2 6-pin console header** (see §3). Most units have a
  0.1″/2.54 mm header; measure yours (a caliper across all 6 pins ÷ 5 gaps = the
  pitch) before buying a connector.
- Small screwdrivers / spudger to open the enclosure.

**Software (on your computer)**
- A serial terminal: `tio`, `picocom`, `minicom`, or `screen`.
- Docker (to build the firmware) **or** a prebuilt release image if one is provided.
- `md5sum`/`sha256sum` for verifying transfers.

## 2. Identify the board & open it

Open the enclosure carefully (clips + screws vary by revision). You're looking for
the Carambola 2 module (a small daughterboard with a metal-can radio) and, near it,
a **6-pin header labeled P2** — the UART console. See `docs/` board photos.

## 3. The serial console (P2)

**Settings: `115200` baud, `8N1`, no flow control. 3.3 V logic.**

The three wires you need are **TX, RX, GND**. Rather than trust a silkscreen, verify
them: the console maps to the **Carambola 2 module pins 43 (RX), 44 (TX), 45 (GND)**
— trace the P2 pins to those with a multimeter's continuity mode. Connect:

| Beep | ↔ | Your adapter |
|---|---|---|
| TX (module 44) | → | RX |
| RX (module 43) | → | TX |
| GND (module 45) | → | GND |

Power on. You should see U-Boot output. Press a key during the 1-second `bootdelay`
to drop to the `ar7240>` prompt. If you see garbage, your baud or voltage is wrong.

## 4. BACK UP THE STOCK FLASH FIRST (do not skip)

This backup is your un-brick insurance. The safest full dump is from a running
Linux on the device, but at minimum record the **bootloader** and **calibration**,
which you must never overwrite:

- **u-boot** (`mtd0`, offset `0x000000`, 256 KB) — the bootloader.
- **art** (`mtd6`, offset `0xff0000`, 64 KB) — wifi calibration + your unit's MAC
  addresses. **Unique per device; there is no replacement if lost.**

Once you can get a shell (after §6 Phase 0), dump every partition:
```sh
for m in /dev/mtd[0-9]; do dd if=$m of=/tmp/backup-$(basename $m).bin; done
```
Copy those files off the device and keep them safe. Also save the U-Boot env:
```sh
dd if=/dev/mtd1 of=/tmp/backup-uboot-env.bin
```

## 5. Build the firmware

See the repo `README.md` → Build. In short (Docker, because OpenWrt's buildroot
needs Linux + a case-sensitive FS):
```sh
docker exec -it beep-build bash /src/scripts/docker-build-setup.sh   # toolchain (long, once)
docker exec -it beep-build bash /src/scripts/build.sh                # the image
# output: bin/targets/ath79/generic/openwrt-ath79-generic-8dev_carambola2-squashfs-sysupgrade.bin
```
You also need the small **initramfs** image for the zero-risk RAM-boot tests
(`...-initramfs-kernel.bin`), plus the stock mainline Carambola 2 image in
`prebuilt/` for Phase 0.

## 6. Flash it — the safe, phased way

**The whole point of this order is that Phases 0–1 write NOTHING to flash.** You
only commit in Phase 2, after you've proven the board and the driver on *your* unit.

### Phase 0 — prove the board on mainline (no flash writes)
At the `ar7240>` prompt, load the **prebuilt mainline initramfs** into RAM and boot
it (`scripts/serial-loady.sh` automates the YMODEM load). Confirm the board comes
up, wifi works, and the ART MAC is correct. Nothing is written — power-cycling
returns you to stock.

### Phase 1 — prove the open firmware in RAM (still no flash writes)
RAM-boot **our** initramfs image the same way. Get a root shell, bring up audio, and
confirm a clean test tone. You can hot-swap the audio driver over serial without
rebooting (`scripts/serial-send.sh` YMODEMs a `.ko` to a running `rz`), which is how
the driver was brought up. Still nothing written to flash.

### Phase 2 — commit (the only writes)
1. Transfer the **sysupgrade** image to the running device's `/tmp` and **verify its
   checksum** matches your build.
2. Flash it: `sysupgrade -n /tmp/…-squashfs-sysupgrade.bin`  (`-n` = don't keep the
   stock config).
3. Reboot to U-Boot and point the bootloader's primary slot at the OpenWrt kernel:
   ```
   setenv beep_primary 0x9f050000
   saveenv
   ```
   OpenWrt lands at flash `0x50000`; the stock `bootb` failsafe expects the primary
   at `0x9f050000`. **This keeps the Beep's own `bootb` boot-count/recovery
   mechanism intact — we're only repointing its primary slot, not bypassing it.**

**Never `erase`/write `mtd0` (u-boot) or `mtd6` (art).** Those are your backstop.

## 7. First boot & setup

On first boot the firmware provisions itself per-device (unique hostname, a setup
code derived from the MAC, a self-signed TLS cert) — no shared/default credentials.

- If it already has wifi details it joins your network; open `http://<its-ip>/`.
- Otherwise it starts a **`Beep-Setup-XXXX` Wi-Fi network (WPA2)** and the light ring
  shows a slow rotating "reconfigure me" pattern. Join that network — the password
  and the admin login are both the **device setup code** (derived from the MAC, so
  it's unique to your unit). A setup page pops up automatically (captive portal);
  pick your Wi-Fi, set a name, done.

From then on it's a normal AirPlay speaker. Updates happen from the web UI
(**signed** firmware upload) — SSH ships **off** by default.

## 8. If something goes wrong

- **Bad Wi-Fi settings / wrong password** → the device self-heals: it can't
  associate, so it brings the `Beep-Setup` AP back automatically. Just reconnect and
  reconfigure. No cable needed.
- **A bad flash / non-booting image** → *today this still needs the UART* (re-do
  Phase 1/2). A hands-off **recovery slot** that auto-recovers a bad flash without a
  cable is in progress (see `docs/RECOVERY-DESIGN.md`); until it ships, keep your
  UART adapter handy for updates.
- **Total worst case** → you have the full flash backup from §4. Re-flash the stock
  dump from U-Boot and you're back to where you started.

## 9. Safety recap

- 3.3 V logic only.
- Back up **all** partitions before writing anything; guard `u-boot` (mtd0) and
  `art` (mtd6) with your life — never write them.
- Phases 0–1 write nothing; only Phase 2 commits.
- Verify every image checksum before flashing.
