# Converged image — one firmware, runtime-selectable AirPlay 1 / 2

## Why
We shipped three images: `ap1` (AirPlay 1 + Snapcast), `ap2` (AirPlay 2, no Snapcast),
and `replaynet` (AirPlay 1 + the replaynet multi-room engine). That forced a flash to
change AirPlay protocol or multi-room engine. This converges them into **one image** whose
behaviour is chosen at **runtime**, and retires Snapcast entirely.

Two facts make it possible:
- **Snapcast is the space hog** (`snapserver` 2.36 MB + `snapclient` 722 KB ≈ 3 MB of
  binaries + Boost). `replaynet` (65 KB) fully replaces it. Dropping Snapcast frees the
  room to keep the AP2 stack (ffmpeg AAC decoder + nqptp) in the same image. Converged
  squashfs ≈ 10.75 MB sysupgrade — the same size as the old `ap2` image — well within the
  ~13 MiB rootfs region.
- **shairport-sync's AP1 vs AP2 is advertisement-driven.** Upstream 5.1 added a runtime
  `service_type` (`auto` | `airplay2` | `classic`). We're pinned to 4.3.2, so we backport
  just that mechanism (`scripts/patches/050-service-type-classic.patch`): `classic` makes
  the AirPlay-2 build advertise **only** `_raop._tcp` (AirPlay 1) and skip the nqptp
  requirement. One binary, two protocols, chosen at runtime.

## The hard invariant
The 400 MHz AR9331 **cannot** decode AirPlay-2 AAC *and* fan out a multi-room group at the
same time. So the two — and only two — valid states are:

| Mode | shairport | multi-room | replaynet daemon | nqptp | double-tap |
|---|---|---|---|---|---|
| **ap1** (default) | classic RAOP, lossless ALAC | **replaynet** (Beep) | up when grouped (always-pipe) | off | forms/leaves a replaynet group |
| **ap2** | AirPlay 2, AAC | **native** (Apple Home) | **stopped + `enabled=0`** | on | inert (grouping is Apple's) |

**AP2 must never be routed through replaynet's source/sink.** This is enforced *actively*,
not incidentally: selecting `ap2` tears replaynet down and points shairport straight at
ALSA; selecting `ap1` brings the replaynet engine back.

## Pieces
- `scripts/patches/050-service-type-classic.patch` — backports `general.service_type`
  onto shairport 4.3.2 (`shairport.c` + `mdns_avahi.c`).
- `scripts/build.sh` — `CONVERGED=1` builds the single image (implies AP2 shairport +
  replaynet engine, drops Snapcast) and seeds `beep.main.airplay_mode=ap1` via the
  `99-beep-converged` uci-default.
- `usr/libexec/beep/beep-airplay-mode` — the switcher (`ap1`|`ap2`|`apply`|`status`). Owns
  `service_type`, `group_engine`, and the `nqptp` service; the single source of truth.
- `usr/libexec/beep/beep-group` — Snapcast retired; engines are now `replaynet` (AP1) or
  `none` (AP2-native, a hard no-op that guarantees replaynet is down and shairport→ALSA).
- `usr/libexec/rpcd/beep` + `acl.d/beep.json` + `www/index.html` — a `set_airplay_mode`
  method, `airplay_mode` in `status`, and an "AirPlay mode" toggle card in the admin UI.

## Trade-offs / notes
- AP2 remains CPU-marginal on this silicon; the toggle lets a user drop to AP1 (lighter,
  lossless) without reflashing — it does not make AP2 glitch-free.
- Switching modes restarts shairport-sync (a brief AirPlay drop, ~10 s) and may require an
  AP2 re-pair in Apple Home.
- When shairport-sync is bumped to ≥ 5.1, patch 050 becomes redundant (the option is
  upstream); the `service_type` config key we write is already the upstream name.
