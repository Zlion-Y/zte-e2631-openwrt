# wsl/ - the build path, the patch generators, and the evidence tools

Everything here runs on the Linux build box: `$E2631_TREE` for the OpenWrt tree,
`$E2631_KIT` for this repo, `$E2631_WORK` (default: the kit's parent) for `out/`. The
three come from `scripts/env.sh`, which every script in here sources, so nothing depends
on this particular machine any more - the directory is a name from how it was developed,
and `scripts/bootstrap.sh` + `scripts/build.sh` are the portable entry points.
`bash -n` for all of them, plus an ash gate for every script we push to the board, is
`lint-kit.sh` (or `make check`, which runs all of it).
The WSL box has gcc but **no node**; the Windows side has node but no toolchain - that
split is why some checks are two commands instead of one.

## Build path (run these in order)

| script | what it does |
|---|---|
| `sync.sh` | copy the workspace repo into `~/e2631` so the build sees it |
| `check-kit.sh` | the gates the retired CI used to run: compile the slot tool with `-Werror`, then have it accept a synthesized package and reject one byte altered. Needs `node build/tools/make_fixture.mjs out/fixture` first (Windows side) |
| `build.sh` | full local `make world`, with the download workarounds in README.md |
| `build-gen1.sh` | the same, pinned to what shipped on the board: sync -> apply -> `package/kernel/mt76/{clean,download,prepare}` -> witnesses -> `target/linux/{clean,prepare}` -> witnesses -> `make world` -> `collect.sh` -> copy to `out/gen1` |
| `collect.sh <out-dir>` | lay the result out the way the packaged artifact does (`kernel` / `root` / `e2631.dtb` / `config-6.18` / `e2631-fwslot`), for `node build/host/release.mjs --dir <out-dir>` |
| `build-regprobe.sh` | build the FW_STATE probe module against the exact kernel we ship (the `/dev/mem` replacement) |
| `probe.sh`, `killbuild.sh` | "which phase is the build in, is it still running" / stop it (patterns bracketed so the killer cannot kill itself) |
| `segdl.sh`, `parget.sh` | segmented parallel GET for distfiles |
| `pull-log.sh <wsl-path> <name>` | copy a WSL log into `out/` so it can be read |

## Patch generators (never hand-write a patch)

| script | generates |
|---|---|
| `gen-efuse-patch.sh` | mt76 `002-mt7915-use-caldata-file-before-efuse.patch` - load the calibration file before the efuse round-trips |
| `gen-retrain-patch.sh` | kernel `031-pci-gate-link-retrain-speed-lift-by-vendor-device.patch` - re-gate the 2.5 GT/s speed lift |
| `gen-fwtimeout-patch.sh` | `003-mt7915-wait-longer-for-fw-state.patch` (1 s -> 8 s) - **falsified and deferred**, kept only so the generator is not lost |

Each generator writes the patch from the source the build actually uses and proves
`APPLIED == GENERATED`; a hand-written patch in this tree failed to apply once
("malformed patch at line 37"). `land-patch.sh` and `check-patch31.sh` are the dry-run
and the landing step for that loop.

## Witnesses (proof the fix is in the artifact, not just the source)

`verify-efusefix.sh` (the function is inlined, so key it off the caller's relocations),
`verify-gen1-kernel.sh` (disassemble the booted kernel and see `bl pci_match_id`),
`inspect-rootfs.sh <dir>` (what is really inside the rootfs: the A/B switcher, whether the
squashfs extracted fully, what the wireless config defaults to),
`mt7915-readl-gap.sh`, `which-mt7915-src.sh` (the kernel tree and the mt76 feed both carry
mt7915 - only one builds the .ko we load), `check-apply`-style greps folded into
`build-gen1.sh`'s witness lines, `lint-kit.sh`, `sh-lint.sh`.

## Probes that answered a question, kept so the answer can be re-checked

`mps-static.sh`, `mps-selftest.sh`, `test-boardspeed.sh` (config-space decode, offline, against
bytes pulled from the live board), `dwc-linkspeed.sh` (our host controller driver does not read
`max-link-speed`), `retrain-quirk-check.sh` (found the un-gated `pcie_failed_link_retrain()`),
`iolock-coverage.sh`, `check-iolock.sh` (the base tree's L2 IO-lock patch vs mt7915's reads),
`l2c-auxctl-decode.sh`, `read-mt7915-eeprom.sh`,
`mtk-disasm.sh`, `mtk-disasm-init.sh`, `mtk-fw-symbols.sh`, `mtk-init-sequence.sh`,
`vendor-ko-family.sh` (the stock `mt7916.ko` is MTK's proprietary driver, not mt76),
`vendor-dtb-wdt.sh`, `twd-wdt-check.sh` (no watchdog can self-recover from an IRQs-off hang here),
`failsafe-check.sh`, `failsafe-route.sh` (failsafe is the way back in without serial TX).

Two conclusions are recorded in `../docs/02-...md` section P rather than in a script, and
should not be re-litigated with a build: `/dev/mem` is unreachable from any fragment
(`# CONFIG_DEVMEM is not set` in the built config while every fragment says `=y`), and the
widened `FW_STATE` wait does not help (a cold bind hung with 8 s too).
