#!/usr/bin/env python3
"""Align the logic-VM dispatch tables of LiT and TB (condition + action handler pointer arrays).

usage: align_logic_tables.py <bugs.exe> <bblit2.exe> <bugs.exe.features.tsv> <bblit2.exe.features.tsv>

Table bases (code-referenced, see docs/re/ENGINE_COMPARISON.md):
  LiT 1.6: cond 0x4ac550 (99 handlers + NULL, ref @0x42a7eb), action 0x4ac6e0 (129 handlers + NULL, ref @0x42c6e9)
  TB:      cond 0x4cd560 (83 slots, ref @0x45d8e8), action 0x4cd6ac (142 slots, ref @0x45e2e5),
           extra tables 0x4cd8e4 / 0x4cd91c (refs @0x45d9dc / 0x45e3cb)
Prints a global (Needleman-Wunsch style) alignment of handler sequences scored by mnemonic similarity.
Positional pairs of tiny handlers are hints only; verify each opcode's semantics before use.
"""
import struct
import sys

import pefile

from match_functions import load, sim


def ptr_run(pe, va, lo, hi):
    out, base = [], pe.OPTIONAL_HEADER.ImageBase
    while True:
        (v,) = struct.unpack("<I", pe.get_data(va - base, 4))
        if not lo <= v < hi: return out
        out.append(v); va += 4


def main():
    lit, tb = pefile.PE(sys.argv[1], fast_load=True), pefile.PE(sys.argv[2], fast_load=True)
    fa, fb = load(sys.argv[3]), load(sys.argv[4])
    lc = ptr_run(lit, 0x4ac550, 0x401000, 0x45c000)
    la = ptr_run(lit, 0x4ac6e0, 0x401000, 0x45c000)
    ta = ptr_run(tb, 0x4cd560, 0x401000, 0x481000)  # cond, action and the two extra tables are contiguous
    lname = [("cond", i) for i in range(len(lc))] + [("act", i) for i in range(len(la))]
    tname = [("cond", j) if j < 83 else ("act", j - 83) if j < 225 else ("t3", j - 225) if j < 239 else ("t4", j - 239)
             for j in range(len(ta))]
    lall = lc + la

    def s(x, y):
        ex, ey = "%08x" % x, "%08x" % y
        return sim(fa[ex]["mn"], fb[ey]["mn"]) if ex in fa and ey in fb else 0.0

    n, m, gap = len(lall), len(ta), 0.55
    S = [[s(x, y) for y in ta] for x in lall]
    D = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            D[i][j] = max(D[i - 1][j - 1] + S[i - 1][j - 1] - gap, D[i - 1][j], D[i][j - 1])
    i, j, rows = n, m, []
    while i and j:
        if D[i][j] == D[i - 1][j - 1] + S[i - 1][j - 1] - gap:
            rows.append((i - 1, j - 1)); i -= 1; j -= 1
        elif D[i][j] == D[i - 1][j]: i -= 1
        else: j -= 1
    print("lit_slot\tlit_addr\ttb_slot\ttb_addr\tsim")
    for i, j in reversed(rows):
        print(f"{lname[i][0]} {lname[i][1]}\t{lall[i]:#x}\t{tname[j][0]} {tname[j][1]}\t{ta[j]:#x}\t{S[i][j]:.2f}")
    print(f"# LiT cond {len(lc)} / act {len(la)}; TB slots {len(ta)}; aligned {len(rows)}", file=sys.stderr)


if __name__ == "__main__":
    main()
