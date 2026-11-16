include $(TOPDIR)/rules.mk

LUCI_TITLE:=LuCI support for SIM Relay
LUCI_DEPENDS:=+luci-base +sim-relay
LUCI_PKGARCH:=all
PKG_LICENSE:=MIT

# po/zh_Hans is built by luci.mk as the separate luci-i18n-sim-relay-zh-cn package.
# Do not add an i18n dependency to the English application package.
include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt scan.mk discovers this package through this marker.
# luci.mk already evaluates BuildPackage; do not evaluate it a second time here.
