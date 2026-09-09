#!/bin/sh
# Volume + gesture logic in rootfs-overlay/usr/libexec/beep/beep-action.
# The knob->DAC math must land on exact 1/NLEDS steps so the knob, the AirPlay volume,
# the web slider, and beepd's LED arc all agree. We run the real script with a temp
# state dir (BEEP_RUN_DIR) and stubbed amixer/logger, then assert the state it writes.
#
# turn math (NLEDS=24): lvl = round(cur*24/100) + step(±1), clamped 0..24;
#                       pct = round(lvl*100/24). One call = exactly one LED step.
: "${REPO_ROOT:=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="volume-math"
t_use_stubs

ACTION="$REPO_ROOT/rootfs-overlay/usr/libexec/beep/beep-action"
t_tmpdir; tmp="$T_TMP"
run="$tmp/run"

reset_run() { rm -rf "$run"; mkdir -p "$run"; }
act() { BEEP_RUN_DIR="$run" sh "$ACTION" "$@"; }   # AMIXER_PCT passed by caller when needed
vol() { cat "$run/volume" 2>/dev/null; }

# --- turn: reads last level from the state file, steps one LED, snaps back to % ---
reset_run; echo 50 > "$run/volume"; act turn 1
assert_eq 54 "$(vol)" "turn +1 from 50% -> 54% (one LED up)"

reset_run; echo 50 > "$run/volume"; act turn -1
assert_eq 46 "$(vol)" "turn -1 from 50% -> 46% (one LED down)"

reset_run; echo 50 > "$run/volume"; act turn 7   # magnitude ignored: still one step
assert_eq 54 "$(vol)" "turn +7 == turn +1 (encoder magnitude distrusted)"

reset_run; echo 100 > "$run/volume"; act turn 1
assert_eq 100 "$(vol)" "turn +1 at 100% clamps to 100%"

reset_run; echo 0 > "$run/volume"; act turn -1
assert_eq 0 "$(vol)" "turn -1 at 0% clamps to 0%"

reset_run; echo 50 > "$run/volume"; act turn 0
assert_eq 50 "$(vol)" "turn 0 is a no-op"

reset_run; echo 50 > "$run/volume"; act turn abc
assert_eq 50 "$(vol)" "turn with non-numeric arg is a no-op"

# state file empty/absent -> fall back to amixer get (AMIXER_PCT drives the stub)
reset_run; AMIXER_PCT=25 act turn 1
assert_eq 29 "$(vol)" "empty state file falls back to amixer get (25% +1 LED -> 29%)"

# rounding must be to the NEAREST LED, not floor — these inputs are NOT exact multiples,
# so they pin the +50 (LED calc) and +NLEDS/2 (%-snap) round-half-up terms.
reset_run; echo 48 > "$run/volume"; act turn 1
assert_eq 54 "$(vol)" "turn +1 from 48% rounds to nearest LED (-> 54%, not floor 50%)"

reset_run; echo 17 > "$run/volume"; act turn 1
assert_eq 21 "$(vol)" "turn +1 from 17% snaps % to nearest (-> 21%, not floor 20%)"

# --- tap: soft mute/unmute using the amixer level and the premute-vol memory ---
reset_run; AMIXER_PCT=40 act tap
assert_eq 0 "$(vol)"                       "tap at 40% mutes to 0%"
assert_eq 40 "$(cat "$run/premute-vol")"   "tap saves premute level (40%)"
assert_status 0 test -f "$run/muted"        # muted flag present

reset_run; echo 40 > "$run/premute-vol"; AMIXER_PCT=0 act tap
assert_eq 40 "$(vol)" "tap at 0% unmutes to saved 40%"
assert_status 1 test -f "$run/muted"        # muted flag cleared

reset_run; AMIXER_PCT=0 act tap
assert_eq 30 "$(vol)" "tap at 0% with no saved level unmutes to default 30%"

# --- phys_confirm: opens a fresh ~60s physical-presence window ---
reset_run; now="$(date +%s)"; act phys_confirm
exp="$(cat "$run/phys-confirm" 2>/dev/null)"
case "$exp" in ''|*[!0-9]*) assert_eq numeric "$exp" "phys-confirm expiry is numeric" ;; *)
	[ "$exp" -gt "$now" ] && [ "$exp" -le "$((now + 61))" ]
	assert_status 0 test "$exp" -gt "$now"
	assert_eq 1 "$([ "$exp" -le "$((now + 61))" ] && echo 1 || echo 0)" "phys-confirm window <= now+61s" ;;
esac

t_done
