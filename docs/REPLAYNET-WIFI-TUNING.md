# replaynet `--resample` — wifi jitter tuning brief

**STATUS: mid-stream dropouts SOLVED (Sep 2026).** Two Beeps play in sync; the wifi sink is
dropout-free during steady playback. One ~15 ms tick remains in the first ~0.2 s per stream
(startup fine-trim convergence) — documented, minor, not yet eliminated. See RESOLUTION.

## RESOLUTION — what actually fixed it (and the method lesson)
Root cause, found by **instrumenting the real snaps** (not theorising): on the single-core
400 MHz AR9331 the wifi stack (ath9k/wpad softirqs) **preempts the playout thread for
150–827 ms**; the DAC buffer empties and the sink snaps to catch up = a dropout. Not network
(ring was full), not CPU (61 % idle), not the servo.

The fix (all hardware-validated with `hw-tune.sh`):
1. **RT playout thread** — `SCHED_FIFO` prio 50 + `mlockall`. musl stubs `sched_setscheduler`
   to ENOSYS, so call the syscall directly. This is the load-bearing fix (max `loop_dt`
   827 ms → 0 ms; residual stalls ≤ ~230 ms).
2. **DAC buffer 120 → 500 ms** (`RN_ALSA_BUF_NS`) — absorbs the residual ≤230 ms stalls RT
   can't preempt (hardirqs). 800 ms was worse (delay-jitter). 500 ms is the knee.
3. **Ring 400 ms → 2 s** (`RN_BUFFER_NS`) — absorbs network delivery jitter (measured RTT
   3–228 ms on the jammed channel).
4. **DAC start threshold = 1 period** — start playing immediately instead of waiting for the
   deep buffer to fill (halved the startup tick).

**What FAILED (looked great in `--simulate`, rejected by hardware):** PI control, median snap
filter, wider clamp, continuous-theta, startup pre-drop, startup snap-grace. Every one
targeted a *theorised* cause; the sim faithfully "confirmed" each and hardware disproved it.
**Measurement — the SNAP-dbg instrumentation + the two-Beep harness — found the truth.**
Ground the sim in measurements; never let it validate a theory. Also fixed on the way: silver
was mis-running hostapd/dnsmasq/odhcpd (now station-only); the RF is a jammed channel 1.

Remaining polish (optional): the startup tick is the fine trim converging from 1.0 to the
DAC's rate offset in the first ~0.2 s. Real fixes would be seeding `rs.step` near the DAC
offset or a brief fast-converge window — NOT a snap-grace (delays it) or a pre-drop
(over-shoots), both tried and reverted.

---

## Original brief (kept for the harness + method; the dropout goal above is met)
The wifi sink used to drop out a few times over a 25 s pure-tone test. This section hands an
agent the reproducible harness and what was learned. Read it fully before changing code.

## The system in one paragraph
`replaynet` is a from-scratch multi-room engine (see `REPLAYNET.md`). A **source** paces
raw S16LE PCM (44.1k stereo, ~1.4 Mbps) at 1× real time; each **sink** NTP-lite-locks the
source clock, anchors an absolute playout schedule (sample `S` audible at a fixed
wall-clock), and buffers `RN_BUFFER_NS` of audio in a ring between the network and the DAC.
A **fan-out** feeds many sinks at once. `--resample` (opt-in) is a HYBRID drift corrector:
a click-free cubic **resample-ratio trim** for fine error, plus a drop/insert **snap** for
coarse error (>20 ms). Everything is integer/no-FPU on a 400 MHz big-endian AR9331.

## The objective
Minimise **COPPER `snap_events`** (each snap = a drop/insert = an audible click/dropout)
while:
- **SILVER `snap_events` stays ≈ 0** (co-located control; the resampler is already perfect
  there — do not regress it),
- **`ring_min` stays > ~200 ms** on both (no starvation → no XRUN),
- **`XRUN = 0`** on both.

There is no microphone in the loop: `snap_events` **is** the ears. It's calibrated — the
operator heard "3 dropouts" on a run whose copper log showed ~3 snap events. Cross-check the
final candidate by asking the operator to listen, but iterate on the metric.

## The harness
```sh
export SSHPASS=<copper root password>        # copper build has the default root password
sh scripts/replaynet/hw-tune.sh -n 3         # 3 runs; edit the source, re-run, compare
```
It cross-compiles the **current** `feed/replaynet/src/replaynet.c` in the `beep-build`
container, deploys to both Beeps, fans a 25 s 440 Hz tone out to both sinks, and prints per
run:
```
SILVER: snap_events=0  err_max_ms=..  ring_min_ms=..  XRUN=0     <- control
COPPER: snap_events=N  err_max_ms=..  ring_min_ms=..  XRUN=0     <- minimise N
```
**Network jitter is high run-to-run** (copper 7 then 13 in back-to-back runs), so judge every
change by the **median of ≥3 runs**, never one. Silver = 10.9.100.169 (key), copper =
10.9.100.173 (password). Both are beep-revival; copper is on wifi (`phy0-sta0`, ~-59 dBm).

## What is already established (don't re-derive)
- **Co-located sync is solved.** Silver-local holds **±0.3–0.7 ms, snap_events=0, zero
  XRUN**. The clock lock, absolute schedule, and resampler math are correct. The endianness
  of PCM (S16LE via `le16_get/le16_put`) is correct. Leave all that alone.
- **Deeper buffer was the big win.** `RN_BUFFER_NS` 400 ms → **1 s** (ring 256 KB → **512 KB**)
  stopped the starvation that caused the worst dropouts. Copper's ring now holds ~600–900 ms
  and rarely dips. Keep it.
- **Snap-debounce FAILED twice (operator confirmed by ear both times).** Requiring `|err|>SNAP`
  for N periods before snapping *delays* each snap while the error grows, then snaps a bigger
  chunk = a *louder* dropout. Copper's large errors are **not** clean single-period phantoms.
  Do not re-add a simple consecutive-count debounce.
- **A drifting/bursty source is the wrong instrument.** Early tests streamed from a laptop
  whose clock slid ~7 ms/run and paced in bursts — it produced sustained errors no sink
  tuning can fix. A real Beep source (matched, stable crystal) is required; the harness uses
  silver as the source. Ignore laptop-source results.
- **CPU/ALSA contention destroys the metric.** A second agent working on copper spiked its
  CPU and grabbed the single WM8524 subdevice, turning ~3 dropouts into a dropout-storm.
  Before trusting a run, confirm copper is idle (`loadavg` < ~0.5, `/proc/asound/card0/pcm0p/
  sub0/status` == `closed`, no other audio procs).

## Where the remaining copper dropouts come from (open question)
Copper's `err` swings ±7–20 ms **while its ring stays healthy (600–900 ms)**. A healthy ring
means arrival jitter is absorbed — so the error is NOT starvation. It is one (or more) of:
1. **`snd_pcm_delay` jitter** on copper's i2s driver — the measured "audible sample" wobbles
   even though real playout is steady. (Seen as ±145-frame jitter on silver; larger on copper.)
2. **Copper clock drift vs the FROZEN theta.** `theta` is locked once at anchor. If copper's
   monotonic clock drifts against silver's after the freeze faster than the ±3000 ppm fine
   trim tracks, `err` grows until it crosses the 20 ms snap threshold.
3. **Sustained network delivery skew** the 1 s buffer masks in depth but not in schedule.
The debounce result (errors survive 3 periods) argues these are **sustained, not phantom** —
which points at (2)/(3) more than (1).

## Ideas to try (roughly ordered; measure each with `-n 3`)
- **Widen the fine trim clamp.** `RN_RSMP_STEP_FINE` (197 ≈ 3000 ppm). At 3000 ppm the trim
  only closes ~132 frames/s, so a growing error reaches 20 ms and snaps before the trim
  catches it. Try 400–650 (≈6000–10000 ppm): the click-free trim absorbs bigger excursions
  so fewer cross into a snap. Trade-off: more audible pitch waver on a pure tone (0.6–1 %) —
  cross-check by ear. **Most promising first move.**
- **Track the clock instead of freezing it.** Replace the frozen `theta` with a slow PLL /
  periodic re-estimate so a drifting source clock is followed by the offset, not fought by
  the trim. Bigger change; addresses hypothesis (2) at the root.
- **Raise `RN_RSMP_SNAP`** (882 ≈ 20 ms) *only if* paired with a wider fine clamp, else the
  trim can't close the larger tolerated error and rooms sit out of sync for seconds.
- **Median/robust `err`** for the snap decision (not a consecutive-count debounce): snap on a
  median-filtered error so a lone spike can't trigger it but sustained error still does.
- **Compression (FLAC/Opus)** to cut the 1.4 Mbps stream ~3–10×. Reduces network CPU + jitter
  headroom on marginal links — the general fix for weak wifi, but a real feature, not a tune.

## Guardrails
- **RAM is tight (64 MB).** No large files in `/tmp` (tmpfs = RAM). Don't leave processes
  running (busybox has no `pkill`; kill by PID — the harness does). An OOM here *freezes* the
  box (needs a power-cycle). Socket buffers are already capped at 256 KB — keep them small.
- **Never regress silver** (the control). If a change pushes silver `snap_events` off 0, it's
  wrong regardless of copper.
- **One variable at a time**, median of ≥3 runs, and note the numbers in your write-up.
- Confirm copper is idle before a run (contention invalidates it).

## Files
- Engine + all knobs: `feed/replaynet/src/replaynet.c` (search `RN_RSMP_`, `RN_BUFFER_NS`,
  `RN_SYNC_`, `run_sink_alsa`).
- Harness: `scripts/replaynet/hw-tune.sh`.
- Design: `docs/REPLAYNET.md`.
