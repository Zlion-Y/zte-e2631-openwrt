// Native cut-over edits: remove the open-source data plane from the kit copy.
import fs from 'node:fs';

const edit = (file, fn) => {
  const src = fs.readFileSync(file, 'utf8');
  const out = fn(src);
  if (out === src) { console.error('NO CHANGE: ' + file); process.exit(1); }
  fs.writeFileSync(file, out);
  console.log('edited ' + file);
};

// 1. seed config: drop the open-source wireless / offload packages, refuse them
//    explicitly so defconfig's DEFAULT_ machinery cannot bring them back.
edit('seed/e2631.config', (s) =>
  s
    .replace(/^CONFIG_PACKAGE_kmod-nft-offload=y\n/m, '')
    .replace(/^CONFIG_PACKAGE_kmod-mt7916-firmware=y\n/m, '')
    .replace(/^CONFIG_PACKAGE_wireless-regdb=y\n/m, '')
    .replace(/^CONFIG_PACKAGE_wpad-mbedtls=y\n/m, '')
    .replace(
      /^# CONFIG_PACKAGE_wpad-basic-mbedtls is not set\n/m,
      `# native-0.1: the open-source data plane is gone from this line. Each
# refusal survives "make defconfig" (same mechanics as the wpad-basic line
# below, verified there on 2026-10-28): mt7915e is the open wireless driver,
# the mt7916 firmware package feeds it, nft-offload is the flowtable hook the
# ghahramani PPE used, and wpad is the open AP userspace.
# CONFIG_PACKAGE_wpad-basic-mbedtls is not set
# CONFIG_PACKAGE_wpad-mbedtls is not set
# CONFIG_PACKAGE_kmod-mt7915e is not set
# CONFIG_PACKAGE_kmod-mt7916-firmware is not set
# CONFIG_PACKAGE_kmod-mt7615e is not set
# CONFIG_PACKAGE_kmod-mac80211 is not set
# CONFIG_PACKAGE_kmod-cfg80211 is not set
# CONFIG_PACKAGE_kmod-nft-offload is not set
`,
    ),
);

// 2. image Makefile: DEVICE_PACKAGES loses the wireless + offload kmods.
edit('overlay/target/linux/sanechips/image/Makefile.e2631', (s) =>
  s.replace(
    '  DEVICE_PACKAGES := kmod-mt7915e kmod-mt7916-firmware e2631-fwslot \\\n\tkmod-nft-offload ubi-utils block-mount \\',
    '  # native-0.1: kmod-mt7915e / kmod-mt7916-firmware / kmod-nft-offload are\n' +
    '  # deliberately gone - the open-source data plane does not ship on this line.\n' +
    '  DEVICE_PACKAGES := e2631-fwslot \\\n\tubi-utils block-mount \\',
  ),
);

// 3. apply.sh: strip the base-tree open-source Ethernet driver patch and the
//    FFE wiring patch before prepare; drop the FFE-patch assert that assumes 034.
edit('apply.sh', (s) =>
  s
    .replace(
      /^echo "kernel config: npu scaffold ON"$/m,
      `# native-0.1: the open-source data plane patches are removed from the base
# tree before prepare. 021 is the ghahramani-built Ethernet switch/gmac
# driver (eth0..3); 034 is the FFE control-plane wiring (P3 rebuilds it under
# the npu line's own rules). Everything else - SPI/NAND/PCIe/slotpart/DTS -
# is SoC infrastructure the vendor port owns and stays.
rm -f "$TREE/target/linux/sanechips/patches-6.18/021-net-ethernet-zte-Add-ZX279128S-Ethernet-switch-drive.patch"
rm -f "$TREE/target/linux/sanechips/patches-6.18/034-zte-add-the-ZX279128S-FFE-control-plane.patch"
echo "native: open-source eth/FFE patches removed from the base tree"
echo "kernel config: npu scaffold ON"`,
    )
    .replace(
      /^assert "FFE control plane source \+ wiring patch present" \\\n\tbash -c "test -f[^\n]*\n/m,
      '',
    ),
);
console.log('all native edits applied');
