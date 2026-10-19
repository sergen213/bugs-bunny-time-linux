#!/usr/bin/env python3
"""TB (bblit2.exe) level geometry exporter: section-1 walk -> TIM textures, octree world models (chunk 0x20)
and octree props (chunk 0x22 tag 0x24) as OBJ+MTL. Format: docs/formats/MODELS.md §10, TB_LEVELS.md.

Usage: tb_world.py LEVEL.bze OUTDIR        (writes tex/, world.obj, world.mtl, models/, summary.txt)
       tb_world.py --check BZEDIR          (structure invariants over every level, no output files)
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bze  # noqa: E402
import tb_sec1  # noqa: E402
from export_level import decode_tim, write_obj  # noqa: E402

LANG_EN = 6  # language index 0 -> section id 6 (FUN_004347c0 command 0x4a)


def level(path):
    """Return (sections by id, [(chunk_type, {tag: payload (first)}, [(tag, payload)], section_id)]).
    Chunk offsets are relative to the section read by the latest command: 3 at start, the language
    section after 0x4a, 4 after 0x2b (FUN_004347c0: pbStack00000008)."""
    kind, secs = bze.load(path)
    S = dict(secs)
    cur, chunks = 3, []
    for _, k, t, tags in tb_sec1.walk(S[1]):
        if k == "cmd":
            cur = 4 if t == tb_sec1.CMD_MODELS else LANG_EN
        elif k == "chunk":
            first = {}
            for _, tg, pl in tags:
                first.setdefault(tg, pl)
            chunks.append((t, first, [(tg, pl) for _, tg, pl in tags], cur))
    return S, chunks


# ---------------------------------------------------------------- octree model (FUN_00404ed0 / FUN_004050e0)
def leaves(m, node, base=None, out=None):
    """Depth-first leaves of the tree rooted at `node`; child offsets are u16 * 4 from the tree root."""
    base = node if base is None else base
    out = [] if out is None else out
    kind = m[node + 2]
    if kind == 0:
        out.append(node)
    elif kind == 1:
        for c in struct.unpack_from("<8H", m, node + 0x10):
            if c:
                leaves(m, base + c * 4, base, out)
    else:
        raise ValueError(f"node {node:#x}: kind {kind}")
    return out


def trees(m):
    """(name, root) of the up to three trees: main @0x18, 'alt' @hdr[0]+0x18, 'detail' @hdr[1]+0x18."""
    h0, h1 = struct.unpack_from("<2I", m, 0)
    return [("main", 0x18)] + [(n, h + 0x18) for n, h in (("alt", h0), ("detail", h1)) if h]


def prim_valid(b0, b1):
    # FUN_00405c90 (CheckPrimValidity)
    return (b0 & 4 and b0 & 0x20 and not b0 & 0x40 and not (not b0 & 8 and b1 & 0x20)
            and not (b1 & 0x40 and b0 & 2))


NO_UV = ((0, 0), (255, 0), (255, 255), (0, 255))  # b1 & 0x40: i0..i3 get (0,0) (1,0) (1,1) (0,1) [FUN_0041f420]


def octree_faces(m, stats):
    """Yield (tree, [xyz]*n, [uv bytes]*n, tex_id, b0, b1) per valid primitive; n = 3 or 4 (PS1 Z order)."""
    uvtab = struct.unpack_from("<I", m, 8)[0]
    for name, root in trees(m):
        for lf in leaves(m, root):
            nv, npr = struct.unpack_from("<HH", m, lf + 0x10)
            V = [struct.unpack_from("<3h", m, lf + 0x14 + 8 * i) for i in range(nv)]
            p = lf + 0x14 + nv * 8
            for i in range(npr):
                r = m[p + 28 * i:p + 28 * i + 28]
                b0, b1 = r[0], r[1]
                if not prim_valid(b0, b1):
                    stats["invalid"] += 1
                    continue
                n = 4 if b0 & 8 else 3
                idx = r[0x14:0x14 + n]
                if max(idx) >= nv:
                    raise ValueError(f"leaf {lf:#x}: index {max(idx)} >= {nv}")
                if b1 & 0x40:
                    uv = NO_UV[:n]
                else:
                    e = uvtab + struct.unpack_from("<H", r, 0x1a)[0]
                    uv = [(m[e + o], m[e + o + 1]) for o in (0, 4, 8, 10)][:n]
                stats[(name, n, b0, b1)] += 1
                yield name, [V[k] for k in idx], uv, struct.unpack_from("<H", r, 2)[0] & 0xfff, b0, b1


def is_octree(m):
    # FUN_004347c0 chunk 0x22 tag 0x24: (u32 @+0x18 & 0xffffff) == 0x1ffff -> octree, else actor format
    return len(m) >= 0x1c and struct.unpack_from("<I", m, 0x18)[0] & 0xffffff == 0x1ffff


def actor_faces(m, stats):
    """Actor/prop format (chunk 0x22 tag 0x24, non-octree; FUN_00406f70/FUN_00406cd0). Yield
    (object, [xyz]*n, [uv bytes]*n, tex_id, b0, b1). Vertices are the per-object tables concatenated; each
    object is transformed by its own matrix at run time (bones), so this is the un-posed vertex data."""
    nobj, ph = struct.unpack_from("<HI", m, 2)
    objs = [struct.unpack_from("<II", m, 8 + 8 * i) for i in range(nobj)]
    if not objs:
        return  # 8-byte model {1, 0}: no geometry
    V, owner = [], []
    for k, (o, n) in enumerate(objs):
        V += [struct.unpack_from("<3h", m, 8 + o + 8 * i) for i in range(n)]
        owner += [k] * n
    nC, nD, nA, nB, oB, oC, oD = struct.unpack_from("<HHBBHHH", m, ph)
    # layout invariants (hold for all 2558 non-empty actor models in the 58 levels)
    if (oB, oC, oD) != (0xC + 0x24 * nA, oB + 0x24 * nB, oC + 0x28 * nC) or 8 + objs[0][0] != ph + oD + 0x28 * nD \
            or 8 + objs[0][0] + 8 * len(V) != len(m):
        raise ValueError("actor layout")
    for base, cnt, size, n in ((0xC, nA, 0x24, 3), (oB, nB, 0x24, 4), (oC, nC, 0x28, 3), (oD, nD, 0x28, 4)):
        for i in range(cnt):
            r = m[ph + base + size * i:ph + base + size * (i + 1)]
            b0, b1 = r[0], r[1]
            if bool(b1 & 0x80) != (size == 0x28):
                raise ValueError("b1 bit 7 (u16 indices) must match the 0x28-byte groups")
            idx = struct.unpack_from("<4H", r, 0x20)[:n] if b1 & 0x80 else r[0x20:0x20 + n]
            uv = NO_UV[:n] if b1 & 0x40 else [(r[o], r[o + 1]) for o in (0x14, 0x18, 0x1C, 0x1E)][:n]
            stats[(n, size, b0)] += 1
            yield owner[idx[0]], [V[k] for k in idx], uv, struct.unpack_from("<H", r, 2)[0] & 0xfff, b0, b1


# ---------------------------------------------------------------- driver
def export(path, out):
    S, chunks = level(path)
    os.makedirs(os.path.join(out, "tex"), exist_ok=True)
    os.makedirs(os.path.join(out, "models"), exist_ok=True)
    summary = [f"{path}: sections {list(S)}"]
    texsizes = {}
    for t, _, tags, sec in chunks:
        if t != 0x28:
            continue
        for tg, pl in tags:
            off, packed = struct.unpack("<II", pl)
            tid = packed & 0xfff
            im, _ = decode_tim(S[sec], off)
            texsizes[tid] = im.size
            im.save(os.path.join(out, "tex", f"{tid:04d}.png"))
    with open(os.path.join(out, "world.mtl"), "w") as fp:
        fp.write("newmtl untex\nKd 0.7 0.7 0.7\n")
        for tid in sorted(texsizes):
            fp.write(f"newmtl tex{tid}\nKd 1 1 1\nmap_Kd tex/{tid:04d}.png\nmap_d tex/{tid:04d}.png\n")
    summary.append(f"textures: {len(texsizes)}")

    stats, faces = collections.Counter(), []
    for t, f, _, sec in chunks:
        if t != 0x20:
            continue
        off, size = struct.unpack("<II", f[0x24])
        tr = struct.unpack("<3i", f[0x10])
        sc = struct.unpack("<i", f[0x12][:4])[0] / 4096  # FUN_004347c0: X word used for all three axes
        rot = struct.unpack("<3h", f[0x11])
        # ponytail: rotation ignored (every TB 0x20 chunk has 0x1000 x3 = identity, checked by --check)
        for tree, pos, uv, tex, b0, b1 in octree_faces(S[sec][off:off + size], stats):
            faces.append(([tuple(v[i] * sc + tr[i] for i in range(3)) for v in pos], uv, tex, b0))
        summary.append(f"world sec{sec}+{off:#x} size {size:#x} pos {tr} scale {sc} rot {rot} id {f[0x21][:2].hex()}")
    nv = write_obj(os.path.join(out, "world.obj"), faces, "world.mtl")
    summary.append(f"world: {len(faces)} faces, {nv} obj verts, invalid prims skipped {stats.pop('invalid', 0)}")
    per = collections.Counter()
    for (tree, n, b0, b1), c in stats.items():
        per[(tree, n)] += c
    summary.append(f"  per tree/vertex count: {dict(per)}")
    missing = sorted({f[2] for f in faces} - set(texsizes))
    summary.append(f"  texture ids used but not loaded: {missing}")

    done, kinds = set(), collections.Counter()
    for t, f, tags, sec in chunks:
        if t != 0x22:
            continue
        for tg, pl in tags:
            if tg != 0x24:
                continue
            off, size = struct.unpack("<II", pl)
            m = S[sec][off:off + size]
            if (sec, off) in done:
                continue
            done.add((sec, off))
            octree = is_octree(m)
            fs = [(pos, uv, tex, b0) for _, pos, uv, tex, b0, _ in
                  (octree_faces if octree else actor_faces)(m, collections.Counter())]
            nobj = 0 if octree else struct.unpack_from("<H", m, 2)[0]
            kinds["octree" if octree else f"actor nobj={'1' if nobj == 1 else '0' if not nobj else '>1'}"] += 1
            if fs:
                name = f"{'o' if octree else 'a'}{nobj}_s{sec}_{off:06x}.obj"
                write_obj(os.path.join(out, "models", name), fs, "../world.mtl")
    summary.append(f"chunk 0x22 tag 0x24 models: {dict(kinds)} (actor OBJs are un-posed, see MODELS.md §10)")
    open(os.path.join(out, "summary.txt"), "w").write("\n".join(summary) + "\n")
    print("\n".join(summary))


def octree_invariants(m):
    """Assert the MODELS.md §10.1 data invariants of one octree model; return (leaves, vertices)."""
    h = struct.unpack_from("<6I", m)
    nl = nverts = 0
    for name, root in trees(m):
        end = 0
        for lf in leaves(m, root):
            x, y, z, sx, sy, sz = struct.unpack_from("<3h3H", m, lf + 4)
            nv, npr = struct.unpack_from("<HH", m, lf + 0x10)
            for i in range(nv):
                vx, vy, vz, w = struct.unpack_from("<4h", m, lf + 0x14 + 8 * i)
                assert w == 0 and x <= vx <= x + sx and y - sy <= vy <= y and z <= vz <= z + sz, hex(lf)
            p = lf + 0x14 + 8 * nv
            for i in range(npr):
                tex, a, b = (struct.unpack_from("<H", m, p + 28 * i + o)[0] for o in (2, 0x18, 0x1a))
                assert tex >> 12 == 7 and a == b, hex(p + 28 * i)
                if not m[p + 28 * i + 1] & 0x40:
                    assert h[2] and b % 12 == 0 and h[2] + b + 12 <= len(m), hex(p + 28 * i)
            end = max(end, p + 28 * npr)
            nl, nverts = nl + 1, nverts + nv
        if name == "main" and h[2]:
            assert end == h[2], "tree A must end at the UV table"
    return nl, nverts


def check(folder):
    """Assert the MODELS.md §10 invariants over every level and print totals."""
    agg = collections.Counter()
    for fn in sorted(os.listdir(folder)):
        S, chunks = level(os.path.join(folder, fn))
        for t, f, tags, sec in chunks:
            refs = []
            if t == 0x20:
                refs.append(struct.unpack("<II", f[0x24]))
                agg["rot!=identity"] += struct.unpack("<3h", f[0x11]) != (4096,) * 3
            elif t == 0x22:
                refs += [struct.unpack("<II", pl) for tg, pl in tags if tg == 0x24]
                for tg, pl in tags:
                    if tg == 0x3f:  # sprite sequence (§10.3)
                        off, size = struct.unpack("<II", pl)
                        m = S[sec][off:off + size]
                        nobj, ph, vo, nv = struct.unpack_from("<HIII", m, 2)
                        nd = struct.unpack_from("<H", m, ph + 2)[0]
                        assert nobj == 1 and nd == nv and 8 + vo == size == ph + 0xC + 16 * nd, (fn, hex(off))
                        assert all(m[ph + 0xC + 16 * i + 3] == 0x64 for i in range(nd)), (fn, hex(off))
                        agg["sprite sequences"] += 1
            for off, size in refs:
                m = S[sec][off:off + size]
                if t == 0x22 and not is_octree(m):
                    agg["actor-format models"] += 1
                    agg["actor faces"] += sum(1 for _ in actor_faces(m, collections.Counter()))
                    continue
                st = collections.Counter()
                agg["world faces" if t == 0x20 else "sky faces"] += sum(1 for _ in octree_faces(m, st))
                agg["world models" if t == 0x20 else "sky domes"] += 1
                agg["invalid prims"] += st["invalid"]
                nl, nv = octree_invariants(m)
                agg["leaves"] += nl
                agg["octree vertices"] += nv
    print(dict(agg))


if __name__ == "__main__":
    if sys.argv[1] == "--check":
        check(sys.argv[2])
    else:
        export(sys.argv[1], sys.argv[2])
