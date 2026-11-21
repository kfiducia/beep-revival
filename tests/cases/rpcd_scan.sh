#!/bin/sh
# The rpcd/beep `scan` verb (rootfs-overlay/usr/libexec/rpcd/beep) — the Wi-Fi network
# list the setup UI renders. iwinfo scan output is untrusted radio input (a neighbor's
# SSID), so the awk parsing + JSON escaping there is the thing that must never break
# JSON.parse in the browser. We drive the real rpcd object (`call scan`) against a fake
# `iwinfo` (tests/stubs/iwinfo) fed hand-built fixtures in the real iwinfo scan layout.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="rpcd-scan"
t_use_stubs

RPCD="$REPO_ROOT/rootfs-overlay/usr/libexec/rpcd/beep"
t_tmpdir; tmp="$T_TMP"

scanf() { IWINFO_SCAN_FILE="$1" sh "$RPCD" call scan </dev/null; }

# hidden SSIDs ("ESSID: unknown") deliberately not covered — known bug, see BEP40

# --- A: ESSID/Signal/Encryption extraction + enc true/false -------------------
fA="$tmp/scanA"
{
	printf '%s\n' 'Cell 01 - Address: 02:00:00:00:00:01'
	printf '%s\n' '          ESSID: "HomeNet"'
	printf '%s\n' '          Mode: Master  Frequency: 2.437 GHz  Band: 2.4 GHz  Channel: 6'
	printf '%s\n' '          Signal: -52 dBm  Quality: 58/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 02 - Address: 02:00:00:00:00:02'
	printf '%s\n' '          ESSID: "CoffeeShop"'
	printf '%s\n' '          Mode: Master  Frequency: 2.442 GHz  Band: 2.4 GHz  Channel: 7'
	printf '%s\n' '          Signal: -71 dBm  Quality: 30/70'
	printf '%s\n' '          Encryption: none'
	printf '%s\n' ''
	printf '%s\n' 'Cell 03 - Address: 02:00:00:00:00:03'
	printf '%s\n' '          ESSID: "OldRouter"'
	printf '%s\n' '          Mode: Master  Frequency: 2.447 GHz  Band: 2.4 GHz  Channel: 8'
	printf '%s\n' '          Signal: -80 dBm  Quality: 10/70'
	printf '%s\n' '          Encryption: WEP Open System (WEP-40)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 04 - Address: 02:00:00:00:00:04'
	printf '%s\n' '          ESSID: "Modern"'
	printf '%s\n' '          Mode: Master  Frequency: 5.180 GHz  Band: 5 GHz  Channel: 36'
	printf '%s\n' '          Signal: -60 dBm  Quality: 50/70'
	printf '%s\n' '          Encryption: WPA3 SAE (CCMP)'
} > "$fA"
out="$(scanf "$fA")"
assert_eq '{"networks":[{"ssid":"HomeNet","signal":-52,"enc":true},{"ssid":"CoffeeShop","signal":-71,"enc":false},{"ssid":"OldRouter","signal":-80,"enc":true},{"ssid":"Modern","signal":-60,"enc":true}]}' \
	"$out" "extraction + enc true/false"

# --- B: dedupe, first one wins -------------------------------------------------
fB="$tmp/scanB"
{
	printf '%s\n' 'Cell 01 - Address: 02:00:00:00:00:11'
	printf '%s\n' '          ESSID: "HomeNet"'
	printf '%s\n' '          Mode: Master  Frequency: 2.412 GHz  Band: 2.4 GHz  Channel: 1'
	printf '%s\n' '          Signal: -60 dBm  Quality: 50/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 02 - Address: 02:00:00:00:00:12'
	printf '%s\n' '          ESSID: "HomeNet"'
	printf '%s\n' '          Mode: Master  Frequency: 2.417 GHz  Band: 2.4 GHz  Channel: 2'
	printf '%s\n' '          Signal: -40 dBm  Quality: 68/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 03 - Address: 02:00:00:00:00:13'
	printf '%s\n' '          ESSID: "Guest"'
	printf '%s\n' '          Mode: Master  Frequency: 2.422 GHz  Band: 2.4 GHz  Channel: 3'
	printf '%s\n' '          Signal: -65 dBm  Quality: 40/70'
	printf '%s\n' '          Encryption: none'
	printf '%s\n' ''
	printf '%s\n' 'Cell 04 - Address: 02:00:00:00:00:14'
	printf '%s\n' '          ESSID: "HomeNet"'
	printf '%s\n' '          Mode: Master  Frequency: 2.427 GHz  Band: 2.4 GHz  Channel: 4'
	printf '%s\n' '          Signal: -45 dBm  Quality: 60/70'
	printf '%s\n' '          Encryption: none'
} > "$fB"
out="$(scanf "$fB")"
assert_eq '{"networks":[{"ssid":"HomeNet","signal":-60,"enc":true},{"ssid":"Guest","signal":-65,"enc":false}]}' \
	"$out" "duplicate ESSID collapsed, first one wins"

# --- C: signal null fallback (missing or unparsable Signal line) --------------
fC="$tmp/scanC"
{
	printf '%s\n' 'Cell 01 - Address: 02:00:00:00:00:21'
	printf '%s\n' '          ESSID: "Strong"'
	printf '%s\n' '          Mode: Master  Frequency: 2.412 GHz  Band: 2.4 GHz  Channel: 1'
	printf '%s\n' '          Signal: -50 dBm  Quality: 60/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 02 - Address: 02:00:00:00:00:22'
	printf '%s\n' '          ESSID: "NoSig"'
	printf '%s\n' '          Mode: Master  Frequency: 2.417 GHz  Band: 2.4 GHz  Channel: 2'
	printf '%s\n' '          Encryption: none'
	printf '%s\n' ''
	printf '%s\n' 'Cell 03 - Address: 02:00:00:00:00:23'
	printf '%s\n' '          ESSID: "BadSig"'
	printf '%s\n' '          Mode: Master  Frequency: 2.422 GHz  Band: 2.4 GHz  Channel: 3'
	printf '%s\n' '          Signal: unknown  Quality: unknown/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
} > "$fC"
out="$(scanf "$fC")"
assert_eq '{"networks":[{"ssid":"Strong","signal":-50,"enc":true},{"ssid":"NoSig","signal":null,"enc":false},{"ssid":"BadSig","signal":null,"enc":true}]}' \
	"$out" "signal falls back to null (missing/unparsable); resets per ESSID"

# --- D: quote escaping + control-byte stripping --------------------------------
fD="$tmp/scanD"
{
	printf '%s\n' 'Cell 01 - Address: 02:00:00:00:00:31'
	printf '%s\n' '          ESSID: "say "hi""'
	printf '%s\n' '          Mode: Master  Frequency: 2.412 GHz  Band: 2.4 GHz  Channel: 1'
	printf '%s\n' '          Signal: -55 dBm  Quality: 55/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	printf '%s\n' ''
	printf '%s\n' 'Cell 02 - Address: 02:00:00:00:00:32'
	printf '          ESSID: "Tab\tEsc\033Bell\007"\n'
	printf '%s\n' '          Mode: Master  Frequency: 2.417 GHz  Band: 2.4 GHz  Channel: 2'
	printf '%s\n' '          Signal: -56 dBm  Quality: 54/70'
	printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
} > "$fD"
out="$(scanf "$fD")"
assert_eq '{"networks":[{"ssid":"say \"hi\"","signal":-55,"enc":true},{"ssid":"TabEscBell","signal":-56,"enc":true}]}' \
	"$out" "embedded quote escaped + control bytes stripped"
assert_eq "" "$(printf '%s' "$out" | tr -d '\040-\176')" "scan output is printable ASCII only"

# --- E: backslash escaping (only when the host awk escapes like BusyBox) ------
probe="$(printf '%s\n' 'a\b' | awk '{gsub(/\\/,"\\\\"); print}')"
if [ "$probe" = 'a\\b' ]; then
	fE="$tmp/scanE"
	{
		printf '%s\n' 'Cell 01 - Address: 02:00:00:00:00:41'
		printf '%s\n' '          ESSID: "back\slash"'
		printf '%s\n' '          Mode: Master  Frequency: 2.412 GHz  Band: 2.4 GHz  Channel: 1'
		printf '%s\n' '          Signal: -50 dBm  Quality: 60/70'
		printf '%s\n' '          Encryption: WPA2 PSK (CCMP)'
	} > "$fE"
	out="$(scanf "$fE")"
	assert_eq '{"networks":[{"ssid":"back\\slash","signal":-50,"enc":true}]}' "$out" "embedded backslash escaped"
else
	printf '  skip %s\n' "backslash escaping: host awk collapses \\\\ (device busybox awk does not)"
fi

# --- F: empty scan --------------------------------------------------------------
out="$(scanf "")"
assert_eq '{"networks":[]}' "$out" "no IWINFO_SCAN_FILE -> empty scan"

fF="$tmp/scanF"
printf '%s\n' 'No scan results' > "$fF"
out="$(scanf "$fF")"
assert_eq '{"networks":[]}' "$out" "iwinfo 'No scan results' -> empty scan"

t_done
