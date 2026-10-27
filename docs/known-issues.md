# Known issues

The honest list, in two forms: a machine-readable block for tooling and grep, and one
short section per issue for the human who has to decide whether to care. "Verified" means
it was reproduced on this board with the evidence named; "unverified" means it is a
reasoned suspicion and must not be treated as a fact.

Schema: `id` is stable, `severity` is `blocker | major | minor`, `status` is
`open | mitigated | fixed | wontfix`, `surface` is what you are trying to do, `evidence`
points at something in the repository or at a measurement.

```yaml
- id: WIFI-ASSOC-HARD-HANG
  severity: major
  status: fixed
  surface: wi-fi
  symptom: a client associating stops the whole box - no console output, no ARP, no oops, no AER record
  verified: yes (docs/02 section P; board/ser-*.log)
  workaround: v1.3 auto-loads the radio after boot; if the box dies the pending marker retries the bring-up on the next boot (after 120 s, since 2026-10-25), so one power cycle recovers the router with Wi-Fi
  next: staged experiment plan in task list - one variable per flash cycle
  note: tigaotg (PR#1) verified dual-band APs, SSID/key change and reboot persistence on this hardware; repeated cold starts and association stress were NOT re-run. The PL310 io-read-lock flag is in the DTS since v1.3 (dmesg confirmed live on tigaotg's build)
- id: WIFI-COLD-BIND-HANG
  severity: major
  status: open
  surface: wi-fi
  symptom: the first firmware download after a power cycle succeeds on roughly one attempt in three
  verified: "yes (three cold boots on this exact image: hung, worked, hung)"
  workaround: since 2026-10-25 e2631-wifi-boot auto-heals - a boot that finds the pending marker clears it and retries the bring-up after 120 s instead of skipping the radio, and a bind that fails cleanly (box alive, radio never ready) gets the one documented rebind (e2631-wifi-on 0 again). A hang still costs one power cycle, but recovery no longer needs a manual reboot; deployed on the live unit as /etc/e2631-wifi-boot-hotfix until a flashed build ships the same logic in /sbin
  note: 2026-10-25 EP config-space comparison (live unit vs board/backup/vendor-ep0-config.txt) shows identical LnkCap/LnkCtl/LinkStatus - ASPM off, Gen1 x1, MPS128/MRRS512 on both - so the root cause is not EP configuration; it sits RC/PHY/driver-side, which means kernel work and a flash cycle. DevCtl differs only in the standard Linux error-reporting enable bits (ours 0x291f, vendor 0x2910)
- id: WIFI-OFF-BY-DEFAULT
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: no radio until you run two commands
  verified: yes (v1.2); v1.3 replaces the two gates with START=95 deferred autoboot
  note: v1.3 default is AUTO-LOAD ON (tigaotg's verified sequencing); opt back out with /etc/e2631-wifi-noautoload. Current first-boot code provisions an OPEN setup AP and does not preset a root password; isolate the setup network and set both passwords immediately (older security-default notes did not match the implementation - wording from zwthys-cyber's fork). Since v1.4 the shipped /etc/config/wireless carries disabled radio sections so the LuCI Wireless page exists from the very first boot (dispatcher caches its menu verdict; zwzzrl's device finding)
- id: HB-CONSOLE-NOISE
  severity: minor
  status: fixed
  surface: console
  symptom: the liveness beacon printed a line to the serial console every 3 s, on every boot, forever
  verified: yes
  workaround: the diagnostic beacon is opt-in since v1.3 - "touch /etc/e2631-hb-enable" restores it
- id: FWSTATE-NEEDS-DEVMEM
  severity: minor
  status: open
  surface: debugging
  symptom: /sbin/e2631-fwstate cannot work in the shipped image ("no /dev/mem")
  verified: yes, on the box
  next: build/wsl/build-regprobe.sh produces the module that reads the same register
- id: DEVMEM-DROPPED
  severity: major
  status: fixed
  surface: kernel config
  symptom: CONFIG_DEVMEM=y is requested in both generic and the target fragment, and the built .config comes out "not set"
  verified: "yes - root cause identified 2026-10-28: include/kernel-defaults.mk:119 appends every CONFIG_KERNEL_* line of the TOP-LEVEL .config to the merged kernel config with the prefix stripped, and the kconfig.pl merge is last-wins. config/Config-kernel.in:1551 declares KERNEL_DEVMEM (default n), so the top-level .config carried '# CONFIG_KERNEL_DEVMEM is not set', which the passthrough turned into '# CONFIG_DEVMEM is not set' AFTER the merged =y lines (visible in build_dir as .config.target:1599 =y vs :8873 =n). Neither fragment was wrong and no kernel patch touched the Kconfig - the package-level option won. The passthrough is generic: ANY '# CONFIG_KERNEL_X is not set' in the top-level .config silently forces kernel symbol X to n; cross-checked against every =y request in generic+target, DEVMEM was the only victim"
  fix: ask for the symbol where it is actually decided - CONFIG_KERNEL_DEVMEM=y in seed/e2631.config (the upstream-sanctioned passthrough), not another fragment edit. After the v1.4 build the out config must read CONFIG_DEVMEM=y with STRICT_DEVMEM/IO_STRICT_DEVMEM explicitly not set and GENERIC_LIB_DEVMEM_IS_ALLOWED=y
  note: on-device acceptance (fwstate reading MT_TOP_MISC through /dev/mem) still needs a flash cycle and stays unchecked under the offline-only rule; FWSTATE-NEEDS-DEVMEM therefore remains open
- id: WIFI-2G4-UNVERIFIED
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: the 5G AP is confirmed from a phone; the 2.4G radio had never been verified on air
  verified: "yes, by tigaotg (PR#1 handoff): phy0-ap0 2.4 GHz ch1 and phy1-ap0 5 GHz ch36 both reached AP-ENABLED and broadcast without logging in"
- id: ETH-THROUGHPUT-CEILING
  severity: major
  status: fixed
  surface: ethernet
  symptom: link trains at 1 Gbps; v1.2 plateaued at ~305/190 Mbit/s with no offload at all
  verified: yes - v1.3 measured 395 Mbps SUSTAINED via OPPO-mall CDN (32 browser threads), CPU 7%, WAN software counter flat (PPE forwards without touching the CPU stack); the earlier "200M ceiling" across three CDNs and 48 connections was Tencent-CDN per-IP rate limiting, not the router
  cause: v1.2 shipped no flow offload (missing kmod-nft-offload + the firewall flag) although the gmac driver has had the full PPE hardware path (zx279128s-ppe.c, TC_SETUP_FT) all along; WiFi-originated flows are architecturally rejected by the PPE (ingress must be a switch port) and stay on the CPU software path (~200M) - the vendor reaches gigabit over WiFi only via its private IDM/NPP engines on the 4.1 kernel
  workaround: none needed for wired; do NOT use /proc/net/dev WAN counters to measure PPE traffic (hardware-forwarded packets are invisible there) - read ppe_flows in /sys/kernel/debug/92000000.ethernet/dma_state instead
  next: find an unthrottled server to probe the true PPE ceiling (395M was still climbing when stopped); PPPoE uplinks may not take the hardware path (sibling ZX279133 hit exactly that)
- id: PPE-IPV6-TCP
  severity: major
  status: mitigated
  surface: ethernet + ipv6
  symptom: "with flow_offloading_hw=1, a LAN client -> router IPv6 TCP connection completes the handshake in ~1 ms and then no payload ever arrives (0 bytes, timeout); IPv4 over the same port and ICMPv6 are fine, so it reads as a random hang. Also stalls IPv6 TCP through the router."
  verified: "yes on hardware by the zwzzrl fork (2026-10-27, freshly flashed image with the v1.3 default 1/1): 1/1 -> connect ok, 0 bytes, 6 s timeout; 1/0 and 0/0 -> HTTP/1.0 200 OK in 1 ms. Reproduced on ports 80/22/53 with both ULA and fe80 destinations, flipped every time by fw4 stop/start. The mechanism is read off the driver source: zx_update_traps() cleared the IPv6 ethertype trap whenever sw_members != 0 (br-lan always has members) while zx_update_mac_tables() had moved our MACs from the trap-DMAC table into the ONU routed table, and the classifier holds IPv4 rules only (zx_ppe_v4_index/zx_ppe_v4_rule) - so an IPv6 frame addressed to the router had no path to the CPU left"
  workaround: "v1.4 fixes the driver instead of dropping the feature (owner decision: wired acceleration stays on by default): the overlay copy of zx279128s-ppe.c keeps the IPv6 ethertype trap on whenever the PPE is active, so IPv6 always reaches the CPU (the trap fires before the flow lookup) and rides the software path - correct, LAN-to-LAN included - while IPv4 keeps the PPE. [实测 for the bug + mechanism; 推断 for the fix: the change is source-verified against the fork's measurements but has NOT been device-verified on this build - if IPv6 TCP stalls again, switch off 硬件流量卸载 in LuCI (Network -> Firewall), which is the fork's measured-good state, and report it]"
  next: "device-verify the trap fix on an IPv6-capable uplink (the fork's exact repro: connect + fetch over ULA and fe80 with 1/1). The vendor's own answer is a full IPv6 classifier (per-class NPU caps npu_ipv6_l3tcp{up,down}_cap, IPv6 SNAT/DNAT RAM, NPTv6, local-IPv6 register files - stock vmlinux never uses nf_flow_table/TC_SETUP_FT at all); replicating it is the large-change option and stays out of scope. Cost of the current fix: IPv6 LAN-to-LAN bridges over the CPU while offload is on"
- id: USERDATA-EXTROOT
  severity: minor
  status: mitigated
  surface: storage
  symptom: the 47 MiB window behind both image slots (0x3f00000..0x6e00000) is unused while the internal jffs2 overlay holds only ~8 MiB
  verified: yes - the window is free in every vendor version checked (all four upgrade packages 4B4..8B8 declare identical slot tables ending at 0x3f00000; the stock kernel declares nothing beyond 0x2fc0000; the programmer dump shows 0x3cc0000..0x72c0000 erased); tigaotg ran a UBIFS extroot on it for weeks
  workaround: "v1.3 ships the partition plus a gated extroot: preinit attaches the UBI only, /etc/init.d/e2631-userdata enables the fstab mount only after reading a real UBIFS superblock (magic 0x06101831), and /sbin/e2631-userdata-init formats the volume only under an explicit E2631_CONFIRM"
  note: boot can never format the volume (a read-write mount of a blank volume would auto-format it; the gate keeps that out of the boot path). The JFFS2-to-UBIFS migration prototype from PR#1 is NOT shipped - whiteouts/xattrs/power-fail atomicity are unsolved there
- id: PACK-NEEDS-VENDOR-TEMPLATE
  severity: major
  status: fixed
  surface: packaging
  symptom: no flashable package without one official E2631 upgrade package as a version-header template
  verified: yes
  note: "fixed by commit 8dfcf4d (2026-10-15): seed/vendor-verhdr-8b8.bin (532 B) ships in-repo as the default; packing with it reproduced the published v1.2 byte for byte. A fully synthesized header remains untested against the vendor page - see the human-readable section below"
- id: ROOTFS-SLOT-ASSUMPTION
  severity: blocker
  status: mitigated
  surface: flashing
  symptom: "an image written to SLOT 0 panics at boot (\"VFS: Unable to mount root fs on /dev/mtdblock7\") and reboot-loops"
  verified: "yes (2026-10-24 serial log, v1.3 first flash: cspboot table showed our kernel@0x700000[0x3fffe0] fs@0xb00000 in slot 0 with serial 0x31 vs vendor@slot1 0x30; the kernel's rootfs partition was hard-wired to the slot-1 window 0x2700000, so mtd7 read encrypted vendor bytes from slot 1)"
  workaround: "v1.4 replaces the bootpara fixup with the zx279128s-slotpart MTD partition parser (compatible 'zte,zx279128s-slotpart'). The v1.3 fixup's premise did not hold on hardware: it read cspboot's bootpara block from RAM at 0x40007000, which the zImage decompressor uses as page tables (dumped on the serial console - it never once found its signature and always fell back to the built-in slot-1 layout). The parser instead re-derives cspboot's decision from the flash itself at partition-parse time - reads both slot headers through the normal mtd path, keeps the valid CRC1 with the higher serial, ties to slot 0 - and on a slot-0 boot moves exactly two partitions (rootfs, slot1-header) into slot 0, keeping the row count and every label so root=/dev/mtdblockN stays correct. Every failure (unreadable/invalid headers, missing DT data) keeps the built-in slot-1 layout, so there is no new failure mode. Decision logic passes 11/11 host unit tests on the exact code the kernel runs (tests/test-slotpart-decisions.c, g_slot_tool); on-device acceptance - actually booting from slot 0 - still needs a flash cycle and stays unchecked under the offline-only rule, hence mitigated, not fixed"
  note: the assumption was architectural since v1.0 (the DTS comment said "OpenWrt is installed into slot 1"); October's 16/16 flashes all wrote slot 1 because the vendor system was always the active one in slot 0 - the precondition broke for the first time on 2026-10-24 after serial churn. As of P0-0 the writers are content-guarded instead of slot-numbered (upgrade in place of the running slot, install requires --replace-other-system to touch another system), and the dts partitions node now carries compatible "zte,zx279128s-slotpart" with the slot geometry the parser reads
- id: ZTE-MESH-CONTROLLER
  severity: minor
  status: open
  surface: mesh
  symptom: "the ZTE mesh controller (vendored cnjn/zte-mesh: the factory multiapd in a private IPC/network/mount namespace) has never run on THIS board"
  verified: no - upstream verified it end to end on the SR1010/ZX279133 with real ZTE child routers (Z506); on E2631 only the packaging is verified
  workaround: "the service ships DISABLED; enabling it pushes the configured Wi-Fi profile to every ZTE child router on br-lan (LuCI: Network -> ZTE Mesh)"
  note: "adaptations from upstream: @TARGET_sanechips instead of @TARGET_zte, PKGARCH dropped (the ARM32 vendor binary is native on this Cortex-A9; on the ZX279133 it runs under AArch64 compat), config default enabled 0 and SSID E2631 instead of ImmortalWrt. Kernel prereqs already on: CONFIG_SYSVIPC, CONFIG_IPC_NS, NET_SCHED; kmod-veth/kmod-sched-core/ip-full come in as deps. The runtime tarball carries proprietary ZTE binaries (marked nonshared), same redistribution posture as the vendor-materials release"
- id: FACTORY-SSID-FROM-TAG
  severity: minor
  status: fixed
  surface: wi-fi
  symptom: the first-boot SSID was a hard-coded "E2631" instead of this unit's own factory name
  verified: "the tag pipeline was validated against the real dump (build/board/backup/mtd2-tag.bin: pair \"ChinaNet-ygckk9\"/\"ChinaNet-ygckk9-5G\", plus what looks like the factory key pair \"dthe3524\" twice); on-device run pending"
  workaround: none needed - e2631-wifi-boot scans the tag's first page for a printable run S whose S+"-5G" sibling also exists (self-validating, no byte offsets pinned), falling back to "E2631"
  note: the tag also carries SN, carrier code (1630@CTCC on this unit) and model "ZXHN E2631"; the factory-key pair was deliberately NOT auto-used - confirm it against the label sticker first
- id: LED-DAEMON-NOOP
  severity: major
  status: fixed
  surface: leds
  symptom: the LED daemon had never driven a single LED since day one
  verified: yes - find_led matched "*:<colour>" but the LED names are "<colour>:<function>" (red:status, green:wan-online), so $red/$green/$blue were always empty
  workaround: fixed in v1.3 - names matched on either side, /etc/diag.sh overlay replaces the stock boot-blink (600 ms slow) and stops the done state lighting green unconditionally
  note: "v1.3 semantics (user-specified): boot <45s red slow blink -> online green steady / offline red steady -> red-to-green on connect, 2 s default-route poll"
- id: NETUNE-RESET-ON-RELOAD
  severity: minor
  status: fixed
  surface: network
  symptom: rps_cpus flips back to 1 (CPU0) around link events despite the boot-time netune
  verified: "yes - watched live through a wan bounce: ifup sets 2, fw4's procd-triggered reload resets it to 1 one to three seconds later"
  workaround: v1.3 ships /sbin/e2631-netune-apply called from the iface hotplug (fast path) and every minute from cron (backstop - last writer wins)
- id: JSMIN-CHANGES-HTDOCS-MD5
  severity: minor
  status: fixed
  surface: build
  symptom: "the md5 of a JS view in the image never matches the source copy, and grep for source-formatted strings finds nothing - looks exactly like a stale build cache"
  verified: "yes - chased as a cache bug for four hours (apk purge + staging purge + full target clean + three rebuilds); the md5 difference was CONFIG_LUCI_JSMIN=y all along (feeds/luci/luci.mk compresses every htdocs JS during package install)"
  workaround: "compare by minification-safe substrings (expect:{ not expect: {), never by md5 against the source; the fixes WERE in the image every time"
- id: SPI-NAND-PART-COVERAGE
  severity: major
  status: wontfix
  surface: flashing another board
  symptom: a unit whose flash is not in the upstream spi-nand table gets no rootfs, so it stays on the vendor slot (not bricked)
  verified: yes for the DS35Q1GA/GB 128/256 Mbit batch (docs/02 section S.8, S.9)
  note: adaptation was evaluated on 2026-10-15 and deliberately not done; the method to do it is documented
- id: APPLY-COPIES-NOT-SYNC
  severity: minor
  status: mitigated
  surface: build
  symptom: a patch deleted from overlay/ stays in the tree and keeps being applied
  verified: yes (two abandoned Dosilicon SPI-NAND patches survived their own deletion)
  workaround: apply.sh now lists tree patches that are in neither the base commit nor overlay/
- id: GENERIC-FRAGMENT-WINS
  severity: minor
  status: fixed
  surface: kernel config
  symptom: asking for a symbol in target/linux/sanechips/config-6.18 changes nothing when generic names it as "is not set"
  verified: yes (softlockup / hung-task detectors)
  workaround: flip it where it is decided; the witness is the built kernel's own .config, never a fragment
- id: WINDOWS-CHECKOUT-TRAPS
  severity: minor
  status: mitigated
  surface: build
  symptom: a CRLF working copy breaks kconfig ("unknown statement \"If\""); a case-insensitive filesystem collapses path pairs that differ only in case
  verified: yes (13 pairs were silently merged once)
  workaround: .gitattributes forces LF; build from `git archive HEAD | tar -x`; compare trees by blob hash, not by name
- id: SUPGRADE-NEEDS-A-BROWSER
  severity: minor
  status: wontfix
  surface: flashing
  symptom: the vendor page's login is RSA + sha256(plaintext + salt) with a CSRF pre-authorisation, so route A cannot be scripted here
  verified: yes (docs/02 section E)
  workaround: log in once in a browser and pick the file; or use host/stage.mjs over the vendor telnet service
- id: DELIVERY-SOURCE-MUST-MATCH-THE-NIC
  severity: minor
  status: mitigated
  surface: flashing
  symptom: host/stage.mjs serves the file from the PC address it is told, and the box can only reach the interface actually wired to it
  verified: yes (the PC's LAN address moved 192.168.5.5 -> 192.168.5.17 when the adapter changed)
  workaround: pass --pc with the address of the interface that has the router on it
- id: NO-CI
  severity: minor
  status: fixed
  surface: contributing
  symptom: nothing runs on a push; the build happens on someone's machine
  verified: yes - .github/workflows/check.yml (make check on push/PR + the upgrade-check refusal assertion) and firmware.yml (on-demand build + prerelease with SHA256SUMS), adopted from the zwthys-cyber fork and adapted; note the workflows are local-only until the owner pushes
  note: the old 'deliberate' note protected a one-machine build; the gates were already CI-shaped (make check), the fork proved it
- id: REGDB-NOT-SHIPPED
  severity: minor
  status: fixed
  surface: wi-fi regulatory
  symptom: "# CONFIG_PACKAGE_wireless-regdb=y was commented out in the seed, so the image leaned on the driver's built-in regdomain and `iw reg get` reported the default instead of a database-backed CN view"
  verified: yes (seed/e2631.config inspection)
  workaround: v1.4 selects CONFIG_PACKAGE_wireless-regdb=y; the shipped etc/config/wireless sets country CN on both radios
  note: on-air acceptance (per-channel power table, DFS CAC behaviour) needs a radio and stays unchecked under the offline-only rule
- id: WIFI-PROVISION-HT20
  severity: major
  status: fixed
  surface: wi-fi rates
  symptom: 2.4G negotiated at most ~287 Mbit/s PHY (never the ~500 the vendor runs) and 5G lost 80 MHz rates - provisioning set NO htmode, and netifd falls back to HT20
  verified: "yes - the v1.4 e2631-wifi-ap provisioned channel/country only; netifd's mac80211 generator defaults to HT20 without an explicit htmode. The vendor's DBDC profile is 2x2 HE40 (574 Mbit/s PHY) for 2.4G"
  workaround: v1.5 provisions per band - 2.4G HE40 + noscan (the vendor's forced-40MHz behaviour), 5G HE80 - in both e2631-wifi-ap and the shipped /etc/config/wireless; the RF calibration itself was always the vendor's own /wlan/caldata blob (chip id 0x7916) handed over whole
  note: rate/RSSI A/B against the vendor unit needs a flash cycle and stays unchecked under the offline-only rule; the per-rate SKU tuning inside the vendor's mt7916.dbdc.*.dat profiles is not consumed by mt76 (those files were never pulled from the vendor box - only listed)
- id: WPAD-BASIC-DEFAULT
  severity: minor
  status: fixed
  surface: wi-fi features
  symptom: the image shipped wpad-basic-mbedtls (the DEFAULT variant), which has no 802.11r/k/v, no mesh, and no WPA3-SAE - P2-3's baseline was unreachable no matter the uci config
  verified: yes (no wpad line in the seed or DEVICE_PACKAGES; OpenWrt picks the default variant)
  workaround: v1.4 selects CONFIG_PACKAGE_wpad-mbedtls=y (selecting a variant suppresses the default); ieee80211k/v/r + SAE + mesh config is left to the user's own wireless setup
  note: roaming/mesh acceptance needs a second radio or client and stays unchecked under the offline-only rule
```

## WIFI-ASSOC-HARD-HANG

The one that decides everything else about Wi-Fi. A station completing association stops
the machine at a level that produces no report: the console goes silent mid-line, ARP
stops answering, and the only recovery is a power cycle. It is not a kernel panic, not an
oops, not an AER event, and it is not new - the three published packages are byte-identical
in the driver, the firmware blob and the bring-up script (`docs/02` section S.7), so no
release introduced it and no release can be blamed for it.

What has been ruled out with measurements: MPS/MRRS config-space differences from the
vendor system (identical), the proprietary-versus-mt76 driver question (the stock
`mt7916.ko` is MTK's own driver, not mt76, so a like-for-like comparison is not
available), and the firmware blob itself (the vendor's 2025-01-03 blob does not fix it).
What is still standing as a hypothesis: something about the first PCIe transaction the
chip issues on behalf of a newly associated station, on a bus where this SoC's L2 cache
has to be serialized for register reads. The staged plan for chasing it is in the task
list, and it is written to be one variable per flash cycle, because a flash cycle here
costs a power cycle if it goes wrong.

Practical consequence, and the reason the image behaves the way it does: the driver is not
loaded at boot. Bringing the radio up is two commands, run with the serial console
attached, and a failed experiment then costs a reboot rather than the box.

## DEVMEM-DROPPED and the instrumentation that depends on it

`CONFIG_DEVMEM` is asked for twice - in `target/linux/generic/config-6.18` and in the
target fragment - and the built config says it is not set. Nothing in the tree asks for
`n`, and `drivers/char/Kconfig` has it as a plain `bool` defaulting to on. The mechanism
that drops it is still unidentified, which is why this is listed as an open issue rather
than a solved one: the honest state is "we do not know, and a fragment edit written on the
assumption that it will work is a waste of a build".

The consequence is that `/sbin/e2631-fwstate`, which reads the MT7916's `MT_TOP_MISC`
register to answer "what is the firmware doing right now", cannot run in the shipped
image - it fails with `no /dev/mem`. `wsl/build-regprobe.sh` builds a module against
exactly the kernel this kit ships, and that module is the replacement: it reads the same
register from inside the kernel, where the address space is available.

## PACK-NEEDS-VENDOR-TEMPLATE

The packer copies the version header from a real package and rewrites the geometry and CRC
fields. That is enough to produce a file the vendor web page accepts, and it has been
accepted - but it means a stranger cannot pack without obtaining a vendor binary first,
which this repository will not provide or link.

Two things are worth knowing if that blocks you. The format is not secret: every field of
four official packages was decoded by recomputing CRC1 and CRC2 (`docs/01` section 2), and
`tools/make_fixture.mjs` already builds a self-consistent header from nothing, which is
what the slot tool's tests run against. So the synthetic route is plausible. What has not
been tested is whether the *page* accepts it: `upgradeKey1/2` (`0x1351/0x5211`) were the
same in all four packages, but the page also compares product and hardware strings, and we
have no data on what it refuses. If you try it, the failure is a rejected upload, not a
bricked board - and `tools/verify_pkg.mjs` will tell you what your header looks like before
you spend the attempt.

## SPI-NAND-PART-COVERAGE

Whether a given unit boots our image depends on the flash part, not on the board: the
kernel matches SPI-NAND on (manufacturer, device) ID, and an unknown ID means the MTD
device never appears, so there is no rootfs and the box falls back to the vendor slot.
For the DS35Q1GA / DS35Q1GB batch the comparison was done end to end (`docs/02` sections
S.8, S.9): the vendor's own table has parts ours lacks, and adapting them is possible but
each part needs its own OOB layout and ECC request - `ECCREQ(4,512)` with 64-byte OOB on
one, `ECCREQ(8,512)` with 128-byte OOB on the sibling, and getting that wrong is a
silently uncorrectable page. The decision on 2026-10-15 was to stop there. The method to
resume it - carve the vendor kernel's flash table out of the package and diff it against
upstream - is scripted in `wsl/vendor-flash-table.sh`.
