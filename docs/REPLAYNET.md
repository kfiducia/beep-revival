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
| **(b)** | timestamp / clock sync + drift correction | done (this PR) |
| (c) | ALSA sink (the stock `beepi2s` role, WM8524) | next |
| (d) | `assign_groups()` consensus + mDNS + double-tap / shairport play hooks | |

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

## Usage

```
replaynet --source --peer <ip:port> [--pcm <file|->]
          [--fake-clock-offset-ns N] [--fake-clock-rate-ppm P]   # test injectors
replaynet --sink   --listen <port>  [--out <file|->]
replaynet --selftest
```
PCM is raw interleaved S16, 44100 Hz, 2 ch (4 B/frame). Step (a/b) sink writes received
PCM straight through (file/stdout/fifo); the ALSA/WM8524 device is step (c). The
`--fake-clock-*` flags make a source lie about its clock (constant offset / rate ppm) so
the sink's estimator and drift controller can be validated end-to-end over the socket.

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

Both pass on the host and under qemu-mips (MIPS32 big-endian).

Package: `feed/replaynet/` (single C file, mirrors `feed/beepd`). Selected in the image
via `CONFIG_PACKAGE_replaynet=y` (built for CI coverage; not auto-started yet).
