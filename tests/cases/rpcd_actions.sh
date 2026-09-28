#!/bin/sh
# The rpcd/beep write verbs (rootfs-overlay/usr/libexec/rpcd/beep) — the mutating side of
# the web admin backend: wifi/name/volume/group/airplay/lms/password/ssh/session-timeout.
# We drive the real rpcd object with a JSON body on stdin and stubbed uci (records `set`s
# to $UCI_SET_LOG) + jsonfilter, and assert on stdout JSON + the uci set-log. All
# service/init.d/helper calls in these verbs are error-suppressed or `|| true`, and the
# script has no `set -e`, so absent on-device helpers no-op harmlessly here; a couple of
# happy paths also write under /var/run/beep, which can emit harmless stderr noise on a
# dev Mac (no write access) — it does not affect the stdout this test captures.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="rpcd-actions"
t_use_stubs

RPCD="$REPO_ROOT/rootfs-overlay/usr/libexec/rpcd/beep"
t_tmpdir; tmp="$T_TMP"
LOG="$tmp/setlog"

# Run rpcd/beep's call $method with the given JSON body; leaves this call's uci `set`s in $LOG.
call() { m="$1"; body="$2"; : > "$LOG"; printf '%s' "$body" | UCI_SET_LOG="$LOG" sh "$RPCD" call "$m"; }

# --- set_wifi -------------------------------------------------------------------
out="$(call set_wifi '{}')"
assert_contains "$out" 'ssid required'              "set_wifi: no ssid -> error"

out="$(call set_wifi '{"ssid":"Net","key":"short"}')"
assert_contains "$out" 'at least 8 characters'      "set_wifi: short key -> error"

out="$(call set_wifi '{"ssid":"MyNet","key":"goodpass8"}')"
log="$(cat "$LOG")"
assert_contains "$out" '"ok":true'                              "set_wifi: valid -> ok"
assert_contains "$log" 'wireless.@wifi-iface[0].ssid=MyNet'      "set_wifi: ssid persisted"
assert_contains "$log" 'wireless.@wifi-iface[0].mode=sta'        "set_wifi: mode persisted"
assert_contains "$log" 'wireless.@wifi-iface[0].encryption=psk2' "set_wifi: encryption persisted"
assert_contains "$log" 'wireless.@wifi-iface[0].key=goodpass8'   "set_wifi: key persisted"
assert_contains "$log" 'wireless.setup.disabled=1'               "set_wifi: setup AP disabled"

out="$(call set_wifi '{"ssid":"Open"}')"
assert_contains "$out" '"ok":true'                                "set_wifi: open network -> ok"
assert_contains "$(cat "$LOG")" 'wireless.@wifi-iface[0].encryption=none' "set_wifi: no key -> open encryption"

# --- set_name ---------------------------------------------------------------
out="$(call set_name '{}')"
assert_contains "$out" 'name required'              "set_name: no name -> error"

out="$(call set_name '{"name":"Living Room"}')"
assert_contains "$out" '"ok":true'                  "set_name: valid -> ok"
assert_contains "$(cat "$LOG")" 'system.@system[0].hostname=Living Room' "set_name: hostname persisted (with space)"

# --- set_volume ---------------------------------------------------------------
out="$(call set_volume '{"percent":"x"}')"
assert_contains "$out" 'percent 0-100'               "set_volume: non-numeric -> error"

out="$(call set_volume '{}')"
assert_contains "$out" 'percent 0-100'               "set_volume: missing -> error"

out="$(call set_volume '{"percent":50}')"
assert_contains "$out" '"volume":50'                 "set_volume: 50 -> volume 50"

out="$(call set_volume '{"percent":150}')"
assert_contains "$out" '"volume":100'                "set_volume: 150 -> clamped to 100"

# --- set_group ------------------------------------------------------------------
out="$(call set_group '{"role":"bogus"}')"
assert_contains "$out" 'role solo|primary|member'    "set_group: bad role -> error"

out="$(call set_group '{"role":"member","server":"10.0.0.5"}')"
log="$(cat "$LOG")"
assert_contains "$out" '"role":"member"'             "set_group: member -> ok"
assert_contains "$log" 'beep.main.group_role=member' "set_group: role persisted"
assert_contains "$log" 'beep.main.group_server=10.0.0.5' "set_group: server persisted"

out="$(call set_group '{"role":"solo"}')"
assert_contains "$out" '"role":"solo"'               "set_group: solo -> ok"
assert_contains "$(cat "$LOG")" 'beep.main.group_role=solo' "set_group: role persisted"

# --- set_airplay_mode -----------------------------------------------------------
out="$(call set_airplay_mode '{"mode":"x"}')"
assert_contains "$out" 'mode ap1|ap2'                "set_airplay_mode: bad mode -> error"

# Apply helper isn't present off-device: this "not in this image" guard is the branch we
# CAN pin here; the actual apply needs the on-device /usr/libexec/beep/beep-airplay-mode.
out="$(call set_airplay_mode '{"mode":"ap2"}')"
assert_contains "$out" 'airplay mode switch not in this image' "set_airplay_mode: no helper -> guarded"

# --- set_lms ----------------------------------------------------------------
out="$(call set_lms '{"enabled":"2"}')"
assert_contains "$out" 'enabled 0|1'                 "set_lms: bad enabled -> error"

out="$(call set_lms '{"enabled":1,"server":"bad host!"}')"
assert_contains "$out" 'hostname or IP'              "set_lms: bad server chars -> error"

# Init script isn't present off-device: this "not in this image" guard is the branch we
# CAN pin here; enable/disable needs the on-device /etc/init.d/squeezelite.
out="$(call set_lms '{"enabled":1,"server":"lms.local"}')"
assert_contains "$out" 'LMS player not in this image' "set_lms: valid server, no helper -> guarded"

out="$(call set_lms '{"enabled":0,"server":"bad!"}')"
case "$out" in
	*'hostname or IP'*) found=yes ;;
	*)                  found=no ;;
esac
assert_eq "no" "$found"                              "set_lms: disable skips server validation"
assert_contains "$out" 'not in this image'           "set_lms: disable still hits the image guard"

# --- set_password -----------------------------------------------------------
out="$(call set_password '{"password":"abc12"}')"
assert_contains "$out" 'min 6 chars'                 "set_password: too short -> error"

out="$(printf '{"password":"secret6"}' | CHPASSWD_LOG="$tmp/pw" sh "$RPCD" call set_password)"
assert_contains "$out" '"ok":true'                   "set_password: chpasswd succeeds -> ok"
assert_contains "$(cat "$tmp/pw")" 'root:secret6'    "set_password: chpasswd fed root:password"

out="$(printf '{"password":"secret6"}' | CHPASSWD_RC=1 sh "$RPCD" call set_password)"
assert_contains "$out" 'could not set password'      "set_password: chpasswd+passwd both fail -> error"

# --- set_ssh ------------------------------------------------------------------
out="$(call set_ssh '{"mode":"x"}')"
assert_contains "$out" 'mode off|on|temp'            "set_ssh: bad mode -> error"

out="$(call set_ssh '{"mode":"off"}')"
assert_contains "$out" '"mode":"off"'                "set_ssh: off -> ok"
assert_contains "$(cat "$LOG")" 'beep.main.ssh_mode=off' "set_ssh: off persisted"

out="$(call set_ssh '{"mode":"on"}')"
assert_contains "$out" '"mode":"on"'                 "set_ssh: on -> ok"
assert_contains "$(cat "$LOG")" 'beep.main.ssh_mode=on'  "set_ssh: on persisted"

out="$(call set_ssh '{"mode":"temp","hours":5}')"
log="$(cat "$LOG")"
assert_contains "$out" '"mode":"temp"'               "set_ssh: temp -> ok"
assert_contains "$out" '"expires":'                  "set_ssh: temp -> expires present"
assert_contains "$log" 'beep.main.ssh_mode=temp'     "set_ssh: temp mode persisted"
assert_contains "$log" 'beep.main.ssh_expires='      "set_ssh: temp expiry persisted"

# --- set_session_timeout (bonus: pure validation, zero side effects) ---------
out="$(call set_session_timeout '{"minutes":"x"}')"
assert_contains "$out" 'minutes must be 1-1440'      "set_session_timeout: non-numeric -> error"

out="$(call set_session_timeout '{"minutes":0}')"
assert_contains "$out" '1-1440'                      "set_session_timeout: out of range -> error"

out="$(call set_session_timeout '{"minutes":45}')"
assert_contains "$out" '"session_timeout_min":45'    "set_session_timeout: valid -> ok"
assert_contains "$(cat "$LOG")" 'beep.main.session_timeout_min=45' "set_session_timeout: persisted"

t_done
