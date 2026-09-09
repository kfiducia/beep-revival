# Boot-progress on the light ring (spike / TRL 3)

**Goal:** make "it's booting and it's okay" *visible*, and — more importantly — make a
**stall visible**. Today a hang anywhere before `beepd` starts (init `START=95`) looks
identical to a healthy boot: the ring just holds the STM8's own power-on "smiley". The
2026-09-08 brick hung at first-boot config-restore and gave **no** on-device signal.
This turns the ring into a coarse progress bar so a stuck stage is obvious.

> **Status: unvalidated on hardware.** The protocol and stage wiring are correct on
> paper (derived from `feed/beepd/src/beepd.c`), but nothing here has been run on a
> unit. See the validation checklist at the bottom before relying on it.

## How the ring is driven

The ring is 24 single-color LEDs on the **STM8L151** MCU (i2c addr `0x23`, bit-banged
`i2c-gpio` bus = `/dev/i2c-0`). A frame is one 25-byte block write to reg `0x80`:
`[led_1 .. led_24, ack]`, linear brightness (no gamma), with a physical→wire rotation
of `+11` (`wire[i] = phys[(i+11) % 24]`). `beepd` owns the bus once it runs; before
that the bus is free for anyone who can open `/dev/i2c-0`.

`usr/libexec/beep/led-stage <stage>` writes exactly that frame — a **safe no-op** if
`/dev/i2c-0` or `i2ctransfer` isn't there yet, so it can be called at the earliest
stages without ever blocking or breaking boot.

## Stage map

| Stage call | Ring | Meaning | Reaches ring? |
|---|---|---|---|
| `early`    | 4/24 dim | kernel + modules alive | only if i2c loads in preinit (see below) |
| `config`   | 8/24 dim | rootfs mounted / config restore | only if i2c loads in preinit |
| `network`  | 16/24    | network up | ✅ (post-init) |
| `ready`    | 24/24    | services healthy (brief) | ✅ then beepd takes over |
| `upgrade`  | 24/24 bright | **flashing — do not power off** | ✅ (services down, bus free) |
| `failsafe` | alternating | failsafe boot | ✅ |

Wired to OpenWrt's `set_state()` via `etc/diag.sh` (which we override because this
board has no diag gpio-LED to drive).

## Time-paced fill (the animation)

Rather than jump between coarse milestones, boot and flash drive a **time-paced fill**:
the ring eases from empty to full over roughly the *duration* of the operation, so the
dots reach the top about when it finishes. `usr/libexec/beep/led-boot-anim <secs> [v]
[keep]` runs a background loop that maps monotonic `/proc/uptime` elapsed → `led-stage
fill <n> <v>` (`n = round(24 · elapsed / secs)`), holding full on overshoot. It's
deliberately **time-based, not real progress** — overshoot/spill-over is acceptable;
being a little short just means the ring sits full for the tail.

`set_state()` wiring:
- `preinit → early` — a brief static arc while the i2c modules load.
- `preinit_regular → led-boot-anim BOOT_ANIM_SECS` — start the boot fill (detached so it
  survives preinit's `exec` of procd; yields the instant `beepd`/`pidof beepd` appears at
  init `START=95`, the "boot done" hand-off).
- `upgrade → led-boot-anim OTA_ANIM_SECS … keep` — fill over the flash (`keep` ignores the
  `beepd` check, since services are being torn down).
- `failsafe → led-stage failsafe` (+ stop the fill); `done →` stop the fill (beepd owns it).

**Calibration:** `BOOT_ANIM_SECS` / `OTA_ANIM_SECS` in `etc/diag.sh` default to `120` /
`60` (≈ observed boot / flash time on this unit). Tune to the real durations — measure
boot with `cut -d. -f1 /proc/uptime` at the moment beepd starts.

Finer app milestones can still be layered in by calling `led-stage <stage>`/`fill`
directly from init scripts — left as a follow-up so this spike stays small.

## Reachability — the important caveat

`upgrade`, `failsafe`, and any post-init milestone reach the ring reliably (i2c modules
are loaded by then). The **earliest** states (`early`/`config`) — the exact
config-restore window where the brick happened — only light up if `i2c-gpio` + `i2c-dev`
are loaded during **preinit**, not at their normal init stage. `etc/modules-boot.d/
09-beep-i2c` does that, but it is the **riskiest, least-tested** part: driving a
bit-banged bus that early must not destabilize boot. If it does, delete that one file —
`upgrade`/post-init indication still works.

What we can *never* light without touching U-Boot (which we never reflash): the
pre-kernel window (SoC ROM → U-Boot → kernel decompress). The STM8 smiley covers that.

## Hardware validation checklist (before graduating past TRL 3)

1. `i2ctransfer -y 0 w26@0x23 0x80 <24×0x28> 0x00` lights a static dim ring — confirms
   the frame/rotation match beepd's output (ring fills from the expected spot).
2. Reboot: confirm the `upgrade` frame appears during a web-OTA flash and the ring shows
   "flashing" until the device reboots.
3. Confirm `led-stage` and beepd never fight the bus (ring doesn't flicker/garble right
   as beepd starts at `done`).
4. With `09-beep-i2c` in place, confirm early boot is not slowed/destabilized and that
   `early`/`config` actually paint during preinit. If not clean → drop that file.
5. Confirm the fill **paces the boot**: the ring eases up over the boot and is roughly
   full about when `beepd` takes over — no big jump/flicker at hand-off. Tune
   `BOOT_ANIM_SECS` to the measured boot time if the ring finishes far early/late.
6. Same for a web-OTA flash: the ring fills over the flash and is ~full by reboot
   (`OTA_ANIM_SECS`). Spill-over (sits full for the tail) is acceptable.
