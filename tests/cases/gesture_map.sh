#!/bin/sh
# Config-driven gesture mapping in rootfs-overlay/usr/libexec/beep/beep-action. This is
# the policy that decides what tap / double_tap do, incl. the source-aware `auto` default
# and the safety rule that only tap/double_tap are configurable. We source beep-action in
# lib mode (BEEP_ACTION_LIB=1) with a uci stub and exercise the resolver + dispatcher
# directly — no real amixer/beep-group/beep-lms-ctl involved.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="gesture-map"
t_use_stubs

export BEEP_UCI="$REPO_ROOT/tests/stubs/uci"   # resolver reads config through this
BEEP_ACTION_LIB=1 . "$REPO_ROOT/rootfs-overlay/usr/libexec/beep/beep-action"

# The uci stub reads config from UCIGET_<dotted_key_with_underscores> env vars.
# Set them at file scope (not inside $(...)) so each assertion sees a clean, explicit state.

# --- gesture_action: `auto` resolves by active source -------------------------
export UCIGET_squeezelite_options_enabled=1
assert_eq lms_playpause "$(gesture_action tap)"        "tap auto + LMS active -> lms_playpause"
assert_eq group_toggle  "$(gesture_action double_tap)" "double_tap auto = group_toggle even in LMS mode"

UCIGET_squeezelite_options_enabled=0
assert_eq mute "$(gesture_action tap)" "tap auto + AirPlay -> mute"

# --- explicit overrides win over auto -----------------------------------------
export UCIGET_beep_gestures_tap=none
assert_eq none "$(gesture_action tap)" "tap override -> none"
UCIGET_beep_gestures_tap=group_toggle
assert_eq group_toggle "$(gesture_action tap)" "tap override -> group_toggle"
unset UCIGET_beep_gestures_tap

export UCIGET_beep_gestures_double_tap=lms_next
assert_eq lms_next "$(gesture_action double_tap)" "double_tap override -> lms_next"
unset UCIGET_beep_gestures_double_tap

# --- run_gesture_action dispatches to the right act_* -------------------------
# Redefine the act_* as markers; run_gesture_action calls them by name (indirect use).
# shellcheck disable=SC2329
act_mute()          { echo MUTE; }
# shellcheck disable=SC2329
act_group_toggle()  { echo GROUP; }
# shellcheck disable=SC2329
act_lms_playpause() { echo PP; }
# shellcheck disable=SC2329
act_lms_next()      { echo NEXT; }
assert_eq MUTE  "$(run_gesture_action mute tap)"                 "dispatch mute"
assert_eq GROUP "$(run_gesture_action group_toggle double_tap)" "dispatch group_toggle"
assert_eq PP    "$(run_gesture_action lms_playpause tap)"        "dispatch lms_playpause"
assert_eq NEXT  "$(run_gesture_action lms_next double_tap)"      "dispatch lms_next"
assert_eq ""    "$(run_gesture_action none tap)"                 "dispatch none -> no-op"
assert_eq MUTE  "$(run_gesture_action bogus tap)"                "dispatch unknown -> mute fallback"

t_done
