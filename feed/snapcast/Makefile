#
# Snapcast (snapserver + snapclient) for OpenWrt 24.10 / ath79 (mips_24kc, big-endian).
#
# Adapted from the archived badaix/snapos OpenWrt Makefile. Snapcast is our
# multi-room sync engine for the Beep: the AirPlay-receiving unit runs snapserver
# (fed by a light AirPlay-1 shairport pipe); every unit runs snapclient and plays
# to the shared server timeline, so a group of Beeps stays sample-accurately synced.
# AirPlay-2 grouping can't run on the 400MHz AR9331 (AAC decode saturates the core);
# Snapcast clients play uncompressed/opus/flac and are featherweight — the right fit.
#
# All deps are present in the 24.10 feeds (libsoxr, libvorbisidec, boost, libflac,
# libopus, libavahi-client, alsa-lib); libatomic comes from the toolchain (snapcast
# uses 64-bit std::atomic, which needs -latomic on 32-bit MIPS).
#
# This is free software, licensed under the GNU General Public License v2.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=snapcast
PKG_VERSION:=0.27.0
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/badaix/snapcast.git
PKG_SOURCE_VERSION:=v0.27.0
PKG_SOURCE_SUBMODULES:=skip
PKG_MIRROR_HASH:=skip
PKG_BUILD_DIR:=$(BUILD_DIR)/snapcast-$(PKG_VERSION)

PKG_LICENSE:=GPL-3.0-or-later
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=Beep Revival

PKG_BUILD_PARALLEL:=1
PKG_USE_MIPS16:=0

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_OPTIONS += \
	-DBUILD_TESTS=OFF \
	-DBUILD_WITH_FLAC=ON \
	-DBUILD_WITH_VORBIS=ON \
	-DBUILD_WITH_TREMOR=ON \
	-DBUILD_WITH_OPUS=ON \
	-DBUILD_WITH_AVAHI=ON \
	-DBUILD_SERVER=ON \
	-DBUILD_CLIENT=ON

# 64-bit std::atomic on mips32 needs the libatomic runtime.
TARGET_LDFLAGS += -latomic

# Snapcast 0.27 predates GCC 13's stricter header hygiene: several headers use
# uint32_t/uint16_t (sample_format.hpp etc.) without including <cstdint>, which
# GCC 13.3.0 no longer pulls in transitively. Force-include it in every C++ TU.
TARGET_CXXFLAGS += -include cstdint

define Package/snapcast/Default
	SECTION:=sound
	CATEGORY:=Sound
	TITLE:=Synchronous multiroom audio player
	URL:=https://github.com/badaix/snapcast
	DEPENDS:=+libstdcpp +libatomic +libavahi-client +libogg +libflac +libopus +boost +libsoxr
endef

define Package/snapserver
	$(call Package/snapcast/Default)
	TITLE+= (server)
	DEPENDS+= +AUDIO_SUPPORT:alsa-lib +libvorbis
endef

define Package/snapclient
	$(call Package/snapcast/Default)
	TITLE+= (client)
	DEPENDS+= +AUDIO_SUPPORT:alsa-lib +libvorbisidec
endef

define Package/snapserver/description
	Snapcast server: reads an audio stream and distributes it, time-stamped, to
	one or more time-synchronised Snapcast clients for multi-room playback.
endef

define Package/snapclient/description
	Snapcast client: plays a Snapcast server's stream in tight sync with every
	other client in the group. Lightweight — decodes PCM/FLAC/Opus, no heavy AAC.
endef

define Package/snapserver/conffiles
/etc/snapserver.conf
endef

define Package/snapserver/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/snapserver $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/snapserver.init $(1)/etc/init.d/snapserver
	$(INSTALL_DIR) $(1)/etc
	$(INSTALL_CONF) $(PKG_BUILD_DIR)/server/etc/snapserver.conf $(1)/etc/snapserver.conf
endef

define Package/snapclient/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/snapclient $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/snapclient.init $(1)/etc/init.d/snapclient
endef

$(eval $(call BuildPackage,snapserver))
$(eval $(call BuildPackage,snapclient))
