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
board has no diag gpio-LED to drive): `preinit→early`, `preinit_regular→config`,
`upgrade→upgrade`, `failsafe→failsafe`, `done→` (silent hand-off to beepd).

Finer app milestones (e.g. `network`, `ready`) can be added by calling
`led-stage <stage>` directly from init scripts (e.g. `beep-netcheck`) — left as a
follow-up so this spike stays small.

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
5. Deliberately stall a boot (e.g. a bad config restore) and confirm the ring **holds a
   partial arc** instead of advancing — the whole point.
