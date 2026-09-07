#!/bin/sh
# sign-release.sh — package a Beep firmware image for the signed web OTA.
#
# Produces a single "signed .bin": the usign detached signature (2 lines) prepended
# to the sysupgrade image. Upload that ONE file in the web UI's "Update firmware"
# panel — the browser splits it (sig = first two lines, image = the rest), sends the
# signature in the URL query and the image in the body, and the device verifies the
# signature against the image with /etc/beep-ota.pub before flashing. (uhttpd drops
# custom request headers to CGI, so the signature can't ride in a header.)
#
# Usage: sign-release.sh <image.bin> <key.sec> [out.signed.bin]
#   USIGN=/path/to/usign  overrides the usign binary (OpenWrt: staging_dir/host/bin/usign)
set -e
IMG="$1"; KEY="$2"; OUT="${3:-${IMG%.bin}.signed.bin}"
USIGN="${USIGN:-usign}"
[ -f "$IMG" ] && [ -f "$KEY" ] || { echo "usage: $0 <image.bin> <key.sec> [out.signed.bin]" >&2; exit 1; }
SIG="$(mktemp)"
"$USIGN" -S -m "$IMG" -s "$KEY" -x "$SIG"
# Prepend the signature, guaranteeing exactly one trailing newline after it so the
# client's "first two lines" split lands exactly at the start of the image.
{ cat "$SIG"; [ -n "$(tail -c1 "$SIG")" ] && echo; cat "$IMG"; } > "$OUT"
rm -f "$SIG"
echo "wrote $OUT ($(wc -c < "$OUT") bytes) — upload this single file in the web UI"
