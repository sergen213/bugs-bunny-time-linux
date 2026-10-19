#!/usr/bin/env python3
"""TB (bblit2.exe) collision mesh ("Plancher", section-1 chunk 0x35 tag 0x36 -> section 4) probe.

Layout and algorithms: docs/re/GAMEPLAY_TB.md §1 (init 0x445d00, octree locate 0x445d90, next-leaf 0x445ed0,
segment query 0x4463e0).

  tb_ground.py --check DIR     parse every level, assert the layout invariants, print totals
  tb_ground.py --spawns DIR    Bugs (id 2) / Taz (id 3) spawn points: downward segment query -> floor dy
  tb_ground.py LEVEL.bze       dump header, surfaces, normal classes
  tb_ground.py                 self-check (synthetic one-quad mesh)
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_sec1  # noqa: E402
import tb_world  # noqa: E402

FLOOR_NY = -11000  # Q14 normal.y below this = floor (0x443f40 contact classification, ~47.8 deg)


def block(path):
    S, chunks = tb_world.level(path)
    for t, f, _, sec in chunks:
        if t == 0x35:
            off, size = struct.unpack("<II", f[0x36])
            return S[sec][off:off + size]
    raise ValueError("no chunk 0x35")


class Mesh:
    """Header (0x445d00): +0 u32 ?, +4 surface table off, +8 surface count, +0xc vertex table off,
    +0x10 polygon table off, +0x14 s16 x0,y0,z0 (+0x1a pad), +0x1c u32 cube size, +0x20 octree root."""

    def __init__(self, b):
        self.b = b
        self.unk0, self.soff, self.ns, self.voff, self.poff = struct.unpack_from("<5I", b, 0)
        x0, y0, z0 = struct.unpack_from("<3h", b, 0x14)
        self.size = struct.unpack_from("<I", b, 0x1c)[0]
        h = self.size >> 1
        self.root = 0x20
        self.c0 = (x0 + h, y0 - h, z0 + h)  # 0x445d00: centre = corner + (h, -h, h)
        self.h0 = h
        # surface record 12 B: u16 a, u16 b, u16 c, u16 d, u32 flags (flags read by the queries)
        self.surf = [struct.unpack_from("<4HI", b, self.soff + 12 * i) for i in range(self.ns)]
        self.nv = (self.soff - self.voff) // 8
        self.verts = [struct.unpack_from("<3h", b, self.voff + 8 * i) for i in range(self.nv)]
        self.polys = {}  # key = index in 4-byte units from the polygon table (as stored in leaves)
        p = self.poff
        while p < self.voff:
            nx, ny, nz, si, d, cx, cy, cz, r, u14, n = struct.unpack_from("<4hi4hHH", b, p)
            edges = [struct.unpack_from("<3hh", b, p + 0x18 + 8 * k) for k in range(n)]
            self.polys[(p - self.poff) // 4] = dict(n=(nx, ny, nz), surf=si, d=d, c=(cx, cy, cz), r=r,
                                                     u14=u14, edges=edges)
            p += 0x18 + 8 * n
        self.pend = p

    # ------------------------------------------------------------------ octree
    def node(self, off):
        kind = self.b[off + 2]
        if kind & 1:
            return "inner", struct.unpack_from("<8h", self.b, off + 4)
        n = struct.unpack_from("<h", self.b, off + 4)[0]
        return "leaf", struct.unpack_from(f"<{n}H", self.b, off + 6)

    def locate(self, p):
        """0x445d90: (leaf offset or None, centre, half). Outside the root cube the result is zeroed:
        (None, (0, 0, 0), 0); 0x4463e0 still marches from that zero cube (0x445ed0)."""
        h = self.h0
        c = list(self.c0)
        if any(abs(p[i] - c[i]) > h for i in range(3)):
            return None, (0, 0, 0), 0
        off = self.root
        while self.b[off + 2] & 1:
            h >>= 1
            o = 0
            if p[0] < c[0]:
                c[0] -= h
            else:
                c[0] += h
                o |= 1
            if p[2] < c[2]:
                c[2] -= h
            else:
                c[2] += h
                o |= 2
            if p[1] < c[1]:
                c[1] -= h
                o |= 4
            else:
                c[1] += h
            child = struct.unpack_from("<h", self.b, off + 4 + 2 * o)[0] * 4
            if child == 0:
                return None, tuple(c), h  # empty octant: caller steps on (0x445ed0)
            off = self.root + child
        return off, tuple(c), h

    def next_leaf(self, c, h, p0, dv):
        """0x445ed0: leave cube (c, h) along p0 + t*dv (t Q12); step 20 units past the exit face."""
        while True:
            best, comp = 0x7fffffff, 0
            for i in (0, 2, 1):  # x, z, y order; ties keep the earlier axis
                if dv[i] == 0:
                    t = 0x7fffffff
                else:
                    face = c[i] - h if dv[i] < 0 else c[i] + h
                    t = tdiv((face - p0[i]) * 0x1000, dv[i])
                if t < best:
                    best, comp = t, dv[i]
            if best > 0x1000:
                return None
            t = best + 0x14000 // abs(comp)
            q = tuple(((dv[i] * t) >> 12) + p0[i] for i in range(3))
            off, c, h = self.locate(q)
            if off is not None:
                return off, c, h
            if h == 0:
                return None

    # ------------------------------------------------------------------ segment query (0x4463e0)
    def query(self, p0, dv, radius=5, qflags=0, mover=None):
        """Nearest polygon hit by segment p0 -> p0+dv, polygons widened by `radius` at every edge.
        Surface filter: the 0x4463e0 query-flag rule, or with `mover` = actor flags +0x10 the 0x446ea0 rule
        (what the movement code collides with). Returns (hit point, poly key, normal Q14) or None.
        Actor tests (query flag 1) are not modelled."""
        off, c, h = self.locate(p0)
        if off is None:
            r = self.next_leaf(c, h, p0, dv)
            off, c, h = r if r else (None, c, h)
        p1 = tuple(p0[i] + dv[i] for i in range(3))
        seen, best = set(), None
        while off is not None:
            _, keys = self.node(off)
            for k in keys:
                if k in seen:
                    continue
                seen.add(k)
                P = self.polys[k]
                f = self.surf[P["surf"]][4]
                if not (passes(f, qflags) if mover is None else mover_passes(f, mover)):
                    continue
                n, d = P["n"], P["d"]
                s0 = (d + dot(n, p0)) >> 14
                s1 = (d + dot(n, p1)) >> 14
                if not (s0 >= 0 and s1 <= 0 and s1 < s0):
                    continue
                rel = tuple(tdiv(dv[i] * s0, s0 - s1) for i in range(3))
                dist = dot(rel, rel)  # SQR 0x401ba0 -> MAC (unsaturated) read by 0x402060
                if best and dist > best[0]:
                    continue
                hit = tuple(p0[i] + rel[i] for i in range(3))
                if all(dot(e[:3], hit) - dot(e[:3], self.verts[e[3]]) <= radius << 14 for e in P["edges"]):
                    best = (dist, hit, k, n)
            if best:
                break
            r = self.next_leaf(c, h, p0, dv)
            off, c, h = r if r else (None, c, h)
        return best[1:] if best else None


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def tdiv(a, b):
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def passes(f, q):
    """0x4463e0 surface filter: flags & 0xf136 == 0 -> always solid; else every set bit needs its query bit."""
    if f & 0xF136 == 0:
        return True
    return ((not f & 0x100 or q & 0x400) and (not f & 0x10 or q & 2) and (not f & 0x20 or q & 4)
            and (not f & 4 or q & 8) and (not f & 2 or q & 0x100)
            and (not f & 0xF000 or q & (f >> 8) & 0xF0))


def mover_passes(f, actor10):
    """0x446ea0 surface filter for an actor with flags word +0x10: 0x100 never solid; 0xf000 class barriers
    solid only for actors with a matching bit in +0x10 bits 28-31; otherwise flag 4 is not solid."""
    if f & 0x100:
        return False
    if f & 0xF000:
        return bool((actor10 >> 16) & f & 0xF000)
    return not f & 4


# ---------------------------------------------------------------------- invariants
def check_mesh(m, warn=None):
    """Hard asserts: byte layout, indices, unit normals, octree reachability. Geometric quality (authoring
    tolerances) is counted into `warn`."""
    warn = collections.Counter() if warn is None else warn
    b = m.b
    assert m.poff < m.voff < m.soff and m.pend == m.voff, "poly table must end at the vertex table"
    assert m.soff + 12 * m.ns == len(b), "surface table must end the block"
    assert (m.soff - m.voff) % 8 == 0
    assert all(struct.unpack_from("<h", b, m.voff + 8 * i + 6)[0] == 0 for i in range(m.nv))
    for k, P in m.polys.items():
        n = P["n"]
        nl = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5
        if n == (0, 0, 0):  # 7 authoring-degenerate polygons: s0 == s1 never passes the query test -> inert
            warn["degenerate polygon (zero normal, inert)"] += 1
            continue
        assert abs(nl - 16384) < 64, (k, n)
        assert 0 <= P["surf"] < m.ns and len(P["edges"]) >= 3
        assert all(0 <= e[3] < m.nv for e in P["edges"])
        for e in P["edges"]:
            v = m.verts[e[3]]
            warn["edge vertex > 2 units off plane"] += abs(dot(n, v) + P["d"]) > 2 << 14
            warn["edge normal not in plane (cos > 0.01)"] += abs(dot(n, e[:3])) > 16384 * 164
            warn["centre outside an edge"] += dot(e[:3], P["c"]) - dot(e[:3], v) > 1 << 14
            dv = [v[i] - P["c"][i] for i in range(3)]
            warn["vertex outside bounding sphere"] += dot(dv, dv) > (P["r"] + 2) ** 2
        warn["edges"] += len(P["edges"])
    box = {k: [(min(m.verts[e[3]][i] for e in P["edges"]), max(m.verts[e[3]][i] for e in P["edges"]))
               for i in range(3)] for k, P in m.polys.items()}
    refs, stack, nodes = collections.Counter(), [(m.root, m.c0, m.h0)], 0
    while stack:
        off, c, h = stack.pop()
        assert 0x20 <= off < m.poff, "octree nodes live between the header and the polygon table"
        nodes += 1
        kind, data = m.node(off)
        if kind == "inner":
            h2 = h >> 1
            for o, ch in enumerate(data):  # octant bits: 1 = +x, 2 = +z, 4 = -y (0x445d90)
                if ch:
                    cc = (c[0] + (h2 if o & 1 else -h2), c[1] + (-h2 if o & 4 else h2), c[2] + (h2 if o & 2 else -h2))
                    stack.append((m.root + ch * 4, cc, h2))
            continue
        listed = set(data)
        for k in data:
            assert k in m.polys, ("leaf references a non-poly", off, k)
            refs[k] += 1
        warn["leaf refs"] += len(data)
        for k, bb in box.items():  # AABB prefilter, then exact convex-polygon/cube SAT (2-unit margin)
            overlap = all(bb[i][0] < c[i] + h - 2 and bb[i][1] > c[i] - h + 2 for i in range(3))
            touch = all(bb[i][0] <= c[i] + h + 2 and bb[i][1] >= c[i] - h - 2 for i in range(3))
            if overlap and k not in listed and m.polys[k]["n"] != (0, 0, 0) and poly_cube(m, k, c, h):
                warn["MISSING leaf ref (polygon intersects the leaf cube)"] += 1
            if k in listed and not touch:
                warn["extra leaf ref (polygon box misses the leaf cube)"] += 1
    assert set(refs) == set(m.polys), "every polygon is in some leaf"
    return nodes, refs


def poly_cube(m, k, c, h, margin=2):
    """Separating-axis test: convex polygon k vs cube (c, h) shrunk by `margin` (box axes, plane normal,
    edge x box-axis crosses). True = they intersect."""
    P = m.polys[k]
    vs = [m.verts[e[3]] for e in P["edges"]]
    axes = [(1, 0, 0), (0, 1, 0), (0, 0, 1), P["n"]]
    for i, a in enumerate(vs):
        b = vs[(i + 1) % len(vs)]
        e = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        axes += [x for x in ((0, -e[2], e[1]), (e[2], 0, -e[0]), (-e[1], e[0], 0)) if x != (0, 0, 0)]
    for ax in axes:
        p = [dot(ax, v) for v in vs]
        w = abs(ax[0]) + abs(ax[1]) + abs(ax[2])
        cc, rr = dot(ax, c), (h - margin) * w
        if min(p) > cc + rr or max(p) < cc - rr:
            return False
    return True


def classify(m):
    c = collections.Counter()
    for P in m.polys.values():
        ny = P["n"][1]
        c["floor" if ny < FLOOR_NY else "ceiling" if ny > -FLOOR_NY else "wall"] += 1
    return c


def levels(folder):
    return sorted(f for f in os.listdir(folder) if f.lower().endswith(".bze"))


def check(folder):
    tot, flags, nvh, warn = (collections.Counter() for _ in range(4))
    for f in levels(folder):
        m = Mesh(block(os.path.join(folder, f)))
        nodes, refs = check_mesh(m, warn)
        tot["levels"] += 1
        tot["polys"] += len(m.polys)
        tot["verts"] += m.nv
        tot["surfaces"] += m.ns
        tot["nodes"] += nodes
        tot.update(classify(m))
        for s in m.surf:
            flags[hex(s[4])] += 1
        for P in m.polys.values():
            nvh[len(P["edges"])] += 1
    print(dict(tot))
    print("surface flags:", dict(flags.most_common()))
    print("vertices/polygon:", dict(sorted(nvh.items())))
    print("geometry tolerances:", dict(warn))
    print(f"{tot['levels']} levels: layout invariants hold")


def player_spawns(path):
    """{2: Bugs, 3: Taz} -> ((x, y, z) s16, flags word +0x10 from tag 0x16) for the instance chunks."""
    out = {}
    for _, kind, t, tags in tb_sec1.walk(tb_sec1.section1(path)):
        if kind != "chunk" or t not in (0x07, 0x0A, 0x63):
            continue
        f = {}
        for _, tg, pl in tags:
            f.setdefault(tg, pl)
        if 0x27 in f and 0x10 in f:
            iid = struct.unpack_from("<H", f[0x27])[0]
            if iid in (2, 3) and iid not in out:
                pos = struct.unpack("<hxxhxxhxx", f[0x10])  # tag 0x10: s16 from bytes 0-1, 4-5, 8-9
                out[iid] = (pos, struct.unpack_from("<I", f.get(0x16, bytes(28)), 8)[0])
    return out


def spawn_floor(m, pos, actor10):
    """Floor under a spawn as the mover sees it: segment from 100 above, 2000 down, mover surface filter."""
    x, y, z = pos
    r = m.query((x, y - 100, z), (0, 2000, 0), mover=actor10)
    if r is None:
        return "none", None, None
    hit, k, n = r
    dy = hit[1] - y  # > 0: floor below the spawn (+Y down)
    kind = "on" if abs(dy) <= 16 else "drop" if dy > 16 else "embedded"
    return (kind if n[1] < FLOOR_NY else "steep"), dy, (k, n)


def spawns(folder):
    tally, rows = collections.Counter(), []
    for f in levels(folder):
        path = os.path.join(folder, f)
        m = Mesh(block(path))
        row = [f"{f:14}"]
        for iid, (pos, a10) in sorted(player_spawns(path).items()):
            kind, dy, hit = spawn_floor(m, pos, a10)
            tally[kind] += 1
            who = "Bugs" if iid == 2 else "Taz "
            if hit is None:
                inside = m.locate(pos)[2] != 0
                row.append(f"{who} {pos} NO FLOOR ({'inside' if inside else 'outside'} the octree cube)")
            else:
                row.append(f"{who} {pos} {kind:8} dy {dy:+5d} n.y {hit[1][1]:6d} surf {m.polys[hit[0]]['surf']}")
        rows.append("  ".join(row))
    print("\n".join(rows))
    print(f"{sum(tally.values())} spawns: {dict(tally)} (on = |dy| <= 16, drop = floor below, "
          f"embedded = floor up to 100 above the origin)")
    return tally


def dump(path):
    m = Mesh(block(path))
    print(f"hdr unk0 {m.unk0:#x} surfaces {m.ns} @{m.soff:#x} verts {m.nv} @{m.voff:#x} polys {len(m.polys)} "
          f"@{m.poff:#x} cube centre {m.c0} half {m.h0}")
    for i, s in enumerate(m.surf):
        print(f"  surface {i}: {s[0]} {s[1]} {s[2]} {s[3]} flags {s[4]:#x}")
    print(" ", dict(classify(m)))


def selftest():
    """One 200x200 floor quad at y = 0 (normal -Y = up in PS1 axes) in a 1-leaf octree."""
    verts = [(-100, 0, -100), (100, 0, -100), (100, 0, 100), (-100, 0, 100)]
    edges = [(0, 0, -16384, 0), (16384, 0, 0, 1), (0, 0, 16384, 2), (-16384, 0, 0, 3)]
    poly = struct.pack("<4hi4hHH", 0, -16384, 0, 0, 0, 0, 0, 0, 142, 0, 4) + b"".join(
        struct.pack("<3hh", *e) for e in edges)
    leaf = struct.pack("<HBBhH", 0xFFFF, 0, 0, 1, 0)
    poff = 0x20 + len(leaf) + 2
    voff = poff + len(poly)
    soff = voff + 8 * len(verts)
    hdr = struct.pack("<5I3hhI", 0, soff, 1, voff, poff, -1024, 1024, -1024, 0, 2048)
    b = hdr + leaf + b"\0\0" + poly + b"".join(struct.pack("<4h", *v, 0) for v in verts) + struct.pack(
        "<4HI", 4096, 4096, 255, 255, 0)
    m = Mesh(b)
    check_mesh(m)
    hit, k, n = m.query((0, -100, 0), (0, 300, 0))
    assert hit == (0, 0, 0) and k == 0 and n == (0, -16384, 0), hit
    assert m.query((0, 50, 0), (0, 300, 0)) is None          # starts below the floor: no hit
    assert m.query((104, -100, 0), (0, 300, 0))[0] == (104, 0, 0)  # within the 5-unit edge margin
    assert m.query((106, -100, 0), (0, 300, 0)) is None
    assert not passes(0x10, 0) and passes(0x10, 2) and passes(1, 0)
    print("selftest ok")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        selftest()
    elif a[0] == "--check":
        check(a[1])
    elif a[0] == "--spawns":
        spawns(a[1])
    else:
        dump(a[0])
