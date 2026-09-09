#!/bin/bash
set -e
command -v unsquashfs >/dev/null || { apt-get update -qq && apt-get install -y -qq squashfs-tools; } >/dev/null 2>&1
cd /tmp
rm -rf rf
unsquashfs -d rf mtd_rootfs.bin >/tmp/unsq.log 2>&1 && echo "unsq_exit=0" || echo "unsq_exit=$?"
grep -iE 'error|failed|fatal' /tmp/unsq.log | head -3 || true
echo '--- codec libs: size + type ---'
for f in usr/lib/libfdk-aac.so.0.0.3 usr/lib/libFLAC.so.8.2.0 usr/lib/libmad.so.0.2.1 usr/lib/libvorbisidec.so.1.0.3; do
  sz=$(stat -c%s "/tmp/rf/$f" 2>/dev/null || echo NA)
  ty=$(file -b "/tmp/rf/$f" 2>/dev/null | cut -c1-24)
  printf '%-34s %8s  %s\n' "$f" "$sz" "$ty"
done
# build a dereferenced sysroot tar for playnet
cd /tmp/rf
tar -czhf /tmp/pn-full.tgz lib usr/lib beep/platform 2>/dev/null
echo "sysroot tar: $(( $(wc -c < /tmp/pn-full.tgz)/1024 )) KB"
