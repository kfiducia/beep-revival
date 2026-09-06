# Build status — honest ledger

Autonomous build session (2026-09-05). Everything below is now **working on
hardware** — the "needs the bench" unknowns were resolved. See the ✅ section.

## ✅ WORKING ON HARDWARE — Phase 1 & 2 done (2026-09-05)
- **🔊 Audio out the jack** — clean 440Hz tone, `aplay` EXIT=0, DMA period IRQ
  climbing (0→254 over 3s). The **make-or-break MBOX-DMA data plane is ported and
  working** (see PORTING.md): descriptor-ring DMA + IRQ handler in
  `driver-i2s/beep-i2s.c`, on the MBOX0 RX channel. Two fixes closed it: a hard
  MBOX reset (RESET module `0x1806001c` bit1) each `prepare` (else 2nd playback
  `-EIO`), and `snd_pcm_hw_constraint_integer(PERIODS)` (else a cyclical pop from
  the ring not covering a partial buffer).
- **📦 Flashed & persistent** on unit #1; boots via the Beep's own `bootb`
  failsafe (`beep_primary` re-pointed at `0x9f050000` — bootcount + recovery
  mechanism kept intact, not bypassed).
- **📶 Networked** — wifi up after switching the DTS to the modern `nvmem-layout`
  calibration binding (legacy `mtd-cal-data` silently failed → ath9k `-5`).
  dropbear + AirPlay live; on the LAN, key-auth SSH, `scp -O` deploy loop.
- Codec + card are packaged in `feed/sound-soc-extra` (kmod-sound-soc-wm8524 /
  -simple-card aren't in OpenWrt 24.10).

## 🆕 Implemented this session — pending flash + on-device test
Written and synced to the feed/overlay; **not yet built/flashed** at time of
writing (build host was mid-rebuild). Treat as "should work, verify on device."

- **AirPlay 2 fits alongside a recovery slot.** Swapped shairport's `libffmpeg-full`
  (~12 MB) for `libffmpeg-audio-dec` (~2.3 MB, AAC+ALAC only, +swresample patched
  back in). Sysupgrade image dropped **15.25 MB → 9.94 MB**, leaving ~5.6 MB for a
  recovery partition. AirPlay 2 stays fully functional (mbedtls + avahi + nqptp all
  present, non-zero). See `scripts/build.sh` step 1b.
- **Real admin/setup web app** (`rootfs-overlay/www/index.html`) — self-contained
  SPA (no CDN; works offline on the setup AP), talks to `/ubus` (uhttpd-mod-ubus)
  with an rpcd **root session login**. Status card, **Wi-Fi scan + join**, device/
  AirPlay naming, volume slider, multi-room role, admin-password change, reboot.
  Replaces the old 5-line form that POSTed to a nonexistent CGI.
- **Enriched rpcd backend** (`usr/libexec/rpcd/beep`) — methods `status` (ssid/ip/
  signal/uptime/version/ap_mode/volume), `scan` (iwinfo→JSON), `set_wifi`,
  `set_name`, `set_volume`, `set_audio`, `set_password`, `reboot`; ACL in
  `acl.d/beep.json`.
- **Closed the empty-root-password hole.** Fresh OpenWrt has a blank root password
  = passwordless admin once on the LAN. `99-beep` now derives a per-device code
  from the MAC and sets it as BOTH the root/admin password and the WPA2 setup-AP
  key (`/etc/beep-code`). Never blank-auth on any network. uhttpd keeps plain HTTP
  for the captive portal (WPA2-encrypted link) and offers per-device HTTPS on 443.
- **Wi-Fi fallback watchdog** (`etc/init.d/beep-netcheck` → `netcheck-loop`) —
  continuous procd service on the **correct STA iface (wwan)**, not the old one-shot
  `wan` check. STA can't associate → `Beep-Setup-XXXX` **WPA2** AP + captive portal;
  STA recovers → tears the AP down. Single-radio AP↔STA handoff handled.
- **LED ring behavior restored** (`beepd/beepd.c`) — dark when idle, **volume arc**
  while the knob turns, **random "party" pulse** while playing (PCM RUNNING), plus
  a rotating **AP-setup comet** driven by `/var/run/beep/led-mode`.
- **Unified system volume** — WM8524 has no hardware volume register, so
  `/etc/asound.conf` adds an ALSA **softvol "Master"**; the knob (`beep-action`),
  the web slider, and AirPlay (shairport `mixer_control_name`) all drive the same
  control.

## Done & high-confidence (authored from the register map / decoded protocol)
- ✅ **Device tree** (`dts/ar9331_beep_dial.dts`) — real I²S register addresses (STEREO 0x180b0000, MBOX 0x180a0000, confirmed vs the unstripped `ath_i2s.ko`), WM8524 mainline codec node, simple-audio-card, i2c-gpio for the STM8, setup key, and the **exact upstream partition table** (u-boot/art preserved).
- ✅ **I²S control plane** (`driver-i2s/beep-i2s.c`) — DT-probed ASoC CPU DAI: clock table (44100→0x11/0xB726, 48000→0x10/0x46AB), CONFIG register (I2S+SPDIF+MASTER, 16-bit, posedge), and the GPIO function-mux pokes (no mainline pinctrl for these). Modern kernel-6.x APIs (ioremap, of_match, devm_snd_soc_register_component).
- ✅ **`beepd`** (`beepd/beepd.c`) — STM8 daemon at i²c **0x23** (the *live* protocol): input decode (signed knob delta + press/release counts, v0/v1), LED ring with the cubic gamma + rotation, tap/double-tap(340ms)/hold(5s) state machine, gesture→`beep-action`. Plain C, compiles against musl.
- ✅ **Provisioning + admin + robustness** — every-boot wifi-fallback → SoftAP + `dnsmasq` captive portal; **authenticated** `rpcd` admin object + ACL (closes the stock anonymous-ubus hole); per-device hostname + self-signed TLS at first boot (no default creds/shared certs).
- ✅ **OpenWrt packaging** — `feed/` (kmod-beep-i2s, beepd), `scripts/build.sh` (rides the carambola2 profile, swaps our DTS, layers feed+files+packages), `docker-build-setup.sh` (toolchain).
- ✅ **Phase-0 prebuilt images** downloaded (`prebuilt/`) for the zero-risk RAM-boot validation.

## Bench items — mostly resolved
- ✅ **I²S DMA "data plane"** (`driver-i2s/PORTING.md`) — **DONE, plays clean
  audio.** The make-or-break turned out tractable: ported the vendored 3.14
  reference to a kernel-6.6 PCM component (descriptor ring built with explicit
  bit-shifts for LE, IRQ handler on miscintc line 7, managed DMA buffer).
- ✅ **Exact OpenWrt package names / codec+card** — resolved: wm8524 + simple-card
  packaged in `feed/sound-soc-extra`; wifi cal via `nvmem-layout` (not the dead
  `mtd-cal-data`). Image builds and runs.
- ⚠️ **WM8524 mute-GPIO pin** — still a DTS placeholder; audio works without it,
  but confirm on hardware if a mute glitch shows up.
- ⚠️ **STM8 v1 read checksum** — not reproduced (stripped `.so`); beepd
  sanity-filters instead (safe; v0 has no checksum). Optional Saleae capture.
- ⚠️ **OTA**: set `REQUIRE_IMAGE_SIGNATURE=1` and *test* a bad image is rejected
  (manual `sysupgrade -n` doesn't need it; matters for any push-update path).

## Next action (Phase 3)
1. **Flash + test the session's work** — build, validate packages non-zero, `scp -O`
   + `sysupgrade` (preserve config so wifi/SSH/key survive), then verify: AirPlay 2
   advertises, the web SPA loads over `/ubus` and scans/joins wifi, the LED modes
   behave, and the knob moves the softvol Master.
2. **Recovery slot** — the image is now **9.94 MB**, leaving ~5.6 MB. Build a minimal
   OpenWrt initramfs into that slot and point `beep_recovery` at it, so `bootb`
   auto-recovers a bad update **without a UART**. (Budget confirmed; design pending.)
3. **Multiroom** — Snapcast (needs a custom `feed/snapcast/`); `beep-action`
   double-tap already stubs group-join.
4. **Flash unit #2** — network-flashable (`scp -O` + `sysupgrade`) after one UART
   clip to RAM-boot it the first time.
