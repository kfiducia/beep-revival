#!/bin/sh
# One-way-door Wi-Fi join revert (usr/libexec/beep/wifi-revert). This is the safety
# logic that stops a bad password from stranding a sealed unit: arm a config snapshot
# in set_wifi, disarm it the FIRST time the new network associates (confirm), and if it
# NEVER associates, revert to the snapshot. The security-critical invariant is the
# one-way door: after a single success we must never revert again (anti-deauth).
#
# We drive the real helper with a file-backed uci stub ($UCI_STATE_DIR) and BEEP_APPLY=0
# to skip live service/wifi reloads, then assert the persisted uci state.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="wifi-revert"
t_use_stubs

WR="$REPO_ROOT/rootfs-overlay/usr/libexec/beep/wifi-revert"
t_tmpdir; tmp="$T_TMP"
export BEEP_RUN="$tmp/run"
export BEEP_APPLY=0
export UCI_STATE_DIR="$tmp/uci"
PREV="$BEEP_RUN/wifi-prev"

seed_good_network() { printf "package wireless\n\nconfig wifi-iface\n\toption ssid 'HomeNet'\n\toption key 'goodpass'\n" > "$UCI_STATE_DIR/wireless"; }
mutate_to_new()     { printf "package wireless\n\nconfig wifi-iface\n\toption ssid 'NewNet'\n\toption key 'wrongpass'\n"  > "$UCI_STATE_DIR/wireless"; }
cur_wireless()      { cat "$UCI_STATE_DIR/wireless" 2>/dev/null; }

mkdir -p "$UCI_STATE_DIR"

# --- arm: snapshots the pre-change config ---
seed_good_network
sh "$WR" arm
assert_status 0 sh "$WR" armed                       # a revert is now pending
assert_contains "$(cat "$PREV/wireless")" "HomeNet"  "arm snapshotted the previous good config"

# --- arm is idempotent: a re-submit keeps the ORIGINAL baseline, not the new attempt ---
mutate_to_new                                         # simulate set_wifi overwriting the creds
sh "$WR" arm                                          # second arm while a join is pending
assert_contains "$(cat "$PREV/wireless")" "HomeNet"  "re-arm keeps the original baseline (not NewNet)"

# --- revert: restores the snapshot and disarms ---
sh "$WR" revert
assert_contains "$(cur_wireless)" "HomeNet"          "revert restored the previous good network"
assert_status 1 sh "$WR" armed                       "revert disarmed the pending revert"

# revert again is a safe no-op (nothing armed) and must not clobber current config
sh "$WR" revert
assert_contains "$(cur_wireless)" "HomeNet"          "revert with nothing armed is a no-op"

# --- the one-way door: confirm disarms, and a later failure must NOT revert ---
rm -rf "$PREV"
seed_good_network
sh "$WR" arm                                          # provisional switch armed
mutate_to_new                                         # radio now on NewNet
sh "$WR" confirm                                      # NewNet associated once -> commit
assert_status 1 sh "$WR" armed                        "confirm disarmed the revert (door closed)"

# NewNet later drops: revert must be a no-op — we stay on NewNet (anti-deauth), never
# snapping back to the old network just because the confirmed one went away.
sh "$WR" revert
assert_contains "$(cur_wireless)" "NewNet"           "post-confirm drop does NOT revert (one-way door holds)"

# --- usage guard ---
assert_status 2 sh "$WR" bogus                        "unknown subcommand -> usage error"

t_done
