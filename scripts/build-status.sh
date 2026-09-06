#!/bin/bash
# Runs ON the iMac. Prints a one-line status of the beep-build container's progress.
export PATH=/usr/local/bin:$PATH
docker exec beep-build bash -c '
  G=/build/openwrt/bin/targets/ath79/generic
  IMG=$(ls $G/*8dev_carambola2*sysupgrade*.bin 2>/dev/null | head -1)
  [ -n "$IMG" ] && { echo "IMAGE_READY $IMG"; exit; }
  grep -qE "make\[[0-9]+\]: \*\*\* .*Error [0-9]" /build/build.log 2>/dev/null && { echo "BUILD_FAILED"; exit; }
  grep -q ALL-DONE /build/build.log 2>/dev/null && { echo "DONE_NO_IMAGE"; exit; }
  if ! ls -d /build/openwrt >/dev/null 2>&1; then
    grep -qiE "^E:|Unable to locate|no installation candidate" /build/setup.log 2>/dev/null && { echo "SETUP_FAILED"; exit; }
  fi
  if [ -f /build/build.log ]; then echo "IMAGE_STAGE ($(wc -l </build/build.log 2>/dev/null) log lines)"
  elif grep -q "TOOLCHAIN DONE" /build/setup.log 2>/dev/null; then echo "TOOLCHAIN_DONE (image next)"
  elif [ -f /build/toolchain.log ]; then echo "TOOLCHAIN_BUILDING ($(wc -l </build/toolchain.log 2>/dev/null) lines)"
  elif ls -d /build/openwrt >/dev/null 2>&1; then echo "FEEDS_OR_CONFIG"
  else echo "APT_OR_CLONE"; fi
' 2>/dev/null
