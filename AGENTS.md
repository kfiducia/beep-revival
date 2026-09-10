# beep-firmware — agent guide (public)

Custom OpenWrt firmware for the reverse-engineered **Beep** wireless speaker
(8devices Carambola2 / Atheros AR9331 + WM8524 I²S DAC). Repo: `kfiducia/beep-revival`.
What/why, hardware, and flashing steps: see **`README.md`** (Layout table + build/flash) and `docs/`.

> **Operator secrets are NOT in this repo.** Device passwords, MACs, LAN IPs, Wi-Fi keys and
> build-host details live in a **gitignored `AGENTS.local.md`** on the operator's machine — this
> repo is **public**, so never commit that file or paste its contents. This file is the public,
> secrets-free agent guide.

## Build (Docker buildroot on macOS)
- `docker exec -it beep-build bash /src/scripts/build.sh` (or run `./scripts/build.sh` inside the
  `beep-build` container). Output: `/build/openwrt/bin/targets/ath79/generic/`.
- AirPlay-2 features: `AIRPLAY2=1 ./scripts/build.sh`. The image converges AirPlay 1/2 behind a
  runtime toggle (Snapcast retired) — see `README.md`.

## Test / lint (off-device, fast inner loop)
- `sh tests/run.sh` runs all cases; `sh tests/run.sh <filter>` runs a subset. Cases live in
  `tests/cases/*.sh` (e.g. `ota_gate.sh`, `volume_math.sh`, `bootcount_decode.sh`); harness
  `tests/lib/harness.sh`; stubs under `tests/stubs/`.
- **shellcheck is pinned to 0.11.0** — match it locally (CI installs the official static binary,
  not the distro's apt shellcheck) or you'll get a different finding set.

## CI gates (`.github/workflows/ci.yml`, required on `main`)
Merge-blocking jobs: **shellcheck** (pinned 0.11.0), the **`sh tests/run.sh`** suite, and a
**`gcc -std=gnu11 -Wall -Wextra` compile** of `feed/beepd/src/beepd.c`. The I²S kernel module is
NOT built in CI.

## Where domain logic lives
- **Button gesture / LED-ring timing** → `feed/beepd/src/beepd.c`: `HOLD_WIFI_MS` = **≥10 s hold →
  Wi-Fi setup AP**, `HOLD_RESET_MS` = **≥30 s → factory reset** (plus the ring-rotation animation).
- **Backend verbs** (status / scan / set_wifi / set_name / set_volume / reboot) →
  `rootfs-overlay/usr/libexec/rpcd/beep`.
- **OTA authorization gate** → `rootfs-overlay/www/cgi-bin/beep-ota`.
- **I²S kernel driver** → `feed/beep-i2s`; **userspace daemon** → `feed/beepd`; rootfs glue →
  `rootfs-overlay/`.
- **Wi-Fi setup AP**: SSID `BeepRevival-Setup-<MAC6>`, IP `192.168.60.1`, WPA2
  (`rootfs-overlay/usr/libexec/beep/wifi-setup-enable`).

## Design docs & roadmap
`docs/RECOVERY-DESIGN.md`, `docs/BE-AP2-AUDIT.md`, and the open GitHub issues (#6 iOS AirPlay-1,
#7 volume push-back) are the shareable roadmap. Default branch: `main`.
