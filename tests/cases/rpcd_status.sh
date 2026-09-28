#!/bin/sh
# The rpcd/beep `status` verb (rootfs-overlay/usr/libexec/rpcd/beep) — the read-only view
# the web admin UI polls. We drive the real rpcd object (`call status`) with stubbed
# uci/amixer/ubus + the generic jsonfilter stub, and pin both the all-defaults shape and
# how uci overrides propagate through. pidof/iwinfo/`/proc/uptime` are absent off-device,
# so the daemon-liveness flags degrade to false and version falls back to "unknown" —
# deterministic on both a dev Mac and a Linux CI runner.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="rpcd-status"
t_use_stubs

RPCD="$REPO_ROOT/rootfs-overlay/usr/libexec/rpcd/beep"

# --- defaults: no uci overrides ------------------------------------------------
out="$(sh "$RPCD" call status </dev/null)"
assert_contains "$out" '"group_role":"solo"'          "default group_role"
assert_contains "$out" '"group_engine":"snapcast"'    "default group_engine"
assert_contains "$out" '"airplay_mode":"ap1"'         "default airplay_mode"
assert_contains "$out" '"ssh_mode":"off"'             "default ssh_mode"
assert_contains "$out" '"session_timeout_min":30'     "default session_timeout_min"
assert_contains "$out" '"ap_mode":false'              "default ap_mode"
assert_contains "$out" '"connected":false'            "default connected"
assert_contains "$out" '"lms_enabled":0'               "default lms_enabled"
assert_contains "$out" '"ssh_expires":null'           "default ssh_expires"
assert_contains "$out" '"gesture_tap":"auto"'         "default gesture_tap"

# --- uci overrides propagate ---------------------------------------------------
out="$(UCIGET_beep_main_group_role=member UCIGET_beep_main_airplay_mode=ap2 \
	UCIGET_beep_main_ssh_mode=on UCIGET_squeezelite_options_enabled=1 \
	UCIGET_wireless_setup_disabled=0 AMIXER_PCT=42 \
	sh "$RPCD" call status </dev/null)"
assert_contains "$out" '"group_role":"member"'  "override group_role"
assert_contains "$out" '"airplay_mode":"ap2"'   "override airplay_mode"
assert_contains "$out" '"ssh_mode":"on"'        "override ssh_mode"
assert_contains "$out" '"lms_enabled":1'        "override lms_enabled"
assert_contains "$out" '"ap_mode":true'         "override ap_mode"
assert_contains "$out" '"volume":42'            "override volume"

# --- bad numerics coerced to safe defaults -------------------------------------
out="$(UCIGET_beep_main_session_timeout_min=abc sh "$RPCD" call status </dev/null)"
assert_contains "$out" '"session_timeout_min":30' "garbage session_timeout_min -> default"

out="$(UCIGET_beep_main_ssh_expires=xyz sh "$RPCD" call status </dev/null)"
assert_contains "$out" '"ssh_expires":null'       "garbage ssh_expires -> null"

t_done
