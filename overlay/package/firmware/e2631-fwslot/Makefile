include $(TOPDIR)/rules.mk

PKG_NAME:=e2631-fwslot
PKG_RELEASE:=1

PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=Zlion-Y

include $(INCLUDE_DIR)/package.mk

define Package/e2631-fwslot
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Flash and select the ZTE ZXHN E2631 firmware slots
endef

define Package/e2631-fwslot/description
  Installs vendor-format upgrade images into the inactive firmware slot and
  moves the boot selection by rewriting only the 512-byte version header, so
  the vendor system in the other slot can be started again without touching a
  byte of its kernel or root filesystem. "info" reports both slots.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	# Static on purpose: the first image has to be written from the vendor
	# system, which runs uClibc and has no musl loader. A static binary is the
	# only thing that runs there unchanged.
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -static -Wall -Werror \
		-o $(PKG_BUILD_DIR)/e2631-fwslot $(PKG_BUILD_DIR)/e2631-fwslot.c
endef

define Package/e2631-fwslot/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/e2631-fwslot $(1)/usr/sbin/e2631-fwslot
	$(INSTALL_DIR) $(1)/lib/upgrade/platform.d
endef

$(eval $(call BuildPackage,e2631-fwslot))
