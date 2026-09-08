# Beep Revival — Development Notes & Findings

A running catalog of attempts, dead ends, root causes, and decisions. Written so
future work (and future me) doesn't re-walk the same rabbit holes. Newest topics
first within each section. Dates are absolute.

---

## 1. Audio path: why AirPlay 2 doesn't work (yet) and what we're doing about it

### 1.1 The measured wall (2026-09-06)
AirPlay-2 **buffered** audio (Apple Music etc.) ships **AAC-LC 44.1 kHz stereo ~256 kbps**
and requires the receiver to decode AAC in real time. On unit #1 (AR9331, MIPS 24Kc,
400 MHz, **no FPU**, 64 MB RAM) this **pegs the core at ~92 % CPU / 0 % idle and the
PCM immediately XRUNs** — playback starts, then dies within seconds. Grouped playback
drags the *other* speaker down too, because AirPlay-2 stalls the whole group waiting
for the laggard.

Instrumented proof (1 Hz sampler during a real session):
```
00–43s  pcm=closed idle~80%   handshake/buffering
44s     cpu 73%    idle=0%    real playback starts -> core saturates instantly
48s     pcm=XRUN              buffer underrun
51s+    pcm=closed idle=0%    never recovers; MemAvailable 9.4MB -> 3.7MB
```
Setting `interpolation = "basic"` (kills soxr drift-correction) changed nothing → the
cost is the **AAC decode itself**, not resampling.

### 1.2 Root cause: soft-float, not bitrate
The 24Kc has **no hardware FPU**. ffmpeg's default AAC decoder is **floating-point**
(the IMDCT/windowing is wall-to-wall float multiply-adds). On soft-float, every float
op is a libgcc emulation call (~26–53 cycles vs a few with an FPU → **10–30× penalty**).
256 kbps is trivial bandwidth; the killer is float math on a chip that emulates it.

### 1.3 The lever: fixed-point AAC (`aac_fixed`)
- Fixed-point AAC-LC decode needs only **~15–40 MHz-equivalent** of an integer core
  (Rockbox does AAC-LC realtime on a **75 MHz FPU-less ARM7**; Helix ~47–130 MHz on
  tiny Cortex-M). We have 400 MHz → large headroom.
- ffmpeg's **`aac_fixed`** decoder was written in 2015 by Imagination's MIPS team
  *specifically for FPU-less MIPS*. It uses an integer FFT + integer SoftFloat and
  never touches the emulated FPU. Outputs **S32P** (32-bit int planar), not float.

### 1.4 Benchmark (2026-09-07) — decisive
qemu-mips (big-endian, soft-float) on the CT, 15 s AAC-LC clip, our built mips ffmpeg:
```
FLOAT   (aac):        utime = 3.115 s
FIXED   (aac_fixed):  utime = 0.293 s   -> ~10.6x cheaper
```
qemu `utime` is host-emulation time (not real cycles), so trust the **ratio**, not the
absolute. Combined with §1.1 (float pegs the real chip and XRUNs), fixed-point at
~1/10.6 the cost lands **comfortably under realtime with several-× headroom.** Three
lines of evidence agree (benchmark ratio, prior on-device float data, research floor).

⚠️ The real cliff is **HE-AAC / SBR** (3–4× heavier, defeated Rockbox on 75 MHz iPods).
AirPlay-2 buffered audio is plain **AAC-LC**, so we're on the safe side — but listen-test
to be sure the sender never negotiates HE-AAC.

### 1.5 The shairport patch (in progress)
The "can not run on this system" gate is **cosmetic**, not architectural — shairport's
DSP pipeline is already integer; the AAC decoder's float output is immediately resampled
to integer S32. The `fltp` requirement is a decoder-*selection* artifact.

**Version caveat:** the research read shairport-sync **master (v5.5)**, which moved the
AAC/ffmpeg decode into `player.c`. Our OpenWrt 24.10 feed ships **v4.3.2**, where the AAC
decode is **NOT in player.c** — it lives elsewhere (TBD: grep the tree). So master's
line refs do NOT map; find the real edit points in 4.3.2.

Confirmed in **v4.3.2 `shairport.c`**:
- `has_fltp_capable_aac_decoder()` def at **lines 120–137** (`#ifdef CONFIG_AIRPLAY_2`).
- Refuse-to-run calls at **161–168** (usage) and **3635–3638** (main, `die()`).

Planned 3-edit patch (adapt line numbers to 4.3.2):
1. Relax/remove the `has_fltp` gate (or accept S32P).
2. Select the fixed decoder: `avcodec_find_decoder_by_name("aac_fixed")` instead of the
   default `avcodec_find_decoder(AV_CODEC_ID_AAC)`.
3. Set the AAC resampler **input_format from `codec_context->sample_fmt`** (mirror the
   ALAC path) instead of hardcoding `AV_SAMPLE_FMT_FLTP`. The resampler already converts
   to S32 for free — no extra shim needed.
Plus: ensure ffmpeg is built `--enable-decoder=aac_fixed` (audio-dec preset omits it).

### 1.6 Prior art / minimum hardware
- Official shairport floor for AP2: **Pi 2 / Pi Zero 2 W** class (~1 GHz, hardware FP,
  NEON). Even a 1 GHz ARMv6 *with* an FPU (original Pi Zero) is borderline.
- **No documented AP2-buffered success on FPU-less MIPS anywhere** — but that's because
  everyone used the **float** decoder. Nobody has tried `aac_fixed` + shairport (zero PRs
  /issues). This combination is net-new.
- Classic **AirPlay 1** (ALAC, integer, no AAC) ran on AR9331 OpenWrt historically — the
  proven path, and what we ship by default.

---

## 2. Multi-room architecture (Snapcast)

**Requirement:** two Beeps must play in sample-accurate sync. **AirPlay-2 grouping is
out** (needs buffered AAC on every speaker → §1.1 wall). **AirPlay-1 has no grouping.**
→ **Snapcast** is the sync engine.

**Model A (chosen, self-contained):** the AirPlay-receiving Beep is the group **primary**
— runs a light **classic AirPlay-1 shairport** → `/tmp/snapfifo` → **snapserver** →
distributed to every **snapclient** (incl. its own). Members run snapclient only. All
play to the shared server timeline. Roles: `solo | primary | member` (uci
`beep.main.group_role`), applied by `rootfs-overlay/usr/libexec/beep/beep-group`, exposed
via rpcd `set_group` + the web UI "Multi-room" card. PCM codec on the wire (no FLAC encode
on the weak primary). AirPlay-1 ingest is mandatory — an AP2 ingest would hit the §1.1
decode wall on the primary.

**Build switch:** `scripts/build.sh` default = classic AirPlay-1 + Snapcast (lean, no
ffmpeg). `AIRPLAY2=1` = the full AirPlay-2 build (ffmpeg + nqptp + BE crypto patch),
preserved for the fixed-point experiment. The two are mutually exclusive images.

### Snapcast port (task done, 2026-09-07)
`feed/snapcast/` — adapted from the archived badaix/snapos Makefile, **v0.27.0**. All deps
present in the 24.10 feeds (libsoxr, libvorbisidec, libatomic, boost, flac, opus, avahi).
Builds `snapserver` (756 KB) + `snapclient` (242 KB) for mips_24kc BE.
- **Fix required:** snapcast 0.27 predates GCC-13 header hygiene — `sample_format.hpp`
  uses `uint32_t`/`uint16_t` without `#include <cstdint>`. Force-included via
  `TARGET_CXXFLAGS += -include cstdint` in the Makefile.
- `-latomic` for 64-bit `std::atomic` on mips32.
- All-Beep group is all big-endian → snapcast's LE wire protocol is consistent; no
  endian-mismatch risk within the group.

---

## 3. Build environment — the saga, root causes, and the rules that fix it

This section exists because a **single mistake (running two builds in one tree)** snowballed
into ~hours of failures. Every failure below was self-inflicted and is now prevented.

### 3.1 The rules (follow these)
1. **Never run two builds against one OpenWrt tree.** The first collision corrupted the
   cross-toolchain and started everything. One build at a time, period.
2. **Build the toolchain at low `-j`.** On many-core hosts, gcc's recursive make **races**
   at high `-j` and fails non-deterministically (`gcc/initial failed`, bare `world Error 1`).
   `build.sh` now caps at `-j6` (override `JOBS=`); build the toolchain itself at `-j1`.
3. **Don't hop container base images against a shared tree.** Host tools (cmake, …) built
   under one glibc won't run under another → `cmake: GLIBC_2.36 not found`. If you must,
   rebuild `tools/`.
4. **Never `make tools/clean` casually.** It wipes the whole host toolset and rebuilding it
   surfaces cascading breakage (`tar` refuses root; `ninja` needs a wiped host `python3`).
5. **Required host deps:** `bc` (kernel `timeconst.h`, else `Error 127`), and export
   **`FORCE_UNSAFE_CONFIGURE=1`** (GNU tar refuses to `./configure` as root). Both are now
   in `docker-build-setup.sh` / `build.sh`.
6. **Host: Ubuntu 22.04** (glibc 2.35, gcc 11) builds OpenWrt 24.10 cleanly. Debian
   bookworm / Ubuntu 24.04 (newer glibc) fail building gcc-13.3.0's libiberty
   (`fibheap.c: LONG_MIN`). This is host-header-version sensitivity, not our code.
7. macOS Docker Desktop's VM filesystem also contributes non-determinism under heavy
   parallel build I/O — another reason to prefer a **native-Linux** build host.

### 3.2 Result
A clean multi-room image builds at ~**10.3 MB** sysupgrade
(`openwrt-ath79-generic-8dev_carambola2-squashfs-sysupgrade.bin`) — well within the flash
budget. The only image-level failure was snapcast's one-line `<cstdint>` (§2), now fixed.
Build on any native-Linux host (Ubuntu 22.04 builds 24.10 cleanly, per rule 6); `build.sh`
honors `OW=` / `SRC=` / `JOBS=` overrides.

---

## 4. Open items
- [ ] Flash multi-room image to unit #1; verify classic AirPlay-1 (low CPU) + 2-Beep sync.
- [ ] Locate the AAC decode file in shairport-sync **4.3.2** (not player.c); write the
      `aac_fixed` 3-edit patch (§1.5); build `AIRPLAY2=1` image; real AirPlay-2 test.
- [ ] Recovery slot (initramfs into freed flash; `beep_recovery` slot).
- [ ] Consider upstreaming the big-endian pair_ap patch and the `aac_fixed` decode option.
