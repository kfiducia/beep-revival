# Packages two ASoC drivers that live in the mainline kernel source tree but are
# NOT packaged by OpenWrt 24.10's core sound.mk:
#   - kmod-sound-soc-wm8524       (WM8524 I2S DAC codec — our DAC)
#   - kmod-sound-soc-simple-card  (the simple-audio-card machine/glue driver
#                                  our device tree binds cpu<->codec with)
#
# These are IN-TREE modules: setting KCONFIG makes the kernel build them, so
# there is no source/Build/Compile here — we only declare the resulting .ko,
# its kconfig symbol, and autoload order. Load after beep-i2s (AutoLoad 55) so
# the CPU DAI + codec exist before the card probes.
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=sound-soc-extra
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

# No userspace/out-of-tree source to build: the kernel compiles these modules
# in-tree once their KCONFIG symbols are set, and we only package the .ko. Empty
# out the default Build/* steps so package.mk doesn't try to `make` an empty
# PKG_BUILD_DIR (that fails with "Error 2").
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef
define Build/Configure
endef
define Build/Compile
endef

define KernelPackage/sound-soc-wm8524
  SUBMENU:=Sound Support
  TITLE:=WM8524 I2S DAC codec (ASoC)
  DEPENDS:=+kmod-sound-soc-core
  KCONFIG:=CONFIG_SND_SOC_WM8524
  FILES:=$(LINUX_DIR)/sound/soc/codecs/snd-soc-wm8524.ko
  AUTOLOAD:=$(call AutoLoad,56,snd-soc-wm8524)
endef
define KernelPackage/sound-soc-wm8524/description
  Wolfson WM8524 stereo DAC ASoC codec driver (hardware/slave mode; the Beep's DAC).
endef
$(eval $(call KernelPackage,sound-soc-wm8524))

define KernelPackage/sound-soc-simple-card
  SUBMENU:=Sound Support
  TITLE:=ASoC simple-audio-card machine driver
  DEPENDS:=+kmod-sound-soc-core
  # SND_SIMPLE_CARD selects SND_SIMPLE_CARD_UTILS; ship + load utils first.
  KCONFIG:=CONFIG_SND_SIMPLE_CARD CONFIG_SND_SIMPLE_CARD_UTILS
  FILES:= \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card-utils.ko \
	$(LINUX_DIR)/sound/soc/generic/snd-soc-simple-card.ko
  AUTOLOAD:=$(call AutoLoad,57,snd-soc-simple-card-utils snd-soc-simple-card)
endef
define KernelPackage/sound-soc-simple-card/description
  Generic simple-audio-card ASoC machine driver — binds the Beep's I2S CPU DAI to
  the WM8524 codec per the device tree.
endef
$(eval $(call KernelPackage,sound-soc-simple-card))
