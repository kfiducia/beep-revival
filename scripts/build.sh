#!/bin/bash
# build.sh — produce the Beep custom OpenWrt image. Runs INSIDE the Debian
# container (docker exec -it beep-build bash /src/scripts/build.sh) after the
# toolchain from docker-build-setup.sh has finished.
#
# Integration approach: ride the upstream 8dev_carambola2 device profile
# (its flash layout already matches the Beep) but swap in our device tree
# (adds I2S/WM8524 + i2c-gpio STM8 + setup key) and layer our feed + files.
set -e
OW=/build/openwrt
SRC=/src
cd "$OW"

echo "== 1. local package feed =="
grep -q 'src-link beepfeed' feeds.conf.default || echo "src-link beepfeed $SRC/feed" >> feeds.conf.default
./scripts/feeds update beepfeed >/dev/null
./scripts/feeds install -a -p beepfeed >/dev/null

echo "== 2. swap in our device tree (keep carambola2 board-name for sysupgrade) =="
DTS="$OW/target/linux/ath79/dts/ar9331_8dev_carambola2.dts"
cp "$SRC/dts/ar9331_beep_dial.dts" "$DTS"
sed -i 's/"beep,dial"/"8dev,carambola2"/' "$DTS"   # board_name match

echo "== 3. bake in the rootfs overlay =="
mkdir -p "$OW/files"
cp -a "$SRC/rootfs-overlay/." "$OW/files/"

echo "== 4. package selection =="
# Write a FRESH .config each build (>, not >>). Appending accumulated stale
# selections across builds (e.g. libffmpeg-full pulled in by a since-removed
# shairport), and `make defconfig` preserves already-set package symbols, so a
# 'lean' rebuild kept shipping ~12MB of ffmpeg. Starting clean avoids that.
cat > .config <<CFG
CONFIG_TARGET_ath79=y
CONFIG_TARGET_ath79_generic=y
CONFIG_TARGET_ath79_generic_DEVICE_8dev_carambola2=y
# our packages
CONFIG_PACKAGE_beepd=y
CONFIG_PACKAGE_kmod-beep-i2s=y
# audio: codec + machine glue + ALSA
CONFIG_PACKAGE_kmod-sound-core=y
CONFIG_PACKAGE_kmod-sound-soc-core=y
CONFIG_PACKAGE_kmod-sound-soc-wm8524=y
CONFIG_PACKAGE_kmod-sound-soc-simple-card=y
CONFIG_PACKAGE_alsa-utils=y
# control bus + gpio
CONFIG_PACKAGE_kmod-i2c-gpio=y
CONFIG_PACKAGE_i2c-tools=y
CONFIG_PACKAGE_libgpiod=y
# debug loop: rz/sz for fast serial .ko transfer into the running system,
# devmem2 for live MBOX/stereo register peeking (busybox devmem also present)
CONFIG_PACKAGE_lrzsz=y
CONFIG_PACKAGE_devmem2=y
CFG

# Extra userspace (AirPlay + web admin + dnsmasq) — FULL build only.
# LEAN=1 omits these so the initramfs is small enough to RAM-boot on 64MB:
# a ~14MB image decompresses past its load address and clobbers its own
# compressed source (LZMA ERROR 1). The full image still boots fine from FLASH;
# the lean image is purely for zero-risk RAM-boot driver testing.
# NOTE: snapcast is NOT in the 24.10 feeds — deferred either way.
if [ -z "${LEAN:-}" ]; then
cat >> .config <<CFG
#CONFIG_PACKAGE_snapcast-client=y  (not in 24.10 feeds; deferred)
CONFIG_PACKAGE_shairport-sync-mini=y
CONFIG_PACKAGE_uhttpd=y
CONFIG_PACKAGE_uhttpd-mod-ubus=y
CONFIG_PACKAGE_rpcd=y
CONFIG_PACKAGE_px5g-mbedtls=y
CONFIG_PACKAGE_dnsmasq=y
CFG
else
# Explicitly DISABLE (not just omit) — build.sh appends to .config, so a prior
# full build's =y lines are still present; last-wins in kconfig must turn them off.
cat >> .config <<CFG
# CONFIG_PACKAGE_shairport-sync-mini is not set
# CONFIG_PACKAGE_uhttpd is not set
# CONFIG_PACKAGE_uhttpd-mod-ubus is not set
# CONFIG_PACKAGE_rpcd is not set
# CONFIG_PACKAGE_px5g-mbedtls is not set
# CONFIG_PACKAGE_dnsmasq is not set
# --- trim rootfs so the initramfs fits in 64MB RAM (22MB tmpfs deadlocked) ---
# wifi stack: irrelevant to an audio tone test, ~3MB of kernel modules
# CONFIG_PACKAGE_kmod-ath9k is not set
# CONFIG_PACKAGE_kmod-ath9k-common is not set
# CONFIG_PACKAGE_kmod-mac80211 is not set
# CONFIG_PACKAGE_kmod-cfg80211 is not set
# CONFIG_PACKAGE_wpad-basic-mbedtls is not set
# CONFIG_PACKAGE_kmod-owl-loader is not set
# opkg metadata (1.8MB) + ALSA UCM profiles (2MB) — not needed for speaker-test
# CONFIG_PACKAGE_opkg is not set
# CONFIG_PACKAGE_alsa-ucm-conf is not set
CFG
  echo "   [LEAN] minimal RAM-boot test image: audio only, no wifi/opkg/ucm (fit 64MB tmpfs)"
fi
make defconfig >/dev/null

echo "== 5. build (this is the long one) =="
# Wipe the rootfs staging so DESELECTED packages' files can't linger — OpenWrt
# only ADDS to an existing root-*, never removes, so a shrunk package set would
# otherwise still bundle the old (deselected) files.
rm -rf "$OW"/build_dir/target-*/root-* 2>/dev/null || true
set -o pipefail   # else the pipe's exit = tee/tail, masking a make failure
make -j"$(nproc)" 2>&1 | tee /build/image-build.log | tail -1
MAKE_RC=$?
[ "$MAKE_RC" -eq 0 ] || { echo "!! make FAILED (rc=$MAKE_RC) — see /build/image-build.log"; exit "$MAKE_RC"; }

# LEAN: surgically drop rootfs fat that's a hard dependency (can't deselect) but
# useless for a tone test — the ALSA UCM profiles (~2MB, alsa-ucm-conf is pulled
# by alsa-utils) and opkg metadata. Keeps /usr/share/alsa/alsa.conf so aplay
# still works. Then rebuild ONLY the initramfs from the trimmed rootfs. This is
# what got the tmpfs from 22MB (OOM-deadlock) down to ~14MB (boots on 64MB).
if [ -n "${LEAN:-}" ]; then
  echo "   [LEAN] trimming UCM/opkg from rootfs + rebuilding initramfs"
  for R in "$OW"/build_dir/target-*/root-*; do
    rm -rf "$R/usr/share/alsa/ucm2" "$R/usr/share/alsa/ucm" "$R/usr/lib/opkg"
  done
  make target/linux/install >>/build/image-build.log 2>&1 || echo "!! initramfs re-link failed"
fi
echo "== images =="
ls -la "$OW"/bin/targets/ath79/generic/*8dev_carambola2*.bin 2>/dev/null || \
  echo "(no image yet — check /build/image-build.log; some kmod/pkg names may need a menuconfig pass)"
