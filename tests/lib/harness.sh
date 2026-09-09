# shellcheck shell=sh
# Tiny zero-dependency POSIX-sh test harness for the Beep firmware shell logic.
#
# Why not bats? Everything under test is #!/bin/sh (BusyBox ash). A hand-rolled sh
# harness runs identically on a dev Mac, an Ubuntu CI runner, and a unit, with no
# install step and no version drift. Each case file sources this, makes assertions,
# and calls t_done at the end (which sets the exit code from the failure count).
#
# Assertions:
#   assert_eq       EXPECTED ACTUAL      [MSG]
#   assert_contains HAYSTACK NEEDLE      [MSG]
#   assert_status   EXPECTED_CODE  CMD...   (runs CMD, compares $?)
#
# Stubs: t_use_stubs prepends tests/stubs to PATH so fake amixer/ubus/usign/... shadow
# the real tools. Stub behavior is driven by environment variables (see each stub).

T_PASS=0
T_FAIL=0
T_NAME="${T_NAME:-$(basename "${0:-tests}")}"

# Locate the repo root so cases can run standalone (REPO_ROOT wins if run.sh set it).
: "${REPO_ROOT:=$(CDPATH= cd -- "$(dirname -- "$0")/../.." 2>/dev/null && pwd)}"
T_STUBS="$REPO_ROOT/tests/stubs"

_t_ok()   { T_PASS=$((T_PASS + 1)); printf '  ok   %s\n' "$1"; }
_t_bad()  { T_FAIL=$((T_FAIL + 1)); printf '  FAIL %s\n' "$1"; }

assert_eq() {
	_e="$1"; _a="$2"; _m="${3:-eq}"
	if [ "$_e" = "$_a" ]; then _t_ok "$_m"
	else _t_bad "$_m"; printf '       expected: [%s]\n       actual:   [%s]\n' "$_e" "$_a"; fi
}

assert_contains() {
	_h="$1"; _n="$2"; _m="${3:-contains}"
	case "$_h" in
		*"$_n"*) _t_ok "$_m" ;;
		*) _t_bad "$_m"; printf '       needle:   [%s]\n       haystack: [%s]\n' "$_n" "$_h" ;;
	esac
}

# assert_status EXPECTED CMD... — run CMD, compare its exit code.
assert_status() {
	_e="$1"; shift
	"$@"; _a=$?
	assert_eq "$_e" "$_a" "exit=$_e for: $*"
}

t_use_stubs() { PATH="$T_STUBS:$PATH"; export PATH; }

# Fresh temp dir for the case, auto-removed when the case exits. Call directly (NOT in a
# $(...) subshell, or the cleanup trap would fire in the subshell and delete it early):
#   t_tmpdir; tmp="$T_TMP"
t_cleanup() { [ -n "${T_TMP:-}" ] && rm -rf "$T_TMP"; }
t_tmpdir() {
	T_TMP="$(mktemp -d "${TMPDIR:-/tmp}/beeptest.XXXXXX")"
	trap 't_cleanup' EXIT INT TERM
}

t_done() {
	printf '%s: %d passed, %d failed\n' "$T_NAME" "$T_PASS" "$T_FAIL"
	[ "$T_FAIL" -eq 0 ]
}
