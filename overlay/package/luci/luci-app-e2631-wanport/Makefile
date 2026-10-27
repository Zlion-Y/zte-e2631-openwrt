# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

# The "any wired port can be the WAN" page. e2631-wanport itself (script plus
# init script, hotplug hook and /etc/config/e2631_ports) ships inside the
# target's base-files, so there is nothing to depend on here: this package is
# only the LuCI front end.
LUCI_TITLE:=WAN/LAN adaptive: pick which wired port is the WAN
LUCI_DEPENDS:=+luci-base
LUCI_PKGARCH:=all

PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_PO_VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
PKG_LICENSE:=GPL-2.0-only

include $(TOPDIR)/feeds/luci/luci.mk

# call BuildPackage - OpenWrt buildroot signature
