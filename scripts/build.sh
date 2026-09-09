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

# ============================================================================
# RECOVERY=1 — build the minimal signed-reflash initramfs for the recovery slot.
# Self-contained (returns before the primary/audio build). See docs/RECOVERY-DESIGN.md.
#
# It reuses the SAME device tree and the SAME rootfs-overlay machinery as the primary
# (per-device MAC->code identity, the wifi-setup AP + dnsmasq captive portal, the
# beep-ota cgi + baked /etc/beep-ota.pub) so a fix to any of those — e.g. the setup-AP
# DHCP — lands in recovery too, then layers recovery-overlay/ on top (a stripped
# reflash-only UI + a boot hook that raises the AP unconditionally, since a stateless
# initramfs is "fresh" every boot). Package set is lean-initramfs-class to clear BOTH
# the ~5.75 MB recovery slot AND the 64 MB unpacked-RAM ceiling: wifi AP + uhttpd +
# usign + dnsmasq only — NO audio/snapcast/ffmpeg/rpcd/px5g/opkg. Signed images verify
# by signature alone (no rpcd session exists here); the unsigned triple-tap path needs
# beepd + i2c and is deliberately omitted from this lean build (recovery restores a
# genuine SIGNED release — sufficient to un-brick).
# ============================================================================
if [ -n "${RECOVERY:-}" ]; then
  echo "== [RECOVERY] minimal signed-reflash initramfs =="
  DTS="$OW/target/linux/ath79/dts/ar9331_8dev_carambola2.dts"
  cp "$SRC/dts/ar9331_beep_dial.dts" "$DTS"
  sed -i 's/"beep,dial"/"8dev,carambola2"/' "$DTS"   # board_name match (sysupgrade target)

  echo "   [RECOVERY] overlay = rootfs-overlay (shared machinery) + recovery-overlay"
  rm -rf "$OW/files"; mkdir -p "$OW/files"
  cp -a "$SRC/rootfs-overlay/." "$OW/files/"
  # Drop primary-only first-boot hooks that have no place in a stateless reflash
  # initramfs (no audio, no multiroom, no STA watchdog, no persistent SSH pref).
  # 99-recovery-ap (from recovery-overlay) raises the AP explicitly instead.
  rm -f "$OW"/files/etc/uci-defaults/99-beep-audio \
        "$OW"/files/etc/uci-defaults/99-beep-group \
        "$OW"/files/etc/uci-defaults/99-beep-ssh \
        "$OW"/files/etc/uci-defaults/99-beep-netcheck \
        "$OW"/files/etc/init.d/beep-audio \
        "$OW"/files/etc/init.d/beep-netcheck \
        "$OW"/files/etc/init.d/replaynet \
        "$OW"/files/etc/uci-defaults/99-beep-replaynet \
        "$OW"/files/etc/config/beep \
        "$OW"/files/etc/config/replaynet \
        "$OW"/files/etc/asound.conf \
        "$OW"/files/etc/shairport-sync.conf \
        "$OW"/files/etc/snapserver.conf 2>/dev/null || true
  cp -a "$SRC/recovery-overlay/." "$OW/files/"

  cat > .config <<CFG
CONFIG_TARGET_ath79=y
CONFIG_TARGET_ath79_generic=y
CONFIG_TARGET_ath79_generic_DEVICE_8dev_carambola2=y
CONFIG_TARGET_ROOTFS_INITRAMFS=y
CONFIG_TARGET_INITRAMFS_COMPRESSION_XZ=y
# wifi + the setup/recovery AP (WPA2)
CONFIG_PACKAGE_kmod-ath9k=y
CONFIG_PACKAGE_wpad-basic-mbedtls=y
# web reflash plane + DHCP/captive portal (reuses the primary's dnsmasq machinery)
CONFIG_PACKAGE_uhttpd=y
CONFIG_PACKAGE_dnsmasq=y
# recovery serves an AP + a single upload form — it never scans, so drop iwinfo
# CONFIG_PACKAGE_iwinfo is not set
# signed OTA: usign verifies the uploaded image against the baked-in pubkey
CONFIG_PACKAGE_usign=y
# BUSYBOX_CUSTOM=y is required for the base64 applet override to take effect (GitHub
# #26) — the recovery cgi also base64-decodes the signature, so recovery needs it too.
CONFIG_BUSYBOX_CUSTOM=y
CONFIG_BUSYBOX_CONFIG_BASE64=y
CONFIG_BUSYBOX_CONFIG_SETSID=y
# explicitly OMIT everything heavy (audio / multiroom / rpcd / opkg): keeps the
# initramfs lean-class for the flash slot AND the 64MB unpacked-RAM ceiling.
# CONFIG_PACKAGE_beepd is not set
# CONFIG_PACKAGE_kmod-beep-i2s is not set
# CONFIG_PACKAGE_kmod-sound-soc-wm8524 is not set
# CONFIG_PACKAGE_kmod-sound-soc-simple-card is not set
# CONFIG_PACKAGE_alsa-utils is not set
# CONFIG_PACKAGE_shairport-sync-mbedtls is not set
# CONFIG_PACKAGE_avahi-daemon is not set
# CONFIG_PACKAGE_snapserver is not set
# CONFIG_PACKAGE_snapclient is not set
# CONFIG_PACKAGE_rpcd is not set
# CONFIG_PACKAGE_uhttpd-mod-ubus is not set
# CONFIG_PACKAGE_px5g-mbedtls is not set
# CONFIG_PACKAGE_opkg is not set
CFG
  make defconfig >/dev/null

  echo "== [RECOVERY] build =="
  # Same staleness guards as the primary build: wipe deselected files + our stale ipks.
  rm -rf "$OW"/build_dir/target-*/root-* 2>/dev/null || true
  find "$OW"/bin "$OW"/build_dir -name '*beepd*.ipk' -delete 2>/dev/null || true
  set -o pipefail
  JOBS="${JOBS:-$(n=$(nproc); [ "$n" -gt 6 ] && echo 6 || echo "$n")}"
  echo "   building with -j$JOBS"
  make -j"$JOBS" 2>&1 | tee /build/recovery-build.log | tail -1
  # Drop the ALSA UCM profiles if any snuck in (harmless here; keeps it lean).
  for R in "$OW"/build_dir/target-*/root-*; do
    [ -d "$R/usr/share/alsa" ] && rm -rf "$R/usr/share/alsa/ucm2" "$R/usr/share/alsa/ucm"
  done
  echo "== [RECOVERY] image =="
  IMG="$(ls "$OW"/bin/targets/ath79/generic/*8dev_carambola2*initramfs-kernel.bin 2>/dev/null | head -1)"
  if [ -n "$IMG" ]; then
    SZ="$(stat -c%s "$IMG" 2>/dev/null || wc -c < "$IMG")"
    printf '   %s\n   %d bytes = %.2f MB (recovery slot target: 5.875 MB / 0x5E0000)\n' "$IMG" "$SZ" "$(awk "BEGIN{print $SZ/1048576}")"
    [ "$SZ" -gt 6160384 ] && echo "   ⚠️  over 5.875 MB — re-check the package set or enlarge the slot (see RECOVERY-DESIGN.md §D)"
  else
    echo "   !! no initramfs image produced — check /build/recovery-build.log"
  fi
  exit 0
fi

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

echo "== 1c. squeezelite (Lyrion / LMS player) build-dep trim =="
# The stock squeezelite package's PKG_BUILD_DEPENDS is the UNION for its full/dynamic
# variants and includes ffmpeg + faad2. We build only the "custom" variant with the
# light codecs (FLAC/MP3/Opus) and NO ffmpeg/AAC path (-DNO_FAAD, no -DFFMPEG — those
# sources are preprocessed out), so those two heavyweight headers are never included.
# Dropping them from the build-deps keeps the DEFAULT image ffmpeg-free at BUILD time
# too (a full ffmpeg compile is the single longest step; the whole point of the classic
# AirPlay-1 default is to avoid it). Persistent, non-idempotent edit — restore pristine
# from git first, exactly like the shairport Makefile handling above.
# NOTE: if a build ever fails on a missing <faad.h>/ffmpeg header, restore that one dep.
SQL="$OW/feeds/packages/sound/squeezelite/Makefile"
if [ -f "$SQL" ]; then
  git -C "$OW/feeds/packages" checkout -- sound/squeezelite/Makefile 2>/dev/null || true
  sed -i 's/^PKG_BUILD_DEPENDS:=.*/PKG_BUILD_DEPENDS:=flac libsoxr libvorbis openssl opusfile/' "$SQL"
  make package/feeds/packages/squeezelite/clean >/dev/null 2>&1 || true
fi

echo "== 2. swap in our device tree (keep carambola2 board-name for sysupgrade) =="
DTS="$OW/target/linux/ath79/dts/ar9331_8dev_carambola2.dts"
cp "$SRC/dts/ar9331_beep_dial.dts" "$DTS"
sed -i 's/"beep,dial"/"8dev,carambola2"/' "$DTS"   # board_name match

echo "== 3. bake in the rootfs overlay =="
# Start FRESH (rm before mkdir) — symmetric with the RECOVERY path. Without this, a
# prior RECOVERY=1 build's overlay lingers in $OW/files (this container is reused for
# back-to-back variant builds), and `cp -a` never deletes destination files absent
# from the source. recovery-overlay/ has files with NO rootfs-overlay counterpart
# (/etc/beep-recovery — which weakens beep-ota auth — and 99-recovery-ap), so they'd
# silently ride into a subsequent production image. Wipe first so that can't happen.
rm -rf "$OW/files"; mkdir -p "$OW/files"
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
# replaynet: fresh multi-room sync engine (reimplements stock playnet, docs/REPLAYNET.md).
# Always built in; its init.d (rootfs-overlay/etc/init.d/replaynet) is registered but
# DORMANT under the default snapcast engine. MULTIROOM=replaynet (below) drops snapcast
# and makes replaynet the active engine.
CONFIG_PACKAGE_replaynet=y
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
# fw_printenv/fw_setenv — the ONLY safe lever to reset U-Boot's bootcount (rewrites
# the env sector). Used by the guarded good-boot reset (/usr/libexec/beep/bootcount-
# reset), which stays a no-op until a bench-verified /etc/beep-fwenv-verified exists.
CONFIG_PACKAGE_uboot-envtools=y
# debug loop: rz/sz for fast serial .ko transfer into the running system,
# devmem2 for live MBOX/stereo register peeking (busybox devmem also present)
CONFIG_PACKAGE_lrzsz=y
CONFIG_PACKAGE_devmem2=y
# CRITICAL: BUSYBOX_CUSTOM=y is the PREREQUISITE for the CONFIG_BUSYBOX_CONFIG_* overrides
# below to take effect — without it `make defconfig` ignores them and the applet is NOT
# built. base64 is not a default busybox applet, so before this the signed-OTA cgi's
# `base64 -d` was missing and every signed upload failed with "bad signature encoding"
# (GitHub #26). Enabling CUSTOM materializes OpenWrt's curated default applet set
# (verified: nothing lost) plus our base64. (setsid IS a default applet — it shipped
# regardless — but keep it explicit now that CUSTOM makes the override real.)
CONFIG_BUSYBOX_CUSTOM=y
# busybox base64 applet — the signed-OTA cgi base64-decodes the release signature
# passed in the query string (uhttpd drops custom headers, so it can't ride in one).
CONFIG_BUSYBOX_CONFIG_BASE64=y
# busybox setsid applet — beep-ota detaches sysupgrade with setsid so a uhttpd CGI
# teardown can't kill it mid-write. Without this applet the cgi silently falls back to
# a plain background (the weaker old behavior), so select it explicitly.
CONFIG_BUSYBOX_CONFIG_SETSID=y
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
# Lyrion Music Server (LMS / Squeezebox) player. The "custom" variant compiles ONLY
# the codecs the 400MHz no-FPU AR9331 can handle in real time — FLAC (fixed-point
# decoder), MP3 (libmpg123, integer), Opus (fixed-point) — and leaves out the
# CPU-killers: AAC/HE-AAC (faad), WMA/ALAC (ffmpeg), and soxr resampling. LMS
# transcodes anything else to FLAC on the server side. Beep ships its own
# init+config in the rootfs overlay to bind volume to the "Master" softvol.
CONFIG_PACKAGE_squeezelite-custom=y
CONFIG_SQUEEZELITE_FLAC=y
CONFIG_SQUEEZELITE_MP3_MPG123=y
CONFIG_SQUEEZELITE_OPUS=y
# Explicitly OFF (defaults are n; pinned for clarity + to document the CPU rationale):
# CONFIG_SQUEEZELITE_AAC is not set
# CONFIG_SQUEEZELITE_MP3_MAD is not set
# CONFIG_SQUEEZELITE_VORBIS is not set
# CONFIG_SQUEEZELITE_VORBIS_TREMOR is not set
# CONFIG_SQUEEZELITE_WMA_ALAC is not set
# CONFIG_SQUEEZELITE_RESAMPLE is not set
# CONFIG_SQUEEZELITE_DSD is not set
# CONFIG_SQUEEZELITE_SSL is not set
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

# MULTIROOM engine selector (default: snapcast). MULTIROOM=replaynet builds a
# snapcast-free image and makes the fresh `replaynet` --node daemon the multi-room
# engine (docs/REPLAYNET.md): the AirPlay-1 shairport pipe feeds replaynet directly and
# it does decode-fed fan-out + consensus itself, replacing snapserver+snapclient (and
# their boost/libatomic/~5 MB). replaynet is already selected above (CONFIG_PACKAGE_
# replaynet=y); here we just drop snapcast and flip the runtime engine on first boot.
if [ "${MULTIROOM:-snapcast}" = replaynet ]; then
  echo "== [MULTIROOM=replaynet] snapcast omitted; replaynet is the multi-room engine =="
  sed -i '/^CONFIG_PACKAGE_snapserver=y/d; /^CONFIG_PACKAGE_snapclient=y/d; /^CONFIG_PACKAGE_libatomic=y/d' .config
  mkdir -p "$OW/files/etc/uci-defaults"
  cat > "$OW/files/etc/uci-defaults/98-beep-multiroom-replaynet" <<'RNDEF'
#!/bin/sh
# Make replaynet the active multi-room engine (image built with MULTIROOM=replaynet):
# select it in beep-group, enable the node, and default it to the shared group so Beeps
# left at defaults auto-form one synced group. shairport is repointed to the replaynet
# pipe by `beep-group apply` (the group_engine=replaynet branch).
uci -q batch <<UCI
set beep.main.group_engine=replaynet
set replaynet.node.enabled=1
set replaynet.node.group=1
commit beep
commit replaynet
UCI
[ -x /usr/libexec/beep/beep-group ] && /usr/libexec/beep/beep-group apply >/dev/null 2>&1
exit 0
RNDEF
  chmod +x "$OW/files/etc/uci-defaults/98-beep-multiroom-replaynet"
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
#
# NOTE (why the old form always false-failed): with `set -o pipefail` on, the old
# `strings "$SPB" | grep -qi airplay2` returned non-zero whenever the match was found.
# `grep -q` exits at the FIRST match and closes the pipe; `strings` then dies of SIGPIPE
# (141), and pipefail promotes that to the pipeline's status — so a genuine AP2 binary
# tripped the "not AP2" branch. Capture the count instead (grep -c reads all input, no
# early close, no SIGPIPE) and test a DEFINITIVE AP2-only marker, distinguishing
# "couldn't find the binary" from "found, but classic".
if [ -n "${AIRPLAY2:-}" ]; then
  SPB="$(ls "$OW"/staging_dir/target-*/root-*/usr/bin/shairport-sync 2>/dev/null | head -1)"
  if [ -z "$SPB" ] || [ ! -f "$SPB" ]; then
    echo "!! AIRPLAY2=1 guard: no built shairport-sync found to verify — check the build"; exit 3
  fi
  # "Startup in AirPlay 2 mode" is emitted only by an AirPlay-2 build; a classic binary
  # lacks it. grep -c (not -q) so the pipe is fully drained under pipefail.
  if [ "$(strings "$SPB" 2>/dev/null | grep -c 'AirPlay 2 mode')" -gt 0 ]; then
    echo "   [AIRPLAY2] verified: $SPB is an AirPlay-2 binary"
  else
    echo "!! AIRPLAY2=1 but $SPB is a CLASSIC (AirPlay-1) binary — --with-airplay-2 strip likely leaked; aborting"
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
