# Security Policy

This is open, community firmware for orphaned Beep "Dial" hardware, maintained on a
best-effort basis. It ships with **no warranty and no liability** (see `LICENSE`,
GPL-2.0 §§11–12) — but security reports are genuinely welcome and taken seriously.

## Reporting a vulnerability

**Please do not open a public issue for security problems.** Use GitHub's private
vulnerability reporting instead:

> Repository **Security** tab → **Report a vulnerability**

That opens a private advisory visible only to the maintainers. Include what you
found, how to reproduce it, and the impact. There is no SLA, but reports are
triaged as promptly as a hobby project allows, and fixes are disclosed publicly
once a patched build is available.

If private reporting is not enabled or you can't use it, open a normal issue that
says only "security — please enable private reporting" **without** vulnerability
details, and a maintainer will follow up.

## Known limitations (already documented — no need to report)

These are accepted, disclosed trade-offs, not undiscovered bugs:

- **Default credentials are derived from the device MAC.** A MAC is not secret, so
  the first-boot admin password and the WPA2 setup-AP key are guessable by anyone on
  the LAN (via ARP) or in Wi-Fi range (the setup SSID publishes part of the MAC)
  until the owner changes the admin password. Change it after setup. See the README
  ⚠️ note.
- **The admin web UI is served over plain HTTP by default** (HTTPS is offered on 443
  but not forced), so admin credentials/session can be observed on the local network.
  Prefer the HTTPS listener.

New findings *beyond* these are in scope and appreciated.

## Supported versions

Only the latest firmware on the default branch is supported. There are no
backported security fixes for older builds — update to the current release.
