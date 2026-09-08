# Beep Revival — the device UI

Everything the Beep tells you and everything you can tell it, without an app. Two
surfaces: the **24-LED ring** (status) + the **knob** (control), and a local
**web admin** for setup and settings.

---

## The knob (physical control)

The knob is a rotary encoder with a push-button, read by the STM8 companion MCU.

| Gesture | Action |
|---|---|
| **Turn** | Volume. Clockwise = up. One LED of the ring per detent; the arc fills from the bottom (6 o'clock) clockwise. Volume is shared — AirPlay, the web slider, and the knob all move the same level. |
| **Single tap** | **Mute / unmute** (soft-mute: saves the level, drops to 0, restores on the next tap). The ring shows a calm dim breath while muted. |
| **Double tap** | **Join the active multi-room group** (Snapcast). *Experimental — see multi-room note.* |
| **Hold ~10 s** | **Enter Wi-Fi setup.** The ring fills to full as you hold; release when full → the device raises its `BeepRevival-Setup-XXXXXX` AP. This is the *only* way to open setup on a configured device (deliberate physical action — it can't be forced by a deauth attack). |
| **Hold ~30 s** | **Factory reset** (wipes config + reboots). After the ring first fills (10 s), it empties and fills a **second** time on a brighter background — that second sweep is the reset countdown. Release before it completes if you didn't mean it. |

---

## The LED ring (status)

24 LEDs. No LED sits exactly on a clock cardinal — 12 o'clock straddles the top
two LEDs, 6 o'clock the bottom two.

| State | What you see |
|---|---|
| **Boot** | A brief smiley (eyes at ~2 & ~10 o'clock, a wide smile across the bottom), then a short sweep. |
| **Wi-Fi connecting** | A single dot orbiting a dim track. |
| **Idle** (connected, nothing playing) | Just the **bottom two LEDs** gently breathing. |
| **Volume** (while turning) | A bright arc from the bottom, clockwise, over a dim full-scale track. 50% = the whole left side. |
| **Playing** (AirPlay) | A sparse, slow **twinkle** — random LEDs spark up and fade, ~30% lit at a time (a "something's playing" shimmer). |
| **Muted** | A slow, dim whole-ring breath (clearly calmer than the playing twinkle). |
| **Button press** | A single whole-ring pulse (tactile feedback). |
| **Holding the knob** | The ring fills as a hold-progress meter (see the gesture table). |
| **Wi-Fi setup mode** | A comet chasing around the ring — "reconfigure me". |

---

## Web admin

Once the Beep is on your Wi-Fi, open **`http://<device-ip>/`** (also HTTPS on 443
with a self-signed cert). From there: rename it (renames AirPlay too), set volume,
change the admin password, toggle SSH (off by default), pick a multi-room role,
and apply **signed firmware updates**.

### First-time setup

A never-configured Beep raises a **`BeepRevival-Setup-XXXXXX`** Wi-Fi network (WPA2).
Join it and a captive portal pops up — pick your Wi-Fi and a name, done.

- **Device password / setup code** = **`C49300`** + the 6 characters at the end of
  the setup SSID. Example: `BeepRevival-Setup-A7F3E1` → **`C49300A7F3E1`**. It's the
  same code for the setup-AP Wi-Fi *and* the admin login.
- It's derivable from the broadcast SSID, so **change the admin password immediately**
  after setup (treat it as a factory default).

### Updating firmware (signed OTA)

Download the `*-sysupgrade.signed.bin` from the [Releases](../../releases) page and
upload it in the web admin's **Update firmware** panel. The device verifies the
`usign` signature against its baked-in public key before flashing — an unsigned
image is rejected unless you physically triple-tap to override.

---

## Multi-room (experimental)

Snapcast-based sync exists (one Beep = *primary*/server, others = *member*), but the
400 MHz AR9331 is CPU-tight when the primary also receives AirPlay and serves the
group during playback, so it's **experimental** for now. Single-room AirPlay is the
solid, default path.
