# beep-revival

Open, un-orphanable custom OpenWrt firmware for the **Beep "Dial"** wireless
audio player (8devices Carambola 2 / Atheros AR9331). Replaces the dead,
cloud-tethered stock firmware with standard-protocol audio (AirPlay / Spotify
Connect / Snapcast / DLNA), a firmware-owned physical UX (knob / tap /
double-tap-to-join), and a small authenticated on-device admin — nothing
load-bearing that can be killed by a vanished app or server.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/K3K7E85Z2)

> ### ⚠️ Security — known limitation, read before you flash
> **The default admin password is derived from the device's MAC address, and a MAC
> is not a secret.** On first boot the firmware sets one per-device "setup code"
> (from the MAC) and uses it as **both** the WPA2 key for the `BeepRevival-Setup`
> AP **and** the root/admin password. Because a MAC is readable over the air (the
> setup SSID even publishes part of it) and via ARP by anything on your LAN, a
> nearby or same-network attacker can derive this code and reach the admin UI until
> you change it. This is a deliberate zero-touch-onboarding trade-off, **not** a
> secret-strength credential.
>
> **What to do:** after setup, immediately change the admin password (System →
> *Set admin password*). Keep SSH **off** unless you need it (it is off by default),
> and use the HTTPS admin UI on port 443 rather than plain HTTP where you can — the
> plain-HTTP admin plane sends the password/session in the clear on your LAN. See
> [`docs/COMPARISON.md`](docs/COMPARISON.md) and the Security section below.

## Status — ✅ working, flashed, on the network (2026-09-05)
- **🔊 Audio works** — clean tone/playback out the jack from an entirely
  from-scratch kernel-6.6 driver: the AR9331 I²S CPU DAI **plus** the MBOX-DMA
  data plane (descriptor-ring DMA + IRQ handler), the WM8524 codec, and
  simple-audio-card. No mainline AR9331 audio driver exists — this port is the
  only one for a modern kernel.
- **📦 Flashed & persistent** on unit #1 — boots from flash via the Beep's own
  `bootb` failsafe (preserved, not bypassed: `beep_primary` re-pointed at our
  kernel; bootcount + recovery-slot mechanism intact).
- **📶 Networked** — wifi (ath9k, nvmem calibration), dropbear, AirPlay
  (`shairport-sync`); joins the LAN, key-auth SSH, `scp -O` deploys.
- **🎛️ Admin/setup web app + self-healing wifi** *(implemented, NOT yet verified on hardware)* —
  a self-contained `/ubus` SPA (wifi scan/join, naming, volume, reboot, admin
  password), a continuous wifi-fallback watchdog → WPA2 `Beep-Setup` AP with a
  pulsing LED ring, per-device credentials (no blank-auth), and the stock LED
  behaviors (idle/volume/party) restored. AirPlay 2 trimmed to a **9.94 MB** image
  so a recovery slot fits. See `BUILD-STATUS.md`.
- **🎧 AirPlay 2 — experimental, under test (2026-09-08):** an `AIRPLAY2=1` build now
  decodes AirPlay-2 **buffered AAC in real time on the FPU-less AR9331** by switching
  ffmpeg to its fixed-point `aac_fixed` decoder (the default float decoder pegs this
  soft-float core and underruns) — the first known `aac_fixed` + shairport-sync port on
  this class of chip. On unit #2 it **sustains playback with headroom** (≈45% idle,
  ≈36% CPU, no XRUN over a full track) — **good performance so far**. The margin is thin,
  so concurrent-load hardening is in progress (knob-volume fork coalescing, audio-thread
  priority). Big-endian pairing/timing fixes (`pair_ap`, nqptp `ntoh64`) are included.
  It is **not the default image** — the lean AirPlay-1 + Snapcast build ships by default;
  build AirPlay 2 with `AIRPLAY2=1 ./scripts/build.sh`. See `docs/DEV-NOTES.md` §1 and
  `docs/BE-AP2-AUDIT.md`.
- **Open (Phase 3):** build a recovery-slot initramfs into the freed ~5.6 MB so
  `bootb` auto-recovers a bad update without a UART; multiroom (Snapcast).

New here? See **[`docs/UI.md`](docs/UI.md)** (how the LED ring + knob work — the
device UI), **[`docs/INSTALL.md`](docs/INSTALL.md)** (rescue/flash your own Beep),
**[`docs/RECOVERY-DESIGN.md`](docs/RECOVERY-DESIGN.md)** (recover without a serial
console), and **[`docs/COMPARISON.md`](docs/COMPARISON.md)** (stock vs. open).

## Layout
| Path | What |
|---|---|
| `dts/ar9331_beep_dial.dts` | device tree: I²S+WM8524, i2c-gpio (STM8), setup key, ART-preserving partitions |
| `feed/beep-i2s/src/beep-i2s.c` | AR9331 I²S CPU DAI + pinmux **+ MBOX-DMA data plane (working)** — descriptor ring, IRQ handler, PCM component; port notes in `driver-i2s/PORTING.md` |
| `driver-i2s/` | porting notes (`PORTING.md`) + `reference/` (franzflasch DMA source we ported from) |
| `feed/beepd/src/beepd.c` | STM8 control daemon (knob/tap/LED @ i²c 0x23) |
| `rootfs-overlay/www/index.html` | self-contained admin/setup SPA (talks to `/ubus`) |
| `rootfs-overlay/usr/libexec/rpcd/beep` | authenticated backend: status/scan/set_wifi/set_name/set_volume/reboot/… |
| `rootfs-overlay/usr/libexec/beep/` | wifi-setup-enable/disable, netcheck-loop (fallback watchdog), beep-action (gestures) |
| `rootfs-overlay/etc/` | init scripts, uci-defaults (per-device code+cert, service enable), asound.conf softvol |
| `feed/` | OpenWrt packages: `kmod-beep-i2s`, `beepd` |
| `scripts/` | `docker-build-setup.sh` (toolchain), `build.sh` (image) |
| `prebuilt/` | stock OpenWrt Carambola2 images for the Phase-0 RAM-boot test |

## Build (Docker on macOS — buildroot needs Linux + case-sensitive FS)
```sh
# toolchain (long, runs in a persistent container against a docker volume):
docker exec -it beep-build bash /src/scripts/docker-build-setup.sh   # already kicked off
# then the image:
docker exec -it beep-build bash /src/scripts/build.sh
# images land in the container at /build/openwrt/bin/targets/ath79/generic/
```

## Flash (the safe order we followed — all done on unit #1)

> **Rescuing your own stock Beep?** Follow the step-by-step, never-brick guide in
> **[`docs/INSTALL.md`](docs/INSTALL.md)** — hardware, serial console, full backup,
> and the phased flash written for someone new to the board. The summary below is
> the same procedure in brief.

1. **Phase 0 (zero risk):** `loady` the *prebuilt* mainline initramfs into U-Boot
   RAM, `bootm`; confirmed board + wifi + ART-MAC on mainline. No flash writes.
2. **Phase 1 (the gate):** RAM-boot our custom initramfs (`serial-loady.sh`),
   bring up audio, chase a clean tone. Iterated the driver *without* rebooting via
   `serial-send.sh` (YMODEM the `.ko` to a running `rz`) + `rmmod`/`insmod`.
3. **Phase 2 (commit):** `serial-send.sh` the sysupgrade to `/tmp`, md5-verify,
   `sysupgrade -n`. Then in U-Boot **`setenv beep_primary 0x9f050000; saveenv`** —
   this keeps the stock `bootb` bootcount/recovery failsafe and just points its
   *primary* slot at our kernel (OpenWrt lands at `0x50000`; stock U-Boot looks at
   `0x550000`). **Never write u-boot / art.** U-Boot + the full `flash.bin` dump
   are the un-brickable backstop.
4. From here unit #1 is network-flashable (`scp -O` + `sysupgrade`) — no serial.
   Unit #2 needs one UART clip to reach step 2, then it's networked too.

## Security (fixes the stock Beep's sins)
No anonymous control API (admin behind `rpcd` session auth), no writable CGI
docroot, no remote-support backdoor, minimal daemon surface, per-device hostname
+ self-signed TLS at first boot, SSH off by default (fail-secure). Signed-OTA is
gated by an admin session **and** a `usign` signature; unsigned flashing requires
a physical triple-tap on the device.

**Known limitation (see the ⚠️ note at the top):** the default admin password and
the WPA2 setup-AP key are both derived from the device MAC, which is not secret —
so they are guessable by anyone on your LAN or in Wi-Fi range until you change the
admin password. The plain-HTTP admin plane also transmits credentials in the clear
on the LAN; prefer HTTPS on 443. These are accepted zero-touch-onboarding
trade-offs, disclosed openly rather than papered over.

## FAQ

**If I upgrade to this firmware, am I locked in because you enforce signing?**

No. Signing is a safety gate against *remote* attacks, not a lock on *you*. You can
always load your own firmware: build (or download) an image, upload it through the
web admin UI exactly like a normal update — but because it isn't signed with our
key, you must **be physically present and triple-tap the Beep button** to authorize
it. That physical tap opens a 60-second window that unlocks the "install unsigned
image" option in the UI.

The reason it works this way: a valid `usign` signature lets a genuine release flash
over the network (convenient, and safe because a stolen admin session still can't
forge the signature). An *unsigned* image can't be flashed remotely at all — no
remote attacker can press the button on your device — so the only way to install one
is to walk up to the hardware and triple-tap. Physical possession is the master key,
which is exactly what "it's your device" should mean. See the Security section above
and [`rootfs-overlay/www/cgi-bin/beep-ota`](rootfs-overlay/www/cgi-bin/beep-ota).

## License
GPL-2.0-or-later. This firmware is built on and derived from GPL-licensed Linux
kernel code (the AR9331 I²S/audio driver derives from Franz Flasch's GPLv2
`ar9331-i2s-alsa` and links the kernel), so the project is and must be GPL. See
[`LICENSE`](LICENSE) for the full text and [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)
for upstream attribution.

**No warranty. Use at your own risk.** As stated in GPL-2.0 §§11–12, this software
is provided "AS IS" with no warranty of any kind, and no contributor is liable for
any damages. **Flashing custom firmware can permanently brick your device.** You
alone are responsible for what you flash and how. There is no support guarantee.
