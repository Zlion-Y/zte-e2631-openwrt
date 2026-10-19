# Known issues


> **Historical verification notice:** Several entries below describe
> earlier v1.2/gen1 firmware tests. The newer `6ab4488` build has
> working automatic Wi-Fi AP initialization and UBIFS Extroot on
> an already initialized device. Older issues are retained until
> each is independently retested; they are not automatically fixed.


The honest list, in two forms: a machine-readable block for tooling and grep, and one
short section per issue for the human who has to decide whether to care. "Verified" means
it was reproduced on this board with the evidence named; "unverified" means it is a
reasoned suspicion and must not be treated as a fact.

Schema: `id` is stable, `severity` is `blocker | major | minor`, `status` is
`open | mitigated | fixed | wontfix`, `surface` is what you are trying to do, `evidence`
points at something in the repository or at a measurement.

```yaml
- id: WIFI-ASSOC-HARD-HANG
  severity: blocker
  status: open
  surface: wi-fi
  symptom: a client associating stops the whole box - no console output, no ARP, no oops, no AER record
  verified: yes (docs/02 section P; board/ser-*.log)
  workaround: keep the radio off; bring it up by hand with a console attached
  next: staged experiment plan in task list - one variable per flash cycle
- id: WIFI-COLD-BIND-HANG
  severity: major
  status: open
  surface: wi-fi
  symptom: the first firmware download after a power cycle succeeds on roughly one attempt in three
  verified: yes (three cold boots on this exact image: hung, worked, hung)
  workaround: rebind (e2631-wifi-on 0 again) - it is the first attempt that is the risk
- id: WIFI-OFF-BY-DEFAULT
  severity: minor
  status: mitigated
  surface: wi-fi
  symptom: no radio until you run two commands
  verified: yes
  workaround: /sbin/e2631-wifi-on 0 && /sbin/e2631-wifi-ap
  note: deliberate. Two independent gates (AutoProbe removed, generated ifaces disabled)
- id: WIFI-2G4-UNVERIFIED
  severity: minor
  status: open
  surface: wi-fi
  symptom: the 5G AP is confirmed from a phone; the 2.4G radio has never been verified on air
  verified: no
- id: FWSTATE-NEEDS-DEVMEM
  severity: minor
  status: open
  surface: debugging
  symptom: /sbin/e2631-fwstate cannot work in the shipped image ("no /dev/mem")
  verified: yes, on the box
  next: build/wsl/build-regprobe.sh produces the module that reads the same register
- id: DEVMEM-DROPPED
  severity: major
  status: open
  surface: kernel config
  symptom: CONFIG_DEVMEM=y is requested in both generic and the target fragment, and the built .config comes out "not set"
  verified: yes (out/gen1/config-6.18:2314 vs generic/config-6.18:1494 and sanechips/config-6.18:147)
  note: why the request is dropped is still unexplained - do not re-add a fragment edit on the assumption it will work
- id: ETH-THROUGHPUT-CEILING
  severity: major
  status: open
  surface: ethernet
  symptom: link trains at 1 Gbps, throughput plateaus at about 305 Mbit/s in and 190 Mbit/s out
  verified: yes (docs/01 section A, host 192.168.1.177)
  cause: no checksum/TSO/SG/RSS offload in the driver, one RX queue, and all of it on CPU0 while both cores are online
  workaround: RPS measured +4.6%; the real fix is the packet engines (NPP/TM), which the vendor kernel has and mainline does not
- id: PACK-NEEDS-VENDOR-TEMPLATE
  severity: major
  status: open
  surface: packaging
  symptom: no flashable package without one official E2631 upgrade package as a version-header template
  verified: yes
  note: the format is fully decoded and tools/make_fixture.mjs synthesizes a header, but the vendor web page has never been shown to accept a synthesized one
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
  status: wontfix
  surface: contributing
  symptom: nothing runs on a push; the build happens on someone's machine
  note: deliberate (the owner publishes releases by hand). make check is the entry point that replaced the workflow's gates
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
