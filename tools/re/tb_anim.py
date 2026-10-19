#!/usr/bin/env python3
"""TB (bblit2.exe) actor animation probe: chunk 0x22 tag 0x25 'packed TOD' assets posing the multi-object
actor models of MODELS.md §10.2. Semantics recovered from the loader FUN_004347c0 (case 0x25), the rig builder
FUN_00469210, the player FUN_00469770, the frame applier FUN_00468280, GsGetLw FUN_00405820 and the per-vertex
transform FUN_00408d00 (slot buffer). Notes: local://tb_anim.md (to be merged into docs/re/GAMEPLAY_TB.md).

Usage: tb_anim.py stats [BZEDIR]                        parse + validate every 0x25 asset of all levels
       tb_anim.py pose LEVEL ENTITY_ID ANIM FRAME OUT.png [front|side|both]
                       ENTITY_ID = tag 0x27 instance id (2 Bugs, 3 Taz), ANIM = anim id (asset role, the
                       state-record u16 @2), FRAME = displayed frame index (frames 0..FRAME applied)
       tb_anim.py --selftest
"""
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_sec1  # noqa: E402
import tb_world  # noqa: E402
from anim import mul, rot_matrix  # noqa: E402  (RotMatrix = Rx*Ry*Rz, same as TB FUN_00404160)

BZE_DIR = "/mnt/1st4TB/BugsBunny/original/tb/Datas/BZE"
ROLE_SKELETON, ROLE_MODEL = 1, 13  # FUN_00469210: asset +0xa == 1 -> rig frame, == 0xd -> actor model


# ---------------------------------------------------------------- format
def plen(w):
    """Payload words of one packet (FUN_00468280 piVar8[2]; FUN_00469320 computes the same sizes)."""
    t, f = w & 0xF00, w & 0xF000
    if t in (0x100, 0xD00):  # coord: rot(0x2000) scale(0x4000) trans(0x8000 full | 0x1000 packed)
        return (3 if f & 0x2000 else 0) + (3 if f & 0x4000 else 0) + (3 if f & 0x8000 else 2 if f & 0x1000 else 0)
    if t == 0xB00:  # s8 deltas, 3 bytes per set flag, rounded up to words
        return (3 * bin(f & 0xE000).count("1") + 1) >> 1
    if t == 0x900:  # actor bbox: 6 x s8 delta + radius | 6 x s16 + radius
        return 4 if f & 0x1000 else 7
    return {0x200: 1, 0x300: 1, 0x400: 4, 0x600: 2, 0x700: 4, 0xC00: 1, 0xE00: 2}.get(t, 0)


def parse(b):
    """-> (res, [(w0, frame_no, [(w, payload words)])]). Asserts the layout invariants (all 13 905 assets)."""
    magic, z1, res, z3, nf = struct.unpack_from("<4BI", b)
    assert (magic, z1, z3) == (0x50, 0, 0) and res in (1, 2, 4, 6) and nf >= 1, "header"
    p, frames = 8, []
    for _ in range(nf):
        w0, npk, fno = struct.unpack_from("<HHI", b, p)
        start, p, pk = p, p + 8, []
        for _ in range(npk):
            w = struct.unpack_from("<H", b, p)[0]
            n = plen(w)
            assert w & 0xF00 in (0x100, 0x300, 0x400, 0x700, 0x800, 0x900, 0xA00, 0xB00, 0xC00, 0xD00, 0xE00), hex(w)
            pk.append((w, struct.unpack_from(f"<{n}H", b, p + 2)))
            p += 2 + 2 * n
        body = p - start - 8
        # w0 is not read by the applier; skeletons (res 1, one frame) store payload bytes + 2, animations words + 2
        assert w0 == (body + 2 if res == 1 else body // 2 + 2), "frame size word"
        frames.append((w0, fno, pk))
    assert p == len(b), f"parsed {p} of {len(b)} bytes"
    return res, frames


def assets(path):
    """(sections, {asset id: dict(role, kind, data, t26)}) for chunk 0x22 (FUN_004347c0)."""
    S, chunks = tb_world.level(path)
    out = {}
    for t, _, tags, sec in chunks:
        if t != 0x22:
            continue
        a = {}
        for tg, pl in tags:
            if tg == 0x27:
                a["role"], a["id"] = struct.unpack("<HH", pl)
            elif tg == 0x26:
                a["t26"] = struct.unpack_from("<H", pl)[0]
            elif tg in (0x24, 0x25, 0x3F):
                off, size = struct.unpack_from("<II", pl)
                a["kind"], a["data"] = tg, S[sec][off:off + size]
        if "id" in a:
            out[a["id"]] = a
    return S, out


def entity(path, iid):
    """Asset list (tag 0x0f, in file order) of the entity whose tag 0x27 instance id is `iid`."""
    for _, kind, t, tags in tb_sec1.walk(tb_sec1.section1(path)):
        if kind == "chunk" and t in (0x07, 0x08, 0x0A, 0x63):
            f = [(tg, pl) for _, tg, pl in tags]
            if any(tg == 0x27 and struct.unpack("<H", pl[:2])[0] == iid for tg, pl in f):
                return [struct.unpack("<H", pl)[0] for tg, pl in f if tg == 0x0F], f
    raise KeyError(iid)


# ---------------------------------------------------------------- rig (FUN_00468280)
class Part:
    def __init__(self):
        self.obj, self.parent = None, 0           # 1-based object index, 1-based parent part (0 = actor root)
        self.hidden, self.flag = True, 0x8008     # node +6 (0x8000 hidden, 8 no object, 4 object)
        self.scale, self.rot, self.trans = [4096] * 3, [0] * 3, [0] * 3  # param block +0x5c (s16)
        self.m = [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]]  # coord.m (Q12)


def s8(x):
    return x - 256 if x & 0x80 else x


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def _rot(p):  # RotMatrix FUN_00404160 then ScaleMatrix FUN_00404710 (column scale) when scale != 1
    r = rot_matrix(*p.rot)
    p.m = [[r[i][j] * 4096 * p.scale[j] / 4096 for j in range(3)] for i in range(3)]


def packed_rot(w, a, b):
    # 0x400/0x700/0xe00: x = a & 0xfff, y = b & 0xfff, z = (w >> 4 & 0xf00) | (a >> 8 & 0xf0) | b >> 12
    return [a & 0xFFF, b & 0xFFF, (w >> 4 & 0xF00) | (a >> 8 & 0xF0) | b >> 12]


def packed_trans(a, b):  # x = s8 lo(a), z = s8 hi(a), y = s16 b
    return [s8(a & 0xFF), s16(b), s8(a >> 8)]


def apply_frame(parts, pk, nobj=None):
    """Apply one frame's packets. Parts are addressed by the packet id = 1-based position in the part list."""
    for w, pl in pk:
        t, f, pid = w & 0xF00, w & 0xF000, w & 0xFF
        if t in (0x800, 0xC00):
            if f & 0x1000 and f & 0xE000 == 0xA000:  # hide: scale 0, matrix zeroed, flag 0x8000
                p = parts[pid - 1]
                p.scale, p.hidden = [0, 0, 0], True
                for i, j in ((0, 0), (0, 1), (1, 1), (1, 2), (2, 2)):  # FUN_00403e90 zeroes m[0,1,4,5,8]
                    p.m[i][j] = 0
            elif f & 0x1000:
                parts.pop(pid - 1)  # FUN_004608a0 + FUN_00460930 (remove)
            elif f >> 13 == 5:
                parts[pid - 1].hidden = False
            else:
                parts.append(Part())  # created at list end; subtype f >> 13 (0 normal, 1 = 0x40 special)
                if f >> 13:
                    parts[-1].flag = (0x8008, 0x40, 0x80, 0x100, 0x200)[f >> 13]
            if t == 0xC00:  # + object (lo byte) + parent (hi byte) on the new/selected part
                p = parts[-1] if not f & 0x1000 else parts[pid - 1]
                o, par = pl[0] & 0xFF, pl[0] >> 8
                p.obj, p.flag = (o or None), (4 if o else 8)
                p.hidden = False
                p.parent = 0 if par in (0, 0xFF) else par
            continue
        if t in (0x900, 0xA00):
            continue  # actor bbox/radius (+0x60..+0x6c, +0x4a) / part position probe (+0x7c, root motion)
        p = parts[pid - 1]
        if t == 0x300:
            p.parent = 0 if pl[0] in (0, 0xFFFF) else pl[0]
        elif t in (0x100, 0xD00):
            p.hidden, q = False, 0
            if f & 0x2000:
                p.rot, q = [s16(x) for x in pl[0:3]], 3
            elif t == 0xD00:
                p.rot = [0, 0, 0]
            if f & 0x4000:
                p.scale, q = [s16(x) for x in pl[q:q + 3]], q + 3
            elif t == 0xD00:
                p.scale = [4096] * 3
            if f & 0x6000 or t == 0xD00:
                _rot(p)
            if f & 0x8000:
                p.trans = [s16(x) for x in pl[q:q + 3]]
            elif f & 0x1000:
                p.trans = packed_trans(*pl[q:q + 2])
            elif t == 0xD00:
                p.trans = [0, 0, 0]
        elif t in (0x400, 0x700):
            if t == 0x400:
                p.hidden, p.scale = False, [4096] * 3
            p.rot = packed_rot(w, pl[0], pl[1])
            _rot(p)
            p.trans = packed_trans(pl[2], pl[3])
        elif t == 0xE00:
            p.rot = packed_rot(w, pl[0], pl[1])
            _rot(p)
        elif t == 0xB00:  # signed byte deltas, read sequentially across the payload words
            bs = [s8(x) for v in pl for x in (v & 0xFF, v >> 8)]
            p.hidden, q = False, 0
            for bit, attr in ((0x2000, "rot"), (0x4000, "scale")):
                if f & bit:
                    setattr(p, attr, [a + d for a, d in zip(getattr(p, attr), bs[q:q + 3])])
                    q += 3
            if f & 0x6000:
                _rot(p)
            if f & 0x8000:
                p.trans = [a + d for a, d in zip(p.trans, bs[q:q + 3])]
        if nobj is not None:
            assert p.obj is None or p.obj <= nobj


def world(parts):
    """GsGetLw FUN_00405820 / CompMatrixLV FUN_00404ae0: workm = parent.workm (x) coord; root = actor coord."""
    out = {}

    def get(i):
        if i not in out:
            p = parts[i - 1]
            m, t = [[x / 4096 for x in r] for r in p.m], list(p.trans)
            if p.parent:
                pm, pt = get(p.parent)
                m, t = mul(pm, m), [sum(pm[a][k] * t[k] for k in range(3)) + pt[a] for a in range(3)]
            out[i] = (m, t)
        return out[i]
    for i in range(1, len(parts) + 1):
        get(i)
    return out


# ---------------------------------------------------------------- model + pose (FUN_00469aa0 / FUN_00408d00)
def model_layout(m):
    """Objects [(vtx offset, n)], vertices per object (x, y, z, w3), prim records. Vertex word 3: slot
    (& 0x7fff) in the actor's screen buffer, 0x8000 = average with the slot's value (seam vertex)."""
    nobj, ph = struct.unpack_from("<HI", m, 2)
    objs = [struct.unpack_from("<II", m, 8 + 8 * i) for i in range(nobj)]
    V = [[struct.unpack_from("<3hH", m, 8 + o + 8 * i) for i in range(n)] for o, n in objs]
    nC, nD, nA, nB, oB, oC, oD = struct.unpack_from("<HHBBHHH", m, ph)
    prims = []
    for base, cnt, size, n in ((0xC, nA, 0x24, 3), (oB, nB, 0x24, 4), (oC, nC, 0x28, 3), (oD, nD, 0x28, 4)):
        for i in range(cnt):
            r = m[ph + base + size * i:ph + base + size * (i + 1)]
            idx = struct.unpack_from("<4H", r, 0x20)[:n] if r[1] & 0x80 else tuple(r[0x20:0x20 + n])
            uv = tb_world.NO_UV[:n] if r[1] & 0x40 else [(r[o], r[o + 1]) for o in (0x14, 0x18, 0x1C, 0x1E)][:n]
            col = [tuple(r[4 + 4 * (k if r[0] & 0x10 else 0):][:3]) for k in range(n)]  # b0 bit 4 Gouraud
            prims.append((idx, uv, struct.unpack_from("<H", r, 2)[0] & 0xFFF, r[0], r[1], col, r[0x1A] >> 5 & 3))
    return objs, V, prims


def slot_check(m):
    """Word-3 invariants -> (kind, vertices, slots). 'slotted': non-seam vertices take slots 0..n-1 in table
    order, except whole objects whose word 3 is all 0 ('unslotted' tails, no prim uses them); seam slots and
    prim indices < n. 'zero': every word 3 is 0 and prims index the concatenated tables (37 models)."""
    objs, V, prims = model_layout(m)
    flat = [v[3] for o in V for v in o]
    pmax = max((i for idx, *_ in prims for i in idx), default=-1)
    if len(flat) > 1 and not any(flat):
        assert pmax < len(flat), "prim index"
        return "zero", len(flat), 0
    own, k = [], 0
    for o in V:
        if k and len(o) > 0 and not any(v[3] for v in o):
            continue  # unslotted object
        own += [v[3] for v in o if not v[3] & 0x8000]
        k += 1
    assert own == list(range(len(own))), "slot order"
    assert all(w & 0x7FFF < len(own) for w in flat), "seam slot"
    assert pmax < len(own), "prim index"
    return "slotted", len(flat), len(own)


def pose_slots(m, parts):
    objs, V, prims = model_layout(m)
    mats, buf = world(parts), {}
    for i, p in enumerate(parts, 1):
        if p.hidden or p.obj is None:
            continue
        mm, t = mats[i]
        if not any(any(r) for r in mm):
            continue  # GsGetLw returns 0 for an all-zero workm -> not drawn
        for x, y, z, w in V[p.obj - 1]:
            v = [mm[a][0] * x + mm[a][1] * y + mm[a][2] * z + t[a] for a in range(3)]
            s = w & 0x7FFF
            buf[s] = [(a + b) * 0.5 for a, b in zip(buf[s], v)] if w & 0x8000 and s in buf else v
    return buf, prims


def rig_for(path, iid):
    S, A = assets(path)
    ids, _ = entity(path, iid)
    by_role = {A[i]["role"]: A[i] for i in reversed(ids) if i in A}  # first link with the role wins (list walk)
    return S, A, by_role


def build(by_role, anim_id, frame):
    m = by_role[ROLE_MODEL]["data"]
    nobj = struct.unpack_from("<H", m, 2)[0]
    parts = []
    apply_frame(parts, parse(by_role[ROLE_SKELETON]["data"])[1][0][2], nobj)
    res, frames = parse(by_role[anim_id]["data"])
    for k in range(frame + 1):
        apply_frame(parts, frames[k][2], nobj)
    return m, parts, res, frames


# ---------------------------------------------------------------- software raster (numpy z-buffer)
def textures(path):
    from export_level import decode_tim
    import numpy as np
    S, chunks = tb_world.level(path)
    out = {}
    for t, _, tags, sec in chunks:
        if t == 0x28:
            for tg, pl in tags:
                off, packed = struct.unpack("<II", pl)
                out[packed & 0xFFF] = np.asarray(decode_tim(S[sec], off)[0], dtype=np.float32)
    return out


def render(buf, prims, tex, view, size=360):
    import numpy as np
    W = H = size
    # (screen x, screen y, depth); actors face +Z in rest pose, PS1 axes (+Y down)
    if view == "front":   # camera on +Z looking along -Z: screen right = -X
        P = {s: (-v[0], v[1], -v[2]) for s, v in buf.items()}
    else:                 # camera on +X looking along -X: screen right = +Z
        P = {s: (v[2], v[1], -v[0]) for s, v in buf.items()}
    pts = np.array(list(P.values()), dtype=np.float64)
    lo, hi = pts.min(0), pts.max(0)
    sc = 0.9 * size / max(hi[0] - lo[0], hi[1] - lo[1])
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    img = np.full((H, W, 3), 40.0)
    zb = np.full((H, W), np.inf)
    for idx, uv, tid, b0, b1, vcol, abr in sorted(prims, key=lambda p: p[3] & 2):  # opaque first
        if not all(i in P for i in idx):
            continue
        order = [(0, 1, 2)] if len(idx) == 3 else [(0, 1, 2), (1, 3, 2)]  # PS1 Z-order quad
        T = tex.get(tid)
        for tri in order:
            q = np.array([P[idx[k]] for k in tri])
            sx = (q[:, 0] - cx) * sc + W / 2
            sy = (q[:, 1] - cy) * sc + H / 2   # +Y down: screen y grows with Y
            x0, x1 = int(max(sx.min(), 0)), int(min(sx.max() + 1, W - 1))
            y0, y1 = int(max(sy.min(), 0)), int(min(sy.max() + 1, H - 1))
            den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
            if x1 < x0 or y1 < y0 or abs(den) < 1e-9:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            l0 = ((sy[1] - sy[2]) * (gx - sx[2]) + (sx[2] - sx[1]) * (gy - sy[2])) / den
            l1 = ((sy[2] - sy[0]) * (gx - sx[2]) + (sx[0] - sx[2]) * (gy - sy[2])) / den
            l2 = 1 - l0 - l1
            ins = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            z = l0 * q[0, 2] + l1 * q[1, 2] + l2 * q[2, 2]
            zs = zb[y0:y1 + 1, x0:x1 + 1]
            n = np.cross(q[1] - q[0], q[2] - q[0])
            shade = 0.55 + 0.45 * abs(n[2]) / (np.linalg.norm(n) + 1e-9)
            if T is not None:
                th, tw = T.shape[:2]
                u = (l0 * uv[tri[0]][0] + l1 * uv[tri[1]][0] + l2 * uv[tri[2]][0]) / 255 * (tw - 1)
                v = (l0 * uv[tri[0]][1] + l1 * uv[tri[1]][1] + l2 * uv[tri[2]][1]) / 255 * (th - 1)
                c = T[np.clip(v.round().astype(int), 0, th - 1), np.clip(u.round().astype(int), 0, tw - 1)]
                ins &= c[..., 3] > 0  # alpha 0 = transparent texel
                col = c[..., :3]
            else:
                col = np.full(gx.shape + (3,), 180.0)
            vc = np.array([vcol[k] for k in tri], dtype=np.float64)  # PS1 modulation: tex * col / 128
            col = col * (l0[..., None] * vc[0] + l1[..., None] * vc[1] + l2[..., None] * vc[2]) / 128
            m = ins & (z < zs)
            if b0 & 2:  # semi-transparent, PS1 ABR from tpage bits 5-6; no z write
                bg = img[y0:y1 + 1, x0:x1 + 1][m]
                f = col[m] * shade
                img[y0:y1 + 1, x0:x1 + 1][m] = (0.5 * bg + 0.5 * f, bg + f, bg - f, bg + 0.25 * f)[abr]
                continue
            zs[m] = z[m]
            img[y0:y1 + 1, x0:x1 + 1][m] = col[m] * shade
    return img.clip(0, 255).astype("uint8")


def pose_cmd(level, iid, anim_id, frame, out, views="both"):
    import numpy as np
    from PIL import Image
    S, A, by_role = rig_for(level, iid)
    m, parts, res, frames = build(by_role, anim_id, frame)
    buf, prims = pose_slots(m, parts)
    tex = textures(level)
    vs = ["front", "side"] if views == "both" else [views]
    Image.fromarray(np.hstack([render(buf, prims, tex, v) for v in vs])).save(out)
    print(f"{out}: entity {iid} anim {anim_id} frame {frame}/{len(frames) - 1} res {res} "
          f"({max(res >> 1, 1)} ticks/frame), {len(parts)} parts, {len(buf)} slots, {len(prims)} prims")


# ---------------------------------------------------------------- stats over all levels
def stats(bze_dir):
    st, res_h, types, roles = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    fails = []
    for path in sorted(glob.glob(os.path.join(bze_dir, "*.bze"))):
        S, A = assets(path)
        st["levels"] += 1
        parsed = {}
        for aid, a in A.items():
            if a.get("kind") == 0x25:
                res, frames = parse(a["data"])  # asserts parse-to-end + header/frame invariants
                parsed[aid] = frames
                st["anims"] += 1
                st["frames"] += len(frames)
                st["last frame empty"] += not frames[-1][2]
                res_h[res] += 1
                st["res-1 one-frame (rig) assets"] += res == 1 and len(frames) == 1
                if a["role"] == ROLE_SKELETON:
                    st["role-1 assets"] += 1
                    st["role-1 assets that are res-1 one-frame"] += res == 1 and len(frames) == 1
                roles[a["role"]] += 1
                for _, _, pk in frames:
                    for w, _ in pk:
                        types[w & 0xF00] += 1
                st["t26"] += "t26" in a
            elif a.get("kind") == 0x24 and not tb_world.is_octree(a["data"]) and struct.unpack_from("<H", a["data"], 2)[0]:
                st["actor models word3 " + slot_check(a["data"])[0]] += 1
        # rig every entity that owns a role-1 skeleton + role-13 model, play all its animations to the end
        for _, kind, t, tags in tb_sec1.walk(S[1]):
            if kind != "chunk" or t not in (0x07, 0x08, 0x0A, 0x63):
                continue
            ids = [struct.unpack("<H", pl)[0] for _, tg, pl in tags if tg == 0x0F]
            by_role = {A[i]["role"]: (i, A[i]) for i in reversed(ids) if i in A}
            if ROLE_SKELETON not in by_role or ROLE_MODEL not in by_role:
                continue
            m = by_role[ROLE_MODEL][1]["data"]
            nobj = struct.unpack_from("<H", m, 2)[0]
            if not nobj:
                st["rigs with empty (8-byte) model, skipped"] += 1
                continue
            sk = parsed[by_role[ROLE_SKELETON][0]]
            st["rigged entities"] += 1
            for aid in {i for i in ids if i in parsed and A[i]["role"] != ROLE_SKELETON}:
                parts = []
                try:
                    apply_frame(parts, sk[0][2], nobj)
                    for _, _, pk in parsed[aid]:
                        apply_frame(parts, pk, nobj)  # IndexError = packet id beyond the part list
                    st["anim plays validated"] += 1
                except (AssertionError, IndexError) as e:
                    fails.append((os.path.basename(path), t, aid, A[aid]["role"], nobj, type(e).__name__))
    print(dict(st))
    print("rig failures:", len(fails), fails[:20])
    print("resolution (ticks/frame = res>>1):", dict(res_h))
    print("packet types:", {hex(k): v for k, v in sorted(types.items())})
    print("distinct anim roles:", len(roles), "top:", roles.most_common(10))
    assert st["levels"] == 58 and st["anims"] == 13905 and not fails
    print("stats ok")


def selftest():
    # packed rotation: header flags carry rz bits 8-11, a bits 12-15 -> rz bits 4-7, b bits 12-15 -> rz bits 0-3
    assert packed_rot(0x5400, 0xA123, 0xC456) == [0x123, 0x456, 0x5AC]
    assert packed_trans(0xFE05, 0xFFF0) == [5, -16, -2]
    assert plen(0x0D01) == 0 and plen(0x1D06) == 2 and plen(0xE100) == 9 and plen(0xA000 | 0xB00) == 3
    assert plen(0x2B00) == 2 and plen(0x0900) == 7 and plen(0x1900) == 4
    # synthetic rig: root null part, child with object 1 under it, rotated 90 deg about Z then translated
    parts = []
    apply_frame(parts, [(0x0801, ()), (0x0C02, (0x0101,))])
    assert len(parts) == 2 and parts[1].obj == 1 and parts[1].parent == 1 and parts[0].hidden
    apply_frame(parts, [(0x0D01, ()), (0xA102, (0, 0, 1024, 0, 0, 0))])
    apply_frame(parts, [(0x1D01, (0x0005, 0x0007))])
    mm, t = world(parts)[2]
    v = [sum(mm[a][k] * (100, 0, 0)[k] for k in range(3)) + t[a] for a in range(3)]
    assert [round(x) for x in v] == [5, 107, 0], v  # Rz(90): (100,0,0) -> (0,100,0); + parent trans (5,7,0)
    apply_frame(parts, [(0x2B02, (0x00FE, 0x0003))])  # rot deltas bytes FE 00 03 -> (-2, 0, +3)
    assert parts[1].rot == [-2, 0, 1027]
    if os.path.isdir(BZE_DIR):
        S, A, by_role = rig_for(os.path.join(BZE_DIR, "ARCA01.bze"), 2)
        m, parts, res, frames = build(by_role, 50, 0)
        buf, prims = pose_slots(m, parts)
        assert 200 < len(buf) <= slot_check(m)[2]  # hidden alternative parts leave slots unwritten
    print("selftest ok")


def main(argv):
    if len(argv) < 2 or argv[1] == "--selftest":
        selftest()
    elif argv[1] == "stats":
        stats(argv[2] if len(argv) > 2 else BZE_DIR)
    elif argv[1] == "pose":
        pose_cmd(argv[2], int(argv[3], 0), int(argv[4], 0), int(argv[5], 0), argv[6], argv[7] if len(argv) > 7 else "both")
    else:
        print(__doc__)


if __name__ == "__main__":
    main(sys.argv)
