# `vendor-verhdr-8b8.bin` — the version-header template

**532 bytes** (`0x214`): the 20-byte outer header of an official ZTE upgrade package
(16-byte magic + `hdr_ofs`) followed by the **512-byte version header** (`verhdr`).
It is the whole of what `tools/mkfactory.mjs pack` reads out of a template - the packer
copies this header, rewrites the fields it has to, recomputes CRC1/CRC2 and appends your
own kernel and rootfs. That is why a stranger can pack a flashable package without
holding any vendor firmware:

    node host/release.mjs --dir <build dir> --out out/pkg.bin     # no --template needed

Provenance - cut from the package this port is developed against:

| | |
|---|---|
| source file | `zxhne2631_hv100_fv1008b88000_firmware-20260916183918.bin` |
| source size / sha256 | 21,234,196 B / `ff8a762ff0092043d94ddb2f0561e31c7d1c0c9a88d3c000fe48724ce4a67ba2` |
| `version` / `hw` / `build` fields in it | `V1.0.0.8B8.8000` / `V1.1.1` / `20260826135759` |
| this slice's sha256 | `d616f33a9c0f95d87722824cb4551f2be67825dee638b38b142abc6f94db597e` |
| how it was cut | `head -c 532 <official.bin> > vendor-verhdr-8b8.bin` |

## Byte-equivalence, measured

Packing the same build directory with this slice and with the full 21 MB vendor package
produces the **same bytes**: both runs give sha256
`02b35106a1259ed1aed04c35cb6b037c635b6fb68f3e2906da2931cb96d12ddc`, which is also exactly
the published `v1.2` package. `cmp` reports no differing byte. Re-run it yourself with:

    node tools/mkfactory.mjs selftest --template <official.bin>   # needs the full package
    node host/release.mjs --dir out/gen1 --out out/a.bin                    # slice by default
    node host/release.mjs --dir out/gen1 --template <official.bin> --out out/b.bin
    cmp out/a.bin out/b.bin

`selftest` and `info` are the only two commands that want the payload, and both say so
instead of printing a misleading CRC verdict when handed a header-only file.

## ⚠ Pick a 6B5-or-newer package if you re-cut this yourself

The upgrade keys are **not stable across vendor versions** (read out of the four packages
this port was compared against, at `verhdr+0x08` / `+0x0c`):

| vendor version | `key1/key2` |
|---|---|
| `V1.0.0.4B4.8000` | `0x238 / 0x2e` |
| `V1.0.0.6B5.8000` | `0x1351 / 0x5211` |
| `V1.0.0.7B5.8000` | `0x1351 / 0x5211` |
| `V1.0.0.8B8.8000` | `0x1351 / 0x5211` ← what ships here |

`host/verify_pkg.mjs` checks the packed header against the template precisely because a
package carrying the wrong pair is the one the vendor flash page refuses. A `4B4` template
would stamp the wrong keys and `verify_pkg.mjs` would then be comparing against the wrong
reference too.

## What is in these 532 bytes, and what is not

Field-by-field decoding is [docs/01-情报汇总与事实核验-2026-10-13.md](../docs/01-情报汇总与事实核验-2026-10-13.md)
section 2, and `tools/mkfactory.mjs` header comment carries the offset table (`M`).
Nothing here is secret that is not already in every package published under this repo's
Releases: `v1.0`, `v1.1` and `v1.2` each embed this same 512-byte header, so the keys,
product string and slot geometry in this slice are the ones already in those downloads.
What the slice does **not** contain is any vendor payload: no kernel, no rootfs, no
filesystem, no configuration, no credentials.
