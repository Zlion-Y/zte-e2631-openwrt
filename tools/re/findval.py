#!/usr/bin/env python3
"""Find little-endian 32-bit constants in a binary and say which section holds them.

    python3 findval.py <file> <hex> [<hex> ...]

Vendor register offsets usually live in a static table (ring base list, WFDMA
descriptor array) rather than as movw/movt immediates, so a raw byte scan finds
them where a disassembly grep cannot.
"""
import struct
import sys

from armdis import Elf


def main():
    path = sys.argv[1]
    vals = [int(v, 16) for v in sys.argv[2:]]
    elf = Elf(path)
    for v in vals:
        pat = struct.pack("<I", v)
        needle = pat
        hits = []
        start = 0
        while True:
            i = elf.buf.find(needle, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
        print("0x%08x: %d hit(s)" % (v, len(hits)))
        for h in hits[:12]:
            where = "?"
            for s in elf.secs:
                if s["off"] <= h < s["off"] + s["size"]:
                    where = "%s+0x%x" % (s["sname"], h - s["off"])
            print("   file 0x%-6x  %s" % (h, where))
    return 0


if __name__ == "__main__":
    sys.exit(main())
