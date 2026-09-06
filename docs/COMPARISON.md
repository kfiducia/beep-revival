# Stock Beep vs. the open firmware

An honest, feature-by-feature comparison of the original cloud-tethered Beep
firmware and this open replacement. The guiding difference: **the stock firmware
depended on a company and a cloud that no longer exist; this one depends on
nothing but your own network.** That's why a stock Beep today is a paperweight and
a reflashed one is a working speaker.

Status key for the open firmware: ✅ working · 🔧 building/plumbed · 🔭 planned.

## At a glance

| | Stock Beep firmware | Open firmware |
|---|---|---|
| **Works today?** | ❌ No — the Beep cloud shut down ~2016, taking the app and streaming with it | ✅ Yes — nothing it needs can be turned off by a third party |
| **Control** | Beep mobile app, account required, routed through Beep's servers | Local **web interface** on the device — any browser, no app, no account ✅ |
| **Audio sources** | In-app streaming services (Spotify, Pandora, iHeartRadio, TuneIn, …) brokered by the cloud | **AirPlay 2** ✅ (stream anything from Apple devices, incl. the Spotify/Pandora apps); **Snapcast** multiroom 🔭; DLNA 🔭 |
| **Multiroom / sync** | Beep's proprietary cloud multiroom | Open **Snapcast** (self-hosted, no cloud) 🔭; AirPlay 2 grouping ✅ |
| **Setup** | App + account + cloud pairing | On-device **Wi-Fi setup AP** + captive portal; per-device code, no account ✅ |
| **Updates** | Pushed from Beep's cloud (now dead) | **Signed** firmware upload from the web UI 🔧; recovery slot backstop 🔭 |
| **If Wi-Fi/config breaks** | Re-pair via app/cloud (impossible now) | **Self-heals** to a setup AP automatically 🔧 (implemented, not yet HW-verified) |
| **Accounts / telemetry** | Beep account required; usage flowed to their servers | **None.** No account, no phone-home, no telemetry ✅ |
| **Longevity** | Tied to one company's survival | Open source; runs on mainline OpenWrt; anyone can rebuild it ✅ |
| **Physical knob / LED ring** | Volume, tap, light ring (cloud-aware) | Firmware-owned: volume arc, tap/gestures, party-mode + setup ring ✅ |

## The core shift: no central hub

- **Stock:** the speaker was a thin client. Streaming, control, multiroom, and
  updates were all brokered by Beep's cloud and the Beep app. When the company
  folded and the servers went dark, the hardware had no way to function — a textbook
  **orphaned IoT** device.
- **Open:** every function is either **on the device** (web UI, setup, physical
  controls, updates) or uses an **open standard you host or already own** (AirPlay
  from your phone, Snapcast on your own box). There is no single point of failure
  that a third party controls. Reflash a hundred of these in 2040 and they still work.

## Audio / streaming — the honest version

- The stock app presented streaming services **inside the Beep app**, logging into
  Spotify/Pandora/etc. through Beep's cloud. Convenient, but that's exactly the
  dependency that killed it.
- The open firmware takes the **standards** route: **AirPlay 2** lets you send audio
  from any Apple device — including the **Spotify, Pandora, YouTube Music, podcast,
  or any other app** on that device — to the speaker. So you don't lose Spotify; you
  just cast it instead of picking it inside a (dead) Beep app.
- What is **not** a 1:1 replacement (yet): a **native, standalone in-device service
  login** (e.g. Spotify Connect via `librespot`) so you could stream without a phone
  as the source. That's a candidate future add 🔭, not shipping today. If you want
  "pick Spotify on the device itself," note this gap.

## Privacy & security

| | Stock | Open |
|---|---|---|
| Account required | Yes (Beep) | **No** |
| Data to third-party servers | Yes | **None** |
| Default credentials | App/cloud-managed | **Per-device** code from the MAC — never a shared/blank password ✅ |
| Remote attack surface | Cloud API + app | Minimal: local web UI behind session auth; **SSH off by default** ✅; no anonymous control API |
| Firmware trust | Vendor cloud push | **Signature-verified** uploads only (usign) 🔧 |

## Repairability & control

- **Stock:** closed. No user access, no source, no way to fix it when the cloud died.
- **Open:** full root, open source, standard OpenWrt underneath, a documented
  **rescue/flash guide** (`docs/INSTALL.md`), and a **recovery design** aimed at
  never needing a UART again (`docs/RECOVERY-DESIGN.md`). You own it.

## What you give up (fair's fair)

- The **polished first-party mobile app** experience. The open firmware is a clean
  responsive web UI, not a native app.
- **In-app service browsing** on the device itself. You stream *to* it (AirPlay)
  rather than picking playlists *on* it — unless/until a Connect-style integration
  lands 🔭.
- Whatever **proprietary cloud multiroom** polish Beep had — replaced by open
  Snapcast/AirPlay grouping, which you host yourself.

## Bottom line

The stock firmware was more turnkey **while the company was alive**. The open
firmware trades a bit of that first-party polish for the thing that actually
matters now: **it works, it's private, it's yours, and no one can orphan it again.**
