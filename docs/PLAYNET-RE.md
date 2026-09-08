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
  RE. **RESOLVED (§4.6):** the key is read by `beep_config_main_get_int`, whose `main` section
  lives in **`beep_devel`** (add `option dummy_audio_output '1'` to `config main 'main'` in
  `/etc/config/beep_devel`) — not `beep_data`, which is why the earlier `beep_data` edit was
  ignored and it fell back to the default `0` under qemu.

## 3. Grouping = LAN peer-to-peer consensus (no cloud)

From `beepmanager_grouping.lua` + `beepmanager_grouper.lua`:

- Each device holds `{source_id, sink_id, signal, source_time}` and **gossips** it to peers
  (device discovery + remote grouping events + local `ubus set_group_id`).
- Every device independently runs the same deterministic **`assign_groups()`** over the
  shared state → all converge on identical grouping, **no arbiter**. Group = devices sharing
  a `source`/`sink`; **duplicate-source ties break by oldest `source_time`** (original source
  wins), then signal/name. "Join" = set your `sink_id` to the target group's `source_id`.
- Discovery is mDNS (`_beepcontrol._tcp`, TXT + `control_port`).

## 4. Peer audio sync (playnet internals — REVERSED)

_Reversed 2026-09-08 by static disassembly of the clean stock `playnet`
(`u2-backup/rootfs.bin`), cross-checked against a qemu-mips run. Enough to build a clean
sync engine — see §4.5 for the resulting spec and §4.6 for what's proven vs. inferred._

### 4.0 What playnet actually is (the RE gotcha that mattered)

`playnet` is compiled as **MIPS16e** (ELF `e_flags 0x74001005`, big-endian, uClibc), not
plain MIPS32. Only the tiny `__uClibc_main` startup stub is MIPS32; **all the real code is
MIPS16**. A normal `objdump -d` decodes ~85% of `.text` as `.word` garbage (it desyncs on
MIPS16 + interleaved PC-relative constant pools + embedded codec tables), which is why the
first pass looked like "mostly data". The binary is stripped of local symbols, so function
names survive only as **logger string literals** (`lprintf(level, "<func>", <line>, fmt,…)`).

**How to disassemble it** (in the iMac `beep-build` container):
```
OD=/build/openwrt/staging_dir/toolchain-mips_24kc_gcc-13.3.0_musl/bin/mips-openwrt-linux-objdump
$OD -d -m mips:16 --start-address=<lo> --stop-address=<hi> playnet   # force MIPS16 decode
```
`-M mips16` is NOT reliable here — with no symbols it falls back to the file-default ISA
(MIPS32) wherever no MIPS16 symbol is nearby. `-m mips:16` forces MIPS16 everywhere (the
MIPS32 startup then shows as garbage, which is fine). Functions were located by scanning the
raw binary for **literal pointer words** to the logger name/format strings (MIPS16 uses
PC-relative constant pools right after each function). All sync code is in
`build/beeptwo/lib/audio/audio.c` (per the embedded source-path string).

### 4.1 The sync message = 36-byte header + 16-byte state payload (52 bytes)

Per sync tick the **source** sends, over one TCP channel, a three-message burst:

| # | message   | built/parsed by            | size      | carries |
|---|-----------|----------------------------|-----------|---------|
| 1 | audio_state | `0x407cb0` (build), `0x404978` (`audio_sync_state_write`, parse) | **52 B** | 36-B framing header + 16-B sync state |
| 2 | sb_state    | `send_sb_state` / `recv_sb_state` (`0x407d3c`) | header | announces the byte length of the PCM chunk to follow |
| 3 | sb_data     | `send_sb_data` (`0x407cf4`) / `recv_sb_data` (`0x407d88`) | variable | the raw decoded PCM ("stream buffer") |

The **16-byte sync-state payload** is the sample-accuracy core. Both the serializer
(`audio_sync_state_read`, `0x40477a`, sender side) and the deserializer
(`audio_sync_state_write`, `0x404978`, receiver side) were disassembled and **agree**:

```c
/* on-wire sync state — 16 bytes, big-endian (network order) */
struct playnet_sync_state {
    uint64_t current_track_jiffies;   /* bytes  [0..8)  — playback position, in SAMPLES @ 44100 Hz */
    uint64_t sync_discarded_samples;  /* bytes  [8..16) — cumulative samples dropped/added for alignment */
};
```

- Serializer: `memcpy(buf+0, &g_current_track_jiffies, 8); memcpy(buf+8, &g_sync_discarded_samples, 8);`
  (globals `0x41b938` and `0x41b940`). Deserializer does the exact reverse. Both **assert
  `len == 16`** ("invalid size" otherwise).
- **Endianness = big-endian / network order, and there is NO byteswap.** Three independent
  proofs: (a) the payload is a raw `memcpy` on a big-endian host, so bytes land MSB-first;
  (b) the header parser at `0x402a90` assembles u32 fields **by hand** as
  `(b0<<24)|(b1<<16)|(b2<<8)|b3`; (c) the drift math loads the payload as a u64 and feeds it
  to `__udivdi3` as a normal big-endian quad. A little-endian reimplementation MUST
  `htobe64`/`be64toh` these fields.
- The **36-byte header** (`0x41bc54`, built by `0x407c50`) is the channel framing: message
  type/opcode, a sequence/counter, and a **big-endian u32 length** (proven by `recv_sb_state`'s
  log "recv_sb_state done with %u bytes **expecting %zu data bytes**" — the header tells the
  receiver how many `sb_data` bytes follow). Individual header field offsets were not fully
  mapped (not needed for a clean reimpl — we define our own header in §4.5), but the
  header→payload split (36|16) and the BE u32 length field are confirmed.

### 4.2 Timing / drift algorithm (the alignment core, audio.c ~404-413)

Plain English: every device counts the **samples it has played** on a shared 44100 Hz clock
(`current_track_jiffies` = a sample counter, not wall-clock jiffies) plus how many samples it
has had to drop or pad to stay aligned (`sync_discarded_samples`). The source keeps shipping
its `(jiffies, discarded)` pair alongside the PCM. When a sink receives it, the sink **forces
its own playback clock to the source's value and re-arms playback at that exact position** —
so all devices are playing the same sample index at the same moment. There is no PLL/gradual
skew correction visible; it's a hard re-align ("override then resume NOW").

Technical, from the disassembly at `0x403b76`–`0x403c1e`:
1. Convert the received sample position to milliseconds:
   `target_ms = base + (sync_jiffies * 1000) / 44100`  (64-bit, via `__udivdi3`; `44100` and
   `1000` are literal immediates → sample-rate and ms/s).
2. **Override** the decoder's live clock: store the received 8-byte value into the decode
   struct at `+184/+188` (and mirror to a global), logging
   `"overriding with sync_jiffies: %llu"` (audio.c:413).
3. Set the decoder's resume position (`+176/+180`), set the run flag (`state |= 0x40`), and
   call the decoder's resume vtable method (`+12`) → the `"sync done, resume NOW."` path
   (logged from `playnet_on_sync_data`, playnet.c:~420). `playnet_on_sync_data` is the ubus/
   uloop callback that fires when a channel delivers sync data; it then drives this re-align.

### 4.3 Transport & ports

- **TCP**, via libubox **`usock`** in `libbeep.so`: `stream_ch_listen` (`0x88c4`),
  `stream_ch_connect`, `on_usock_connect_done`, `usock_wait_connect`, with a
  `msg_socket_send_message` framing layer. `AF_INET` (family `2`). `playnet` imports only
  `stream_ch_listen` from libbeep; the (de)serialization + state machines are local to
  `playnet`.
- Channels are opened/torn down **over ubus** (`beep.playnet`, `beep.playnet-%s`), driven by
  `beepmanager`/grouping (§3). The listen/connect port comes from UCI keys **`stream_port`**
  and **`port_base`** (also surfaced as ubus status fields); the base port literal seen is
  **`9001`** (`li v1,9001`), i.e. per-channel port ≈ `port_base + channel`.

### 4.4 State machines (for reference)

- **Send** side (`send_audio_state`, `0x404810`): global state var `@0x41bb50`, advancing
  `2 → 3 → 4 → 5` = serialize state → send audio_state → send sb_state → send sb_data.
- **Recv** side (`recv_audio_state`, `0x404a40`): state var `@0x41bb58`, `8 → 9 → 10 → 11` =
  recv+deserialize audio_state → recv sb_state (sets expected data length `@0x41bb60`-ish) →
  recv sb_data (loops until the announced byte count is received) → done.
- Key function offsets (clean `u2-backup` playnet, all MIPS16):
  `audio_sync_state_read 0x40477a`, `audio_sync_state_write 0x404978`,
  `send_audio_state 0x404810`, `recv_audio_state 0x404a40`, build-audio_state-msg `0x407cb0`,
  send_sb_data `0x407cf4`, recv_sb_state `0x407d3c`, recv_sb_data `0x407d88`,
  channel-reset `0x407728`, channel_write `0x4080f8`, playnet_on_sync_data `0x402a0c`,
  drift/override `0x403b34`.

### 4.5 → clean reimplementation spec (what to actually build)

One TCP channel per (source→sink) link. Framing per tick:

```
[ header: type + seq + be32 pcm_len ]           # our own compact header (stock used 36 B)
[ sync_state: be64 track_samples, be64 discarded_samples ]   # 16 B, big-endian
[ pcm: pcm_len bytes of decoded PCM ]
```

Sink alignment: on each tick, set local playback sample index := `track_samples` from the
source (hard re-align), accounting for `discarded_samples`, then resume. Clock unit = samples
@ 44100 Hz. Little-endian build → wrap the two u64s in `htobe64`/`be64toh`. This is a
hard-snap model (not a soft PLL); good enough for the stock UX and the measured CPU budget
(§5.1). If audible on re-align, add a soft-skew variant later.

### 4.6 Confidence & remaining validation

- **High confidence (proven both directions in the disassembly):** 16-byte payload layout,
  its two fields and meaning, big-endian order, the 36|16 message split, the 52-byte
  audio_state size, the 3-message burst, the sample-clock/override drift algorithm, TCP/usock
  transport, the state machines and function offsets above.
- **Inferred (not byte-mapped):** the internal field offsets *within* the 36-byte stock
  header (type/seq positions). Not needed for the clean reimpl, which defines its own header.
- **Remaining live confirmation — a 2-instance qemu wire capture — is NOT yet done.** Getting
  two `playnet`s to actually exchange sync frames under qemu-mips needs a much larger harness
  than §5: a complete sysroot (`libpthread`/`librt`/`libubox`/codecs, all missing from the
  current `pn-sys.tgz`), `dummy_audio_output=1` in the **`beep_devel`** `main` section (read
  via `beep_config_main_get_int` — NOT `beep_data`; this resolves the §2 open item), a running
  `ubus`, and a `beepmanager` stand-in to open the channel. The static cross-validation above
  is strong enough to implement against; the capture is a belt-and-suspenders check best run
  as part of implementation step (a) (2-node PCM stream), where the harness gets built anyway.

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

## 5.1 CPU budget (measured) + the Snapcast decision

Measured the always-on `codec=pcm` sync pipeline on real hardware (unit `beep-silver`,
AR9331) by driving PCM through `shairport->pipe->snapserver->snapclient`: **~81% idle**
(snapserver ~5%, snapclient low, ~18% sys). A second client (a Mac `snapclient` 0.35)
connected to silver's snapserver and negotiated the 44100:16:2 synced stream in <1s. So
**an always-on peer-sync engine is affordable on this chip**, and the "always-on source +
double-tap-to-join-the-active-source" UX model works. This was validated *with Snapcast as
a stand-in* only to prove the budget/UX.

**Decision: drop Snapcast; build the fresh integrated engine (playnet revamp).** Snapcast's
rigid server/client topology + role churn is the wrong fit; the goal is one clean daemon in
the stock playnet mould (decode + peer sync + consensus + ALSA sink), no Snapcast. The
budget measurement above says that's feasible.

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

**Open items:** ~~confirm the `dummy_audio_output` UCI package~~ (done, §4.6: `beep_devel`);
~~disassemble `audio_sync_state_send/recv` for the wire format~~ (done, §4); map playnet's
full ubus method surface (`beep.playnet` methods); run the 2-instance sync **wire capture** to
live-confirm §4 (needs the fuller harness described in §4.6 — build it as part of impl step (a)).
