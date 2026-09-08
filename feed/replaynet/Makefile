include $(TOPDIR)/rules.mk
PKG_NAME:=replaynet
PKG_RELEASE:=1
include $(INCLUDE_DIR)/package.mk
define Package/replaynet
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Beep multi-room sync engine (fresh reimplementation of stock playnet)
  # Step (a): PCM transport + on-wire framing only. No audio/ALSA deps yet — the
  # WM8524 ALSA sink is step (c). See docs/PLAYNET-RE.md §4.
  DEPENDS:=+libc
endef
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -o $(PKG_BUILD_DIR)/replaynet $(PKG_BUILD_DIR)/replaynet.c
endef
define Package/replaynet/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/replaynet $(1)/usr/sbin/replaynet
endef
$(eval $(call BuildPackage,replaynet))
