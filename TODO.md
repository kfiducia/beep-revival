# Roadmap / TODO

Open work items after the `v1.0.0` release, in priority order. Items link to
their design notes where one exists. Contributions welcome — see an item you
want to pick up? Open an issue to claim it.

## Post-1.0 priorities (in order)

### 1. Un-brickable recovery slot
Give the device a second, always-bootable firmware slot so a bad flash or a
failed OTA can never leave it dead. Two parts:

- **Reverse the U-Boot `bootcount` / dual-boot logic**, then carve out a
  recovery partition (`beep_recovery`) that survives a `sysupgrade`.
- **Build a minimal recovery initramfs** — just enough to bring up Wi-Fi (or the
  setup AP) and serve the web reflash UI, so a user can always recover over the
  network with no UART/soldering.

Design: [`docs/RECOVERY-DESIGN.md`](docs/RECOVERY-DESIGN.md).

### 2. Lighter multi-room (Snapcast) config
The primary (server) role currently saturates the AR9331 during playback —
`shairport-sync → pipe → snapserver + local snapclient` on a 400 MHz single-core
no-FPU part is too much at once, so multi-room is shipped **experimental**.
Investigate a lighter path (offload the server role, cheaper codec/resampling,
or a dedicated-server topology) so primary playback stays glitch-free.

### 3. AirPlay 2 big-endian audit
Finish auditing the AP2 SETUP / timing code paths for big-endian (MIPS) bugs —
byte-order handling in the pairing/timing structs is the likely culprit for
remaining AP2 flakiness on this platform.

Notes: [`docs/BE-AP2-AUDIT.md`](docs/BE-AP2-AUDIT.md),
[`docs/BE-AP2-LIBPLIST.md`](docs/BE-AP2-LIBPLIST.md).

## Backlog / future enhancements

- **Identify the paperclip (recessed) button.** It reads as zero signal on every
  AR9331 GPIO and on the STM8 companion MCU — most likely a hardware reset line
  straight to the SoC rather than a software-readable input. Confirm against the
  datasheet; if it's not readable, document it as reset-only. (Setup mode is
  already reachable via a knob long-hold, so this is not on the critical path.)
- **Install microsite.** Publish the step-by-step install guide as a web page
  (MAC-derived default password step, setup-AP IP `192.168.60.1`, links to this
  repo's releases).
