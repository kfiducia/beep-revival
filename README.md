# beep-firmware

Open, un-orphanable custom OpenWrt firmware for the **Beep "Dial"** wireless
audio player (8devices Carambola 2 / Atheros AR9331). Replaces the dead,
cloud-tethered stock firmware with standard-protocol audio (AirPlay / Spotify
Connect / Snapcast / DLNA), a firmware-owned physical UX (knob / tap /
double-tap-to-join), and a small authenticated on-device admin — nothing
load-bearing that can be killed by a vanished app or server.

Background + full research: `docs/` (symlinks to `~/beep-revival/`):
`RESEARCH-SYNTHESIS.md` (start here), `FIRMWARE-BUILD-PLAN.md`, `ARCHITECTURE.md`,
and the security post-mortem `ATTACK-SURFACE.md`.

## Layout
| Path | What |
|---|---|
| `dts/ar9331_beep_dial.dts` | device tree: I²S+WM8524, i2c-gpio (STM8), setup key, ART-preserving partitions |
| `driver-i2s/beep-i2s.c` | AR9331 I²S CPU DAI + pinmux (control plane; **PCM/DMA port pending** — see `driver-i2s/PORTING.md`) |
| `driver-i2s/reference/` | franzflasch DMA source we're porting |
| `beepd/beepd.c` | STM8 control daemon (knob/tap/LED @ i²c 0x23) |
| `rootfs-overlay/` | init scripts, provisioning (SoftAP+captive portal), authenticated `rpcd` admin, per-device secrets |
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

## Flash (safe order — RESEARCH-SYNTHESIS.md)
1. **Phase 0 (zero risk, do first):** `loady` the *prebuilt* initramfs into unit
   #1's U-Boot RAM, `bootm`; confirm mainline boots + wifi + ART-MAC + SSH — no
   flash writes. Validates ~90% of the platform.
2. **Phase 1 (the gate):** RAM-boot our custom initramfs, bring up the audio
   driver, get a clean `speaker-test` tone (Saleae-verify clocks first).
3. Commit via SSH `sysupgrade` on unit #1 (rooted); one UART clip for unit #2.
   Never write u-boot / art.

## Security (fixes the stock Beep's sins — see ATTACK-SURFACE.md)
No anonymous control API (admin behind `rpcd` session auth), no default creds
(per-device hostname + self-signed TLS at first boot), no writable CGI docroot,
no remote-support backdoor, minimal daemon surface. **OTA must set
`REQUIRE_IMAGE_SIGNATURE=1`** — OpenWrt does not verify signatures by default.
