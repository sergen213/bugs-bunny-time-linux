#!/usr/bin/env python3
"""Prototype section-1 (load instruction) tag-stream walker. Tag sizes: BugsDecomp doc/bze.md,
extended where noted in docs/formats/MODELS.md. Usage: sec1.py sec0_1.bin"""
import struct
import sys

# chunk type -> {tag: payload length}
TAGS = {
    0x00: {0x01: 8, 0x02: 8, 0x04: 8, 0x44: 12, 0x4c: 48, 0x4d: 48},
    0x05: {0x06: 3},
    0x09: {0x10: 12, 0x11: 6, 0x16: 8, 0x1c: 8, 0x1f: 1, 0x33: 32},
    0x20: {0x10: 12, 0x11: 6, 0x12: 12, 0x21: 4, 0x24: 8, 0x39: 2, 0x43: 1},
    0x22: {0x0d: 12, 0x24: 8, 0x25: 8, 0x26: 4, 0x27: 4, 0x28: 3, 0x3f: 12, 0x40: 8},
    0x29: {0x2a: 8},
    0x36: {0x37: 8},
    0x3b: {0x2b: 12, 0x3c: 4, 0x3d: 6, 0x3e: 28},
    0x45: {},
    0x47: {0x48: 8},
    0x49: {0x4a: 48},
}
ENT = {0x0b: 4, 0x0c: 4, 0x0f: 2, 0x10: 12, 0x11: 6, 0x12: 12, 0x13: 1, 0x15: 1, 0x16: 8, 0x17: 36,
       0x18: 16, 0x19: 4, 0x1a: 4, 0x1b: 6, 0x1c: 8, 0x1d: 4, 0x1e: 2, 0x1f: 1, 0x27: 4, 0x30: 32,
       0x31: 32, 0x32: 12, 0x34: 24, 0x35: 40, 0x38: 2, 0x39: 2, 0x3a: 4, 0x41: 20, 0x42: 6, 0x46: 2}
for t in (0x07, 0x08, 0x0a):
    TAGS[t] = ENT


def walk(d):
    """Yield (offset, kind, chunk_type, [(tag, payload)]). Stops at 0x2f or unknown byte."""
    p = 0
    if d[:2] == b"\x2d\x45":  # pseudo-chunk: '2d' then (45 xx)* then '2e' (FUN_0042e7a0 @0x42ea40)
        p = 1
        tags = []
        while d[p] == 0x45:
            tags.append((0x45, d[p + 1:p + 2]))
            p += 2
        assert d[p] == 0x2e, hex(p)
        p += 1
        yield 0, "pseudo", 0x45, tags
    while p < len(d):
        c = d[p]
        if c == 0x2f:
            yield p, "end", None, []
            return
        if c in (0x2c, 0x4b):
            yield p, "cmd", c, []
            p += 1
            continue
        if c != 0x2d:
            yield p, f"unknown byte {c:#x}", None, []
            return
        start = p
        t = d[p + 1]
        p += 2
        table = TAGS.get(t)
        tags = []
        while d[p] != 0x2e:
            tag = d[p]
            if table is None or tag not in table:
                yield start, f"unknown tag {tag:#x} in chunk {t:#x} at {p:#x}", t, tags
                return
            n = table[tag]
            tags.append((tag, d[p + 1:p + 1 + n]))
            p += 1 + n
        p += 1
        yield start, "chunk", t, tags


def main():
    d = open(sys.argv[1], "rb").read()
    for off, kind, t, tags in walk(d):
        s = " ".join(f"{tg:02x}:{pl.hex()}" for tg, pl in tags)
        print(f"{off:06x} {kind} {'' if t is None else f'{t:02x}'} {s}")


if __name__ == "__main__":
    main()
