include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk
PKG_NAME:=beep-i2s
PKG_RELEASE:=1
include $(INCLUDE_DIR)/package.mk
define KernelPackage/beep-i2s
  SUBMENU:=Sound Support
  TITLE:=Beep AR9331 I2S CPU DAI (ASoC)
  DEPENDS:=@TARGET_ath79 +kmod-sound-soc-core
  FILES:=$(PKG_BUILD_DIR)/beep-i2s.ko
  AUTOLOAD:=$(call AutoLoad,55,beep-i2s)
endef
define KernelPackage/beep-i2s/description
  Control-plane I2S DAI + pinmux for the Beep (WM8524). PCM/DMA port pending.
endef
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef
$(eval $(call KernelPackage,beep-i2s))
