#!/bin/bash
# Runs INSIDE the Debian container. Clones OpenWrt, configures for the Beep, builds toolchain.
set -e
export FORCE_UNSAFE_CONFIGURE=1
cd /build
if [ ! -d openwrt ]; then
  apt-get update -qq
  # NOTE: no gcc-multilib/g++-multilib — x86-only, absent on arm64 hosts and not
  # needed (OpenWrt builds its own MIPS cross-toolchain from source).
  # bc: the kernel build needs it to generate include/generated/timeconst.h (else
  # "bc: not found" -> Error 127). quilt: package patch management.
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential clang flex bison g++ gawk \
    gettext git libncurses-dev libssl-dev python3 python3-setuptools \
    rsync swig unzip zlib1g-dev file wget time bc quilt >/dev/null
  git clone --depth 1 -b v24.10.0 https://git.openwrt.org/openwrt/openwrt.git
fi
cd openwrt
./scripts/feeds update -a >/dev/null 2>&1
./scripts/feeds install -a >/dev/null 2>&1
# minimal config for the carambola2 target; packages get layered later
cat > .config <<CFG
CONFIG_TARGET_ath79=y
CONFIG_TARGET_ath79_generic=y
CONFIG_TARGET_ath79_generic_DEVICE_8dev_carambola2=y
CONFIG_PACKAGE_kmod-sound-core=y
CONFIG_PACKAGE_kmod-sound-soc-core=y
CONFIG_PACKAGE_alsa-utils=y
CONFIG_PACKAGE_kmod-i2c-gpio-custom=y
CONFIG_PACKAGE_i2c-tools=y
CONFIG_PACKAGE_libgpiod=y
CFG
make defconfig >/dev/null
echo "=== toolchain build starting $(date) ==="
make -j"$(nproc)" tools/install toolchain/install >/build/toolchain.log 2>&1 && echo "TOOLCHAIN DONE" || echo "TOOLCHAIN build hit an error (see /build/toolchain.log)"
