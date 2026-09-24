#!/bin/sh
# The set_gestures rpcd verb (rootfs-overlay/usr/libexec/rpcd/beep) — the WRITE path for the
# UI-configurable gesture map. It must accept only whitelisted actions and persist them to
# beep.gestures.<gesture>. We drive the real rpcd object (`call set_gestures`) with the JSON
# body on stdin and stubbed uci (records `set`s to $UCI_SET_LOG) + jsonfilter.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="set-gestures"
t_use_stubs

RPCD="$REPO_ROOT/rootfs-overlay/usr/libexec/rpcd/beep"
t_tmpdir; tmp="$T_TMP"
LOG="$tmp/setlog"

# Run set_gestures with the given JSON body; leaves this call's uci `set`s in $LOG.
callg() { : > "$LOG"; printf '%s' "$1" | UCI_SET_LOG="$LOG" sh "$RPCD" call set_gestures; }

# --- valid actions are accepted and written ----------------------------------
out="$(callg '{"tap":"mute"}')"
assert_contains "$out" '"ok":true'                    "valid tap -> ok"
assert_contains "$(cat "$LOG")" 'beep.gestures.tap=mute' "valid tap -> persisted"

out="$(callg '{"double_tap":"lms_next"}')"
assert_contains "$out" '"ok":true'                             "valid double_tap -> ok"
assert_contains "$(cat "$LOG")" 'beep.gestures.double_tap=lms_next' "valid double_tap -> persisted"

out="$(callg '{"tap":"auto","double_tap":"group_toggle"}')"
assert_contains "$(cat "$LOG")" 'beep.gestures.tap=auto'            "both: tap=auto persisted"
assert_contains "$(cat "$LOG")" 'beep.gestures.double_tap=group_toggle' "both: double_tap persisted"

# --- invalid actions are rejected and NOTHING is written ----------------------
out="$(callg '{"tap":"bogus"}')"
assert_contains "$out" 'invalid tap'   "bad tap -> error"
assert_eq ""  "$(cat "$LOG")"          "bad tap -> no uci write"

out="$(callg '{"double_tap":"nope"}')"
assert_contains "$out" 'invalid double_tap' "bad double_tap -> error"
assert_eq ""  "$(cat "$LOG")"               "bad double_tap -> no uci write"

# a lms_playpause value (the AirPlay-context override) is valid too
out="$(callg '{"tap":"lms_playpause"}')"
assert_contains "$out" '"ok":true' "tap=lms_playpause accepted"

t_done
