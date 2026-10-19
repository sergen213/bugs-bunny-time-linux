#!/usr/bin/env python3
"""Prototype BZE container reader + LZSS decompressor (LiT and TB).

Research prototype, stdlib only. Format: research/BugsDecomp/doc/bze.md and docs/formats/BZE.md
(TB: different header, sections stored raw). Usage:
    bze.py FILE.bze [OUTDIR]   -> lists sections; writes sec<idx>_<id>.bin if OUTDIR given
    bze.py --selftest
"""
import os
import struct
import sys


def lzss_decompress(src, limit=None):
    """Decompress one section. `limit` = valid input bytes (header 'actual size')."""
    if limit is None:
        limit = len(src)
    b0 = src[0]
    osize = b0 & 7
    step = (b0 >> 3) & 3
    off_bits = 9 + osize
    len_bits = 7 - osize
    if len_bits <= 0:
        raise ValueError("invalid osize 7")
    items = int.from_bytes(src[1:4], "big") + 1
    lut_size = 1 << len_bits
    thres = 19 if lut_size >= 32 else lut_size // 2 - 1
    lut = [3 + ((i - thres) << step) + thres if i > thres else 3 + i for i in range(lut_size)]
    lmask = lut_size - 1
    omask = (1 << off_bits) - 1
    out = bytearray()
    p = 4
    n = 0
    # bze.md: stored count is sometimes the real count (off-by-one) -> also stop at input end.
    while n < items and p < limit:
        flags = src[p]
        p += 1
        for bit in range(8):
            if n >= items or p >= limit:
                break
            if flags >> bit & 1:
                out.append(src[p])
                p += 1
            else:
                if p + 1 >= limit:
                    break
                t = src[p] << 8 | src[p + 1]
                p += 2
                off = (t >> len_bits) & omask
                ln = lut[t & lmask]
                if off > len(out):
                    raise ValueError(f"bad backref off={off} at out={len(out)} in={p}")
                if off == 0:
                    # FUN_00430ff0 runs stored_count+1 items; on some sections the last one is
                    # padding with off=0, which copies stale destination bytes. Emit zeros.
                    out += bytes(ln)
                    n += 1
                    continue
                s = len(out) - off
                for i in range(ln):
                    out.append(out[s + i])
            n += 1
    return bytes(out)


def read_header(d):
    """Return (kind, [(id, actual, filesize, offset)]). kind 'lit' or 'tb'."""
    a, b, c = struct.unpack_from("<III", d, 0)
    if a == 1:  # LiT: version=1, count, entries at 0x08
        kind, count, base = "lit", b, 8
    elif a == 0:  # TB: 0, memory budget (e.g. 222000), count, entries at 0x0c (bblit2 FUN_004347c0)
        kind, count, base = "tb", c, 12
    else:
        raise ValueError(f"unknown BZE header {a:#x}")
    secs = []
    off = 0x800
    for i in range(count):
        sid, act, fsz = struct.unpack_from("<III", d, base + 12 * i)
        secs.append((sid, act, fsz, off))
        off += fsz
    return kind, secs


def section_bytes(kind, d, act, fsz, off):
    # TB stores sections raw; its LZSS path (FUN_00436e90) only runs with the "/comp" switch.
    return d[off:off + act] if kind == "tb" else lzss_decompress(d[off:off + fsz], act)


def load(path):
    """Return (kind, [(id, decompressed_bytes)]) in file order."""
    d = open(path, "rb").read()
    kind, secs = read_header(d)
    return kind, [(sid, section_bytes(kind, d, act, fsz, off)) for sid, act, fsz, off in secs]


def selftest():
    # header 0x0b: osize=3 (12-bit offset, 4-bit length), step=1; 3 items stored as 2.
    # items: literal 'a', literal 'b', backref off=2 len=lut[0]=3 -> "ab" + "aba"
    src = bytes([0x0b, 0, 0, 2, 0b011, ord("a"), ord("b"), 0x00, 0x20])
    assert lzss_decompress(src) == b"ababa", lzss_decompress(src)
    print("selftest ok")


def main():
    if sys.argv[1] == "--selftest":
        return selftest()
    path = sys.argv[1]
    d = open(path, "rb").read()
    kind, secs = read_header(d)
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    if outdir:
        os.makedirs(outdir, exist_ok=True)
    print(f"{path}: {kind}, {len(secs)} sections")
    for i, (sid, act, fsz, off) in enumerate(secs):
        raw = section_bytes(kind, d, act, fsz, off)
        print(f"  [{i}] id={sid:2d} off={off:#08x} actual={act:#07x} file={fsz:#07x} -> {len(raw):#08x}")
        if outdir:
            open(os.path.join(outdir, f"sec{i}_{sid}.bin"), "wb").write(raw)


if __name__ == "__main__":
    main()
