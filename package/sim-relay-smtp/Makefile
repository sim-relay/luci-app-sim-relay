include $(TOPDIR)/rules.mk

PKG_NAME:=sim-relay-smtp
PKG_VERSION:=0.3.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/sim-relay-smtp
  SECTION:=mail
  CATEGORY:=Mail
  TITLE:=Optional SMTP delivery helper for SIM Relay
  PKGARCH:=all
  DEPENDS:=+sim-relay +msmtp +ca-bundle
endef

define Build/Compile
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Package/sim-relay-smtp/install
	$(INSTALL_DIR) $(1)/usr/share/sim-relay
	echo 'SMTP delivery uses /usr/bin/msmtp with TLS.' > $(1)/usr/share/sim-relay/smtp-helper
endef

$(eval $(call BuildPackage,sim-relay-smtp))
