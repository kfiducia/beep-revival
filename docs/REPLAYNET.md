# replaynet — fresh multi-room sync engine

`replaynet` is a clean reimplementation of the stock Beep multi-room engine
(`playnet`), built on our musl/OpenWrt userland instead of shipping the 2015
MIPS16/uClibc binary. It reproduces the stock *design* — one integrated daemon doing
decode + sample-accurate peer sync + LAN group consensus + an ALSA sink — reverse
engineered in **`docs/PLAYNET-RE.md` §4** (read that first for the protocol origins).
Snapcast is deliberately dropped (issue #9).

## Roadmap

| step | scope | status |
|------|-------|--------|
| **(a)** | 2-node PCM stream + on-wire framing | done |
| **(b)** | timestamp / clock sync + drift correction | done |
| **(c)** | ALSA sink (the stock `beepi2s` role, WM8524) | done |
| **(d)** | grouping consensus + discovery + fan-out + play hooks | done (this PR) |

## Wire protocol v2

All fields are **big-endian (network order)** — the stock protocol is network-order,
proven three ways in §4. We serialize/parse by hand (`be32_put`/`be64_get`) so a
little-endian build stays correct. One TCP connection (`TCP_NODELAY`) carries typed
messages; the source connects to the sink.

```
common header — 16 bytes
  u32 magic      "RPLY" (0x52504C59)
  u8  version    2
  u8  type       1=AUDIO  2=PING  3=PONG
  u16 reserved
  u32 seq        per-type counter
  u32 body_len   bytes of body following

AUDIO body — 28 B fixed + pcm_len bytes of interleaved S16 PCM (44100 Hz, 2 ch)
  u64 track_samples      head-sample index @ 44100     (stock: current_track_jiffies)
  u64 discarded_samples  source-side cumulative drop/pad (stock: sync_discarded_samples; step d)
  u64 source_time_ns     source monotonic-clock ns when track_samples is emitted
  u32 pcm_len

PING body (sink->source) —  8 B:  u64 t1
PONG body (source->sink) — 24 B:  u64 t1_echo, u64 server_rx_ns, u64 server_tx_ns
```

The source **paces to real time** (1x playback): the head sample of each chunk, index
`track_samples`, is emitted at `pace_start + track_samples/44100`. So `source_time_ns`
tracks true playback time and the timestamps mean something.

## Clock sync (step b)

Multiroom needs a shared time base: "present sample N at instant T" must mean the same
thing on every node. Each sink runs an NTP/PTP-lite handshake over the audio socket —
`PING(t1)` → `PONG(t1, server_rx, server_tx)` → note `t4` — and computes the offset

```
theta = ((server_rx - t1) + (server_tx - t4)) / 2      # source_clock = local_clock + theta
rtt   = (t4 - t1) - (server_tx - server_rx)
```

It keeps the least-jittered (lowest-RTT) sample over the first few pings, then **locks
and freezes** theta (PTP/Snapcast style) so the drift controller — not a moving offset
— absorbs ongoing skew. The source answers pings *promptly during its pacing sleep*, so
the request/response delay stays symmetric and theta is accurate to well under a
millisecond on LAN. A source sample stamped `source_time_ns` is scheduled for local
presentation at `source_time_ns - theta + RN_BUFFER_NS` (80 ms jitter buffer).

_(A long-running daemon should periodically re-lock and re-anchor; that is future work.)_

## Drift correction (step b)

Each node's audio clock (its crystal, later the WM8524's) runs a few hundred ppm off
the source's. With the shared time base, the sink measures how far the playout schedule
has moved relative to real elapsed time — the headroom `want_local - now` drifts at
exactly the relative-rate error — and `rn_drift_decide()` returns frames to **drop**
(behind) or **insert** (ahead) to null it. It is a hysteretic controller (±64-frame /
~1.5 ms deadband, ≤441-frame / 10 ms step so corrections never pop), pure and
deterministic so it is unit-tested (`--selftest`) and reused verbatim by the step (c)
DAC writer. Step (b) computes and logs the correction trajectory; step (c) applies the
drops/inserts to the real device.

## ALSA sink (step c)

`--alsa <device>` makes the sink play to a real ALSA device (the stock `beepi2s` role)
instead of writing PCM to a file. On the Beep that is `default` → `softvol("Master")` →
`hw:0,0` (the WM8524); `plug` handles any rate conversion, so we open at 44100/S16/2ch.

Playout path: a jitter ring buffer sits between the network reader and the DAC writer.
On start the sink locks the clock offset, anchors the schedule, waits until the anchor
sample's local presentation time (`source_time_ns - theta + RN_BUFFER_NS`), **trims the
prebuffer that accumulated during the lock to the setpoint**, then feeds the device.
`snd_pcm_writei` paces consumption at the DAC clock; between writes the socket is drained
into the ring.

Drift is corrected by a **buffer-level servo** (Snapcast style): hold the total latency
(ring bytes + `snd_pcm_delay()` queued frames) at the setpoint (`RN_BUFFER_NS`, 400 ms —
the Beep is on wifi, where bursts overflow/starve a small buffer; wired LANs are fine far
lower). If the DAC clock runs slow the latency grows → drop frames; if it runs fast →
insert silence. The buffer level is the integrator, so a fixed offset is corrected once
(not every iteration) — rate-limited (250 ms) with a ~100 ms tolerance so transient wifi
jitter rides *in* the buffer and only sustained drift triggers a correction.
`rn_drift_decide()` (unit-tested) makes the drop/insert decision. Sample-drop/insert is
the spike-level correction; smoothing it with resampling is future work, as is tightening
inter-sink alignment below the setpoint tolerance (the synchronized start already aligns
them to ~1 ms).

## Grouping & orchestration (step d)

`--node` turns replaynet into the multiroom brain (the stock `beepmanager` role). Every
node:

- **Gossips** its `{id, source, sink, signal, source_time}` on the LAN (UDP multicast
  `239.7.42.99`; mDNS `_beep._tcp` via avahi is the production transport per §6 — the
  multicast stand-in carries the same TXT-equivalent fields) and keeps a peer table with
  a 6 s TTL.
- **Runs consensus** — `rn_assign_groups()`, a deterministic C port of the stock
  `assign_groups()` (docs/PLAYNET-RE.md §3): resolve duplicate source claims by oldest
  `source_time` (tie → greater id); drop sources with no listener; elect a source
  (highest signal, then id) for any group that has listeners but no source. It is pure
  and unit-tested (`--selftest`). Crucially the node **gossips only its voluntary claim**
  (set by `become-source`) and recomputes elected roles locally every ~500 ms — feeding a
  computed/elected role back into gossip makes ownership oscillate.
- **Acts on its role**: always keeps a sink player listening; when it is the source of a
  group it forks a **fan-out** that streams one identically-timestamped feed to every
  member — including itself via `127.0.0.1` — so the whole group (source included) plays
  in sync off the step (b) clock sync + step (c) servo.

**Triggers** arrive as one-line commands on a UNIX control socket, so the firmware's
existing hooks drive it with no code coupling:

```
replaynet --ctl become-source     # shairport "playing" hook: stream-here -> I am the source
replaynet --ctl "join <id>"       # double-tap hook: join that device's group
replaynet --ctl leave             # drop out
replaynet --ctl status
```

`become-source` sets `sink = source = my id` (my own group); `join G` sets `sink = G`
(the source of group G is device G, or the elected fallback). Group id == the source
device's id, matching the stock model where "join" = point your sink at the source.

## Usage

```
replaynet --source --peer <ip:port> [--pcm <file|->]
          [--fake-clock-offset-ns N] [--fake-clock-rate-ppm P]   # test injectors
replaynet --sink   --listen <port>  [--out <file|-> | --alsa <device>]
replaynet --node   --id <name> [--group <id>] [--alsa <dev>] [--pcm <src>]
          [--signal N] [--no-audio]                              # multiroom node (step d)
replaynet --ctl <cmd>   (become-source | join <id> | leave | status)
replaynet --selftest
```
PCM is raw interleaved S16, 44100 Hz, 2 ch (4 B/frame). Without `--alsa` the sink writes
received PCM straight through (file/stdout/fifo) — used by the host/qemu regression. The
`--fake-clock-*` flags make a source lie about its clock (constant offset / rate ppm) so
the sink's estimator and drift servo can be validated end-to-end over the socket.

## Testing

`scripts/replaynet/loopback-test.sh <run-prefix>` runs a source+sink over loopback and
asserts: selftest passes; PCM round-trips byte-for-byte; the wire header decodes
big-endian v2; an injected +5 ms clock offset is recovered to within ±1 ms; and an
injected +3000 ppm rate skew is tracked by the drift controller. Runs on the host and
under qemu-mips (the "2-instance qemu capture" that live-confirms §4 against our engine):

```
scripts/replaynet/loopback-test.sh ./replaynet                    # host build
scripts/replaynet/loopback-test.sh "qemu-mips /tmp/rn/replaynet"  # cross-built, on-target
```

Both pass on the host and under qemu-mips (MIPS32 big-endian). The `--alsa` path can't run
under qemu (no sound device); it is validated on the real unit:

```
# same-unit loopback into the WM8524:
replaynet --sink --listen 5056 --alsa default &   # opens default -> softvol -> hw:0,0
replaynet --source --peer 127.0.0.1:5056 --pcm sine.pcm
# cross-device (real multiroom): sink on one Beep, source on another
#   copper$  replaynet --sink   --listen 5060 --alsa default
#   silver$  replaynet --source --peer <copper-ip>:5060 --pcm sine8.pcm
```
Confirmed on real hardware, and the 440 Hz tone is **audible** out the speaker:
- **Same-unit (beep-silver):** ALSA opens 44100/S16/2ch; clock locks (~40 us loopback);
  buffer servo holds latency stable; the small steady net drop is the WM8524 I2S clock
  drifting vs the system clock, corrected — clean drain.
- **Cross-device (beep-silver -> beep-copper, over wifi):** clock sync locks across two
  independent units (theta absorbs the per-boot CLOCK_MONOTONIC uptime difference,
  ~3295 s here); the full 8 s streams and plays on copper; with the 400 ms buffer the
  latency holds steady (~+/-70 ms of setpoint, net correction ~0) so wifi jitter rides in
  the buffer instead of forcing drops (an 80 ms buffer glitched badly, which drove the
  400 ms choice).

**Grouping (step d)** is unit-tested in `--selftest` (duplicate-source resolution,
signal-based election, orphan culling, idempotence) and validated live:
- **3 nodes on one host (`--no-audio`):** all start in group 1 → converge on one elected
  source, the rest sinks; `--ctl become-source` on A + `--ctl "join A"` on B then makes A
  the source of group A with B its sink and C alone — stable across repeated runs.
- **2 Beeps (silver + copper) with `--alsa`:** UDP-multicast gossip crosses the wifi;
  `become-source` on silver + `join silver` on copper makes silver fan out to itself
  (127.0.0.1) and copper; **both units report identical playout latency/ring** — i.e. the
  same samples on the same schedule (in sync), and the tone is **audible on both**.

Package: `feed/replaynet/` (single C file, mirrors `feed/beepd`, `DEPENDS +alsa-lib`,
built `-DRN_ALSA -lasound`). Selected in the image via `CONFIG_PACKAGE_replaynet=y`
(built for CI coverage; not auto-started yet).

## Tight-sync engine (post-step-d work, branch `feature/replaynet-pipeline`)

The step-c buffer-fill servo only held each sink's *latency* near a setpoint, so under
wifi jitter sinks drifted ~100–300 ms apart (audible flam). Replaced with an
**absolute-schedule servo**: each sink steers so source sample `S` is audible at
`want_local + (S − anchor_sample)/RATE`. That mapping is invariant to which frame a node
anchored on (the anchor terms cancel to `source_time_base + S/RATE − theta + BUFFER`), so
every node targets the *same* (source-sample, wall-clock) point and stays sample-aligned
regardless of buffer jitter. The sink tracks source-sample position (`rx_next_src` from
each frame's `track_samples`, `played_src` for what it has fed the DAC) and nulls the
error by drop/insert, rate-limited, with a **ring floor** so a low-latency co-located
sink can't drop itself into starvation/XRUN. The click-free corrector is built and
unit-tested as a pure component — `cubic_q16` (Catmull-Rom) plus a streaming `rn_rsmp`
resampler (feed input / pull output at a Q16.16 `step`, phase + window carried across
calls). `--selftest` covers unity fidelity, **cross-block continuity** (strictly monotone
→ no splice → no click), and rate accuracy at a non-unity ratio. What remains is wiring it
into `run_sink_alsa` in place of drop/insert (map the schedule error to a small ppm on
`step`) and tuning the ppm gain/clamp on hardware — the math is proven, so that's a small,
low-risk change once a unit is available to listen on.

`--fanout` is a first-class mode (one source → many sinks) used by the node. Its sends
are **non-blocking**: a frame goes to a sink only when it's writable (`POLLOUT`), else
that frame is skipped (dropped after ~13 s stuck). This fixed a fatal deadlock — a
blocking send to a slow/locking sink stopped the fan-out answering *its* clock-sync
pings, so it never locked, never drained, and stayed blocked, starving every other sink.

**Hardware result (2 Beeps):** the local sink holds **sub-millisecond** schedule error
(−22 µs to −3 ms) sustained; a remote sink hit **+1.3 ms** on a healthy link. Under a
weak copper link (−72 dBm, ~1.4 Mbps raw PCM doesn't fit) the fan-out gracefully skips
frames to copper while the other stays perfectly synced. Follow-ups: cubic resampler for
click-free correction; FLAC/opus so weak wifi fits; per-sink gap-fill on skips.

## Productionization: replacing the snapcast pipeline

Today: AirPlay-1 shairport decodes → `/tmp/snapfifo` → snapserver → snapclient (per unit)
→ ALSA `default` (softvol Master → WM8524). replaynet replaces the whole snapserver +
snapclient sync layer with one `replaynet --node` daemon reading the shairport pipe.

Wired on this branch (all reversible — the default engine is still snapcast):

- **`/etc/init.d/replaynet`** (procd, mirrors squeezelite): launches
  `replaynet --node --id <hostname> --alsa default --pcm /tmp/beep-pcm`, `nice -12`,
  respawn. Registered at boot by **`/etc/uci-defaults/99-beep-replaynet`** but DORMANT
  (`start_service` returns unless `replaynet.node.enabled=1`).
- **`/etc/config/replaynet`** — UCI (`id`, `group`, `device`, `signal`, `enabled=0`).
- **`beep-group`** gains a `group_engine` selector (`snapcast` default | `replaynet`). The
  replaynet branch stops snapcast, enables the node, repoints shairport to a **pipe →
  `/tmp/beep-pcm`** with **`sessioncontrol` hooks** (`run_this_before_play_begins →
  replaynet --ctl become-source`, `..._after_play_ends → ... leave`), and maps roles to
  the control socket. Double-tap already calls `beep-group toggle`, which routes to
  `replaynet --ctl toggle` (join the active source, or leave) — no `beep-action` change.
- **`scripts/build.sh`**: `MULTIROOM=replaynet ./build.sh` drops
  snapserver/snapclient/libatomic (~5 MB + the C++ runtime) and drops a uci-default that
  sets `group_engine=replaynet` + enables the node. Default build is unchanged (snapcast).

Runtime switch on a running unit: `uci set beep.main.group_engine=replaynet;
uci commit beep; /usr/libexec/beep/beep-group apply`.

**Still to validate on hardware** (needs a build + flash): the full shairport-fed path
end-to-end (phone → shairport → FIFO → replaynet → synced playout), the shairport hooks
firing become-source/leave, and a soak run. Also open: real mDNS discovery (avahi
`_beep._tcp`, libs already on the image), `beep-source`/`rpcd` status readouts still name
snapcast, and FLAC/opus for weak links.

## Bring-up / validation checklist (when we build + flash)

Fastest first pass is the **runtime switch on a live unit** (no reflash): deploy the
`replaynet` binary, flip the engine, and drive it — this is what the hardware iterations
used.

```
# on the Beep (per unit):
uci set beep.main.group_engine=replaynet
uci set replaynet.node.enabled=1
uci set replaynet.node.group=1                 # shared group so units auto-form one
uci commit
/usr/libexec/beep/beep-group apply             # stops snapcast, points shairport at the
                                               # /tmp/beep-pcm pipe + hooks, starts the node
beep-source status                             # -> multiroom engine=replaynet node=up
replaynet --ctl status                         # -> id/sink/source
```

Then validate, in order:
1. **Node up + fed:** `pidof replaynet`; `ls -l /tmp/beep-pcm` (a fifo); shairport conf
   shows `output_backend = "pipe"` and a `sessioncontrol` block.
2. **AirPlay plays (single unit):** stream from a phone → audio out the WM8524; the
   shairport hook fires `--ctl become-source` (check `logread | grep replaynet`, role → SOURCE).
3. **Two units in sync:** double-tap the 2nd Beep → `beep-group toggle` → it joins; both
   play the same audio. Confirm by ear (tight, no flam) and in logs (`playout: ... sched
   err` small on both). Stop → `--ctl leave`.
4. **Volume parity:** the knob / web slider / LED arc still move the shared Master (replaynet
   plays through `default` → softvol). Note: in pipe mode the phone's AirPlay volume no
   longer pre-attenuates (same as the old snapcast-primary path).
5. **Full image build:** `MULTIROOM=replaynet ./scripts/build.sh`, flash, confirm the
   uci-default made replaynet the engine at first boot and the above still holds.
6. **Soak:** run hours; watch for XRUN storms, fd/mem leaks, reconnect after a peer drop,
   and behaviour when wifi degrades (the fan-out should skip a weak sink, not stall others).

Known deltas to expect: drop/insert correction can click on a sharp drift step (cubic
resampler is the fix); a weak-wifi sink (< ~−70 dBm) can't carry ~1.4 Mbps raw PCM and will
glitch (FLAC/opus is the fix); `beep-source snapcast on/off` still uses the "snapcast" verb
though it drives replaynet under the engine switch.
