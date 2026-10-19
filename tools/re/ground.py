#!/usr/bin/env python3
"""LiT ground/collision block probe (section-1 chunk 0x36 tag 0x37 -> section 4).

Layout and query semantics: CollisionRE notes (bugs.exe 0x434c30 init, 0x436b60 zone lookup,
0x436c20 point ground query, 0x434cd0 move trace). Research use only; exports stay outside the repo.
    ground.py LEVEL.bze [--obj OUT.obj] [--check]   one LiT level: zone table, stats, optional
                                                    heightfield OBJ, optional world-floor comparison
    ground.py --all DIR                             stats over every LiT *.bze in DIR
    ground.py --tb LEVEL.bze                        TB chunk 0x35 tag 0x36 ("Plancher") header
    ground.py --selftest
"""
import argparse
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import bze  # noqa: E402
import sec1  # noqa: E402

CELL, SUB = 0x140, 0x28  # 320-unit cells of 8x8 40-unit sub-cells
HOLE, WALL = 0x7E, 0x7F
# 0x44-byte zone record (offsets relative to record; +0x1c/+0x20 relative to block start)
REC = struct.Struct("<HHHHHHiiiiII4i4i")
FIELDS = ("id", "hscale", "cols", "rows", "size_x", "size_z", "min_x", "min_z", "y_bottom", "y_top",
          "cellmap", "tiles", "box_min", "box_size")
# result kinds (short result[1]) of 0x436c20 / 0x434cd0
K_NOZONE, K_FLOOR, K_WALL, K_HOLE = 1, 2, 6, 8
K_SLIDE = 0x20  # OR-ed when the sub-cell byte is positive


def parse_block(blk):
    """-> list of zone dicts. Mirrors FUN_00434c30 (u32 count, count x 0x44-byte records)."""
    n = struct.unpack_from("<I", blk, 0)[0]
    zones = []
    for i in range(n):
        v = REC.unpack_from(blk, 4 + i * 0x44)
        z = dict(zip(FIELDS[:12], v[:12]), box_min=v[12:16], box_size=v[16:20])
        assert z["id"] < 0x3C, f"zone id {z['id']} (engine traps >= 0x3c)"
        z["map"] = struct.unpack_from(f"<{z['cols'] * z['rows']}H", blk, z["cellmap"])
        zones.append(z)
    return zones


def find_block(path):
    """-> (kind, sec4 bytes, offset, size) of the collision block, or offset None."""
    kind, secs = bze.load(path)
    by = dict(secs)
    s1 = by[1]
    if kind == "tb":  # structural tokens are LiT-1: chunk '2c 35', tag 36 {u32 off, u32 size}
        p = s1.find(b"\x2c\x35\x36")
        return kind, by.get(4, b""), *((None, 0) if p < 0 else struct.unpack_from("<II", s1, p + 3))
    for _, k, t, tags in sec1.walk(s1):
        if t == 0x36:
            return kind, by[4], *struct.unpack("<II", dict(tags)[0x37])
    return kind, by.get(4, b""), None, 0


def zone_lookup(zones, x, y, z, hint=-1):
    """FUN_00436b60: records with id==hint first (they are contiguous), then all records in order.
    Inside test: min_x <= x < min_x+size_x, min_z <= z < min_z+size_z, y_top < y <= y_bottom."""
    def inside(r):
        return (r["y_top"] < y <= r["y_bottom"] and r["min_x"] <= x < r["min_x"] + r["size_x"]
                and r["min_z"] <= z < r["min_z"] + r["size_z"])
    if hint >= 0:
        i = next((k for k, r in enumerate(zones) if r["id"] == hint), len(zones))
        while i < len(zones) and zones[i]["id"] == hint:
            if inside(zones[i]):
                return zones[i]
            i += 1
    return next((r for r in zones if inside(r)), None)


def subcell(blk, r, x, z):
    """-> (cell word, raw signed sub-cell byte). C '/' and '%' truncate toward zero (x>=min_x here)."""
    lx, lz = x - r["min_x"], z - r["min_z"]
    w = r["map"][lz // CELL * r["cols"] + lx // CELL]
    b = blk[r["tiles"] + (w & 0xFFF) * 64 + (lz % CELL // SUB) * 8 + lx % CELL // SUB]
    return w, b - 256 if b > 127 else b


def query(zones, blk, x, y, z, hint=-1):
    """FUN_00436c20 point ground query -> dict(kind, raw, hoff, ground_y, flags, zone).
    ground_y = y_bottom + hoff with hoff = -|raw| * hscale (PS1 +Y down: floor rises above y_bottom)."""
    r = zone_lookup(zones, x, y, z, hint)
    if r is None:
        return dict(kind=K_NOZONE, raw=0, hoff=0, ground_y=None, flags=0, zone=-1)
    w, raw = subcell(blk, r, x, z)
    out = dict(raw=raw, flags=w & 0xF000, zone=r["id"], ground_y=r["y_bottom"], hoff=0)
    if raw == HOLE:
        out["kind"] = K_HOLE
    elif raw == WALL:
        out["kind"] = K_WALL
    else:
        out["kind"] = K_FLOOR | (K_SLIDE if raw > 0 else 0)
        out["hoff"] = -abs(raw) * r["hscale"]
        out["ground_y"] = r["y_bottom"] + out["hoff"]
    return out


def stats(zones, blk):
    t0 = min(r["tiles"] for r in zones)
    assert (len(blk) - t0) % 64 == 0
    ntile = (len(blk) - t0) // 64
    nib, val = collections.Counter(), collections.Counter()
    for r in zones:
        assert r["size_x"] == r["cols"] * CELL and r["size_z"] == r["rows"] * CELL
        assert r["tiles"] == t0, "tile pool is shared by all zones"
        for w in r["map"]:
            nib[w >> 12] += 1
            assert (w & 0xFFF) < ntile
    for b in blk[t0:]:
        b = b - 256 if b > 127 else b
        val["0x7e hole" if b == HOLE else "0x7f wall" if b == WALL else "0" if b == 0
            else "+ slide" if b > 0 else "- floor"] += 1
    return ntile, nib, val


def export_obj(path, zones, blk):
    """Flat 40x40 quad per sub-cell (engine heights are piecewise constant, no interpolation).
    Groups: floor / slide (positive byte) / hole (0x7e, at y_bottom) / wall (0x7f, at y_top)."""
    vs, faces = [], collections.defaultdict(list)
    for r in zones:
        for cz in range(r["rows"]):
            for cx in range(r["cols"]):
                for sz in range(8):
                    for sx in range(8):
                        x = r["min_x"] + cx * CELL + sx * SUB
                        z = r["min_z"] + cz * CELL + sz * SUB
                        _, raw = subcell(blk, r, x, z)
                        if raw == HOLE:
                            g, y = "hole", r["y_bottom"]
                        elif raw == WALL:
                            g, y = "wall", r["y_top"]
                        else:
                            g, y = ("slide" if raw > 0 else "floor"), r["y_bottom"] - abs(raw) * r["hscale"]
                        base = len(vs) + 1
                        vs += [(x, y, z), (x + SUB, y, z), (x + SUB, y, z + SUB), (x, y, z + SUB)]
                        faces[g].append(base)
    with open(path, "w") as fp:
        fp.write("".join(f"v {x} {-y} {z}\n" for x, y, z in vs))  # PS1 +Y down -> flip
        for g, fs in faces.items():
            fp.write(f"g {g}\n" + "".join(f"f {b} {b + 3} {b + 2} {b + 1}\n" for b in fs))
    return {g: len(fs) for g, fs in faces.items()}


def check_world(path, zones, blk):
    """Compare sub-cell heights with chunk-0x20 world polygons (vertical ray at the sub-cell centre;
    nearest world surface height). Needs Pillow via export_level."""
    import export_level as el
    kind, secs = bze.load(path)
    by = dict(secs)
    tris = []
    for _, k, t, tags in sec1.walk(by[1]):
        if t != 0x20:
            continue
        d = dict(tags)
        o, _ = struct.unpack("<II", d[0x24])
        tr = struct.unpack("<3i", d.get(0x10, bytes(12)))
        sc = struct.unpack("<i", d.get(0x12, b"\0\x10\0\0")[:4])[0] / 4096
        for pos, *_ in el.model_faces(by[4], o, {}):
            p = [el.transform(v, tr, sc) for v in pos]
            tris += [(p[0], p[1], p[2])] + ([(p[1], p[3], p[2])] if len(p) == 4 else [])
    grid = collections.defaultdict(list)
    for t in tris:
        xs, zs = [v[0] for v in t], [v[2] for v in t]
        for gx in range(int(min(xs) // CELL), int(max(xs) // CELL) + 1):
            for gz in range(int(min(zs) // CELL), int(max(zs) // CELL) + 1):
                grid[gx, gz].append(t)

    def heights(x, z):
        for (ax, ay, az), (bx, by_, bz), (cx, cy, cz) in grid[int(x // CELL), int(z // CELL)]:
            den = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
            if abs(den) < 1e-6:
                continue
            u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / den
            v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / den
            if u >= -1e-6 and v >= -1e-6 and u + v <= 1 + 1e-6:
                yield u * ay + v * by_ + (1 - u - v) * cy

    dys, miss = [], 0
    for r in zones:
        for cz in range(r["rows"] * 8):
            for cx in range(r["cols"] * 8):
                x, z = r["min_x"] + cx * SUB + SUB // 2, r["min_z"] + cz * SUB + SUB // 2
                _, raw = subcell(blk, r, x, z)
                if raw in (HOLE, WALL):
                    continue
                gy = r["y_bottom"] - abs(raw) * r["hscale"]
                hs = list(heights(x, z))
                if not hs:
                    miss += 1
                    continue
                dys.append(min((h - gy for h in hs), key=abs))
    a = sorted(abs(d) for d in dys)
    if not a:
        return "no comparable samples"
    q = lambda f: a[min(len(a) - 1, int(f * len(a)))]  # noqa: E731
    within = lambda lim: sum(1 for d in a if d <= lim) / len(a)  # noqa: E731
    return (f"world check: {len(tris)} world tris, {len(a)} floor sub-cells with a world surface "
            f"below/above ({miss} without): |dy| median {q(.5):.1f} p90 {q(.9):.1f} p99 {q(.99):.1f}; "
            f"within 16u {within(16):.1%}, within 40u {within(40):.1%}, within 100u {within(100):.1%}")


def summarize(path, obj=None, check=False):
    kind, s4, off, size = find_block(path)
    name = os.path.basename(path)
    if off is None:
        print(f"{name}: no chunk 0x36")
        return None
    blk = s4[off:off + size]
    zones = parse_block(blk)
    ntile, nib, val = stats(zones, blk)
    print(f"{name}: block sec4+{off:#x} size {size:#x}, {len(zones)} zones, {ntile} tiles (8x8 bytes)")
    for i, r in enumerate(zones):
        print(f"  [{i:2}] id {r['id']:2} hscale {r['hscale']:2} cells {r['cols']}x{r['rows']} "
              f"x[{r['min_x']},{r['min_x'] + r['size_x']}) z[{r['min_z']},{r['min_z'] + r['size_z']}) "
              f"y({r['y_top']},{r['y_bottom']}] box_min {r['box_min'][:3]} box_size {r['box_size'][:3]}")
    print(f"  cell flag nibble histogram {dict(sorted(nib.items()))}; tile bytes {dict(val)}")
    if obj:
        print(f"  wrote {obj}: {export_obj(obj, zones, blk)}")
    if check:
        print("  " + check_world(path, zones, blk))
    return zones, ntile, nib, val


def tb_summary(path):
    kind, s4, off, size = find_block(path)
    assert kind == "tb"
    if off is None:
        print(f"{os.path.basename(path)}: no chunk 0x35")
        return
    b = s4[off:off + size]
    h = struct.unpack_from("<5I3hHI", b, 0)
    # bblit2 FUN_00445d00: +4 surface table (count +8, 12-byte records, flags u32 @+8),
    # +0xc vertex table, +0x10 polygon table, s16 corner @+0x14, u32 size @+0x1c, octree nodes @+0x20
    print(f"{os.path.basename(path)}: TB floor block sec4+{off:#x} size {size:#x}: polygonal octree, not "
          f"the LiT heightfield. surface tbl +{h[1]:#x} x{h[2]}, vertex tbl +{h[3]:#x}, poly tbl +{h[4]:#x}, "
          f"corner {h[5:8]}, size {h[9]}")
    for i in range(h[2]):
        print(f"  surface {i}: {b[h[1] + 12 * i:h[1] + 12 * i + 12].hex()}")


def selftest():
    # one zone id 3, hscale 16, 1x1 cell at (0,0), y(-1000, 0]; tile 0 bytes: [0]=-5, [1]=+5, [8]=0x7e, [9]=0x7f
    tiles = bytearray(64)
    tiles[0], tiles[1], tiles[8], tiles[9] = 0xFB, 5, HOLE, WALL
    rec = REC.pack(3, 16, 1, 1, CELL, CELL, 0, 0, 0, -1000, 4 + 0x44, 4 + 0x44 + 2,
                   0, 0, 0, 0, CELL, -1000, CELL, 0)
    blk = struct.pack("<I", 1) + rec + struct.pack("<H", 0x2000) + bytes(tiles)
    zones = parse_block(blk)
    q = query(zones, blk, 10, -1, 10)
    assert (q["kind"], q["hoff"], q["ground_y"], q["flags"], q["zone"]) == (K_FLOOR, -80, -80, 0x2000, 3), q
    assert query(zones, blk, 50, -1, 10)["kind"] == K_FLOOR | K_SLIDE
    assert query(zones, blk, 10, -1, 50)["kind"] == K_HOLE
    assert query(zones, blk, 50, -1, 50)["kind"] == K_WALL
    assert query(zones, blk, 10, 5, 10)["kind"] == K_NOZONE  # below y_bottom
    assert query(zones, blk, 10, -1000, 10)["kind"] == K_NOZONE  # y_top is exclusive
    assert zone_lookup(zones, 319, 0, 319, hint=3) is zones[0]
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bze", nargs="?")
    ap.add_argument("--obj")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--all")
    ap.add_argument("--tb")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    elif a.tb:
        tb_summary(a.tb)
    elif a.all:
        nz, nib, val, nt, flagged = [], collections.Counter(), collections.Counter(), [], {}
        hs = collections.Counter()
        for p in sorted(glob.glob(os.path.join(a.all, "*.bze"))):
            r = summarize(p)
            if r:
                zones, ntile, n1, v1 = r
                nz.append(len(zones))
                nt.append(ntile)
                nib.update(n1)
                val.update(v1)
                hs.update(z["hscale"] for z in zones)
                if n1.get(2):
                    flagged[os.path.basename(p)] = n1[2]
        print(f"\nTOTAL: {len(nz)} levels with chunk 0x36, zones {sum(nz)} (min {min(nz)}, max {max(nz)}), "
              f"tiles {sum(nt)} (max {max(nt)}), hscale {dict(hs)}")
        print(f"cell flag nibble histogram {dict(sorted(nib.items()))}; levels with flag 0x2000 cells {flagged}")
        print(f"tile byte classes {dict(val)}")
    else:
        summarize(a.bze, a.obj, a.check)


if __name__ == "__main__":
    main()
