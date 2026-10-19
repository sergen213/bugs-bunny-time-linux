#!/usr/bin/env python3
"""Dump the six byte-indexed jump tables of the TB level loader FUN_004347c0 straight from bblit2.exe
(evidence for TB_LEVELS.md §3: which tag bytes have a handler; everything else hits the default = skip
the tag byte only). Usage: tb_switches.py .../tb/Datas/BIN/bblit2.exe"""
import struct
import sys

# (name, first case value, case count, byte index table VA, dword jump table VA, default target VA)
# read from the dispatch sequences at 0x434d3f, 0x434dad, 0x43504e, 0x435307, 0x4356e7, 0x435a76
SWITCHES = [
    ("chunk type", 0x00, 0x64, 0x436ACC, 0x436A98, 0x43643A),
    ("chunk 0x22 tags", 0x0D, 0x54, 0x436B54, 0x436B30, 0x434FA0),
    ("chunk 0x20 tags", 0x10, 0x53, 0x436BCC, 0x436BA8, 0x43527B),
    ("chunk 0x00 tags", 0x01, 0x4C, 0x436C3C, 0x436C20, 0x4355A9),
    ("chunk 0x3a tags", 0x2A, 0x3D, 0x436C9C, 0x436C88, 0x43589A),
    ("entity chunk tags (07 08 09 0a 63)", 0x0B, 0x5B, 0x436D54, 0x436CDC, 0x436430),
]


def main():
    exe = open(sys.argv[1], "rb").read()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", exe, pe + 24 + opt + 40 * i + 8) for i in range(nsec)]

    def off(va):
        rva = va - 0x400000
        return next(rp + rva - sva for vs, sva, rs, rp in secs if sva <= rva < sva + vs)

    for name, first, n, idx, jt, default in SWITCHES:
        cases = {}
        for i in range(n):
            target = struct.unpack_from("<I", exe, off(jt + 4 * exe[off(idx + i)]))[0]
            if target != default:
                cases.setdefault(target, []).append(first + i)
        print(f"{name}: " + ", ".join(f"{'/'.join(f'{v:02x}' for v in vals)}->{t:#x}"
                                         for t, vals in sorted(cases.items(), key=lambda kv: kv[1][0])))


if __name__ == "__main__":
    main()
