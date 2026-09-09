#!/bin/bash
# playnet safe RE harness: run under qemu-mips user-mode with strace, no hardware.
S=/tmp/pnsys
rm -rf "$S" && mkdir -p "$S" && tar -xzf /tmp/pn-sys.tgz -C "$S"
echo "=== playnet ELF interp/needed ==="
/build/openwrt/staging_dir/toolchain-mips_24kc_*/bin/mips-openwrt-linux-objdump -p "$S/beep/platform/playnet" 2>/dev/null | grep -E 'INTERP|NEEDED' | head
echo "=== RUN under qemu-mips -strace (bounded 12s) ==="
timeout 12 qemu-mips -strace -L "$S" \
  -E LD_LIBRARY_PATH=/beep/platform:/usr/lib:/lib \
  "$S/beep/platform/playnet" >/tmp/pn-stdout.log 2>/tmp/pn-strace.log || echo "(exited/timed out: $?)"
echo "=== playnet own stdout/stderr ==="
cat /tmp/pn-stdout.log 2>/dev/null | head -30
echo "=== config / ubus / device / socket touches (what it expects) ==="
grep -aE '(open|openat|access|stat|connect|bind|socket)\(' /tmp/pn-strace.log 2>/dev/null \
  | grep -aiE 'ubus|/etc/config|beep|/dev/|\.sock|/var/run|/tmp/' | head -40
echo "=== tail: last syscalls before it stopped ==="
tail -20 /tmp/pn-strace.log 2>/dev/null
echo "=== strace line count (how far it got) ==="; wc -l < /tmp/pn-strace.log
