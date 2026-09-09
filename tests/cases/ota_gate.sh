#!/bin/sh
# OTA authorization gate in rootfs-overlay/www/cgi-bin/beep-ota — the security-critical
# logic that decides WHO may flash WHAT. A regression here is a remote-flash hole, so we
# pin every gate decision. We drive the real CGI with request env + stubbed
# ubus/jsonfilter/usign/setsid, and assert the HTTP Status it emits. We deliberately stop
# at each gate (never reach a real flash), so no image/rootfs faking is needed.
#
# Threat model: SIGNED needs a valid signature (+admin session on a primary); UNSIGNED
# needs a FRESH physical triple-tap (the phys-confirm window), which no remote actor has.
: "${REPO_ROOT:=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="ota-gate"
t_use_stubs

OTA="$REPO_ROOT/rootfs-overlay/www/cgi-bin/beep-ota"
t_tmpdir; tmp="$T_TMP"
PC="$tmp/phys-confirm"          # overrides the on-device /var/run/beep/phys-confirm
now="$(date +%s)"

# --- GET: reveals the unsigned option only while a physical window is open ---
out="$(REQUEST_METHOD=GET PHYS_CONFIRM="$tmp/none" sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 200'      "GET returns 200"
assert_contains "$out" '{"phys":false}'   "GET with no phys window -> phys:false"

echo "$((now + 60))" > "$PC"
out="$(REQUEST_METHOD=GET PHYS_CONFIRM="$PC" sh "$OTA" </dev/null)"
assert_contains "$out" '{"phys":true}'    "GET with fresh phys window -> phys:true"

echo "$((now - 10))" > "$PC"
out="$(REQUEST_METHOD=GET PHYS_CONFIRM="$PC" sh "$OTA" </dev/null)"
assert_contains "$out" '{"phys":false}'   "GET with expired phys window -> phys:false"

echo "not-a-number" > "$PC"
out="$(REQUEST_METHOD=GET PHYS_CONFIRM="$PC" sh "$OTA" </dev/null)"
assert_contains "$out" '{"phys":false}'   "GET with garbage phys value -> phys:false"

# --- method guard ---
out="$(REQUEST_METHOD=PUT sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 405'      "non-GET/POST -> 405"

# --- UNSIGNED path: physical presence is mandatory, admin session is NOT sufficient ---
out="$(REQUEST_METHOD=POST QUERY_STRING='unsigned=1' PHYS_CONFIRM="$tmp/none" sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 403'      "unsigned upload without phys triple-tap -> 403"
assert_contains "$out" 'triple-tap'       "403 explains the triple-tap requirement"

echo "$((now + 60))" > "$PC"
out="$(REQUEST_METHOD=POST QUERY_STRING='unsigned=1' PHYS_CONFIRM="$PC" sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 400'      "unsigned + fresh phys but empty body -> 400"
assert_contains "$out" 'empty image'      "400 names the empty image"

# --- SIGNED path: signature required, and admin session on a primary ---
out="$(REQUEST_METHOD=POST QUERY_STRING='' sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 400'      "signed path with no signature -> 400"
assert_contains "$out" 'no signature'     "400 names the missing signature"

# sig present but no admin session (and not a recovery image) -> 401
out="$(REQUEST_METHOD=POST QUERY_STRING='sig=QUJD' sh "$OTA" </dev/null)"
assert_contains "$out" 'Status: 401'      "signed sig without admin session -> 401"

# sig present + admin session, but signature verification FAILS -> 400 (usign stub returns 1)
out="$(printf 'FAKEIMG' | REQUEST_METHOD=POST QUERY_STRING='session=abc123&sig=QUJD' \
	UBUS_ACCESS=true USIGN_RESULT=1 sh "$OTA")"
assert_contains "$out" 'Status: 400'                  "admin + bad signature -> 400"
assert_contains "$out" 'signature verification failed' "400 names the failed verification"

rm -f /tmp/ota.bin /tmp/ota.sig   # the CGI writes these under its default OTA tmp
t_done
