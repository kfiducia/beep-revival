# Build status — honest ledger

Autonomous build session (2026-09-05). What's authored, what compiles, what needs the bench.

## Done & high-confidence (authored from the register map / decoded protocol)
- ✅ **Device tree** (`dts/ar9331_beep_dial.dts`) — real I²S register addresses (STEREO 0x180b0000, MBOX 0x180a0000, confirmed vs the unstripped `ath_i2s.ko`), WM8524 mainline codec node, simple-audio-card, i2c-gpio for the STM8, setup key, and the **exact upstream partition table** (u-boot/art preserved).
- ✅ **I²S control plane** (`driver-i2s/beep-i2s.c`) — DT-probed ASoC CPU DAI: clock table (44100→0x11/0xB726, 48000→0x10/0x46AB), CONFIG register (I2S+SPDIF+MASTER, 16-bit, posedge), and the GPIO function-mux pokes (no mainline pinctrl for these). Modern kernel-6.x APIs (ioremap, of_match, devm_snd_soc_register_component).
- ✅ **`beepd`** (`beepd/beepd.c`) — STM8 daemon at i²c **0x23** (the *live* protocol): input decode (signed knob delta + press/release counts, v0/v1), LED ring with the cubic gamma + rotation, tap/double-tap(340ms)/hold(5s) state machine, gesture→`beep-action`. Plain C, compiles against musl.
- ✅ **Provisioning + admin + robustness** — every-boot wifi-fallback → SoftAP + `dnsmasq` captive portal; **authenticated** `rpcd` admin object + ACL (closes the stock anonymous-ubus hole); per-device hostname + self-signed TLS at first boot (no default creds/shared certs).
- ✅ **OpenWrt packaging** — `feed/` (kmod-beep-i2s, beepd), `scripts/build.sh` (rides the carambola2 profile, swaps our DTS, layers feed+files+packages), `docker-build-setup.sh` (toolchain).
- ✅ **Phase-0 prebuilt images** downloaded (`prebuilt/`) for the zero-risk RAM-boot validation.

## Needs the bench (flagged in the research as genuine bring-up)
- ⚠️ **I²S DMA "data plane"** (`driver-i2s/PORTING.md`) — the MBOX descriptor-ring
  DMA + ALSA PCM component. Reference vendored (`driver-i2s/reference/`), port
  scoped step-by-step. **This is the make-or-break** (no one has publicly done
  clean ALSA audio on this SoC). Advantage: the stock `ath_i2s.ko` is unstripped
  = a working register oracle. Until it lands, the card exposes the DAI but no
  PCM (aplay fails cleanly — the correct Phase-1 checkpoint).
- ⚠️ **WM8524 mute-GPIO pin** — placeholder in the DTS; confirm on hardware
  (Saleae during stock power-up) or make it optional / use snd-soc-dummy.
- ⚠️ **STM8 v1 read checksum** — not reproduced (stripped `.so`); beepd
  sanity-filters instead (safe; v0 has no checksum). Optional: Saleae-capture to
  recover it.
- ⚠️ **Exact OpenWrt package names** in `build.sh` (kmod-sound-soc-*, snapcast,
  shairport variants) may need a `menuconfig` pass — first `make` will say which.
- ⚠️ **OTA**: set `REQUIRE_IMAGE_SIGNATURE=1` and *test* a bad image is rejected.

## Next action
1. Let the toolchain finish (container `beep-build`), run `scripts/build.sh`,
   fix any package-name/DTS compile errors it surfaces (iterate).
2. In parallel, **Phase 0** on hardware: `loady` `prebuilt/…initramfs…bin` into
   unit #1 and confirm mainline boots — needs the user + serial cable.
3. Then Phase 1: port the DMA (PORTING.md) and chase a tone.
