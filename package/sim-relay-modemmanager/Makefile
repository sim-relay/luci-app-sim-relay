include $(TOPDIR)/rules.mk
PKG_NAME:=sim-relay-modemmanager
PKG_VERSION:=0.5.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT
include $(INCLUDE_DIR)/package.mk

define Package/sim-relay-modemmanager
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Optional private PIN helper for SIM Relay ModemManager devices
  DEPENDS:=+sim-relay +libdbus
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(CURDIR)/../../src/mm_pin.c $(PKG_BUILD_DIR)/
endef

define Build/Configure
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_CPPFLAGS) -std=c99 -Wall -Wextra -Werror -I$(STAGING_DIR)/usr/include/dbus-1.0 -I$(STAGING_DIR)/usr/lib/dbus-1.0/include $(PKG_BUILD_DIR)/mm_pin.c $(TARGET_LDFLAGS) -ldbus-1 -o $(PKG_BUILD_DIR)/sim-relay-mm-pin
endef

define Package/sim-relay-modemmanager/install
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/sim-relay-mm-pin $(1)/usr/libexec/
endef
$(eval $(call BuildPackage,sim-relay-modemmanager))
