# Installing / flashing a Beep — moved

The install and flashing instructions now live in a **single canonical guide**:

## → [`FLASHING-WALKTHROUGH.md`](FLASHING-WALKTHROUGH.md)

It is the one place for the whole lifecycle:

- **First-time flash** — the one-time, phased UART procedure (serial console → developer
  unlock → full verified backup → **lean RAM-boot** → the single `sysupgrade` write →
  bootloader repoint), built so `u-boot` and `art` are never touched and you always have a
  way home.
- **Staying updated** — after the first flash there's no separate "install" step: updates
  are just a **signed image uploaded in the web UI**, verified on-device before it writes.

> ⚠️ **No warranty — you flash at your own risk** ([`LICENSE`](../LICENSE), GPL-2.0
> §§11–12). Flashing embedded hardware can brick a device; read the guide fully first.

*(This page used to hold a second, parallel copy of the flash steps. That drifted out of
sync with the walkthrough, so it now redirects here — one guide, one source of truth.)*
