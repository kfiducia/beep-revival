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
| **(a)** | 2-node PCM stream + on-wire framing | **this PR** |
| (b) | timestamp/clock sync + drift correction (off `track_samples`) | next |
| (c) | ALSA sink (the stock `beepi2s` role, WM8524) | |
| (d) | `assign_groups()` consensus + mDNS + double-tap / shairport play hooks | |

## Wire format (step a)

The stock engine sent a 3-message burst per tick (`audio_state` = 36-B header + 16-B
sync-state, then `sb_state`, then `sb_data`/PCM — see §4.1). We consolidate that into
one self-describing frame per tick, carrying the reversed 16-byte sync-state inline so
steps (b)+ can drive clock alignment straight off it:

```
struct rn_frame_hdr   // 32 bytes, ALL FIELDS BIG-ENDIAN (network order)
  u32 magic              "RPLY" (0x52504C59)
  u8  version            1
  u8  type               1 = audio
  u16 reserved
  u32 seq                monotonic frame counter (gap detection)
  u64 track_samples      playback position, in frames @ 44100 Hz   (stock: current_track_jiffies)
  u64 discarded_samples  cumulative frames dropped/padded to stay aligned (stock: sync_discarded_samples)
  u32 pcm_len            bytes of PCM following the header
// then pcm_len bytes of interleaved S16 PCM (44100 Hz, 2 ch, 4 B/frame)
```

Big-endian is not incidental — the stock protocol is network-order, proven three ways
in §4. Fields are serialized/parsed by hand (`be32_put`/`be64_get`), so a little-endian
build stays correct.

Transport is a single TCP connection (source → sink), `TCP_NODELAY`. Step (a) uses one
explicit peer; discovery/multi-sink fan-out is step (d).

## Usage

```
replaynet --source --peer <ip:port> [--pcm <file|->]   # reads raw S16 PCM, streams frames
replaynet --sink   --listen <port>  [--out <file|->]   # receives frames, writes PCM through
```
Step (a) sink writes received PCM straight through (to a file/stdout/fifo). Clock
alignment off `track_samples` is step (b); the ALSA/WM8524 sink is step (c).

## Testing

`scripts/replaynet/loopback-test.sh` runs a source+sink over loopback, checks the PCM
round-trips byte-for-byte, then captures the real on-wire bytes and decodes the header
to prove it is big-endian. Works on the host and under qemu-mips (the "2-instance qemu
capture" that live-confirms §4 against our engine):

```
scripts/replaynet/loopback-test.sh ./replaynet                 # host build
scripts/replaynet/loopback-test.sh "qemu-mips /tmp/rn/replaynet" # cross-built, under qemu
```

Package: `feed/replaynet/` (single C file, mirrors `feed/beepd`). Selected in the image
via `CONFIG_PACKAGE_replaynet=y` (built for CI coverage; not auto-started yet).
