#!/bin/sh
# beep-lms-ctl player-lookup + server-resolution logic. The fragile part is parsing the
# LMS `players` line: it's ONE space-joined, URL-encoded string whose values (the MAC!)
# themselves contain %3A, so we must split on the FIRST %3A only and pick the id from the
# per-player block whose `name` matches our hostname. Fixtures below are the real shape
# captured from LMS 9.1.1. Sourced in lib mode (BEEP_LMS_LIB=1); no network I/O.
: "${REPO_ROOT:=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)}"
. "$REPO_ROOT/tests/lib/harness.sh"
T_NAME="lms-ctl"
t_use_stubs

export BEEP_LMS_UCI="$REPO_ROOT/tests/stubs/uci"        # must be set BEFORE sourcing
export BEEP_LMS_NETSTAT="$REPO_ROOT/tests/stubs/netstat"
BEEP_LMS_LIB=1 . "$REPO_ROOT/rootfs-overlay/usr/libexec/beep/beep-lms-ctl"

# --- parse_pid: one player (the real captured line) ---------------------------
ONE='players 0 100 count%3A1 playerindex%3A0 playerid%3Ac4%3A93%3A00%3A02%3A35%3A45 uuid%3A ip%3A10.9.100.174%3A53252 name%3Abeep-copper seq_no%3A0 model%3Asqueezelite connected%3A1'
assert_eq "c4:93:00:02:35:45" "$(parse_pid "$ONE" beep-copper)" "extract our player id by name"
assert_eq ""                  "$(parse_pid "$ONE" beep-silver)" "no match -> empty (not the wrong id)"

# --- parse_pid: two players, pick the block whose name matches -----------------
TWO='players 0 100 count%3A2 playerindex%3A0 playerid%3Aaa%3Abb%3Acc%3Add%3Aee%3Aff ip%3A10.9.100.1%3A5000 name%3Abeep-silver playerindex%3A1 playerid%3Ac4%3A93%3A00%3A02%3A35%3A45 ip%3A10.9.100.174%3A53252 name%3Abeep-copper'
assert_eq "c4:93:00:02:35:45"  "$(parse_pid "$TWO" beep-copper)" "two players -> copper id"
assert_eq "aa:bb:cc:dd:ee:ff"  "$(parse_pid "$TWO" beep-silver)" "two players -> silver id"

# --- lms_server: pinned server_addr wins --------------------------------------
assert_eq "10.9.100.99" "$(export UCIGET_squeezelite_options_server_addr=10.9.100.99; lms_server)" \
	"pinned server_addr used verbatim"

# --- lms_server: discovery fallback when server_addr is empty -----------------
assert_eq "10.9.100.175" "$(unset UCIGET_squeezelite_options_server_addr; lms_server)" \
	"blank addr -> discover the :3483 peer from netstat"

t_done
