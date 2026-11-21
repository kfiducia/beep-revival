#!/bin/sh
# The Firmware row (`status` -> version in rootfs-overlay/usr/libexec/rpcd/beep). A stamped image
# shows its stamp; an unstamped one must still start with "Beep Revival" (never a bare OpenWrt
# banner). Paths are overridden via BEEP_VERSION_FILE / BEEP_VERSION_ROM / BEEP_OPENWRT_RELEASE.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="version-stamp"
t_use_stubs

RPCD="$REPO_ROOT/rootfs-overlay/usr/libexec/rpcd/beep"
t_tmpdir; tmp="$T_TMP"

# ver <version-file> <rom-file> <openwrt_release> — the version field of the real `status` verb.
ver() { BEEP_VERSION_FILE="$1" BEEP_VERSION_ROM="$2" BEEP_OPENWRT_RELEASE="$3" sh "$RPCD" call status </dev/null | sed -n 's/.*"version":"\([^"]*\)".*/\1/p'; }

NOPE="$tmp/nope"

printf 'Beep Revival v1.5.2' > "$tmp/stamp"
assert_eq "Beep Revival v1.5.2" "$(ver "$tmp/stamp" "$NOPE" "$NOPE")" "stamped primary"

: > "$tmp/empty"
printf 'Beep Revival v1.5.1' > "$tmp/rom"
assert_eq "Beep Revival v1.5.1" "$(ver "$tmp/empty" "$tmp/rom" "$NOPE")" "0-byte primary -> /rom copy"

printf "DISTRIB_DESCRIPTION='OpenWrt 23.05.3 r23809-234f1a2efa'\n" > "$tmp/rel"
out="$(ver "$NOPE" "$NOPE" "$tmp/rel")"
assert_eq "Beep Revival (unstamped; OpenWrt 23.05.3 r23809-234f1a2efa)" "$out" "unstamped -> branded + build"
case "$out" in OpenWrt*) assert_eq "not-OpenWrt" "OpenWrt" "unstamped must not start with OpenWrt" ;; esac

assert_eq "Beep Revival (unstamped)" "$(ver "$NOPE" "$NOPE" "$NOPE")" "unstamped, no release file"

printf "DISTRIB_ID='OpenWrt'\n" > "$tmp/rel2"
assert_eq "Beep Revival (unstamped)" "$(ver "$NOPE" "$NOPE" "$tmp/rel2")" "unstamped, no DISTRIB_DESCRIPTION"

t_done
