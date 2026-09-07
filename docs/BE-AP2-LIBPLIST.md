# libplist Big-Endian Audit — does bplist corrupt the AirPlay-2 SETUP #2 plist on AR9331?

**Target:** Atheros AR9331 / Carambola2, **big-endian** MIPS32r2 (`mips_24kc`), OpenWrt 24.10, musl.
**Library under test:** `libplist` **2.4.0** — the exact version OpenWrt 24.10's packages feed ships
(confirmed from `openwrt-24.10` `libs/libplist/Makefile`: `PKG_VERSION:=2.4.0`,
source `github.com/libimobiledevice/libplist/releases/download/2.4.0`). shairport-sync 4.3.2's
`DEPENDS` lists `+libplist`, and `rtsp.c` builds the SETUP responses with `plist_new_uint` /
`plist_dict_set_item` and serializes with `plist_to_bin`.
**Source read:** cloned at tag `2.4.0`; all line refs below are into `src/bplist.c` /
`configure.ac` of that tag.

---

## TL;DR / headline verdict (read this first)

**Context.** AirPlay-2 SETUP #2 hands the Mac a binary plist (bplist00) carrying the stream's
data/control/event ports and buffer sizes as integers. Apple's bplist format stores every
multi-byte integer **big-endian on the wire**. The leading theory was that libplist, cross-built for
a big-endian host, might *double-swap* those integers (an unconditional swap that assumes a
little-endian host), so the ports/offsets reach the Mac as little-endian garbage and it aborts.

**Plain-English verdict.** libplist's bplist code is **written to be endian-correct on big-endian**,
**but** its correctness rides on **one single preprocessor symbol, `__BIG_ENDIAN__`, which is not a
compiler built-in here — it is generated into `config.h` by `./configure`** (autoconf's
`AC_C_BIGENDIAN`). If that configure-time endian detection fired correctly for the MIPS-BE
cross-build (it almost always does, see below), **every integer, offset-table entry, and trailer
field is emitted big-endian and the SETUP #2 plist is NOT corrupted** — libplist is exonerated. If
that one detection *misfired*, the corruption would be **total** (not just the ports — the offset
table and trailer too), which matches the "Mac reads garbage and goes silent" symptom exactly.

So this is not a latent source bug you can point at; it is a **build-configuration single point of
failure** with a clean binary yes/no test. **My confidence that libplist is correct here is
medium-high**, and it becomes **high** the moment either of two cheap checks passes:

1. **On the build host:** `grep __BIG_ENDIAN__ config.h` in libplist's build dir — must be defined.
2. **On the device:** run `docs/probes/plist-be-probe.c` (below) and read the **byte order**, not the
   round-trip.

**Because I can't rule out the build-config misfire from source alone, weight also stays on the
audio-thread theory** (a `die()` in the buffered-audio processor after SETUP #2). See §5.

---

## 1. Background — how bplist stores integers, and where the endianness lives

`bplist00` is a fixed **big-endian** container: a `bplist00` magic, a body of objects, an
offset table, and a 32-byte trailer (`num_objects`, `root_object_index`, `offset_table_offset`, all
64-bit **big-endian**). Integers inside object nodes are also big-endian. None of this depends on the
host CPU — it is a wire format, so a correct library must convert host-order values to/from
big-endian on **every** host.

libplist does that conversion with a family of macros at the top of `src/bplist.c`. The load path
(`be16toh`/`be32toh`/`be64toh` via `UINT_TO_HOST`) and the store path (the same `be*toh` macros —
libplist reuses the "to host" macros symmetrically for "to big-endian" because a byte-swap is its own
inverse) both funnel through these:

```c
// src/bplist.c
#ifndef be64toh
#ifdef __BIG_ENDIAN__
#define be64toh(x) (x)          // line 139 — identity on BE  (CORRECT)
#else
#define be64toh(x) bswap64(x)   // line 141 — swap on LE
#endif
#endif
```

(Identical structure for `be16toh` at 121-127, `be32toh` at 129-135, and the odd-width `beNtoh` at
145-149.) The key fact: **the whole thing is gated on `#ifdef __BIG_ENDIAN__`.**

---

## 2. The crux — `__BIG_ENDIAN__` is NOT a compiler built-in; `./configure` defines it

**This is the finding that decides the theory.** `__BIG_ENDIAN__` (double-underscore-suffix form) is
an Apple/Darwin-style macro. **GNU GCC targeting `mips-openwrt-linux-musl` does *not* predefine it** —
GCC exposes `__MIPSEB__`, `_MIPSEB`, and `__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`, but **not** bare
`__BIG_ENDIAN__`. Nor does bplist.c pull it in from a system header: its includes are `<config.h>`,
`<stdlib.h>`, `<stdio.h>`, `<string.h>`, `<assert.h>`, `<ctype.h>`, `<inttypes.h>`, and the project's
own headers — **no `<endian.h>`, no `<arpa/inet.h>`, no `<sys/param.h>`** (grepped the full include
closure). So the `#ifndef be64toh` guards are always true (nothing else defines `be64toh`), and the
`#ifdef __BIG_ENDIAN__` inside them decides everything.

Where does `__BIG_ENDIAN__` come from, then? **From libplist's own `configure`:**

```m4
# configure.ac:57-59
# Checking endianness
AC_C_BIGENDIAN([AC_DEFINE([__BIG_ENDIAN__], [1], [big endian])],
               [AC_DEFINE([__LITTLE_ENDIAN__], [1], [little endian])])
```

and `configure.ac:9` is `AC_CONFIG_HEADERS([config.h])`, so automake compiles every TU with
`-DHAVE_CONFIG_H`, and `bplist.c:23-25` does `#ifdef HAVE_CONFIG_H / #include <config.h>`. **So on a
correctly-detected big-endian build, `config.h` contains `#define __BIG_ENDIAN__ 1`, and all the
`be*toh` macros collapse to identity — which is exactly right for a big-endian host.**

### Does `AC_C_BIGENDIAN` detect BE correctly when *cross-compiling*?

**Plain English:** yes, essentially always, with the autoconf libplist requires. `configure.ac:4` is
`AC_PREREQ(2.68)`. Autoconf ≥ 2.60 implements `AC_C_BIGENDIAN` with a method that works **without
running** a test binary: it compiles two short arrays containing the ASCII tell-tales
`BIGenDianSyS` / `LiTTleEnDian` laid out as 16-bit words, then **greps the resulting object file**
for whichever spelling survived — the byte order of the object reveals the target's endianness. Grep
needs only a *compiled* `.o`, so it is correct under cross-compilation. This is the same mechanism
every OpenWrt big-endian package relies on, and OpenWrt builds hundreds of them for `mips_24kc`.

**Nuance / how it could still misfire (the residual risk):**
- If a stale `ac_cv_c_bigendian=no` (or `...=unknown`) were injected via an autoconf *cache file* or
  a `CONFIG_SITE`, detection is bypassed and `__BIG_ENDIAN__` never gets defined. (No such override
  is present in the libplist tree; the risk is entirely in the surrounding build environment.)
- If the package were ever built with a **pre-2.60 autoconf** (not the case for a normal OpenWrt
  buildroot), the cross path would fall back to preprocessor macros and, finding no `__BIG_ENDIAN__`,
  guess wrong.
- If `HAVE_CONFIG_H` somehow weren't defined for this TU (it is, under automake), `config.h` wouldn't
  be included and the macro would be absent.

All three are **build-environment** faults, not defects in libplist's C. Each is caught by the
`grep config.h` check in §4.

---

## 3. What breaks if `__BIG_ENDIAN__` is missing — and why it matches the symptom

If detection misfired, `be64toh`/`be32toh`/`be16toh` become `bswap*` on a big-endian host, i.e. an
**unconditional swap on a host that needed none — a net byte-reversal of every multi-byte integer in
the plist.** This is not limited to the port values shairport cares about; tracing `src/bplist.c`,
the same macros serialize **all** of:

- **Integer node values** — `write_uint()` (`bplist.c:994-1003`, `val = be64toh(val)` at 999) and
  `write_int()` (`980-992`, `be64toh` at 989). `plist_new_uint(port)` lands here.
- **Array/dict child reference indices** — `write_array` (`be64toh` at 1138), `write_dict`
  (1157, 1163). Corrupting these desyncs the object graph.
- **The offset table** — `be64toh(offsets[i])` at `bplist.c:1412`. Corrupting these means the Mac
  can't locate objects at all.
- **The trailer** — `num_objects`, `root_object_index`, `offset_table_offset` all via `be64toh`
  (`1421-1423`). Corrupting `offset_table_offset` makes the whole plist unparseable.

**So what:** a mis-detected build doesn't just hand the Mac wrong port numbers — it hands it a
structurally invalid bplist whose trailer points into nonsense. The Mac's plist parser rejects it,
SETUP #2 is answered with garbage, and the sender **goes silent with no further RTSP** — precisely
the reported symptom (pairing + both SETUPs succeed because those are decided by the transport cipher
and shairport's own logging *before* the response body is re-parsed by the Mac; the abort lands when
the Mac tries to consume the SETUP #2 **reply** plist). Conversely, a correctly-detected build makes
all of the above identity ops and the reply is byte-perfect.

**Reads are affected symmetrically** — `parse_int_node` → `UINT_TO_HOST` → `be*toh`
(`bplist.c:263`, macro at `151-161`), and the trailer parse `be64toh(trailer->...)` at `831-833`.
But note the trap this sets for testing: **on the same device, a wrong build corrupts encode and
decode identically, so a local encode→decode round-trip still returns the original value.** That is
why the on-device probe in §6 keys off the **raw byte order**, not the round-trip.

**Reals/dates are a separate, independent path and are fine on BE.** `write_real`/`parse_real_node`
use `float_bswap32/64` (`bplist.c:171-180`), gated on `__FLOAT_WORD_ORDER__` — which GCC *does*
predefine correctly on MIPS (`== __ORDER_BIG_ENDIAN__`), so reals are identity/correct on BE
regardless of the `__BIG_ENDIAN__` question. Only the **integer/offset/ref/trailer** path (the one
that matters for SETUP #2 ports) rides on `__BIG_ENDIAN__`.

**Confidence:**
- That the integer path is endian-correct **iff `__BIG_ENDIAN__` is defined**: **high** (direct code
  trace above).
- That the OpenWrt cross-build actually defines it: **medium-high** (autoconf ≥2.68 grep-object
  method is reliable cross; residual risk is a stale cache/site override in the buildroot). Verify.

---

## 4. The fix — only needed if the build misfired (and then it's a build fix, not a source patch)

**There is no source bug to patch in libplist 2.4.0** — the C is correct given a correct `config.h`.
If §6's probe shows FAIL, do **not** hand-edit `bplist.c`; fix the detection so `config.h` gets
`__BIG_ENDIAN__`:

1. **First, confirm the fault on the build host.** In the libplist build directory under the OpenWrt
   buildroot (`.../build_dir/target-mips_24kc_musl*/libplist-2.4.0/`):
   ```sh
   grep -E '__BIG_ENDIAN__|__LITTLE_ENDIAN__|ac_cv_c_bigendian' config.h config.log
   ```
   Correct build shows `#define __BIG_ENDIAN__ 1` in `config.h` and
   `ac_cv_c_bigendian=yes` in `config.log`. If you instead see `__LITTLE_ENDIAN__`, or neither, or
   `ac_cv_c_bigendian=no/unknown`, detection misfired.

2. **Preferred fix — force the cache the honest way** (belt-and-braces; harmless if already correct).
   Add to the OpenWrt package Makefile's configure environment:
   ```make
   CONFIGURE_VARS += ac_cv_c_bigendian=yes
   ```
   This makes `AC_C_BIGENDIAN` take the "big endian" branch deterministically for this always-BE
   target, so `config.h` gets `__BIG_ENDIAN__` regardless of any grep-object hiccup.

3. **Belt-and-braces alternative** — inject the define directly:
   ```make
   TARGET_CFLAGS += -D__BIG_ENDIAN__=1
   ```
   Equivalent effect (the `#ifndef be64toh` guards still fire; `__BIG_ENDIAN__` now defined). Use
   only if you can't touch `CONFIGURE_VARS`.

**Nuance:** apply (2) only after the probe confirms a real failure. If detection is already correct,
`ac_cv_c_bigendian=yes` is a no-op and safe to leave in as a guard; `-D__BIG_ENDIAN__` is likewise a
no-op on a correct build but would be *wrong* if ever reused on an LE target, so prefer the
`CONFIGURE_VARS` form.

---

## 5. Sanity-check on the competing theory (audio-thread `die()` after SETUP #2)

Because libplist only comes up *conditionally* clean, the audio-path theory keeps real weight. Brief
scan of shairport 4.3.2 (`sps432/`), not a deep audit:

- The AirPlay-2 buffered stream is serviced by `rtp_buffered_audio_processor` (`rtp.c`). Right after
  it starts it **blocks** on `while (have_ptp_timing_information(conn) == 0) usleep(1000);`
  (`rtp.c:2396-2397`) — i.e. it waits for PTP to converge before touching audio. It then does
  `if (conn->input_bytes_per_frame == 0) die("conn->input_bytes_per_frame is zero!");`
  (`rtp.c:2403-2404`). `input_bytes_per_frame` is set from the codec type earlier in the same
  function (`rtp.c:2334/2342/2347/2354`).
- Other `die()`s in this path: `rtp.c:1474-1475` (`input_rate == 0`), `rtp.c:2246-2250` (condvar
  init). Any of these closes the connection.

**Plain English:** none of these is an *endian* bug on its face — they are guards that fire on a
zero/garbage parameter. But they are a plausible **second-order** consequence of the *same* libplist
question: the codec/format parameters that set `input_rate` / `input_bytes_per_frame` arrive in the
SETUP plists and are read with `plist_get_uint_val`. If those integers were byte-swapped by a
mis-built libplist, you could land on a garbage codec type → `input_bytes_per_frame == 0` → `die()`.
So the libplist check in §6 also de-risks this path. If libplist proves correct, then a `die()` here
(visible in `shairport-sync -vv`, per audit probe #2) points instead at a genuine format/PTP issue,
not endianness. **The symptom detail — the Mac stops sending *before* SETRATEANCHORTIME — slightly
favors the "Mac rejected the SETUP #2 reply" explanation over a device-side `die()` after the reply**,
which again puts the libplist byte-order check first in line.

---

## 6. Ready-to-run on-device probe

Full source: **`docs/probes/plist-be-probe.c`** (in this repo). It does
`plist_new_uint(0x0123456789ABCDEF)` → `plist_to_bin` → **hexdump** → checks the raw value-byte order
→ `plist_from_bin` → `plist_get_uint_val`, and prints a PASS/FAIL verdict plus the bytes so you can
eyeball them.

**Why the byte order and not the round-trip:** as shown in §3, a wrong build swaps encode and decode
identically, so `plist_get_uint_val` returns the original value **even when the wire bytes are
backwards**. The probe therefore decides on the serialized bytes:

- **ENDIAN-CORRECT (PASS, exit 0):** value bytes appear as `01 23 45 67 89 AB CD EF` (big-endian).
- **CORRUPTED (FAIL, exit 1):** value bytes appear reversed, `EF CD AB 89 67 45 23 01` — i.e.
  `__BIG_ENDIAN__` was not defined; rebuild per §4.

(`plist_new_uint(0x0123456789ABCDEF)` has `length == 16` because the value exceeds `INT_MAX`, so it
serializes via `write_uint()` as marker `0x14` + 8 zero bytes + the 8 value bytes — see
`plist.c:457-464` and `bplist.c:1360-1366`, `994-1003`.)

**Cross-compile against the OpenWrt 24.10 staging sysroot** (adjust the two globs to your tree; the
GCC version in the toolchain dir name varies):

```sh
OW=/build/openwrt
TOOLCHAIN=$(ls -d $OW/staging_dir/toolchain-mips_24kc_gcc-*_musl)
SYSROOT=$(ls -d $OW/staging_dir/target-mips_24kc_musl)
export PATH="$TOOLCHAIN/bin:$PATH"
export STAGING_DIR="$OW/staging_dir"
mips-openwrt-linux-musl-gcc -O2 -static \
    -I"$SYSROOT/usr/include" \
    docs/probes/plist-be-probe.c \
    -L"$SYSROOT/usr/lib" -lplist-2.0 \
    -o /tmp/plist-be-probe
```

`-static` links libplist's code into the probe, which is what we want to test. If you'd rather
exercise the *exact* shared object the firmware loads, drop `-static` and `scp` the matching
`libplist-2.0.so.*` next to the binary (or just run it on-device, where that `.so` is already
installed).

**Run on the AR9331 device:**

```sh
scp -O /tmp/plist-be-probe root@<device>:/tmp/
ssh root@<device> '/tmp/plist-be-probe; echo exit=$?'
```

Read the `plist_to_bin output` hexdump and the `==== VERDICT ====` line. **Exit 0 = libplist
exonerated** (shift all weight to §5's audio path); **exit 1 = libplist confirmed corrupting SETUP #2**
(apply §4 and rebuild).

---

## 7. Bottom line for the investigation

1. **libplist 2.4.0's bplist integer/real serialization contains no big-endian *source* bug.** The
   code is correct on BE **provided `config.h` defines `__BIG_ENDIAN__`**, and that define is produced
   by `AC_C_BIGENDIAN`, which is reliable under cross-compilation with the autoconf libplist requires.
   *(High confidence on the code; medium-high that the build got it right — verifiable.)*
2. **The theory is therefore reframed from "source bug" to "one build-config switch."** It is neither
   dismissed nor confirmed from source alone; it hinges on a single grep-able define, and its failure
   mode (total plist corruption → silent Mac abort) fits the symptom.
3. **Do this next, in order:** (a) `grep __BIG_ENDIAN__ config.h` on the build host — 10 seconds,
   likely settles it; (b) run `docs/probes/plist-be-probe.c` on the device and read the byte order.
   If both say correct, libplist is out, and the remaining suspect is the buffered-audio `die()`/PTP
   path (§5), best localized with `shairport-sync -vv` watching for output after `rtsp.c:3277`.
