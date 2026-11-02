#!/usr/bin/env python3
"""Disassemble one function out of a relocatable ARM .ko by symbol name.

    python3 armdis.py <ko> <symbol> [max_insns]
    python3 armdis.py <ko> --grep <hex-imm>      # find code touching a constant

No dependency beyond capstone: binutils here is x86-only and refuses ARM
objects, so the ELF32 section headers and .symtab are parsed by hand.
Relocatable objects keep st_value relative to its section, i.e. an offset
inside the file image of that section.
"""
import struct
import sys

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_LITTLE_ENDIAN

SHT_SYMTAB = 2
STT_FUNC = 2


class Elf:
    def __init__(self, path):
        self.buf = open(path, "rb").read()
        b = self.buf
        if b[:4] != b"\x7fELF" or b[4] != 1:
            raise SystemExit("not ELF32")
        (self.shoff,) = struct.unpack_from("<I", b, 0x20)
        self.shentsize, self.shnum, self.shstrndx = struct.unpack_from("<HHH", b, 0x2E)
        self.secs = []
        for i in range(self.shnum):
            off = self.shoff + i * self.shentsize
            name, typ, flags, addr, soff, size, link, info, align, entsize = \
                struct.unpack_from("<IIIIIIIIII", b, off)
            self.secs.append(dict(name=name, type=typ, addr=addr, off=soff, info=info,
                                  size=size, link=link, entsize=entsize))
        strtab = self.secs[self.shstrndx]
        for s in self.secs:
            s["sname"] = self._str(strtab["off"], s["name"])

    def _str(self, base, off):
        end = self.buf.index(b"\0", base + off)
        return self.buf[base + off:end].decode("latin1")

    def section(self, name):
        for s in self.secs:
            if s["sname"] == name:
                return s
        return None

    def data(self, s):
        return self.buf[s["off"]:s["off"] + s["size"]]

    def symbols(self):
        for s in self.secs:
            if s["type"] != SHT_SYMTAB:
                continue
            strtab = self.secs[s["link"]]
            n = s["size"] // 16
            for i in range(n):
                name, value, size, info, other, shndx = \
                    struct.unpack_from("<IIIBBH", self.buf, s["off"] + i * 16)
                yield dict(idx=i, name=self._str(strtab["off"], name), value=value,
                           size=size, shndx=shndx, info=info)

    def relocs(self, target_name):
        """offset inside `target_name` -> symbol name, for CALL/JUMP relocations.

        A relocatable object leaves the branch target blank and records it in
        .rel<sec>; without this every `bl` decodes as a branch to itself.
        """
        syms = {s["idx"]: s["name"] for s in self.symbols()}
        out = {}
        for s in self.secs:
            if s["type"] != 9 or not s["sname"].startswith(".rel"):
                continue
            linked = self.secs[s["info"]]["sname"] if s["info"] < len(self.secs) else ""
            if linked != target_name:
                continue
            n = s["size"] // 8
            for i in range(n):
                off, info = struct.unpack_from("<II", self.buf, s["off"] + i * 8)
                sym, typ = info >> 8, info & 0xFF
                if typ in (1, 2, 28, 29, 43, 44, 45):   # branch + ABS32/MOVW/MOVT
                    out[off] = syms.get(sym, "?%d" % sym)
        return out


def dis(code, base, limit):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN)
    for ins in md.disasm(code, base):
        yield ins
        limit -= 1
        if limit <= 0:
            return


def main():
    ko, arg = sys.argv[1], sys.argv[2]
    elf = Elf(ko)

    if arg == "--callers":
        target = sys.argv[3]
        funcs = [s for s in elf.symbols()
                 if s["info"] & 0xF == STT_FUNC and s["shndx"] < len(elf.secs)
                 and elf.secs[s["shndx"]]["sname"] == ".text"]
        funcs.sort(key=lambda s: s["value"])
        idx_by_name = {s["name"]: s["idx"] for s in elf.symbols()}
        want_idx = idx_by_name.get(target)
        if want_idx is None:
            print("no such symbol:", target)
            return 1
        # scan every .rel section, whatever it targets: the SDK reaches HAL
        # helpers through ops tables, so the reference is usually a pointer
        # stored in .data, not a `bl` in .text.
        for s in elf.secs:
            if s["type"] != 9 or s["link"] >= len(elf.secs):
                continue
            tgt_sec = elf.secs[s["info"]]["sname"] if s["info"] < len(elf.secs) else "?"
            n = s["size"] // 8
            for i in range(n):
                off, info = struct.unpack_from("<II", elf.buf, s["off"] + i * 8)
                if info >> 8 != want_idx:
                    continue
                owner = None
                if tgt_sec == ".text":
                    for f in funcs:
                        if f["value"] <= off and (owner is None or
                                                  f["value"] > owner["value"]):
                            owner = f
                print("  %s+0x%05x  %s" %
                      (tgt_sec, off,
                       ("in %s +0x%x" % (owner["name"], off - owner["value"]))
                       if owner else "(table slot)"))
        return 0

    if arg == "--at":
        want = int(sys.argv[3], 16)
        best = None
        for sym in elf.symbols():
            if sym["info"] & 0xF != STT_FUNC:
                continue
            sec = elf.secs[sym["shndx"]] if sym["shndx"] < len(elf.secs) else None
            if not sec or sec["sname"] != ".text":
                continue
            if sym["value"] <= want and (best is None or sym["value"] > best["value"]):
                best = sym
        print("0x%x is inside %s +0x%x" % (want, best["name"],
                                           want - best["value"]) if best else "?")
        return 0

    if arg == "--grep":
        pat = int(sys.argv[3], 16)
        want = "#0x%x" % pat
        sec = elf.section(".text")
        hits = 0
        for ins in dis(elf.data(sec), 0, 10 ** 9):
            if want in ins.op_str:
                print("  %05x: %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
                hits += 1
                if hits > 40:
                    break
        return 0

    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    for sym in elf.symbols():
        if sym["name"] != arg or sym["info"] & 0xF != STT_FUNC:
            continue
        sec = elf.secs[sym["shndx"]] if sym["shndx"] < len(elf.secs) else None
        if not sec:
            continue
        rel = elf.relocs(sec["sname"])
        code = elf.data(sec)[sym["value"]:sym["value"] + (sym["size"] or 256)]
        print("%s  %s+0x%x  size=%d" % (sym["name"], sec["sname"], sym["value"],
                                        sym["size"]))
        for ins in dis(code, sym["value"], limit):
            note = ""
            if ins.address in rel:
                note = "   -> " + rel[ins.address]
            print("  %05x: %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, note))
        return 0
    print("symbol not found:", arg)
    return 1


if __name__ == "__main__":
    sys.exit(main())
