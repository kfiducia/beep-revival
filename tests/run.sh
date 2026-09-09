#!/bin/sh
# Run every tests/cases/*.sh in its own process and aggregate the result.
# Exit 0 iff all cases pass. No dependencies beyond a POSIX sh + coreutils.
#
#   ./tests/run.sh            # run all cases
#   ./tests/run.sh volume     # run only cases whose filename matches *volume*
set -u

REPO_ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
export REPO_ROOT
filter="${1:-}"

fails=0
ran=0
for case in "$REPO_ROOT"/tests/cases/*.sh; do
	[ -e "$case" ] || { echo "no test cases found"; exit 1; }
	name="$(basename "$case" .sh)"
	case "$name" in *"$filter"*) ;; *) continue ;; esac
	ran=$((ran + 1))
	printf '\n=== %s ===\n' "$name"
	if sh "$case"; then :; else fails=$((fails + 1)); fi
done

printf '\n========================================\n'
if [ "$fails" -eq 0 ]; then
	printf 'ALL %d CASE FILE(S) PASSED\n' "$ran"
	exit 0
fi
printf '%d of %d CASE FILE(S) FAILED\n' "$fails" "$ran"
exit 1
