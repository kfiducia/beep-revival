# LMS (Lyrion Music Server / Squeezebox) support — spike test notes

**TRL 3 — spike.** Adds a Lyrion Music Server *player* to the Beep by riding OpenWrt's
upstream `squeezelite` package (v1.9.9.1432). No protocol was reimplemented: squeezelite
connects to an LMS instance on the LAN over SlimProto, the same way the old Squeezebox
players did.

## What this build has / doesn't have

| | |
|---|---|
| **Sources** | AirPlay-1 (classic) + Snapcast (multi-room) + **squeezelite (LMS)** |
| **LMS codecs built in** | FLAC, MP3 (libmpg123), Opus |
| **LMS codecs left out** | AAC/HE-AAC, WMA, ALAC, DSD, soxr resampling — LMS transcodes to FLAC |
| **Volume** | bound to the shared ALSA `Master` softvol (`-V Master`) — knob / web / LED all track it |
| **Output** | ALSA `default` PCM; `-C 5` releases the card when idle so AirPlay/Snapcast can use it |
| **Server** | auto-discovered on the LAN (or pin `squeezelite.options.server_addr`) |
| **Player name** | device hostname |

On-device marker: `cat /etc/beep-build-variant`.

## Why codecs were restricted (AR9331 = 400 MHz, big-endian, **no FPU**)

- The squeezelite audio pipeline is **integer end-to-end**: internal samples are `s32_t`,
  gain is 16.16 fixed-point (`(s64_t)gain * sample >> 16`, no float per sample), and the
  decoder→pipeline boundary is integer (FLAC native int; mpg123 `MPG123_ENC_SIGNED_16/32`;
  Opus `op_read` into `opus_int16*`). The only `float` is a one-shot gain-coefficient
  conversion at volume-change time.
- **Big-endian is compile-time correct**: `output_pack.c` has an explicit `#else`
  big-endian branch for every ALSA format, gated by `SL_LITTLE_ENDIAN =
  (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)` — the compiler's own target byte order.
  On this BE toolchain it emits the correct byte-swapped output natively (unlike the
  shairport pipe, which needed patch `030` to force little-endian).
- The one heavy codec on this silicon is HE-AAC/SBR — deliberately **not** compiled in.

Source validation is strong; the only thing it can't prove is DAC clocking → **listen for
static** on first play.

## Build

```bash
# on the iMac build host (source is rsynced to ~/beep-firmware, a non-git bind mount):
docker exec -d beep-build bash -lc 'cd /build/openwrt && env BEEP_DEV=1 SRC=/src bash /src/scripts/build.sh > /build/sq-build.log 2>&1'
# output: bin/targets/ath79/generic/*8dev_carambola2*sysupgrade.bin
```

`BEEP_DEV=1` keeps SSH enabled for on-device debugging.

## Test on a Beep

1. Run a Lyrion Music Server on the LAN.
2. The Beep appears as a player (its hostname) in the LMS web UI / app.
3. Play FLAC/MP3 → audio out; LED arc shows "playing".
4. Turn the knob and move the LMS/web slider → they track together (shared `Master`).
5. Listen for static (big-endian sanity), and check flash headroom on the built image.

## Known follow-ups (deferred — spike)

- Confirm `PKG_BUILD_DEPENDS` trim (ffmpeg/faad2 dropped) doesn't break the compile; restore
  a single dep if a header is missing (`scripts/build.sh` step `1c`).
- Coexistence policy when two sources want the card at once (currently first-come; `-C 5`
  lets squeezelite yield). A UCI "role"/arbitration in `beep-group` is the graduation step.
- Optional: tap-gesture → LMS play/pause via the server's JSON-RPC (mirror the Snapcast RPC).
