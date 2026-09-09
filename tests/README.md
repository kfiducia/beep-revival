# Test suite

Fast, dependency-free checks for the parts of the firmware that can run **off-device**.
They catch regressions in the pure decision logic before it ships to the AR9331.

## Run

```sh
sh tests/run.sh            # all cases
sh tests/run.sh volume     # only cases whose name matches *volume*
```

No install step — just a POSIX `sh` and coreutils. The same suite runs in CI
(`.github/workflows/ci.yml`, `shell unit tests` job).

## What's covered

| Case file | Script under test | Why it matters |
|---|---|---|
| `cases/bootcount_decode.sh` | `scripts/beep-bootcount-probe.sh` | Misreading the strike counter hides how close a unit is to dropping to recovery. |
| `cases/volume_math.sh` | `rootfs-overlay/usr/libexec/beep/beep-action` | Knob → DAC → LED must agree on exact 1/24 steps; off-by-one drifts the slider. |
| `cases/ota_gate.sh` | `rootfs-overlay/www/cgi-bin/beep-ota` | The signed/unsigned flash authorization gate — a regression is a remote-flash hole. |

## How it works

The scripts under test talk to hardware (`/dev/mtd1`, `amixer`, `ubus`, `usign`, …). The
tests run the **real scripts** with two seams so no device is needed:

- **Env-var path overrides** baked into the scripts (`MTD`, `BEEP_RUN_DIR`, `PHYS_CONFIRM`).
  These default to the on-device paths, so production behavior is byte-identical when unset.
- **Stub commands** in `tests/stubs/` shadow the hardware tools on `PATH`; their behavior is
  driven by environment variables (e.g. `AMIXER_PCT`, `UBUS_ACCESS`, `USIGN_RESULT`).

`tests/lib/harness.sh` is a tiny assertion harness (`assert_eq`, `assert_contains`,
`assert_status`). Each `cases/*.sh` sources it and ends with `t_done`.

## Adding a case

1. Create `tests/cases/<name>.sh`, source the harness, make assertions, end with `t_done`.
2. If the script writes to a fixed path, add an env-var override seam (default = the real
   path) rather than mocking the filesystem.
3. **Mutation-check it**: temporarily break the logic and confirm a test goes red. A test
   that stays green when the code is wrong is worse than no test.

## NOT covered here (needs the real AR9331)

CI cannot exercise these — verify them on a unit before a release:

- AirPlay 1/2 handshake and audio output
- An actual `sysupgrade` flash + first-boot config restore + bootcount/recovery behavior
- The optical knob, taps/holds, and the STM8 LED ring
- Wi-Fi setup AP and network join

The full-image build lives in `.github/workflows/release.yml`.
