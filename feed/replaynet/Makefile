include $(TOPDIR)/rules.mk
PKG_NAME:=replaynet
PKG_RELEASE:=1
include $(INCLUDE_DIR)/package.mk
define Package/replaynet
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Beep multi-room sync engine (fresh reimplementation of stock playnet)
  # PCM transport + framing (a) + clock sync/drift (b) + ALSA/WM8524 sink (c).
  # alsa-lib: the -DRN_ALSA sink writes decoded PCM to the "default" (softvol->hw:0,0)
  # device. libflac: the -DRN_FLAC adaptive transport codec (auto raw->FLAC under wifi
  # contention). See docs/REPLAYNET.md and docs/PLAYNET-RE.md §4.
  DEPENDS:=+libc +alsa-lib +libflac
endef
define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef
define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -DRN_ALSA -DRN_FLAC \
		-o $(PKG_BUILD_DIR)/replaynet $(PKG_BUILD_DIR)/replaynet.c -lasound -lFLAC
endef
define Package/replaynet/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/replaynet $(1)/usr/sbin/replaynet
endef
$(eval $(call BuildPackage,replaynet))
