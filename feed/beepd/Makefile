include $(TOPDIR)/rules.mk
PKG_NAME:=beepd
PKG_RELEASE:=1
include $(INCLUDE_DIR)/package.mk
define Package/beepd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Beep Dial control daemon (STM8 knob/tap/LED)
  # alsa-lib: read the "Master" softvol so the LED arc reflects sender/web volume
  # changes (not just the knob). libm: mapped-volume (amixer -M) curve.
  DEPENDS:=+libc +alsa-lib
endef
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -o $(PKG_BUILD_DIR)/beepd $(PKG_BUILD_DIR)/beepd.c -lasound -lm
endef
define Package/beepd/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/beepd $(1)/usr/sbin/beepd
endef
$(eval $(call BuildPackage,beepd))
