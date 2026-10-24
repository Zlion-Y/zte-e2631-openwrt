# SPDX-License-Identifier: GPL-2.0-only
#
# The ZTE multiapd Controller runtime, vendored from cnjn/zte-mesh (SR1010,
# ZX279133) for the E2631 (ZX279128S). The runtime tarball with the vendor
# binaries and an ns-run for /usr/lib/zte-mesh/root is committed next to this
# Makefile; regenerate it in a cnjn/zte-mesh checkout with
# `sh tools/build/runtime.sh install`. ZTE_MESH_RUNTIME can point at another one.
#
# E2631 adaptation notes (the changes from the upstream package):
#   - DEPENDS @TARGET_zte -> @TARGET_sanechips (this tree's target name)
#   - PKGARCH dropped: the binaries are ARM32 EABI, native on this Cortex-A9
#     target (on cnjn's ZX279133 they run under AArch64 CONFIG_COMPAT instead)
#   - files/zte-mesh.config defaults: enabled 0 (upstream ships '1') and the
#     pushed SSID renamed ImmortalWrt -> E2631
include $(TOPDIR)/rules.mk

PKG_NAME:=zte-mesh
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
# The tarball carries proprietary ARM32 vendor code; keep it out of shared builds.
PKG_FLAGS:=nonshared

include $(INCLUDE_DIR)/package.mk

define Package/zte-mesh
  SECTION:=net
  CATEGORY:=Network
  TITLE:=ZTE mesh controller runtime (sandboxed vendor multiapd)
  DEPENDS:=@TARGET_sanechips +ip-full +kmod-veth +kmod-sched-core
endef

define Package/zte-mesh/description
  Runs the ZTE multiapd Controller in a private IPC/network/mount namespace
  and attaches its wire to the LAN bridge. On this 32-bit ARM target the ARM32
  vendor binaries run natively (no CONFIG_COMPAT needed); the kernel must have
  SysV IPC (CONFIG_SYSVIPC, CONFIG_IPC_NS - both already on).
endef

define Package/zte-mesh/conffiles
/etc/config/zte_mesh
endef

ZTE_MESH_RUNTIME ?= $(CURDIR)/zte-mesh-runtime.tar.gz

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	tar -C $(PKG_BUILD_DIR) -xzf $(ZTE_MESH_RUNTIME)
endef

define Build/Compile
endef

define Package/zte-mesh/install
	$(INSTALL_DIR) $(1)/usr/lib/zte-mesh $(1)/usr/libexec/zte-mesh
	$(CP) $(PKG_BUILD_DIR)/root $(1)/usr/lib/zte-mesh/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/ns-run $(1)/usr/libexec/zte-mesh/ns-run
	$(INSTALL_BIN) ./files/attach $(1)/usr/libexec/zte-mesh/attach
	$(INSTALL_DIR) $(1)/etc/init.d $(1)/etc/config $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) ./files/zte-mesh.init $(1)/etc/init.d/zte-mesh
	$(INSTALL_CONF) ./files/zte-mesh.config $(1)/etc/config/zte_mesh
	$(INSTALL_BIN) ./files/luci.zte-mesh $(1)/usr/libexec/rpcd/luci.zte-mesh
endef

$(eval $(call BuildPackage,zte-mesh))