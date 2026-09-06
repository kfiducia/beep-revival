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

echo "== 1b. AirPlay 2 on a minimal ffmpeg =="
# shairport-sync hardcodes +libffmpeg-full (~12MB: all video/filters). AirPlay 2
# only needs AAC+ALAC *decode*, which libffmpeg-audio-dec provides at ~2-3MB
# (--enable-small, audio codecs only). Patch the dep so AirPlay 2 keeps working
# but the image shrinks enough to also fit a recovery slot.
SPS="$OW/feeds/packages/sound/shairport-sync/Makefile"
[ -f "$SPS" ] && sed -i 's/+libffmpeg-full/+libffmpeg-audio-dec/g' "$SPS"
# audio-dec --disable-swresamples and doesn't ship libswresample, but shairport
# links it (the resampler). Force swresample ON + into the audio-dec/mini install.
FM="$OW/feeds/packages/multimedia/ffmpeg/Makefile"
if [ -f "$FM" ]; then
  sed -i 's/--disable-swresample/--enable-swresample/g' "$FM"
  sed -i 's/{avcodec,avformat,avutil}/{avcodec,avformat,avutil,swresample}/g' "$FM"
  # audio-dec/install = custom/install, whose unconditional copy is line 740
  # lib{avcodec,avdevice,avformat,avutil}.so.* (swresample only in a conditional
  # line audio-dec never triggers) — add swresample there so the .ipk ships it.
  sed -i 's/lib{avcodec,avdevice,avformat,avutil}\.so\.\*/lib{avcodec,avdevice,avformat,avutil,swresample}.so.*/g' "$FM"
  # AirPlay 2 HARD-requires a FLOAT (FLTP) AAC decoder (shairport's
  # has_fltp_capable_aac_decoder(); else it dies "can not run on this system").
  # The audio-dec preset's decoder list OMITS aac entirely (only alac/flac/...),
  # and the block ends with a --disable-decoder line — append the float aac
  # decoder + parser there so AirPlay 2 works while ffmpeg stays small (~+200KB).
  sed -i 's/--disable-decoder=pcm_bluray,pcm_dvd/--disable-decoder=pcm_bluray,pcm_dvd --enable-decoder=aac --enable-parser=aac/' "$FM"
fi
# we patched both Makefiles → force a clean rebuild so the changes take
make package/feeds/packages/ffmpeg/clean >/dev/null 2>&1 || true
make package/feeds/packages/shairport-sync/clean >/dev/null 2>&1 || true

echo "== 2. swap in our device tree (keep carambola2 board-name for sysupgrade) =="
DTS="$OW/target/linux/ath79/dts/ar9331_8dev_carambola2.dts"
cp "$SRC/dts/ar9331_beep_dial.dts" "$DTS"
sed -i 's/"beep,dial"/"8dev,carambola2"/' "$DTS"   # board_name match

echo "== 3. bake in the rootfs overlay =="
mkdir -p "$OW/files"
cp -a "$SRC/rootfs-overlay/." "$OW/files/"
# dev builds keep SSH enabled (see 99-beep-ssh); shipping builds disable it
if [ -n "${BEEP_DEV:-}" ]; then mkdir -p "$OW/files/etc"; touch "$OW/files/etc/beep-dev"
  echo "   [DEV] BEEP_DEV set — SSH stays enabled on this image"; fi

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
# AirPlay 2: use the mbedtls variant (it links avahi, so it actually ADVERTISES;
# the mini variant uses tinysvcmdns which CANNOT advertise AirPlay 2 → invisible).
# ffmpeg is swapped full→audio-dec below (AAC+ALAC only, ~2-3MB vs ~12MB) so
# AirPlay 2 stays functional AND a ~5MB recovery slot still fits in 16MB flash.
CONFIG_PACKAGE_shairport-sync-mbedtls=y
CONFIG_PACKAGE_avahi-daemon=y
CONFIG_PACKAGE_libffmpeg-audio-dec=y
# CONFIG_PACKAGE_libffmpeg-full is not set
CONFIG_PACKAGE_uhttpd=y
CONFIG_PACKAGE_uhttpd-mod-ubus=y
CONFIG_PACKAGE_rpcd=y
CONFIG_PACKAGE_px5g-mbedtls=y
CONFIG_PACKAGE_dnsmasq=y
# admin web app needs iwinfo (wifi scan/signal in the rpcd backend)
CONFIG_PACKAGE_iwinfo=y
# signed OTA: usign verifies the uploaded image against the baked-in pubkey
CONFIG_PACKAGE_usign=y
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
# CRITICAL: OpenWrt keeps a stale .ipk for a /src-linked feed package even after
# the .ko is recompiled, and silently INSTALLS the old module (this shipped the
# no-DMA driver in the image more than once). Nuke our packages' build dirs AND
# their .ipk files so every build installs the freshly-compiled module.
find "$OW"/bin "$OW"/build_dir -name '*beep-i2s*.ipk' -delete 2>/dev/null || true
find "$OW"/bin "$OW"/build_dir -name '*beepd*.ipk' -delete 2>/dev/null || true
find "$OW"/bin "$OW"/build_dir -name '*sound-soc-extra*.ipk' -delete 2>/dev/null || true
rm -rf "$OW"/build_dir/target-*/linux-*/beep-i2s \
       "$OW"/build_dir/target-*/linux-*/beepd \
       "$OW"/build_dir/target-*/linux-*/sound-soc-extra 2>/dev/null || true
set -o pipefail   # else the pipe's exit = tee/tail, masking a make failure
make -j"$(nproc)" 2>&1 | tee /build/image-build.log | tail -1
MAKE_RC=$?
[ "$MAKE_RC" -eq 0 ] || { echo "!! make FAILED (rc=$MAKE_RC) — see /build/image-build.log"; exit "$MAKE_RC"; }

# LEAN: surgically drop rootfs fat that's a hard dependency (can't deselect) but
# useless for a tone test — the ALSA UCM profiles (~2MB, alsa-ucm-conf is pulled
# by alsa-utils) and opkg metadata. Keeps /usr/share/alsa/alsa.conf so aplay
# still works. Then rebuild ONLY the initramfs from the trimmed rootfs. This is
# what got the tmpfs from 22MB (OOM-deadlock) down to ~14MB (boots on 64MB).
TRIM_RAN=0
for R in "$OW"/build_dir/target-*/root-*; do
  [ -d "$R/usr/share/alsa" ] || continue
  rm -rf "$R/usr/share/alsa/ucm2" "$R/usr/share/alsa/ucm"   # UCM ~2MB, unused by our fixed simple-card — drop in BOTH builds
  [ -n "${LEAN:-}" ] && rm -rf "$R/usr/lib/opkg"            # opkg metadata: LEAN-only (full/flashed keeps opkg usable)
  TRIM_RAN=1
done
[ "$TRIM_RAN" = 1 ] && { echo "   trimmed UCM$( [ -n "${LEAN:-}" ] && echo '+opkg' ) → rebuilding image"; make target/linux/install >>/build/image-build.log 2>&1 || echo "!! image re-link failed"; }
echo "== images =="
ls -la "$OW"/bin/targets/ath79/generic/*8dev_carambola2*.bin 2>/dev/null || \
  echo "(no image yet — check /build/image-build.log; some kmod/pkg names may need a menuconfig pass)"
