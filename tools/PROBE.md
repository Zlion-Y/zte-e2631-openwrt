# Reading the output of `probe-e2631.sh`

Every unknown in the device tree has an ID (`TODO-N1`, `TODO-W1`, ...). The same
IDs appear here, so the loop is: boot the RAM image, run the script, look up the
ID, edit the device tree, rebuild.

The open ones are `N1` (PHY addresses and the port map), `W1` (the PCIe reset
line), `L1` (LEDs and buttons), `A1` (where the MAC lives) and `F1` (SPI-NAND
timing). `M1` is closed and `U1` is withdrawn; both sections are kept because
what closed them, and why, is the part worth re-reading later.

Nothing in this file is a measurement of an E2631 by itself. Each row states
what the evidence *would* mean once you have it, and where the number came from.

---

## Status 2026-10-14, from a booted OpenWrt on this exact board

This file came from the stage-1 repo (`stage1-scaffold` branch); it is kept here
because it is the list of questions the device tree was written against. What a
flashed image answered, with the witness:

| ID | question | where it stands |
|---|---|---|
| `M1` | DRAM size | **closed** - 256 MiB, as the section below already said |
| `N1/N2` | PHY addresses, port map | **closed** - the four internal GEPHYs answer MDIO at `0a/0b/0c/0d` (driver `Generic`), and the netdev names come from the DTS `label` as `zx279128s-main.c:1313` (`strscpy(netdev->name, label)`) intends. Not closed: which physical panel jack is `lan1` vs `lan2` vs `lan3` - only one port had a cable during any of these boots |
| `A1` | where the MAC lives | **closed** - `macaddr_tag_1e`, and the ports now reference it through nvmem. Before that every boot shipped four random MACs, which is the kind of bug that only shows up as "this client will not roam" |
| `L1` | LEDs and buttons | **LEDs closed** - flat 40 = green, 60 = red, 8 = blue, all ACTIVE_LOW, and the lamp service is in the image. **Buttons not settled** - the two samples for 18/20 disagreed, so their active level is still a guess |
| `W1` | PCIe reset and enable lines | **practically closed** - adding `enable-gpios = <&gpio3 7 GPIO_ACTIVE_HIGH>` next to `reset-gpios = <&gpio3 5>` is what made both root ports train and the chip enumerate; the two lines reset the whole package, which is why the host driver sequences them. Still worth remembering: the vendor U-Boot drives flat GPIO 51/52/54/55 low before Linux hands over |
| `F1` | SPI-NAND timing | **closed** - 50 MHz, `spi-rx-bus-width = <2>`, `spi-tx-bus-width = <1>`, on-die ECC, and the first boot reported `bad=0 ecc_fail=0` with corrections happening. The real requirement was not timing but patches 029/030: clean markers have to live in the data area, because on-die ECC regenerates page ECC on every program |
| `U1` | USB | **stays withdrawn** - `&usb` is `status = "disabled"` in the board file, the built kernel has `CONFIG_USB_SUPPORT=y`/`CONFIG_USB=m` and no host controller driver at all, and no USB package is in the rootfs manifest |

---

## TODO-M1 — how much DRAM this board has: **closed**

**Command:** the `identity` section, lines `memory` and `dts reg`.

Not open any more. The stock firmware's own device tree answers it: its
`/proc/device-tree/memory/reg` reads `40 00 00 00 10 00 00 00`, i.e. base
0x40000000 with size 0x10000000 (256 MiB), and its `chosen/bootargs` carries
`mem=256M`, which is what the kernel rewrote that node from. `/proc/iomem` on
the same box agrees — `System RAM 0x40000000-0x4fffffff`. The device tree here
now uses 0x10000000. The earlier 128 MiB cell was a conservative guess; it
booted because it is a subset, not because it was right.

Kept for the case where a build of our own disagrees:

| Observation | What it means | What to change |
|---|---|---|
| `MemTotal` ≈ 245 MB | correct, the cell matches the hardware | nothing |
| `MemTotal` ≈ 118 MB | the cell was reverted to the old 128 MiB value | set it back to `0x10000000` |
| the kernel stops with no message shortly after `Freeing unused kernel image` | the cell is larger than the DRAM actually present — the only failure 256 MiB should be able to cause | check this before anything else |

One trap worth knowing: the vendor tree also carries a second node,
`memory@40000000`, still saying `0x20000000` (512 MiB). It is a stale leftover
that nobody rewrote. It is not evidence of 512 MiB and not evidence of a
contradiction.

## TODO-F1 — SPI-NAND timing

**Command:** `flash` section, plus `dmesg | grep -iE 'spi|nand|ecc|bad'`.

- A clean run registers one mtd of `0x8000000` (128 MiB) with `erasesize`
  `0x20000` (128 KiB) and no ECC lines.
- `spi-nand ... : failed to read ...` or a storm of ECC corrections at 50 MHz:
  set `spi-max-frequency = <10000000>`. 10 MHz is known-good on this family.
- `unrecognised JEDEC ID 0x01 0x15`: that is the HYF1GQ4UTACAE that U-Boot
  reports, and it is covered by the HeyangTek driver in the MTD tree. The
  separate bad-block-marker fix (target patch 027) is what makes its BBT usable.

## TODO-N1 / TODO-N2 — where the PHYs actually are

**Command:** `mdio` section. It prints phylib's view first, then a raw scan.

The raw scan is the answer. The phylib section only confirms the device tree's
guess, because `of_mdiobus_register` never probes an address the device tree did
not name.

Reading the raw table:

| Column | Meaning |
|---|---|
| `ID1(reg2)` | `0xffff` is reported as "nothing there" — the bus leaves the data line pulled up, so an absent PHY reads all ones |
| `ID2(reg3)` | the revision/model low bits |
| `OUI/codec` | bytes 2-5 of ID1, i.e. the IEEE OUI and model |

The H3600's WAN PHY is a Sanechips ZX5201 with a full ID of `0x84b95011`. If the
scan shows that ID, the WAN PHY is present and `reg` in `&mdio` should hold that
address. Four other addresses answering with a different ID are the internal
GEPHYs, and those become the `phy-handle` values of `gmac_port0` to `gmac_port3`.

Two cautions:

- The script runs `/etc/init.d/network stop` before the raw scan. Without it,
  phylink polls the same block concurrently and you get transactions
  interleaved into each other.
- The scan pokes registers that are otherwise owned by the MDIO driver. It
  reads; it does not write anything but the controller's own command and status
  registers. If the box hangs here, that is the reason, not a flash problem.

`TODO-N2` is the `pinctrl-0 = <&ext_phy_pins>` question. Only chase it if the
WAN PHY stays silent at **every** address: the H3600 routes that PHY's signals
through a pin-mux group, and on this board the same group may be unwritten.

## TODO-A1 — where the MAC address lives

Not answered by the script; the script answers the *layout* question that has
to be settled first. Once the partition table is known (below), read the
candidate partitions:

```sh
dd if=/dev/mtdN bs=256 skip=$((0x1e / 256)) count=1 | hexdump -C | head
```

The H3600 stores its LAN base MAC at offset `0x1e` inside a 1 MiB read-only
partition named `tag`, and derives WAN as base+1. On a ZTE board the same field
usually sits in a partition holding the parameter tags. Look for the factory
MAC printed on the label of your unit — find it in the dump, and the offset is
the position you found it at.

## The layout scan — the actual point of this exercise

**Command:** `flash`, all 128 MiB.

The script scans for these signatures and reports only 4096-aligned hits:

| Signature | What it identifies |
|---|---|
| `33333333cccccccc88888888dddddddd` | the ZTE slot marker the H3600's cspboot looks for |
| `333333336666666699999999cccccccc` | the H3600 header magic at `+0xf4` |
| `27051956` | uImage |
| `d00dfeed` | FIT image |
| `55424923` | `UBI#` |
| `716e73786863` | `hsqshn` — squashfs little-endian |
| `85190320` / `20031985` | JFFS2, both byte orders |

Then, for each slot-marker hit, it decodes the fields the H3600 bootloader
reads: kernel length at `+0x34`, kernel CRC32 at `+0x3c`, rootfs length at
`+0x40`, image serial at `+0x1f4`.

Three outcomes, in order of how much work they save:

1. **Marker found, fields decode as sane numbers** (length under 4 MiB, CRC
   present, rootfs length 0). The two boards share the boot image format, and
   the H3600's `platform.sh` becomes adaptable rather than a rewrite.
2. **Marker found only once, or fields decode as garbage.** Shared format,
   different field positions. Read the 512 bytes around the hit as a hexdump
   and diff it against the H3600 trailer.
3. **No marker at all.** This board's boot loader uses a different image
   format. Then a flash write is not recoverable by falling back to a second
   slot, because there is no evidence it has one — and the stage-2 work has to
   start by reversing cspboot itself.

Case 3 is also the case in which the AES-128-ECB transparent decryption layer
that the E2631's vendor notes mention becomes the deciding factor: if the
partitions only make sense after decryption, plaintext written by OpenWrt will
never boot. Look for that when every candidate partition you dumped is
high-entropy with no recognizable magic anywhere in the scan.

## TODO-L1 — LEDs and buttons

**Command:** `gpio` section, the LED sweep.

The sweep drives one line at a time high for a second across all six banks,
which sit at `0x94407000 + 0x40 * n`. Read the panel; write down the number for
each LED. Buttons: read `/sys/kernel/debug/gpio` (in the log file) and press
each one while watching, or export the candidate lines as input.

The output is the `gpios` phandle for the `leds` and `keys` nodes. The H3600's
pattern is a useful starting point: eleven LEDs, all active-low, and polled
buttons, because this GPIO driver has no interrupt support.

Then fill in the four `led-*` aliases, and only then is failsafe reachable from
the front panel.

## TODO-W1 — the PCIe reset and enable lines

**Command:** `pcie` section (or `everything`).

Both controllers ship as `okay`, with the H3600's `<&gpio3 5>` / `<&gpio3 7>`
pair as a starting guess. Read the section like this:

| Observation | What it means | What to change |
|---|---|---|
| `14c3:xxxx` in the device list | the guess was right — that is the MediaTek radio and PCIe is up | keep the GPIOs, go on to the calibration offset |
| controller bound, nothing below it | the link came up but the radio stayed in reset | try the opposite polarity, then sweep `gpio3` |
| the log stops inside the PCIe driver | the reset line belongs to something else | set both nodes back to `disabled` and boot again |

A wrong reset line is the one guess in this tree that can hang the box, which
is why the device tree says so at the node. The retry is free: the image never
writes flash. To find the line by hand, run the `gpio` sweep on the `gpio3`
bank while watching the `pcie` section on a second console.

## TODO-U1 — USB: withdrawn, and the `usb` section is now a negative control

**Command:** `usb` section.

USB is off on purpose, in three places (the device tree, `env/kernel-config`, and
the absence of any USB kmod). Reaching a controller on this board hangs it and
sends the watchdog into a reboot loop, so there is nothing here to measure and
nothing to enable later without a kernel first shown not to hang.

That leaves the section with one useful job, and it is not the one it was
written for: **its correct output is empty.** A root hub under
`/sys/bus/usb/devices`, a `dwc3` or `xhci` line in `dmesg`, anything at all —
each of those means a USB module reached the image, which is a build mistake
worth chasing rather than a result worth recording. Read it as a regression
check, not as a TODO.

## The peripheral sections — `pcie`, `usb`, `net`, `buses`

These four were added once the port reached the point of driving peripherals
rather than only dumping flash. Three answer a board-level question that no
amount of offline reading can settle; the fourth is a guard:

| Section | Answers |
|---|---|
| `pcie` | is the MT7916 on the bus, and are the reset/enable GPIOs right |
| `usb` | **expect nothing** — output here means USB leaked into the image |
| `net` | which front-panel jack is which `gmac_portN`, from the carrier bit |
| `buses` | thermal zone, watchdog and interrupt activity, for a quick health read |
