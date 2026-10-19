#!/usr/bin/env python3
"""TB (bblit2.exe) section-1 load-instruction walker. Grammar: docs/formats/TB_LEVELS.md, recovered from
the TB level loader FUN_004347c0 and the logic-tag encoder FUN_00433ab0.

Usage:
    tb_sec1.py FILE.bze            dump the stream of one level
    tb_sec1.py --all DIR           parse every *.bze in DIR, report files that do not parse to the last byte
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bze  # noqa: E402

# Logic-tag payload sizes, FUN_00433ab0 (tags 'M'..'`', used in entity chunks and chunk 0x22).
LOGIC = {0x4d: 2, 0x4e: 4, 0x4f: 8, 0x50: 8, 0x51: 8, 0x52: 8, 0x53: 6, 0x54: 12, 0x55: 12, 0x56: 16,
         0x57: 20, 0x58: 8, 0x59: 4, 0x5a: 8, 0x5b: 8, 0x5c: 2, 0x5d: 8, 0x5e: 12, 0x5f: 16, 0x60: 8}
# 0x62: no case in the loader's tag switches (default = skip the tag byte only); present in the data as the
# last tag of every 0x20 and entity chunk -> payload 0 (see TB_LEVELS.md §3.9).
ENT = {0x0b: 4, 0x0c: 4, 0x0f: 2, 0x10: 12, 0x11: 6, 0x12: 12, 0x13: 1, 0x15: 1, 0x16: 28, 0x19: 4,
       0x1a: 4, 0x1b: 6, 0x1c: 8, 0x1d: 4, 0x1e: 2, 0x1f: 1, 0x27: 4, 0x2f: 48, 0x31: 12, 0x37: 10,
       0x38: 2, 0x39: 12, 0x40: 20, 0x41: 6, 0x45: 2, 0x61: 14, 0x62: 0, 0x65: 2, **LOGIC}
TAGS = {
    0x00: {0x01: 8, 0x02: 8, 0x04: 12, 0x43: 12, 0x4b: 56, 0x4c: 56},
    0x05: {0x06: 3},
    0x07: ENT, 0x08: ENT, 0x09: ENT, 0x0a: ENT, 0x63: ENT,
    0x20: {0x10: 12, 0x11: 6, 0x12: 12, 0x21: 4, 0x24: 8, 0x38: 2, 0x42: 1, 0x62: 0},
    0x22: {0x0d: 12, 0x24: 8, 0x25: 8, 0x26: 4, 0x27: 4, 0x3e: 12, 0x3f: 8, **LOGIC},
    0x28: {0x29: 8},
    0x35: {0x36: 8},
    0x3a: {0x2a: 8, 0x3c: 14, 0x3d: 36, 0x66: 3},
    0x46: {0x47: 8},
    0x48: {0x49: 56, 0x68: 56},
}
CHUNK, CHUNK_END, END, CMD_MODELS, CMD_LANG = 0x2c, 0x2d, 0x2e, 0x2b, 0x4a


class ParseError(Exception):
    pass


def walk(d):
    """Yield (offset, kind, type, [(tag_offset, tag, payload)]). Strict: bytes the game would silently
    skip (unknown top-level byte, chunk type or tag) raise ParseError with the offset."""
    p = 0
    if d[0] == CHUNK and d[1] == 0x44:  # pseudo-chunk: 2c (44 xx)* 2d; each 44 reads+discards a section
        p = 1
        tags = []
        while d[p] == 0x44:
            tags.append((p, 0x44, d[p + 1:p + 2]))
            p += 2
        if d[p] != CHUNK_END:
            raise ParseError(f"pseudo-chunk: {d[p]:#x} at {p:#x}")
        p += 1
        yield 0, "pseudo", 0x44, tags
    while True:
        if p >= len(d):
            raise ParseError(f"no end byte 0x2e before {p:#x}")
        c = d[p]
        if c == END:
            yield p, "end", None, []
            return
        if c in (CMD_MODELS, CMD_LANG):
            yield p, "cmd", c, []
            p += 1
            continue
        if c != CHUNK:
            raise ParseError(f"top-level byte {c:#x} at {p:#x}")
        start, t = p, d[p + 1]
        table = TAGS.get(t)
        if table is None:
            raise ParseError(f"chunk type {t:#x} at {p + 1:#x}")
        p += 2
        tags = []
        while d[p] != CHUNK_END:
            tag = d[p]
            if tag not in table:
                raise ParseError(f"tag {tag:#x} in chunk {t:#x} at {p:#x}")
            n = table[tag]
            tags.append((p, tag, d[p + 1:p + 1 + n]))
            p += 1 + n
        p += 1
        yield start, "chunk", t, tags


def section1(path):
    kind, secs = bze.load(path)
    if kind != "tb":
        raise ParseError("not a TB BZE")
    return dict(secs)[1]


def parse_all(folder):
    bad = 0
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".bze"))
    for f in files:
        d = section1(os.path.join(folder, f))
        try:
            last = max(off for off, kind, _, _ in walk(d) if kind == "end")
            # Section-1 payload = actualSize; anything after the end byte must be zero padding.
            tail = d[last + 1:]
            status = "ok" if not tail.strip(b"\0") else f"trailing {len(tail)} bytes after end"
            status += f" (end @{last:#x}, {len(tail)} zero pad)" if tail and status == "ok" else ""
        except ParseError as e:
            status = f"FAIL {e}"
        except IndexError:
            status = "FAIL ran off the end"
        bad += not status.startswith("ok")
        print(f"{f:16} {len(d):7} {status}")
    print(f"{len(files) - bad}/{len(files)} parse to the last byte")
    return bad


def fmt(tag, pl):
    if len(pl) == 12 and tag in (0x10, 0x12):
        return f"{tag:02x}:{struct.unpack('<3i', pl)}"
    if len(pl) == 6 and tag == 0x11:
        return f"{tag:02x}:{struct.unpack('<3h', pl)}"
    return f"{tag:02x}:{pl.hex()}"


def players(d):
    """{2: Bugs, 3: Taz} -> (chunk offset, template id, position, rotation) from entity chunks 07/0a/63
    whose tag 0x27 instance id is 2 or 3 (FUN_004347c0 case 0x27: DAT_004d26cc / DAT_004d2768)."""
    out = {}
    for off, kind, t, tags in walk(d):
        if kind != "chunk" or t not in (0x07, 0x0A, 0x63):
            continue
        f = {}
        for _, tg, pl in tags:
            f.setdefault(tg, pl)
        if 0x27 in f and 0x10 in f:
            iid, tmpl = struct.unpack("<HH", f[0x27])
            if iid in (2, 3) and iid not in out:
                out[iid] = (off, tmpl, struct.unpack("<3i", f[0x10]), struct.unpack("<3h", f.get(0x11, bytes(6))))
    return out


def main():
    if sys.argv[1] == "--all":
        sys.exit(1 if parse_all(sys.argv[2]) else 0)
    if sys.argv[1] == "--players":
        for fn in sorted(os.listdir(sys.argv[2]), key=str.upper):
            p = players(section1(os.path.join(sys.argv[2], fn)))
            print(f"{fn:14} " + "  ".join(f"{('Bugs', 'Taz')[k - 2]}@sec1+{v[0]:#x} tmpl {v[1]} pos {v[2]} rot {v[3]}"
                                         for k, v in sorted(p.items())))
        return
    for off, kind, t, tags in walk(section1(sys.argv[1])):
        print(f"{off:06x} {kind} {'' if t is None else f'{t:02x}'} " + " ".join(fmt(tg, pl) for _, tg, pl in tags))


if __name__ == "__main__":
    main()
