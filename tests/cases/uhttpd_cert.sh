#!/bin/sh
# TLS cert generation for uhttpd (usr/libexec/beep/gen-uhttpd-cert).
#
# Regression for GitHub issue #105: the per-device cert MUST carry a non-empty SAN, or
# modern browsers reject it as ERR_CERT_INVALID — a hard, non-proceedable error that made
# the admin UI unreachable over HTTPS. px5g emits the SAN extension unconditionally, so
# "no -addext subjectAltName" is exactly the empty-SAN bug. We also cover the version-marker
# self-heal: because the cert is on the sysupgrade keep-list, an already-provisioned unit
# keeps its old empty-SAN cert across an update — the marker forces a one-time regen.
#
# Drives the real helper with a px5g stub ($PX5G) that records its argv and a file-backed
# uci stub for the hostname lookup.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="uhttpd-cert"
t_use_stubs

GEN="$REPO_ROOT/rootfs-overlay/usr/libexec/beep/gen-uhttpd-cert"
t_tmpdir; tmp="$T_TMP"
export BEEP_ETC="$tmp/etc"; mkdir -p "$BEEP_ETC"
export PX5G="$T_STUBS/px5g"
export PX5G_ARGS_LOG="$tmp/px5g.args"
export UCI_STATE_DIR="$tmp/uci"; mkdir -p "$UCI_STATE_DIR"

MAC6="023545"

ran() { if [ -f "$PX5G_ARGS_LOG" ]; then echo yes; else echo no; fi; }

# --- fresh device: generates a cert WITH a SAN and writes the version marker ---
sh "$GEN" "$MAC6"
args="$(cat "$PX5G_ARGS_LOG" 2>/dev/null)"
assert_contains "$args" "-addext"                                "px5g invoked with an -addext SAN (not CN-only — the #105 bug)"
assert_contains "$args" "subjectAltName=DNS:beep-${MAC6}"        "SAN includes the stable device DNS name"
assert_contains "$args" "subjectAltName=DNS:beep-${MAC6}.local"  "SAN includes the mDNS .local name"
newkey_val="$(awk '/^-newkey$/{getline; print; exit}' "$PX5G_ARGS_LOG")"
assert_eq "ec" "$newkey_val"                                     "cert uses an EC key (RSA-2048 TLS is too slow on the AR9331)"
assert_eq "3" "$(cat "$BEEP_ETC/beep-cert-ver" 2>/dev/null)"     "version marker written after generation"
if [ -f "$BEEP_ETC/beep-uhttpd.crt" ]; then _crt=yes; else _crt=no; fi
assert_eq "yes" "$_crt"                                          "cert file created"

# --- idempotent: current cert + matching marker -> no regen (px5g not called again) ---
rm -f "$PX5G_ARGS_LOG"
sh "$GEN" "$MAC6"
assert_eq "no" "$(ran)"                                          "current cert (marker matches) is not regenerated"

# --- self-heal: a kept cert with a stale/missing marker IS regenerated one time ---
rm -f "$BEEP_ETC/beep-cert-ver"        # simulate a unit updated from pre-#105 firmware
sh "$GEN" "$MAC6"
assert_eq "yes" "$(ran)"                                         "missing version marker forces a one-time regen"
assert_eq "3" "$(cat "$BEEP_ETC/beep-cert-ver" 2>/dev/null)"     "regen refreshes the version marker"

# --- renamed device: SAN also covers the current hostname's .local ---
rm -f "$BEEP_ETC/beep-cert-ver"
export UCIGET_system__system_0__hostname="copper"   # uci stub returns this for system.@system[0].hostname
sh "$GEN" "$MAC6"
args="$(cat "$PX5G_ARGS_LOG" 2>/dev/null)"
assert_contains "$args" "subjectAltName=DNS:copper.local"        "renamed unit: SAN covers <hostname>.local"
assert_contains "$args" "subjectAltName=DNS:beep-${MAC6}"        "renamed unit: still keeps the stable MAC name"

# --- px5g failure: no version marker written (so the next boot retries) ---
rm -f "$BEEP_ETC/beep-cert-ver"
export PX5G_FAIL=1
sh "$GEN" "$MAC6"
if [ -f "$BEEP_ETC/beep-cert-ver" ]; then _mk=yes; else _mk=no; fi
assert_eq "no" "$_mk"                                            "px5g failure does not write the version marker"
unset PX5G_FAIL

t_done
