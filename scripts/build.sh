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
cat >> .config <<CFG
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
# audio sources (lean default per RESEARCH-SYNTHESIS.md; others via opkg later)
CONFIG_PACKAGE_snapcast-client=y
CONFIG_PACKAGE_shairport-sync-mini=y
# admin/provisioning
CONFIG_PACKAGE_uhttpd=y
CONFIG_PACKAGE_uhttpd-mod-ubus=y
CONFIG_PACKAGE_rpcd=y
CONFIG_PACKAGE_px5g-mbedtls=y
CONFIG_PACKAGE_dnsmasq=y
CFG
make defconfig >/dev/null

echo "== 5. build (this is the long one) =="
make -j"$(nproc)" 2>&1 | tee /build/image-build.log | tail -1
echo "== images =="
ls -la "$OW"/bin/targets/ath79/generic/*8dev_carambola2*.bin 2>/dev/null || \
  echo "(no image yet — check /build/image-build.log; some kmod/pkg names may need a menuconfig pass)"
