// Generate native wiring patches 037-041: the zte/{Kconfig,Makefile} pair is
// created fresh (021 is gone on this line) and the parent ethernet Kconfig/
// Makefile gain the zte hooks. Authoritative method: write the b/ tree, then
// git diff --no-index with a=/ b=/ labels.
import fs from 'node:fs';
import { execSync } from 'node:child_process';

const W = 'D:/AI/Qoder/E2631-OpenWRT/native/_wire';
fs.rmSync(W, { recursive: true, force: true });
fs.mkdirSync(W, { recursive: true });

const shim = (work) => (cmd) => execSync(cmd.replaceAll('%W%', work), { encoding: 'utf8' });

function writePatch(pdir, a, b, out) {
  // git diff --no-index --src-prefix=a/ --dst-prefix=b/ <a> <b>, path labels fixed manually
  let d = execSync(`git diff --no-index --default-prefix "${a}" "${b}" || true`, { encoding: 'utf8' });
  // normalize the paths git printed into a/<tree> b/<tree>
  d = d
    .replaceAll(`a${a.replaceAll('\\', '/')}`, 'a/' + out)
    .replaceAll(`b${b.replaceAll('\\', '/')}`, 'b/' + out)
    .replaceAll(/^index .*\n/gm, '');
  d = d.replace(/^diff --git .*\n/m, '');
  fs.writeFileSync(pdir, d);
}

const PARENT = process.argv[2] ?? 'D:/AI/Qoder/E2631-OpenWRT/_wire-parent';
let K = `# SPDX-License-Identifier: GPL-2.0-only
config NET_VENDOR_ZTE
	bool "ZTE devices"
	depends on ARCH_ZTE || COMPILE_TEST
	default y
	help
	  If you have a network (Ethernet) device belonging to this class, say Y.

	  Note that the answer to this question doesn't directly affect the
	  kernel: saying N will just cause the configurator to skip all
	  the questions about ZTE devices.

if NET_VENDOR_ZTE

config ZX_NPU
	bool "ZTE ZX279128S npu scaffold"
	depends on OF
	help
	  Builds the npu scaffold. It exposes /proc/zx-npu/status and performs
	  no MMIO access; the vendor-native wired and MT7916 data paths are
	  added incrementally on the native line.

	  If unsure, say Y on this board.

`;
let M = `# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ZX_NPU) += zx279128s-npu.o
`;

// snapshots after each stage, for the five diffs
const stages = [];
const snap = (name) => {
  const dir = `${W}/${name}`;
  fs.mkdirSync(`${dir}/drivers/net/ethernet/zte`, { recursive: true });
  fs.writeFileSync(`${dir}/drivers/net/ethernet/zte/Kconfig`, K);
  fs.writeFileSync(`${dir}/drivers/net/ethernet/zte/Makefile`, M);
  fs.copyFileSync(`${PARENT}/Kconfig.orig`, `${dir}/drivers/net/ethernet/Kconfig`);
  fs.copyFileSync(`${PARENT}/Makefile.orig`, `${dir}/drivers/net/ethernet/Makefile`);
  stages.push({ name, dir });
};

snap('s1'); // 037 base

K += `config ZX_NPU_AUDIT
	bool "ZTE ZX279128S npu read-only register audit"
	depends on OF
	help
	  Maps the NPP/IDM/TM/PP accelerator windows and exposes a proc file
	  that reads a small documented set of registers only when userspace
	  writes "run". It never writes a register.

	  If unsure, say Y on this board.

`;
M += `obj-$(CONFIG_ZX_NPU_AUDIT) += zx279128s-npu-audit.o
`;
snap('s2'); // 038

K += `config ZX_NPU_IDM
	bool "ZTE ZX279128S vendor-native IDM fabric engine"
	depends on OF
	help
	  Builds the zxnpu0 netdev and the IDM RX/TX ring engine. The
	  hardware is untouched until zxnpu_idm.enable=1 and the netdev is
	  opened; the default firmware therefore keeps every write out.

	  If unsure, say Y on this board.

`;
M += `obj-$(CONFIG_ZX_NPU_IDM) += zx279128s-npu-idm.o
`;
snap('s3'); // 039

K += `config ZX_NPU_PPE
	bool "ZTE ZX279128S vendor-native PPE/CLA access"
	depends on OF
	help
	  Low-level CLA/SBRG indirect table access for the vendor-native
	  wired data plane. The proc interface is inert until
	  zxnpu_ppe.enable=1.

	  If unsure, say Y on this board.

`;
M += `obj-$(CONFIG_ZX_NPU_PPE) += zx279128s-npu-ppe.o
`;
snap('s4'); // 040

K += `config ZX_CSP_WLAN
	bool "ZTE ZX279128S vendor-native MT7916 G3 skeleton"
	depends on PCI && OF
	help
	  Vendor-native MT7916 host-driver skeleton. Binds only the 14c3:790a
	  ext-HIF function, reads BAR0 and the vendor firmware headers, and
	  performs no write until zxnpu_wlan.enable=1 and commit=1.

	  If unsure, say Y on this board.

endif # NET_VENDOR_ZTE
`;
M += `obj-$(CONFIG_ZX_CSP_WLAN) += zxnpu-wlan.o
`;
snap('s5'); // 041

// parent files: pristine 6.18 ethernet/{Kconfig,Makefile} come from the WSL tree

fs.mkdirSync(`${PARENT}/drivers/net/ethernet`, { recursive: true });
fs.copyFileSync(`${PARENT}/Kconfig.orig`, `${PARENT}/drivers/net/ethernet/Kconfig`);
fs.copyFileSync(`${PARENT}/Makefile.orig`, `${PARENT}/drivers/net/ethernet/Makefile`);
snap('s0'); // pristine (no zte dir, no hooks)

// parent edits live in each stage dir as full-tree files; add hooks to s1..s5
for (const s of stages.filter((x) => x.name !== 's0')) {
  let pk = fs.readFileSync(`${s.dir}/drivers/net/ethernet/Kconfig`, 'utf8');
  if (!pk.includes('ethernet/zte/Kconfig')) {
    pk = pk.replace(' endif # ETHERNET', 'source "drivers/net/ethernet/zte/Kconfig"\n\nendif # ETHERNET');
    fs.writeFileSync(`${s.dir}/drivers/net/ethernet/Kconfig`, pk);
  }
  let pm = fs.readFileSync(`${s.dir}/drivers/net/ethernet/Makefile`, 'utf8');
  if (!pm.includes('CONFIG_NET_VENDOR_ZTE')) {
    pm += 'obj-$(CONFIG_NET_VENDOR_ZTE) += zte/\n';
    fs.writeFileSync(`${s.dir}/drivers/net/ethernet/Makefile`, pm);
  }
}

// emit the five patches
const OUT = 'D:/AI/Qoder/E2631-OpenWRT/native/overlay/target/linux/sanechips/patches-6.18';
const pairs = [
  ['037-zte-add-the-ZX279128S-NPU-scaffold.patch', 's0', 's1'],
  ['038-zte-add-the-ZX279128S-NPU-read-only-audit.patch', 's1', 's2'],
  ['039-zte-add-the-ZX279128S-NPU-IDM-engine.patch', 's2', 's3'],
  ['040-zte-add-the-ZX279128S-NPU-PPE-CLA-access.patch', 's3', 's4'],
  ['041-zte-add-the-ZX279128S-CSP-WLAN-G3-skeleton.patch', 's4', 's5'],
];
for (const [file, a, b] of pairs) {
  let d = execSync(
    `git -C "${W}" diff --no-index --default-prefix "s${a === 's0' ? '0' : a}" "${W}/${a}" "${W}/${b}" || true`,
    { encoding: 'utf8' },
  );
  d = d
    .replaceAll(`${W}/`, '')
    .replaceAll(/^index .*\n/gm, '')
    .replace(/^diff --git a\/s[0-9] b\/s[0-9]\n/m, '')
    .replaceAll(/^--- a\/s[0-9]\//gm, '--- a/')
    .replaceAll(/^\+\+\+ b\/s[0-9]\//gm, '+++ b/');
  fs.writeFileSync(`${OUT}/${file}`, d);
  console.log(file, d.split('\n').length, 'lines');
}
console.log('done');
