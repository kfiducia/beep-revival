#!/bin/bash
# build.sh — produce the Beep custom OpenWrt image. Runs INSIDE the Debian
# container (docker exec -it beep-build bash /src/scripts/build.sh) after the
# toolchain from docker-build-setup.sh has finished.
#
# Integration approach: ride the upstream 8dev_carambola2 device profile
# (its flash layout already matches the Beep) but swap in our device tree
# (adds I2S/WM8524 + i2c-gpio STM8 + setup key) and layer our feed + files.
set -e
# GNU tar (and a few other host tools) refuse to ./configure as root; the build
# container runs as root, so bypass the check (harmless — it's a throwaway container).
export FORCE_UNSAFE_CONFIGURE=1
OW="${OW:-/build/openwrt}"
SRC="${SRC:-/src}"
cd "$OW"

echo "== 1. local package feed =="
grep -q 'src-link beepfeed' feeds.conf.default || echo "src-link beepfeed $SRC/feed" >> feeds.conf.default
./scripts/feeds update beepfeed >/dev/null
./scripts/feeds install -a -p beepfeed >/dev/null

echo "== 1b. AirPlay build mode (default: classic AirPlay-1 for Snapcast; AIRPLAY2=1: buffered AAC) =="
# The AR9331 (400MHz, no SIMD) CANNOT decode AirPlay-2 buffered AAC in real time —
# measured 0% idle + instant PCM XRUN the moment playback starts (see BUILD-STATUS).
# So the DEFAULT image ships CLASSIC AirPlay-1 (realtime ALAC, ~10x lighter) as the
# light *ingest* for Snapcast multi-room. AIRPLAY2=1 keeps the full AirPlay-2 build
# intact for on-device CPU-reduction experiments (kept, not deleted).
SPS="$OW/feeds/packages/sound/shairport-sync/Makefile"
FM="$OW/feeds/packages/multimedia/ffmpeg/Makefile"
SPP="$OW/feeds/packages/sound/shairport-sync/patches"
# CRITICAL: the mode-specific seds below are PERSISTENT, non-idempotent edits to the
# shared packages-feed Makefiles. Restore them to pristine (from the feed's git)
# before EACH build — otherwise a prior build's edits leak in. (This bit us: a
# default build strips --with-airplay-2, then an AIRPLAY2 build on the same tree
# silently produced a classic binary because the strip was still in the Makefile.)
git config --global --add safe.directory "$OW/feeds/packages" 2>/dev/null || true
git -C "$OW/feeds/packages" checkout -- sound/shairport-sync/Makefile multimedia/ffmpeg/Makefile 2>/dev/null || true
# Universal (both modes): make the pipe backend emit little-endian S16 so Snapcast
# (which assumes LE) gets clean audio on this big-endian target instead of static.
# The patch compiles out on little-endian hosts, so it's harmless everywhere else.
mkdir -p "$SPP"
cp "$SRC/scripts/patches/030-pipe-output-little-endian.patch" "$SPP/" 2>/dev/null || true
# Universal (both modes): make the AirPlay-1 Apple-Response a real private-key SIGNATURE
# again. OpenWrt's 100-mbedtls3fix.patch ports the mbedTLS call to v3 by dropping the
# MBEDTLS_RSA_PRIVATE mode arg, which silently switches it to a PUBLIC-key encrypt — so
# modern iOS rejects the classic RAOP handshake (macOS, which doesn't verify, still works).
# Applies AFTER 100- (hence 110-); restores signing via mbedtls_rsa_private. See the patch
# header + docs/DEV-NOTES.md for the on-device proof.
cp "$SRC/scripts/patches/110-ap1-apple-response-mbedtls3-sign.patch" "$SPP/" 2>/dev/null || true
# Give shairport a scheduling edge on the single-core AR9331 so the LED bit-banged-i2c
# churn / SSH / knob->amixer forks can't starve the audio thread and XRUN playback
# (measured: AP2 AAC decode leaves only ~45% idle, and concurrent load glitches it).
# A safe NEGATIVE NICE via procd — deliberately NOT SCHED_FIFO: blanket real-time on a
# single core risks the decode preempting the network read that feeds it and
# self-deadlocking into an underrun. Idempotent (the feed's init isn't git-restored).
SPI="$OW/feeds/packages/sound/shairport-sync/files/shairport-sync.init"
if [ -f "$SPI" ] && ! grep -q 'procd_set_param nice' "$SPI"; then
  # '#' delimiter avoids escaping the command path; \n\t inserts a tab-indented line
  # right after the command set (order among procd params is irrelevant at instance close).
  sed -i 's#\(procd_set_param command /usr/bin/shairport-sync\)#\1\n\tprocd_set_param nice -12#' "$SPI"
fi
if [ -n "${AIRPLAY2:-}" ]; then
  echo "   [AIRPLAY2] AirPlay-2 build — experimental on this silicon; ffmpeg full->audio-dec + BE crypto patch"
  # shairport-sync hardcodes +libffmpeg-full (~12MB). AirPlay 2 only needs AAC+ALAC
  # *decode* → libffmpeg-audio-dec (~2-3MB, --enable-small). Patch the dep down.
  [ -f "$SPS" ] && sed -i 's/+libffmpeg-full/+libffmpeg-audio-dec/g' "$SPS"
  # audio-dec doesn't ship libswresample, but shairport links it. Force it ON + into install.
  if [ -f "$FM" ]; then
    sed -i 's/--disable-swresample/--enable-swresample/g' "$FM"
    sed -i 's/{avcodec,avformat,avutil}/{avcodec,avformat,avutil,swresample}/g' "$FM"
    # CRITICAL: the Build/InstallDev block (headers+lib+.pc → staging, what shairport
    # BUILDS against) uses the avdevice-inclusive brace {avcodec,avdevice,avformat,avutil}.
    # Without swresample there, libavcodec's link test fails and shairport's configure
    # reports "AirPlay 2 support requires libavcodec". Add swresample to that pattern too.
    sed -i 's/lib{avcodec,avdevice,avformat,avutil}/lib{avcodec,avdevice,avformat,avutil,swresample}/g' "$FM"
    # Enable BOTH aac (float) and aac_fixed (fixed-point). aac_fixed is integer-only
    # and ~10x cheaper on the FPU-less AR9331 (measured); our decode patch selects it.
    sed -i 's/--disable-decoder=pcm_bluray,pcm_dvd/--disable-decoder=pcm_bluray,pcm_dvd --enable-decoder=aac,aac_fixed --enable-parser=aac/' "$FM"
  fi
  # AirPlay 2 on BIG-ENDIAN MIPS: our LE-framing pair_ap patch (shairport-sync #1683).
  mkdir -p "$SPP"
  cp "$SRC/scripts/patches/010-airplay2-bigendian-pairing.patch" "$SPP/" 2>/dev/null || true
  # Fixed-point AAC decode: select ffmpeg's aac_fixed (integer S32P) instead of the
  # float decoder so AAC-LC decode fits the 400MHz no-FPU core. See docs/DEV-NOTES.md §1.
  cp "$SRC/scripts/patches/020-aac-fixed-decode.patch" "$SPP/" 2>/dev/null || true
  # nqptp PTP timing (BIG-ENDIAN): fix ntoh64() transposing the 64-bit PTP
  # correctionField on BE (nqptp's hand-rolled swap is LE-only). Latent on a flat
  # single-switch subnet (correctionField is 0 there) but a real BE correctness bug
  # in the AP2 timing path. See docs/BE-AP2-AUDIT.md Finding 1 / docs/DEV-NOTES.md §1.
  NQP="$OW/feeds/packages/net/nqptp/patches"
  mkdir -p "$NQP"
  cp "$SRC/scripts/patches/040-nqptp-bigendian-ntoh64.patch" "$NQP/" 2>/dev/null || true
  # new patch → force nqptp re-prepare so the patch is actually applied
  make package/feeds/packages/nqptp/clean >/dev/null 2>&1 || true
  # Force a clean ffmpeg restage so the swresample InstallDev fix actually takes —
  # stale staged libav* from a prior variant can otherwise leave libswresample missing.
  make package/feeds/packages/ffmpeg/dirclean >/dev/null 2>&1 || true
  # ONLY ffmpeg's libs — NOT libav* wildcard (that also nukes libavahi-client/common/core,
  # which shairport needs, and made configure fail on Avahi instead).
  rm -f "$OW"/staging_dir/target-*/usr/lib/libav{codec,device,filter,format,util}.so* \
        "$OW"/staging_dir/target-*/usr/lib/lib{swresample,swscale,postproc}.so* 2>/dev/null || true
  # Build + STAGE ffmpeg NOW (before the world build). shairport's runtime DEPENDS on
  # libffmpeg doesn't force ffmpeg's InstallDev to finish before shairport configures
  # under -j, so shairport could otherwise configure before libswresample is staged and
  # fail "requires libavcodec". Staging it here first makes the ordering deterministic.
  echo "   [AIRPLAY2] pre-building ffmpeg (audio-dec + swresample) so shairport finds it"
  make package/feeds/packages/ffmpeg/compile -j4 >/dev/null 2>&1 || { echo "!! ffmpeg pre-build failed"; exit 4; }
else
  echo "   [default] classic AirPlay-1 — strip --with-airplay-2 + AP2-only deps (no ffmpeg/nqptp/sodium/gcrypt)"
  if [ -f "$SPS" ]; then
    # drop the AirPlay-2 configure flag (and MQTT, unused) → classic RAOP. KEEP --with-pipe
    # (shairport writes raw PCM to a fifo that snapserver reads — the multi-room ingest).
    sed -i '/--with-airplay-2/d; /--with-mqtt-client/d' "$SPS"
    # trim AP2-only deps from the default DEPENDS line so nothing heavy is pulled in
    sed -i '/DEPENDS:=@AUDIO_SUPPORT/ { s/+libplist //g; s/+libsodium //g; s/+libgcrypt //g; s/+libffmpeg-full //g; s/+libffmpeg-audio-dec //g; s/+nqptp //g; s/+libmosquitto //g }' "$SPS"
  fi
  # the AP2 crypto patch targets pair_ap (not compiled in a classic build) — keep it out
  rm -f "$SPP/010-airplay2-bigendian-pairing.patch" 2>/dev/null || true
  # nqptp isn't built in the classic image (dep stripped above) — keep its patch out too
  rm -f "$OW/feeds/packages/net/nqptp/patches/040-nqptp-bigendian-ntoh64.patch" 2>/dev/null || true
fi
# Makefile/source changed → force a clean shairport rebuild so the mode switch takes
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
# busybox base64 applet — the signed-OTA cgi base64-decodes the release signature
# passed in the query string (uhttpd drops custom headers, so it can't ride in one).
CONFIG_BUSYBOX_CONFIG_BASE64=y
CFG

# Extra userspace (AirPlay + web admin + dnsmasq) — FULL build only.
# LEAN=1 omits these so the initramfs is small enough to RAM-boot on 64MB:
# a ~14MB image decompresses past its load address and clobbers its own
# compressed source (LZMA ERROR 1). The full image still boots fine from FLASH;
# the lean image is purely for zero-risk RAM-boot driver testing.
if [ -z "${LEAN:-}" ]; then
cat >> .config <<CFG
# shairport-sync mbedtls variant links avahi so it ADVERTISES over mDNS (the mini
# variant's tinysvcmdns cannot). In DEFAULT mode 1b strips --with-airplay-2 from
# this same package → it becomes a light classic AirPlay-1 receiver.
CONFIG_PACKAGE_shairport-sync-mbedtls=y
CONFIG_PACKAGE_avahi-daemon=y
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
if [ -n "${AIRPLAY2:-}" ]; then
cat >> .config <<CFG
# AIRPLAY2 image: buffered AAC needs a float-capable ffmpeg decoder (trimmed to
# audio-dec in 1b). No Snapcast here — this image is for AP2 CPU-reduction work.
CONFIG_PACKAGE_libffmpeg-audio-dec=y
# CONFIG_PACKAGE_libffmpeg-full is not set
# CONFIG_PACKAGE_snapserver is not set
# CONFIG_PACKAGE_snapclient is not set
CFG
else
cat >> .config <<CFG
# DEFAULT image: Snapcast multi-room. The AirPlay-receiving Beep runs snapserver
# (fed by the classic AirPlay-1 shairport pipe); every Beep runs snapclient and
# plays to the shared server timeline → sample-accurate sync. Classic AirPlay-1
# pulls no ffmpeg. libatomic: snapcast's 64-bit std::atomic needs it on mips32.
CONFIG_PACKAGE_snapserver=y
CONFIG_PACKAGE_snapclient=y
CONFIG_PACKAGE_libatomic=y
# CONFIG_PACKAGE_libffmpeg-full is not set
# CONFIG_PACKAGE_libffmpeg-audio-dec is not set
CFG
fi
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
# Cap parallelism. On many-core hosts OpenWrt's recursive sub-makes (notably gcc's
# bootstrap, and several base packages: zlib/usign/libjson-c) RACE at very high -j
# and fail non-deterministically with a bare "world Error 1". Capping at 6 (override
# with JOBS=) builds reliably; the toolchain itself must be built at low -j too.
JOBS="${JOBS:-$(n=$(nproc); [ "$n" -gt 6 ] && echo 6 || echo "$n")}"
echo "   building with -j$JOBS (cap avoids high-parallelism gcc/base-package races)"
make -j"$JOBS" 2>&1 | tee /build/image-build.log | tail -1
MAKE_RC=$?
[ "$MAKE_RC" -eq 0 ] || { echo "!! make FAILED (rc=$MAKE_RC) — see /build/image-build.log"; exit "$MAKE_RC"; }

# GUARD: an AIRPLAY2 build MUST actually contain AirPlay 2. This silently regressed
# once when a prior default build's --with-airplay-2 strip leaked into the Makefile,
# producing a classic binary that no one caught until it was flashed. Fail loudly.
if [ -n "${AIRPLAY2:-}" ]; then
  SPB="$(ls "$OW"/staging_dir/target-*/root-*/usr/bin/shairport-sync 2>/dev/null | head -1)"
  if [ -n "$SPB" ] && strings "$SPB" 2>/dev/null | grep -qi airplay2; then
    echo "   [AIRPLAY2] verified: shairport binary contains AirPlay 2"
  else
    echo "!! AIRPLAY2=1 but the built shairport-sync is NOT an AirPlay-2 binary — aborting"
    exit 3
  fi
fi

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
