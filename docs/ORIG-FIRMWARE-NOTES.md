# Notes from the original Beep firmware source

In 2026 the **original Beep firmware source** was published at
[`github.com/shawnlewis/beepmusic-orig`](https://github.com/shawnlewis/beepmusic-orig)
— a sanitized snapshot of the 2015 code at release **`v0.9.12r2`** (June 15, 2015).
This doc records what we learned by reading it, how it **confirms or corrects** work we'd
done by reverse-engineering the shipped binaries, and where our firmware can borrow from it.

It is a *study* of stock, not a plan to run stock code. Everything actionable is tracked in
GitHub issues (see [Improvements → issue map](#improvements--issue-map)); the umbrella is **#94**.

> **Provenance & licensing.** Beep-owned code is MIT; bundled third-party code keeps its own
> licenses. The snapshot has **no** private signing keys, **no** cloud backend, and Spotify/Pandora
> are stripped. It "has not been rebuilt or tested on hardware," and its two U-Boot snapshots are
> "not claimed to be the exact versions used for a shipped image" — so where the source and our
> **retail-binary** RE disagree, the binary wins for the shipped image (this matters once, for
> MIPS16e — see §4).

File paths below are relative to the `beepmusic-orig` tree unless prefixed with our repo path.

---

## TL;DR

- **UART-free recovery of a *bricked* stock unit: essentially no.** The only automatic, no-serial
  path is the **bootcount dual-image failsafe** (self-heals *if* the recovery slot survives). The
  bootloader has **no Wi-Fi** and the Beep has no wired Ethernet, so no bootloader network recovery
  is possible; the stock **update daemon can't recover** either (needs the vendor's private key and
  only patches files). See §1.
- **Our bootcount/env geometry, endianness handling, and setup-AP subnet choice are all confirmed
  correct by the source.** See §3, §5, §6.
- **Several playnet RE beliefs are corrected** — no stock resampler, wall-clock start-deadline sync,
  compressed-TCP transport, mDNS+`cluster_id` discovery. See §4.
- **The recessed "paperclip" button is the stock `MOD_SW_1` setup button on GPIO11 (active-low),
  which the vendor disabled as "broken on some production devices."** Keep the 30 s knob-hold as the
  guaranteed factory reset. See §7.

---

## 1. UART-free recovery (the headline question)

**Context.** The enclosure is sealed; after deploy there's no serial console. So the question that
matters is: if a unit running *stock* firmware is stuck, can it be rescued over the network or by a
button, with no UART? Three surfaces could in principle help — the bootloader, the update daemon,
and shell access. Two are dead ends.

**Plain English.** For a genuinely bricked unit, no. The one thing stock does automatically is fall
back to a second copy of the firmware baked into flash — and only if that copy is intact. There is
no web-recovery server in the shipped bootloader, and even if there were, the bootloader can't turn
on Wi-Fi, and the Beep has no Ethernet jack — so nothing on the network could reach it. The stock
"phone home and update" path can't be repurposed either, because it demands a package signed by a
key nobody outside the company has.

**Technical detail.**

- **Shipped bootloader = `u-boot-1.1.4`, board `BEEP_CM2`** (proven by an exact address match: recovery
  image `@0x9f050000`, primary `@0x9f550000`, env `@0x9f040000` — consistent across `common/beep.c`,
  `configs/beep_cm2.h`, and the OpenWrt mtd layout). The repo *also* ships `u-boot_mod` (the pepe2k
  build with the famous web updater), but that is **not** what shipped and its web failsafe is
  `#ifdef`-compiled-*out* for Beep anyway.
- **Dual-image bootcount failsafe** (`common/beep.c:156-248`): `MAX_BOOTCOUNT = 3`. If the primary
  firmware fails to complete 3 boots, U-Boot boots the recovery image at `0x9f050000`. If **both**
  slots are dead → `"Failed to boot either partitions"` → halt (UART only).
- **No Wi-Fi in the bootloader** — zero `ath9k`/`wmac`/`80211` references in either U-Boot tree; the
  AR9331 WMAC needs the full Linux driver. Since the Beep is Wi-Fi-only, **any** bootloader network
  recovery (httpd/TFTP) is physically unreachable. *Implication for our #8: the network-reflash half
  must live in a Linux recovery initramfs, never in U-Boot.*
- **GPIO button-forces-recovery is disabled on production** (`beep_cm2.h:13-17`, see §7).
- **USB-stick recovery** (`do_usb_boot` → flash `8dev_recovery.bin`) exists in the shipped U-Boot but
  is gated on `usbboot=force` in the factory env (unverifiable from source) or the disabled button,
  plus a physically exposed USB host port the Beep almost certainly lacks. Don't count on it.
- **Stock update daemon can't recover** — see §2.

**Verdict.** Automatic, UART-free self-heal exists *only while at least one on-flash image survives*.
A truly bricked unit (both slots bad — the classic bad-web-OTA outcome) is UART-only. The realistic
no-UART rescue for a still-booting unit is SSH + `mtd write` / bootcount fix — i.e. ordinary shell
access, not any hidden recovery channel.

---

## 2. Update / OTA mechanism and signing

**Context.** How did a stock Beep update itself, and could that path be reused to push a rescue image?

**Plain English.** The device was an HTTPS *client* that phoned the Beep cloud every 2 minutes and
*pulled* updates. There is no port to push an update *to*. And it will only install a package that's
been cryptographically signed by Beep's private key — which was never published — so no one can build
a package a stock unit accepts. On top of that, a normal package only patches files into the running
system; it flatly refuses to reflash the kernel/rootfs. So this path is closed for recovery.

**Technical detail.**

- **Package format** (`.bpk`) = a `[signature header][gzip'd tar]`. The tar's first member is
  `manifest.yml`; item types are `file`/`rm`/`sh`/`lua` (firmware types are commented out). Built by
  `tools/bpktool.py`; parsed on-device by `device/src/beepupdate/package.c` / `verify.c`.
- **Signing = RSA (OpenSSL), mandatory.** `open_package()` calls
  `verify_embedded_file(stream, FILE_TYPE_BINARY, /*require_secure=*/true, NULL)` **before parsing any
  tar byte** (`package.c:35`). `require_secure=true` means signature-only — a hash-only package is
  refused (`verify.c:1031-1063`). The signature carries a 4-byte key-ID looked up in a baked table;
  the real device build (`BEEP_DEVICE`) trusts exactly one key: **id `0x2BC6607D`, 4096-bit RSA**
  (`key.c`). Only the *public* half ships; the private key is absent and not derivable.
- **Delivery = cloud pull only.** A cron job (`*/2`) POSTs to
  `https://reverb.beepdevices.com:40937/1/update/config` then `/1/update/file` (`server.c`), HTTPS
  enforced. The server address is UCI-overridable (`beep_static.main.update_serv`/`update_port`) — the
  only LAN hook — but that's moot given the signature requirement.
- **Apply = file-patching, not flashing.** `package_process` runs pre-scripts → removes → extracts
  files onto the live rootfs → post-scripts, and **explicitly rejects "firmware" packages**
  (`package.c:910-932`). Bootcount is managed separately at boot, not around updates.

**Lesson for our `beep-ota`.** Our usign/ed25519 + admin-session + physical-tap design is simpler and
solid. Worth borrowing from stock: a **key-ID + small on-device keyring** so a compromised key can be
rotated/revoked via a normal update (unknown IDs are skipped, not fatal) — our single-pubkey model has
no rotation story today.

---

## 3. Bootcount / env geometry (confirmations)

**Context.** Our bootcount handling was reverse-engineered; the source lets us check it.

**Plain English.** We had it exactly right. The counter lives in the top half of the env sector, is a
wear-friendly bit-clear, and gets "consumed" once per healthy boot — which is precisely why writing it
with `fw_setenv` corrupts it.

**Technical detail (all confirmed).**

- `/dev/mtd1`: env `0x0–0x7FFF`, **BOOTCOUNT `0x8000–0xFFFF`**, 64 KB sector
  (`beepupdate.h:292-296`; U-Boot side `beep.c:9-13` at `0x9f048000`).
- Nibble bit-clear counter: `0xf`=0 boots → `0x7`→`0x3`→`0x1` (`beep.c:65-88`).
- Reset once per good boot by `beepupdate bc` from `init.d/bootcount` (START=17), walking a byte at a
  time and only erasing the sector at the end (`system.c:365-506`).
- `fw_env.config` mirrors the geometry (`/dev/mtd1 0x0 0x8000 0x10000` + redundant `/dev/mtd6`).

---

## 4. Playnet multi-room sync (RE corrections)

**Context.** replaynet was reverse-engineered from the shipped `playnet` **binary** (see
[`PLAYNET-RE.md`](./PLAYNET-RE.md)). The source both confirms and corrects that RE. Reconciliation is
tracked in **#92**; sync-accuracy follow-through is **#73**.

**Structural fact first:** "playnet" is three layers. `playnet.c` is only a *sink* (TCP :32299, ubus
`beep.playnet`); the *source/master* is a Lua **distributor** pushing compressed frames + control; and
steady-state sync is a **shared wall-clock start deadline**, not per-packet timestamps. The 16-byte
blob we reversed is the **join/resync** payload, not a steady-state header.

| RE belief | Verdict | Truth (file:line in `beepmusic-orig`) |
|---|---|---|
| 16-B BE `{track_jiffies, discarded_samples}`, hard-snap | **Confirmed, w/ correction** | Two `uint64_t`, raw `memcpy` (BE only because CPU is BE, not `htonl`); it's the join/resync blob. `audio.c:116-117,856-898` |
| "jiffies" = kernel jiffies | **Corrected** | Wall-clock **ms from `CLOCK_REALTIME`** (monotonic deliberately disabled); assumes **NTP-synced clocks**. `beeplib.h:95` |
| source=loopback vs remote=resample skew | **Corrected — no resampler exists** | Drop-samples / insert-silence only; all nodes are symmetric networked sinks on one deadline. Our source-room skew is **our** servo's artifact, not stock behavior. `decode_i2s_backend.c:170-273`, `distributor.lua:1234-1264` |
| multicast gossip discovery | **Corrected** | Standard **mDNS/DNS-SD** (`_beepcontrol._tcp` / `_beephttp._tcp`), membership by **`cluster_id` TXT** filter. `beepdiscovery.c` |
| UDP PCM transport | **Corrected** | **TCP unicast of *compressed* frames** (mad/tremor/fdk-aac/flac/pcm), port 32299; reliability from TCP; `SYNC_PT` seq handler is a no-op stub. `playnet.c:23,380`, `beep_stream_ch.c` |
| playnet is MIPS16e | **Keep open** | The *source tree* builds `-mips32r2` (`PKG_USE_MIPS16:=0`), but our **binary** RE found the shipped image is MIPS16e (ELF `0x74001005`). The retail image may have been built differently — **binary RE stays authoritative for the shipped image.** |

**Worth adopting:** stock's inter-room servo aligns every player to the **most-behind** one with a
**3 ms** threshold, skip-forward only (`distributor.lua:1234-1264`, 20-sample moving average) — a good
reference for tightening replaynet drift (#73). Note stock had **no** resampler, so our `--resample`
(#39) is our own addition on top of the proven drop/insert default.

---

## 5. Audio pipeline / I²S

**Context.** The real engine is `device/src/lib/audio/` (SqueezeOS-derived), **not** `athplay.c`
(that's a WAV test tool). Cross-referenced against `feed/beep-i2s` and `feed/beepd`.

**Confirmations.**

- **RT scheduling** — stock playout ran `SCHED_FIFO` **priority 45** (chosen to sit just below the
  PREEMPT_RT kernel's 50 for IRQ threads) + `mlockall(MCL_CURRENT|MCL_FUTURE)` + `mallopt`(trim/mmap
  off) + page pre-fault + `nice -20` (`decode_i2s_backend.c:506-548,623`). This is exactly our
  wifi-softirq-dropout fix — replaynet already does it. Whether shairport/squeezelite (currently a
  deliberate `nice -12`) should too is **#90**.
- **Endianness / `PCM_SWAP`** — byte order to the DAC is load-bearing; `PCM_SWAP` is the lever. Stock
  (big-endian) hard-enabled it *and* hand-flipped 16-bit samples; our little-endian ath79 driver
  correctly ties the swap to `S16_LE`. So "PCM_SWAP is the knob" = confirmed; "big-endian is
  universally correct" = platform-specific. Emit `S16_LE` from userspace and let the driver swap.
- **Fixed-point codecs, reclock-don't-resample** — libmad / Tremor (integer Vorbis) / FDK-AAC, and the
  DAC clock is reprogrammed per track rate rather than software-resampling on the FPU-less AR9331.
  Validates our fixed-point AAC choice.
- **Single volume gain stage** — one Q16 `audio_gain()` feeds the sink (`audio.c:1198-1211`), applied
  per-sample; the DAC's hardware volume register is unused on playback. Corroborates the "knob/web =
  sole volume authority" interim (PR #4, issue #7).

**Open item.** Our `feed/beep-i2s/src/beep-i2s.c:53-62` flags a TX-vs-RX playback DMA MBOX register
uncertainty, and our default `BEEP_PLAYBACK_TX=0` (RX regs) appears to contradict our own "stock used
TX" comment. The stock driver's `mode`→register mapping (`ath_i2s.c:437,1061-1071`) is now readable —
resolve + HW A/B in **#91**.

---

## 6. Controller, control API, Wi-Fi setup, discovery

**Context.** Stock split the input/LED job across three layers our `beepd` fuses into one: the **STM8L
MCU** (raw button/knob counts + LED PWM over I²C `0x23`), a **Lua `beepio` daemon** (gestures →
actions, all UCI-driven), and **`beephead`/`beep.wifisetup`/`beepdiscovery`** (network control,
onboarding, discovery).

- **Data-driven button vocabulary we dropped** (`/etc/config/io`): tap = play/pause, double-tap
  (≤340 ms) = skip, triple-tap = "magic", knob-turn = volume, 5 s hold = toggle Wi-Fi setup. There was
  **no** factory-reset gesture — our 30 s reset is a beep-revival addition. Restoring transport control
  is **#93**.
- **Connect-confirm-then-revert onboarding** — `beep.wifisetup connect` takes `return_on`/`confirm_time`
  and **reverts the UCI change if the join isn't confirmed** (`daemon.c:611-699`). Our `set_wifi` has no
  verified-join rollback, so a bad password can strand a sealed unit. This is **#89** (highest safety
  payoff).
- **Discovery = mDNS + `cluster_id`** (`beepdiscovery.c`) — the reference design for multi-room that
  doesn't depend on UDP broadcast crossing network segments.
- **`events` long-poll** (`beep.head→events(seq)`) — server-push state stream, cleaner than our
  poll-only `status` if a companion UI lands.
- **Our 192.168.60.1 setup-AP subnet is validated** — stock sat on **192.168.1.1/24**, the exact
  home-subnet collision we avoid.
- **I²C correctness bits to preserve** if the MCU read path is ever rewritten: the 25th PWM byte `0xAA`
  read-ACK handshake and the CRC8 on the knob-status frame prevent lost/double-counted events.

---

## 7. The recessed "paperclip" button

**Context.** We'd never located the pinhole button's pin; prior bench testing found it "reads zero on
every AR9331 GPIO and on the STM8 MCU" (issue #13).

**Plain English.** The source identifies it: it's the stock **`MOD_SW_1` "setup button on io board,"
on GPIO11, active-low.** But the vendor's own production bootloader has it **commented out**, with the
note that GPIO11 is *broken on some production devices*. That matches our "reads zero" result. So we
can't reliably move factory-reset onto it — we keep the 30 s knob-hold as the guaranteed path.

**Technical detail.**

- Bootloader: `include/beep.h:13` (`BEEP_GPIO_BTN_SETUP_ID`), `ap121-beep.c:99-117` (`button_read`,
  active-low). Holding it ~5 s at boot forces the recovery image (`common/beep.c:156-178`).
- Disabled on production (`configs/beep_cm2.h:13-17`):
  ```c
  // Beep3 MOD_SW_1 (setup button on io board).
  // GPIO11 is broken on some production devices.  Disabling the recovery button for now.
  //#define CONFIG_BEEP_GPIO_BTN_SETUP_BIT      11
  ```
- Stock Linux registered it as gpio-keys-polled, GPIO11 active-low, `KEY_WPS_BUTTON`
  (`patches-3.8/650-MIPS-ath79-carambola2.patch:52-100`), consumed by
  `hotplug.d/button/beep-buttons`.
- Our tree already declares it — `dts/ar9331_beep_dial.dts:87-95` (GPIO11 `ACTIVE_LOW`, polarity
  "TODO: confirm") — but **nothing consumes it**. Factory-reset/Wi-Fi-setup are read from the STM8L
  knob press over I²C (`feed/beepd/src/beepd.c`, `HOLD_RESET_MS 30000` / `HOLD_WIFI_MS 10000`).
- Possible pin conflict: stock `device/ath_i2s/atheros.h:100-110` lists GPIO11 in the I²S "stereo"
  block, though our verified-live driver muxes I²S onto GPIO18–23, leaving GPIO11 electrically free in
  our design.

**Recommendation.** Keep the 30 s knob-hold as the guaranteed factory reset. If we want the paperclip
button, treat GPIO11 as an **opt-in bonus, feature-detected per unit** at boot (reads a clean, stable
level, and stays clean while I²S runs?), wiring a consumer only where it passes — never removing the
fallback. A bench A/B on real hardware is the remaining step (#13).

---

## Improvements → issue map

| Area | Issue | Payoff |
|---|---|---|
| Wi-Fi onboarding confirm-then-revert | **#89** | Stops a bad password stranding a sealed unit |
| RT scheduling for AirPlay/LMS playout | **#90** | Fewer dropouts under wifi load (measure vs `nice -12`) |
| beep-i2s TX-vs-RX playback DMA register | **#91** | Resolve a latent correctness ambiguity |
| Reconcile replaynet vs stock source | **#92** | Correct docs; feeds sync-accuracy (#73) |
| Restore data-driven button gestures | **#93** | Real transport control from the knob |
| Umbrella / tracking | **#94** | Plan container |
| Paperclip button (GPIO11) | **#13** | Identified; keep knob-hold fallback |
| Recovery design constraints | **#8 / #23** | No bootloader Wi-Fi → recovery must be a Linux initramfs |
| Volume back-channel | **#7** | Single-gain design corroborated |

---

## Confidence & caveats

- Recovery (§1–2) and geometry (§3) are read directly from the compiled-path source — high confidence.
  The one genuine unknown is the runtime `usbboot` value in the factory-flashed env (not in the tree).
- Playnet (§4) is high-confidence *for this source tree*; the **MIPS16e** point is the one place the
  retail binary may differ — keep the binary RE authoritative for the shipped image.
- The paperclip button's physical form/population (§7) can't be confirmed from source; the GPIO11
  identification and the vendor's "broken on production" note are the solid parts.

_Basis: a five-part analysis of `github.com/shawnlewis/beepmusic-orig` @ `v0.9.12r2` — bootloader/recovery,
update mechanism, playnet sync, audio/I²S, and controller/API._
