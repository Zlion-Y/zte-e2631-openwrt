# OpenWrt for the ZTE ZXHN E2631 (Sanechips ZX279128S)

[中文](README.md) · **English**

**To build this: [BUILD.md](BUILD.md).** It is the single supported path from a clone to
a package that flashes, and it lists the two inputs this repository cannot hand you (the
pinned base tree, the vendor version-header template) plus the failure modes.
`make check` runs every gate. [docs/architecture.md](docs/architecture.md) maps the
pieces; [docs/known-issues.md](docs/known-issues.md) is the machine-readable honest list,
including what is still broken on real hardware.

This one repository holds the whole port: the overlay that turns the base tree into an
E2631 build (`overlay/`), the local build path (`wsl/`, grouped in `wsl/INDEX.md`, and the
portable entry points in `scripts/`), the board-side scripts and the vendor-side flash
plans (`board/`), the Windows-side tooling (`host/`), and the two working documents with
the evidence notes (`docs/`). There is no CI any more - the build happens in WSL on this
machine, and `make check` (which runs `wsl/check-kit.sh` and the ash/dts gates) plus
`node build/host/release.mjs` are the checks that used to live in a workflow.

    git clone --branch pr-zte-zxhn-h3600 https://github.com/Zlion-Y/openwrt-sanechips-mirror openwrt
    git -C openwrt checkout db8b8d4130457dbe46558a7cea872dd2dae539e6
    ./apply.sh openwrt

`scripts/bootstrap.sh` does those three lines, the feeds and the seed config, and
witnesses each one; `scripts/build.sh` then continues to a flashable package.

Packaging into the vendor upgrade format is `node host/release.mjs --dir <out/build-dir>`
from anywhere: `out/`, the base tree and the template package resolve through
`E2631_WORK`/`E2631_TREE`/`E2631_TEMPLATE` (environment or `.env`, see
`.env.example`), so the run no longer depends on which directory you stood in. It packs,
then re-derives the geometry independently and screens the artifact for the things that
stop a boot. The version-header template **ships here** as `seed/vendor-verhdr-8b8.bin` -
532 bytes, the outer header plus that 512-byte version header - so packing needs no
`--template` and no vendor firmware on your disk; measured, packing one build directory
with the slice and with the full 21 MB official package gives **the same bytes**
(`02b35106…`, which is the published `v1.2`). [seed/README.md](seed/README.md) carries the
provenance and the caveats about re-cutting your own.

Three screens run before the forty-minute build is allowed to be the test:
`tools/dts_lint.mjs` resolves the board file's includes, `&labels` and
dt-binding macros against the kernel patches and the fetched headers; the
slot tool is compiled natively with `-Werror` and has to accept a synthesized
package and reject one byte-flipped copy; and `tools/verify_img.mjs` checks
the finished image for the things that stop a boot - `root=` against the
partition order the dtb really creates, built-in flash/squashfs/jffs2
drivers, the forced command line, USB and watchdog staying disabled,
soft-float, and the root filesystem's package list.

What is deliberately different from the H3600 board file:

* one WAN + three LAN, all four on the integrated GbE PHYs - no external PHY
* 256 MiB RAM, not 512
* whole-image A/B slots (0x700000 / 0x2300000, 28 MiB each) instead of a
  4 MiB kernel partition plus UBI
* no USB: the vendor kernel has no USB at all, and probing the controller on
  boards where it is not populated reboots the SoC
* soft-float userspace: this SoC implements no VFP, so CONFIG_VFP must stay off
* JFFS2 clean markers go in the data area, not the OOB (patches 029 and 030):
  SPI-NAND with on-die ECC regenerates page ECC on every program, so a page
  written twice comes back uncorrectable. Without this the overlay fails to
  remount and the box is stuck on a read-only root.
* the kernel command line is compiled in and CONFIG_CMDLINE_FORCE is on: the
  vendor U-Boot passes `root=/dev/mtdblock8 rootfstype=jffs2 init=/bin/sh`,
  which names the vendor layout and would replace PID 1 with a shell.

The two config edits above are per-target, so a tree that has been overlaid here
builds the E2631, not the H3600.

## Building it inside WSL

For a Linux box with no Windows in the loop, use `scripts/bootstrap.sh` and
`scripts/build.sh` - they are the portable path and they read the same `.env`. What
follows is the WSL-specific half of this machine: the same steps, plus the workarounds
for the network here.

The Windows side of this machine has no cross toolchain, WSL does, and
`wsl/build.sh` runs the same steps CI does against `$E2631_TREE` with this repo at
`$E2631_KIT`. What differs is the network: no proxy here, and the mirrors that
answer throttle per connection (kernel.org 73 KB/s, sources.cdn.openwrt.org
25 KB/s, sources.openwrt.org 14 KB/s, musl.libc.org nothing at all inside a 12 s
probe). Three things keep that from owning the build:

* `wsl/segdl.sh` fetches every distfile as eight parallel byte ranges and is
  wired in through `DOWNLOAD_TOOL_CUSTOM` - the only hook scripts/download.pl
  offers, since its select_tool() prefers curl and never reaches aria2c. The cap
  is per connection rather than per host, so linux-6.18.55.tar.xz (147 MiB)
  arrived in about three minutes at ~288 KB/s aggregate, and its sha256 matched
  the one in the download.pl command line, so the result is byte-exact rather
  than merely fast. A server that answers a range request with 200 and the whole
  file (mirrors.huaweicloud.com does) falls back to one connection, otherwise
  every worker would receive the same bytes.
* `CURL_OPTIONS="--speed-limit 20000 --speed-time 20"` gives curl a transfer
  watchdog, which OpenWrt does not set by default, so a mirror that accepts the
  connection and sends nothing fails over instead of blocking forever. A
  finished toolchain once sat for five minutes on the 1 MiB musl tarball.
* `wsl/parget.sh <url> <dl/name> [conns] [sha256]` is the same trick by hand,
  for a file that has to land before the build is restarted.

`make download` is deliberately not run up front: it walks every package in the
tree, so it was fetching broadcom-wl and libwebsockets - neither of which this
build selects - while the selected packages waited.

`wsl/collect.sh <out-dir>` then lays the build out the way the CI artifact does
(`kernel` / `root` / `e2631.dtb` / `config-6.18` / `e2631-fwslot`), which is
what `node build/host/release.mjs --dir out/<dir>` packs and screens. The Windows
drives are mounted inside the distro, so nothing has to be piped over stdio.
`wsl/INDEX.md` groups the other scripts in there (build path / patch generators /
witnesses / probes) so the file count does not hide which ones to run.

Publishing is the last local step, since there is no CI any more:
`node build/host/publish.mjs --tag <t> --bin out/<package>.bin --notes out/<notes>.md`
creates the Release, uploads the package plus a `SHA256SUMS`, and prints GitHub's own
asset digest so the upload can be compared with the local sha256. It refuses a file whose
first 16 bytes are not the four vendor magic words, so a half-packed package cannot be
published by accident.

One gate gotcha worth knowing: `wsl/inspect-rootfs.sh` used to read `/tmp/finalroot`
without ever extracting into it, so it reported whatever the previous run had left there --
it now extracts first. Any "what is really in the rootfs" answer that comes from a
directory you did not just unpack is a stale answer.

## Status on the real board (2026-10-24, v1.3; the v1.2 notes are kept below)

Current package: Release **`v1.3`** -- the theme is "from flashable to usable", with the evidence
chain for every item in [docs/known-issues.md](docs/known-issues.md):

1. **Flow offloading on by default (hardware acceleration)**: the v1.2 ceiling of 305/190 Mbit/s
   was never a missing engine -- the gmac driver has carried the PPE hardware path
   (`zx279128s-ppe.c`, TC_SETUP_FT) all along; what was missing is the nft flowtable statement
   package and the firewall switch.  v1.3 ships `kmod-nft-offload` plus
   `flow_offloading=1` (and hw), and defaults RPS/XPS, IRQ affinity and PAUSE-frame
   advertisement off (that last one traced to an upload ceiling on the sibling ZX279133 port,
   right.com.cn thread-8486503).  Known limits: PPPoE may not take the hardware path; the PPE
   table holds 384 entries, overflow falls back to software.
2. **Wi-Fi autoload on by default**: sequencing and recovery from tigaotg's handoff PR#1
   (verified on this hardware: dual-band auto-broadcast, SSID/key change survives reboot).
   First boot provisions WPA2-PSK (key derived from the unit MAC, printed on the console),
   never an open network.  Opt out with `/etc/e2631-wifi-noautoload`.  The association
   hard-hang stays open (see known-issues).
3. **47 MiB userdata partition (optional extroot, triple-gated)**: the window
   0x3f00000..0x6e00000 is free in every vendor version checked (all four upgrade packages
   declare identical slot tables; the stock kernel declares nothing beyond 0x2fc0000; the
   programmer dump shows 0x3cc0000..0x72c0000 erased).  preinit only attaches the UBI; the
   activation gate requires a real UBIFS superblock before enabling the fstab mount (boot can
   never format the volume); the initializer needs an explicit E2631_CONFIRM.
4. **The diagnostic heartbeat is opt-in** (`touch /etc/e2631-hb-enable` restores it); LuCI moves
   to the official Bootstrap theme (apply.sh no longer clones a third-party repo).

### The v1.2 wording, kept for its measured numbers

`v1.2` -- `e2631-openwrt-v1.2.bin`, 20,972,052 B, sha256
`02b35106a1259ed1aed04c35cb6b037c635b6fb68f3e2906da2931cb96d12ddc`.  Its kernel region is
byte-identical to the `gen1` image in the table below (the one flashed and run on the box), so the
boot path carried no new variable; everything that differed was in the rootfs:

| release | rootfs side |
|---|---|
| `v1.0` | first package that flashes and boots; no `/sbin/e2631-wifi-ap`; the 600 s trial gate still auto-reverted to the stock slot |
| `v1.1` | adds `/sbin/e2631-wifi-ap`; the first-boot auto-revert becomes opt-in (`touch /etc/e2631-trial-arm`) |
| `v1.2` | fixes that helper: named-section parsing (the generator writes `radioN`/`default_radioN`, not `@wifi-iface[n]`), the `+N` path suffix for a second phy on one device, and a `wifi down/up` cycle instead of a bare `wifi reload`.  Confirmed on the box: `phy1-ap0 type AP ssid E2631 channel 36 (5180 MHz)`, hostapd running, **phone sees the 5G network** |

Manual radio, two commands (`/sbin/e2631-wifi-on 0` then `/sbin/e2631-wifi-ap`) -- the verified
v1.2 path, still available as the fallback.

| | |
|---|---|
| flash through the vendor `supgrade.html` page | **works** - the page accepted the package as-is, wrote slot 1, serial went 33 -> 34 on its own, and rebooted into our OpenWrt |
| switch back to the vendor system from OpenWrt | **works** - `e2631-fwslot switch` + `reboot`, also reachable from failsafe |
| LuCI (Argon theme), `reboot`, the three-colour LED | **work** |
| Ethernet | link trains at 1 Gbps, but throughput plateaus at ~305 Mbit/s in and ~190 Mbit/s out - the driver has no checksum/TSO/SG/RSS offload and one RX queue, and all of that work lands on CPU0 even though both cores are online |
| Wi-Fi (MT7916) | **usable, not stable, off by default** - see below |

The radio does not come up unless `/etc/e2631-wifi-autoload` exists or someone runs
`/sbin/e2631-wifi-on 0`. That is a deliberate choice, not a workaround: with the driver
loaded from `rc.d`, a failed experiment cost the ability to get back into the box.
Everything up to "AP beaming, monitor interface, air RX" has been verified alive on this
board. Two things have not: the **first firmware download after a power cycle** succeeds on
roughly one attempt in three (three cold boots on this exact image: hung, worked, hung),
and a **client associating** still hard-hangs the machine, silently, with no AER record and
no oops. Both are documented with their sample data in `docs/02-...md` section P.

## What is deliberately not in here

`out/` stays outside the repository, at `$E2631_WORK` (on this machine
`D:\AI\Qoder\E2631-OpenWRT\out`): every build directory, packed image, log and one-off
experiment script. It is 1.1 GB of things that are either regenerable or evidence with a
filename quoted in `docs/`, and `docs/` says so inline - a path written as `out/xxx.log`
means that directory, not this repo. `.env` is not in here either (`.env.example` is): it
is the one file that may name a path from somebody's disk. Also excluded by
`.gitignore`: `board/harvest-H8.txt` (a 356 MB vendor system harvest),
`board/backup/` and `board/web/` (mtd and driver dumps, the scraped vendor UI),
`board/config18.xml` and `board/webcreds.out.txt` (a vendor config export and a deliberate
grep for password fields - neither belongs on GitHub), and `host/askpass.bat` (a
git-credential helper whose only job is to hand a token to git).

A note on `host/`: several of those scripts are one-off analyses that read evidence
files - transcripts, mtd dumps, the vendor config export - which are deliberately not in
git. They are here because `docs/` cites the numbers they printed, not because they are
runnable on your machine. The ones that are part of the supported path are
`release.mjs` (pack + both screens), `verify_pkg.mjs`, `publish.mjs`, `stage.mjs` and
`test-token-expansion.mjs`; the build-side tools are in `tools/` and `wsl/`.

Two sibling repos of ours are related. `Zlion-Y/e2631-mainline-zx279128s` (mainline
Linux port) stays a separate effort - it is still where the real "this SoC boots"
evidence comes from, and since 2026-10-14 it holds nothing but the kernel: the TFTP /
U-Boot bring-up path it used to ship was retired to that repo's `v0.1.1` tag.
`Zlion-Y/e2631-openwrt`, the stage-1 initramfs scaffold, has been folded in here: its
history is the `stage1-scaffold` branch (`git log --oneline stage1-scaffold`), and
`tools/PROBE.md` - the list of unknowns the first board file was written against - now
lives on `main` with a status table saying which of those unknowns a flashed board closed
and which it did not. Everything else on that branch is an older copy of what is here: its
board DTS, `apply.sh` and seed config predate the slot geometry, the nvmem MAC wiring and
the JFFS2 patches, and its `tools/*.py` screens were rewritten as `.mjs` because this
machine has no usable python.

The vendor telnet password is not in here either - and `scripts/secret-scan.sh` is now the gate that keeps it that way, scanning shapes rather than one literal value. Every plan and expect script that used to carry
it now says ${ZTE_TELNET_PW}, and host/telnet_sh.mjs plus host/stage.mjs expand that from the
environment and refuse to run without it - verified by host/test-token-expansion.mjs, which
puts a fake telnet endpoint on 127.0.0.1 and checks the real value arrives on the wire and
the literal token never does.

## Acknowledgements

- **@ghahramani** - the whole H3600 line: [openwrt/openwrt#25469](https://github.com/openwrt/openwrt/pull/25469),
  the build branch [ghahramani/openwrt, branch `zte-zxhn-h3600`](https://github.com/ghahramani/openwrt/tree/zte-zxhn-h3600),
  the 24-patch mainline RFC [ghahramani/linux, branch `h3600-linux-rfc`](https://github.com/ghahramani/linux/tree/h3600-linux-rfc),
  and the [engineering write-up](https://ghahremani.medium.com/resurrecting-isp-e-waste-how-an-ai-breakthrough-unlocked-the-hyperoptic-zte-h3600-ported-modern-60e74c87c11e).
  The base tree this repository overlays is a mirror of that branch
  ([`Zlion-Y/openwrt-sanechips-mirror`](https://github.com/Zlion-Y/openwrt-sanechips-mirror), pinned at `db8b8d4`) -
  without it this port starts from an empty `target/`. Several of its published claims did not survive contact with
  this board (the `kernel1`/`kernel2` partition names, "erase `kernel1` to get stock back", the `TESTED.md` sha256
  and its "USB 3.0" line); each is listed with the measurement that closed it in
  [docs/01-情报汇总与事实核验-2026-10-13.md](docs/01-情报汇总与事实核验-2026-10-13.md).
  The thanks and the corrections are both meant sincerely.
- [cubingmucus998/zte-zx279128s-recon](https://github.com/cubingmucus998/zte-zx279128s-recon) - the public recon
  material for this SoC. A large share of the first DTB, partition-table and flash facts came from there.
- **@cnjn** - [linux-mainline-zte-zxslc-sr1010](https://github.com/cnjn/linux-mainline-zte-zxslc-sr1010), prior art
  on ZX279133 UART reverse-engineering and the mainline patch series. Several E2631 conclusions were only confirmed,
  or only overturned, against it.
- [openwrt.style.dev/h3600](https://openwrt.style.dev/h3600/) - the overseas H3600 adaptation record. It answers
  502 as of 2026-10-15, and it carries three mutually incompatible revert procedures, so treat it as a lead, not an
  authority.
- [The 恩山 (right.com.cn) thread on this device](https://www.right.com.cn/forum/thread-8487689-1-3.html) - the
  Chinese-language community discussion, including the vendor-side behaviour nobody had written down in English.
- **The OpenWrt and Linux kernel projects**, and the upstream mt76 maintainers (**@nbd168** and co.). We carry exactly
  one mt76 patch (`overlay/package/kernel/mt76/patches/002-mt7915-use-caldata-file-before-efuse.patch`) and three
  target patches (`029`/`030` JFFS2-on-SPI-NAND, `031` the PCIe link-retrain gate); everything else is upstream as-is.

If any of the above is wrong or incomplete, the fix belongs in `docs/01-...md` next to the evidence, not in a footnote here.

