#!/bin/bash
# playnet RE harness v3: dummy_audio_output=1 -> skip beepi2s -> reach ubus + sync/net.
S=/tmp/pnsys
rm -rf "$S" && mkdir -p "$S" && tar -xzf /tmp/pn-sys.tgz -C "$S"
# enable dummy audio so it doesn't fork the hardware beepi2s
sed -i "/config main 'main'/a\\	option dummy_audio_output '1'" "$S/etc/config/beep_data"
echo "=== beep_data now ==="; cat "$S/etc/config/beep_data"
echo "=== RUN playnet under qemu-mips -strace (bounded 15s) ==="
timeout 15 qemu-mips -strace -L "$S" -E LD_LIBRARY_PATH=/beep/platform:/usr/lib:/lib \
  "$S/beep/platform/playnet" >/tmp/pn3-out.log 2>/tmp/pn3-strace.log || echo "(exit/timeout $?)"
echo "=== playnet init narrative (its own DEBUG/INFO/ERROR) ==="
grep -aE 'playnet -' /tmp/pn3-strace.log | sed 's/\x1b\[[0-9;]*m//g' | sed 's/.*playnet -/playnet -/' | head -60
echo "=== NETWORK/ubus syscalls (sync + discovery + control) ==="
grep -aE '(socket|bind|listen|connect|accept|setsockopt|sendto|recvfrom|getsockname)\(' /tmp/pn3-strace.log | sed 's/\x1b\[[0-9;]*m//g' | head -40
echo "=== ubus / control socket paths it opened ==="
grep -aE 'open(at)?\(|connect\(' /tmp/pn3-strace.log | grep -aiE 'ubus|\.sock|/var/run' | head
echo "=== syscall count / tail ==="; wc -l < /tmp/pn3-strace.log; tail -8 /tmp/pn3-strace.log | sed 's/\x1b\[[0-9;]*m//g'
