include $(TOPDIR)/rules.mk

PKG_NAME:=sim-relay
PKG_VERSION:=0.6.0
PKG_RELEASE:=6
PKG_LICENSE:=MIT
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/sim-relay
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Lightweight multi-device SIM SMS service
  DEPENDS:=+libcurl +cJSON +rpcd +libsodium +uci
endef

define Package/sim-relay/conffiles
/etc/config/sim-relay
endef

# Keep this directory in the repository: Build/Prepare copies the shared source.
define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/src
	$(CP) $(CURDIR)/../../src/* $(PKG_BUILD_DIR)/src/
	$(CP) $(CURDIR)/../../CMakeLists.txt $(PKG_BUILD_DIR)/
endef

define Package/sim-relay/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/config
	$(INSTALL_DIR) $(1)/etc/sim-relay $(1)/usr/libexec/rpcd $(1)/lib/upgrade/keep.d
	chmod 0700 $(1)/etc/sim-relay
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sim-relayd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/sim-relay.init $(1)/etc/init.d/sim-relay
	$(INSTALL_BIN) ./files/sim-relay.rpc $(1)/usr/libexec/rpcd/sim-relay
	$(INSTALL_CONF) ./files/sim-relay.config $(1)/etc/config/sim-relay
	$(INSTALL_DATA) ./files/sim-relay.keep $(1)/lib/upgrade/keep.d/sim-relay
endef

$(eval $(call BuildPackage,sim-relay))
