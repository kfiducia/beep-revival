# Big-Endian AirPlay-2 Audit (shairport-sync 4.3.2 + nqptp) on AR9331 / mips_24kc BE

**Target:** Atheros AR9331 / Carambola2, **big-endian** MIPS32r2, OpenWrt 24.10, musl.
**Scope:** endian bugs in the AP2 SETUP / encrypted-RTSP framing / PTP-timing paths that would
explain the observed "Mac aborts shortly after SETUP" symptom.
**Already fixed (do not re-touch):** transport-cipher framing in `pair_ap/pair_homekit.c`
(`scripts/patches/010-airplay2-bigendian-pairing.patch`).

---

## TL;DR / headline conclusion (read this first)

**Context.** The prime hypothesis going in was that the chacha20-poly1305 RTSP transport cipher
frames each block with a 2-byte **little-endian** length that doubles as the poly1305 AAD, and that a
BE host would mangle it. That is a real hazard — but it is the exact thing patch 010 already fixes,
in **both** directions (encrypt and decrypt), including the LE block length, the LE AAD, and the
8-byte LE nonce counter.

**Plain English — why I'm confident the cipher is no longer the problem.** The device's own log,
as quoted in the symptom, contains **two different SETUP messages**, and they come from **two
different code branches** that can only both run if the cipher works in *both* directions on this
BE box:

- `"AP2 PTP connection from <Mac> to self"` is `rtsp.c:2902`, inside the **`streams == NULL`** branch
  (the first, "PTP-connection" SETUP).
- `"SETUP AP2 ... the SETUP Record"` (`rtsp.c:3258`) and `"doesn't include DACP-ID"`
  (`rtsp.c:3277`) are inside the **`streams != NULL`** branch (`rtsp.c:3211+`) — i.e. the **second,
  audio-stream** SETUP.

For the Mac to send the *second* SETUP, it had to **decrypt the device's response to the first
SETUP**. That response is produced by `write_encrypted() -> pair_encrypt()` (`rtsp.c:1145`,
`1149`) — the patched encrypt path. So on this BE device, post-patch:

1. **decrypt works** (both incoming SETUPs were parsed and logged), and
2. **encrypt works** (the Mac accepted the first SETUP response and proceeded).

**So the transport-cipher framing is NOT the remaining cause.** The stall is happening *after*
SETUP #2, not "after the first SETUP." That reframes where to look: the data-port hand-off,
`SETRATEANCHORTIME`, `RECORD`, and PTP — see findings below.

**What I actually found in the still-broken paths:** exactly one genuine big-endian correctness
bug — `ntoh64()` in nqptp (Finding 1). It is real and worth patching, but it is **downstream** of
the RTSP abort and is most likely **inert on your flat single-switch subnet** (correction field is
normally zero there), so I rank it as a real bug but a **low-probability root cause** for *this*
symptom. Every other endian-sensitive site in the exercised paths is either handled by `libplist`
or by correct `ntohl/ntohs/nctoh*` helpers (Findings 2–4). I did not find a second smoking gun in
the source; I say so plainly rather than invent one, and I list the cheap on-device probes that
will actually localize the abort.

---

## Finding 1 — `ntoh64()` double-swaps the PTP correction field on big-endian (nqptp)

**Ranking:** most significant *confirmed* endian defect in the code, but see the confidence note —
it is unlikely to be the specific cause of *your* abort.

### Context / background
`nqptp` parses PTP Sync and Follow_Up messages from the grandmaster (the Mac). Each PTP common
header carries a 64-bit **`correctionField`** (IEEE-1588 §13.3), a signed nanoseconds-×2⁻¹⁶
value that must be added to the `preciseOriginTimestamp` to get true master time
(`nqptp-message-handlers.c:282` for Sync, `:360` for Follow_Up). PTP is **big-endian on the wire**.

Unlike the timestamp fields — which are read with `nctohl()/nctohs()` (byte-pointer reads that call
`ntohl/ntohs`, correct on any host) — the correction field is read as a whole `uint64_t` straight
out of the packed struct that overlays the receive buffer, then passed through a *hand-rolled*
`ntoh64()` in `general-utilities.c:60`.

### Plain English — what goes wrong, and the "so what"
`ntoh64()` was written assuming a **little-endian host**. I extracted the exact function and ran it
on a known value:

```
in-memory field  = 0x0807060504030201   (LE host reading wire bytes 01 02 .. 08)
ntoh64 output    = 0x0102030405060708    <-- correct on little-endian
BE case: ntoh64(0x0102030405060708) = 0x0807060504030201   <-- should have been identity
```

On a **big-endian** host the whole-`uint64_t` read of the wire bytes is *already* the correct
value, so a correct `ntoh64` must be the **identity**. Instead the function swaps the two 32-bit
halves (each half's inner `ntohl` is a no-op on BE, but the code then re-assembles them
`(low<<32)|high`). Result: the correction field is corrupted (halves transposed) on BE.

**So what:** when the correction field is non-zero, `correctedPreciseOriginTimestamp =
preciseOriginTimestamp + correction_field` (`nqptp-message-handlers.c:366`) is wrong by a large,
garbage amount, so the computed clock offset is nonsense and PTP never converges — nqptp would show
no usable master. **But** on a flat single-switch IPv4 subnet with an ordinary (non-transparent-clock)
switch, the grandmaster's Sync/Follow_Up correction field is essentially always **0**, and
`ntoh64(0) == 0` regardless of endianness — so the bug is inert there. It also sits **downstream**
of the RTSP handshake: your Mac aborts at RTSP before PTP would matter.

### Confidence
- That this is a **real BE bug**: **high** (proven by direct execution of the function's own code).
- That it is the **cause of your abort**: **low** — it's downstream of the stall and normally inert
  on your network. Patch it anyway; it removes a latent correctness bug and one variable from the
  investigation.

**What would confirm/refute causal impact:** add `debug(1,...)` (or watch the existing commented
debug at `nqptp-message-handlers.c:362`) to print `msg->header.correctionField` and the `ntoh64`
output for real traffic. If the raw field is always `0`, this bug is confirmed irrelevant to the
symptom (refuted as cause) though still worth fixing.

### Technical detail + proposed fix
File: `nqptp/general-utilities.c:60-71`. The safe, endian-agnostic rewrite reads the 8 bytes
big-endian by construction (same idiom as the already-correct `nctoh64`), so it is correct on both
LE and BE and needs no `#if __BYTE_ORDER` guard:

```diff
--- a/general-utilities.c
+++ b/general-utilities.c
@@
-uint64_t ntoh64(const uint64_t n) {
-  uint64_t fiddle = n;
-  uint32_t fiddle_hi = fiddle & 0xFFFFFFFF;
-  fiddle_hi = ntohl(fiddle_hi);
-  fiddle = fiddle >> 32;
-  uint32_t fiddle_lo = fiddle & 0xFFFFFFFF;
-  fiddle_lo = ntohl(fiddle_lo);
-  fiddle = fiddle_hi;
-  fiddle = fiddle << 32;
-  fiddle = fiddle | fiddle_lo;
-  return fiddle;
-}
+uint64_t ntoh64(const uint64_t n) {
+  /* beep-revival: endian-agnostic. The argument holds the 8 wire bytes as read
+     from a packed struct (i.e. host-order interpretation of big-endian wire data).
+     Reconstruct the value from its bytes in network (big-endian) order so this is
+     correct on both little- and big-endian hosts. The previous implementation was
+     hard-coded for little-endian and half-swapped the value on big-endian. */
+  uint8_t b[8];
+  memcpy(b, &n, sizeof(b));
+  uint64_t v = 0;
+  for (int i = 0; i < 8; i++)
+    v = (v << 8) | b[i];
+  return v;
+}
```

**Nuance:** this is a bug **only if** the correction field is non-zero (transparent-clock switches,
boundary clocks, or a HomePod-style grandmaster reporting residence time). Also note `handle_sync`
(`:282`) computes `correction_field` and then discards it — only `handle_follow_up` (`:360`)
actually uses it, so the practical effect is confined to the follow-up path.

---

## Finding 2 — Transport-cipher RTSP framing (prime suspect #1): already fixed AND verified working on BE

### Context / background
After pairing, the RTSP channel is chacha20-poly1305 encrypted. AP2/HAP frames each block as
`[2-byte LE length][ciphertext][16-byte tag]`, with the 2-byte length also used verbatim as the
poly1305 **AAD**, and the nonce being 4 zero bytes followed by an **8-byte little-endian** message
counter. The read/write helpers are `read_encrypted()`/`write_encrypted()`
(`rtsp.c:1109`, `:1145`), which call `pair_decrypt()`/`pair_encrypt()` → the static
`decrypt()`/`encrypt()` in `pair_ap/pair_homekit.c:3000` / `:2944`.

### Plain English
In the **unpatched** 4.3.2 source these functions do `memcpy(&block_len, cipher_block, 2)`,
`memcpy(cipher_block, &block_len, 2)`, and `memcpy(nonce+4, &counter, 8)` in **host** byte order
(the literal `// TODO BE or LE?` comments are still in the tree at
`pair_homekit.c:2974, 2977, 3021, 3029`). On BE that corrupts the length, the AAD and the nonce, so
decryption fails and the sender aborts. **Patch 010 already replaces all of these with explicit
little-endian byte assembly, in both `encrypt()` and `decrypt()`.** As argued in the TL;DR, the
presence of *both* SETUP log lines proves the patched encrypt and decrypt both work on this BE
device. There is **no residual BE bug** in this path.

### Confidence
**High** that this path is correct post-patch and is not the remaining cause — supported by log
evidence, not just code reading.

### Technical detail
`rtsp.c` uses only the patched helpers for the RTSP control channel
(`timed_read_from_rtsp_connection` → `read_encrypted`, `rtsp.c:1225-1229`; response send →
`write_encrypted`, `rtsp.c:1560-1566`). No alternate/hand-rolled framing path exists in `rtsp.c`
for the control channel. Nothing to change.

---

## Finding 3 — SETUP response fields (prime suspect #3): no BE bug in source

### Context / background
The device writes event/timing/data/control ports and the audio buffer size back to the sender in
the SETUP responses (first SETUP: `rtsp.c:3110-3112`; audio SETUP: `rtsp.c:3309-3311`, `3379-3383`,
`3397`, `3432`).

### Plain English
Every one of these values is inserted with **`plist_new_uint(...)`** and the whole response is
serialized with **`plist_to_bin()`** (`rtsp.c:3446`). `libplist` stores integers in its own
canonical **big-endian** wire form independently of host endianness, so the ports are emitted
correctly on a BE host. There is **no** `memcpy`/`struct`/`htons` hand-packing of these 16/32-bit
values into the response buffer. Likewise the *incoming* values (`shk` session key via
`plist_get_data_val`, `type`/`rtpTime`/`networkTimeSecs` via `plist_get_uint_val`) are read through
`libplist`, which is endian-safe.

### Confidence
**High** that there is no BE bug here **in shairport's source**. Residual risk lives entirely in
whether **`libplist` itself was cross-compiled correctly for BE** — that's a library/build concern,
not a source defect, and it is easy to smoke-test (see "How to test cheaply").

---

## Finding 4 — Anchor math / RTP timestamps / audio wire reads (prime suspect #4): no BE bug in source

### Context / background
`SETRATEANCHORTIME` (`handle_setrateanchori`, `rtsp.c:1943`) and the audio/control receivers
(`rtp.c`) handle 32- and 64-bit network-time and RTP-timestamp values.

### Plain English
- `handle_setrateanchori` reads `networkTimeSecs`, `networkTimeFrac`, `rtpTime`,
  `networkTimeTimelineID`, `rate` via `plist_get_uint_val` and then does pure host-side arithmetic
  (`rtsp.c:1962-2003`). Endian-safe.
- The realtime audio receiver and AP1/AP2 control receivers read the wire with `ntohs()`, `ntohl()`
  and the byte-pointer helpers `nctohs()/nctohl()/nctoh64()` (`rtp.c:240, 269, 372-397, 541-544,
  2712, 2737-2742`, and `nctoh64`/`nctohl` in the AP2 control receiver at `rtp.c:1775-1786`). These
  are all correct on BE. In particular the buffered-audio TCP length prefix is read then
  `data_len = ntohs(data_len)` (`rtp.c:2699-2712`) — correct.

Note these paths run **only after** `RECORD`/streaming begins, i.e. **downstream** of the abort, so
even a bug here couldn't explain a pre-`SETRATEANCHORTIME` stall. None found regardless.

### Confidence
**High** (source is clean and, in any case, out of the pre-abort window).

---

## Finding 5 — nqptp control-port parsing & the other nqptp byte-order helpers: clean

- `handle_control_port_messages` (`nqptp-message-handlers.c:50`) parses the shairport→nqptp control
  strings (`"T <ip> ..."`, `"B"`, `"P"`, `"E"`) with `strsep` — purely textual, endian-neutral.
- `nctohl/nctohs/nctoh64` (`general-utilities.c:37-58`) do byte-pointer `memcpy` + `ntohl/ntohs`;
  correct on BE. `hcton64` (`general-utilities.c:24`, used for outgoing announcements at
  `nqptp.c:460,465`) uses `htonl` symmetrically; correct.
- The nqptp↔shairport **shared memory** (`shm_structure`) is written and read on the **same host**,
  so its multi-byte fields (`master_clock_id`, offsets) never cross an endianness boundary — no bug
  (`ptp-utilities.c:77-137`).
- `ntoh64` is the **only** hand-rolled swap that is wrong on BE (Finding 1). No `#if __BYTE_ORDER`
  blocks exist in nqptp to be "missing/wrong".

---

## Honest gaps — what I could NOT pin down from source alone
1. **The actual trigger for the abort after SETUP #2 is not a byte-order defect in the shairport /
   nqptp *source* that I can point at.** The exercised pre-abort paths are endian-clean beyond the
   already-applied patch 010.
2. Two BE-plausible causes remain that are **not visible in this source tree** and need an on-device
   check: (a) a **`libplist` built incorrectly for BE** (would corrupt the SETUP #2 response the Mac
   just received), and (b) a decode/`die()` or thread-startup failure in the audio path that closes
   the RTSP socket (shairport logging would show it). Both are testable cheaply below.

---

## How to test cheaply (smallest on-device checks, ranked)

1. **Confirm/deny a `libplist` BE problem without any rebuild.** On the device, feed a tiny known
   plist through the installed library and read it back:
   ```sh
   # if plistutil is present (libplist tools):
   printf '<?xml version="1.0"?><plist version="1.0"><dict><key>p</key><integer>55555</integer></dict></plist>' \
     | plistutil -f bin -o - | plistutil -f xml -o -
   ```
   If `55555` does **not** round-trip (or the binary form has byte-swapped integers), libplist is
   miscompiled for BE — that alone would make the Mac reject the SETUP #2 response. This is the
   single highest-value probe.

2. **See exactly how far the handshake gets** without a full rebuild: run shairport with verbose
   logging (`shairport-sync -vv`, or set `general = { ... }` verbosity) and watch for anything
   logged *after* `rtsp.c:3277`'s "doesn't include DACP-ID" — specifically whether you ever see
   `SETRATEANCHORI` (`rtsp.c:1944`) or a `die()`/decoder error from the buffered-audio thread
   (`rtp.c:2707, 2722`). The first line the Mac *doesn't* elicit tells you the failing exchange.

3. **Confirm Finding 1 is inert (or not) with one log line.** Re-enable the commented debug at
   `nqptp-message-handlers.c:362` (prints raw `correctionField` vs `ntoh64` output). If the raw
   field is always `0` on your subnet, Finding 1 is confirmed irrelevant to the symptom — still
   worth fixing, but not the cause.

4. **Cheapest way to prove the transport cipher is fine on BE** (should already pass, per the log
   evidence): the fact that `rtsp.c:3277` is reached at all *is* the test — reaching the
   `streams != NULL` branch required decrypting SETUP #2 and encrypting the SETUP #1 response.

### Suggested patch ordering
Apply Finding 1 as `030-nqptp-bigendian-ntoh64.patch` (self-contained, harmless, removes a latent
BE bug). Then use probes #1 and #2 to locate the real post-SETUP stall before writing any further
firmware patch — the evidence points away from cipher/framing and toward either a BE `libplist`
build or an audio-path failure, neither of which is a shairport/nqptp source endian bug.
