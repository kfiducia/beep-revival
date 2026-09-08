# Stock `playnet` — reverse engineering toward a fresh reimplementation

_Goal: understand how the stock Beep's multi-room engine (`playnet`) actually works, so we
can **reimplement it cleanly** on our musl/OpenWrt firmware — not ship the 2015 uClibc
binary. Companion to issue #9 (multi-room). Written 2026-09-08._

## 0. Source of truth (which dump to use)

Two flash dumps exist under `~/beep-revival/dump/`:

- **`flash.bin` / `mtd_rootfs.bin` — CORRUPT, do not use for binaries.** Captured by
  hex-dumping flash over the U-Boot serial console (`ar7240> md.b 0x9f000000 0x1000000`,
  the 81 MB `flash_dump.log`) and reconstructing to binary. Serial hex-capture is lossy →
  **283 zero-byte files, 43 dead `.so` libs**. `mtd_rootfs.bin` is byte-identical to
  `flash.bin` @ `0x650000` (same bad source). Fine for boot/layout analysis only.
- **`dump/u2-backup/` — CLEAN, the good source.** A per-partition **SSH** dump
  (`beep-backup.sh`, with a MANIFEST) of unit 2 while still on **stock firmware
  (OpenWrt 3.8.13, 2015)**. `rootfs.bin` `unsquashfs`es with **zero errors**; all libs are
  intact ELFs. Extract with `unsquashfs -d u2rf rootfs.bin`. **Preserve this** — it's the
  last known-clean stock userland (both hardware units are now on our firmware).

## 1. Component map (stock `beep/platform/`)

| Binary | Role |
|---|---|
| **`playnet`** (56 KB MIPS) | the audio engine: decode + **peer sync** orchestration |
| **`beepi2s`** | the **hardware audio sink** — playnet *forks* it to drive I²S/ALSA |
| `beepdiscovery` | LAN peer discovery via mDNS (`_beepcontrol._tcp`, `_beephttp._tcp`) |
| `beepcomm` | control/messaging daemon |
| `beepcloud` | cloud link (app UI / service logins only — **not** grouping) |
| `libbeep.so` | config (UCI), GPIO/I²C, mDNS glue; playnet's main support lib |
| `lua/beepmanager_grouping.lua`, `beepmanager_grouper.lua` | **the grouping brain** (readable Lua) |

## 2. Process & audio model (confirmed via qemu-strace, §5)

- `playnet` reads config, runs `decode_play_init`, then **`decode_play_fork`s `beepi2s`**:
  `beepi2s -v -d default -c default -b 30000 -p 3 -s 16 -f 3`
  (`-d/-c` ALSA device/card, `-b` buffer, `-p` periods, `-s` sample bits=16, `-f` format).
  **`beepi2s` is the only thing that touches the real I²S hardware** — which is exactly why
  running `playnet` bare on a live Beep grabbed the hardware and rebooted it.
- Structured logs: `YYYYMMDD HH:MM:SS.mmm <epoch_ms> LEVEL playnet - <func>:<line> <msg>`.
- Config is UCI. `beep_config_data_get` reads **`/etc/config/beep_data`** (`group_id`,
  `device_name`). `beep_config_main_get_int` / `beep_config_devel_get_int` read devel/main
  keys — **`playnet_gain`** (default 14155) and **`dummy_audio_output`** (default 0). Setting
  `dummy_audio_output=1` should bypass the `beepi2s` fork → the clean way to run headless for
  RE (⚠️ still confirming which UCI package that key lives in — `beep_data` didn't take;
  likely `beep_devel`).

## 3. Grouping = LAN peer-to-peer consensus (no cloud)

From `beepmanager_grouping.lua` + `beepmanager_grouper.lua`:

- Each device holds `{source_id, sink_id, signal, source_time}` and **gossips** it to peers
  (device discovery + remote grouping events + local `ubus set_group_id`).
- Every device independently runs the same deterministic **`assign_groups()`** over the
  shared state → all converge on identical grouping, **no arbiter**. Group = devices sharing
  a `source`/`sink`; **duplicate-source ties break by oldest `source_time`** (original source
  wins), then signal/name. "Join" = set your `sink_id` to the target group's `source_id`.
- Discovery is mDNS (`_beepcontrol._tcp`, TXT + `control_port`).

## 4. Peer audio sync (playnet internals — from symbols; wire format TBD)

- Sync API: `audio_sync_state_send/recv/read/write`, `playnet_on_sync_data`,
  `sync_timestamp`, `sync_jiffies`, `sync_discarded_samples`, `"sync done, resume NOW"`.
- Transport: TCP stream sockets, `stream_ch_listen` / `stream_port` / `port_base`; controlled
  locally over **ubus** (`beep.playnet`, `beep.playnet-%s`).
- **Still to reverse:** the exact on-wire sync-state struct and how source→sink alignment is
  computed (needs static disassembly of `audio_sync_state_send/recv` + a 2-instance capture).

## 5. Safe RE harness (qemu-mips user-mode — no hardware risk)

`scripts/playnet-re/*.sh`. Method: extract the **clean u2 sysroot** (uClibc runtime + libs +
`beep/platform` + `etc/config`) and run playnet under **`qemu-mips` (big-endian) user-mode**
with `-strace`, so every syscall/socket/config read is visible and **no real hardware can be
touched**. Do NOT run playnet bare on a live Beep (it reboots the host via `beepi2s`/GPIO).

```
apt-get install qemu-user            # in the build container
qemu-mips -strace -L <u2-sysroot> -E LD_LIBRARY_PATH=/beep/platform:/usr/lib:/lib \
   <u2-sysroot>/beep/platform/playnet
```

This confirmed the process/audio model in §2. `playnet` loads and runs on our image via the
bundled uClibc (feasible), but revival-as-binary is a dead end for production (2015 uClibc +
must fork `beepi2s` + needs `beepmanager`); the harness is for **understanding**, not shipping.

## 6. Reimplementation sketch (the actual deliverable)

A fresh, small daemon on musl/OpenWrt that reproduces the *design*, not the binary:

1. **Discovery:** advertise/browse an mDNS service (reuse the stock `_beepcontrol._tcp` shape
   or a new `_beep._tcp`) with a TXT carrying `device_id`, `source_id`, `sink_id`,
   `source_time`, `signal`.
2. **Consensus:** port `assign_groups()` (readable Lua → shell/C/Rust) — deterministic, run on
   every node over gossiped state. "Stream anywhere" = the streamed-to node becomes `source`;
   double-tap sets another's `sink` to it.
3. **Sync transport:** either (a) drive **Snapcast** (maintained; the streamed-to node runs
   snapserver, sinks run snapclient) — coordination layer only; or (b) a lean PCM+timestamp
   peer stream modeled on playnet's `audio_sync_state` if Snapcast's overhead is unacceptable.
4. **Audio sink:** a small ALSA writer (the `beepi2s` role) fed by the source's decoded PCM.

**Open items:** confirm the `dummy_audio_output` UCI package; map playnet's full ubus method
surface; disassemble `audio_sync_state_send/recv` for the wire format; 2-instance sync capture.
