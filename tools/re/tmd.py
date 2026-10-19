#!/usr/bin/env python3
"""Prototype walker for the PC 'TMD-like' model blobs in BZE section 4 (and language sections).
Layout: docs/formats/MODELS.md. Record strides come from FUN_00423eb0 (bugs.exe v1.6).
Usage: tmd.py sec9_4.bin OFFSET   -> prints object/primitive-group summary"""
import struct
import sys

# type byte -> (record stride in bytes, grouped?) ; grouped = u16[0] of first record is the count
GROUP = {0x14: 16, 0x18: 20, 0x1c: 24, 0x20: 28, 0x24: 20, 0x28: 24, 0x48: 24, 0x2c: 32, 0x30: 36,
         0x34: 24, 0x38: 28, 0x3c: 32, 0x40: 40, 0x4a: 28, 0x4c: 28, 0x4e: 32, 0x64: 16}
SINGLE = {0x00: 16, 0x04: 16, 0x08: 56, 0x0c: 56, 0x10: 20}


def objects(d, base):
    """Yield dicts for each object of the model at `base` (points at the 0x41 id)."""
    ident, flags, nobj = struct.unpack_from("<III", d, base)
    assert ident == 0x41, f"not a model at {base:#x}: {ident:#x}"
    tab = base + 12
    for i in range(nobj):
        vt, nv, nt, nn, pt, np_, sc = struct.unpack_from("<IIIIIIi", d, tab + 28 * i)
        yield dict(vert=tab + vt, nvert=nv, norm=tab + nt, nnorm=nn, prim=tab + pt, nprim=np_, scale=sc)


def groups(d, p, nprim):
    """Yield (offset, type, count, stride) primitive groups; follows FUN_00423eb0 exactly."""
    while nprim > 0:
        t = d[p + 3]
        cnt = struct.unpack_from("<H", d, p)[0]
        if t in GROUP:
            yield p, t, cnt, GROUP[t]
            p += cnt * GROUP[t]
            nprim -= cnt
        elif t in SINGLE:
            yield p, t, 1, SINGLE[t]
            p += SINGLE[t]
            nprim -= 1
        elif t == 0x44:  # embedded float3 list: u16 count, u8, u8 type, then count*12 bytes
            yield p, t, cnt, 12
            p += 4 + cnt * 12
            nprim -= 1
        else:
            raise ValueError(f"unknown primitive type {t:#x} at {p:#x}")


def main():
    d = open(sys.argv[1], "rb").read()
    base = int(sys.argv[2], 0)
    for o in objects(d, base):
        print({k: hex(v) for k, v in o.items()})
        for p, t, c, s in groups(d, o["prim"], o["nprim"]):
            print(f"  {p:#08x} type={t:#04x} count={c} stride={s} first={d[p:p + s].hex(' ')}")


if __name__ == "__main__":
    main()
