#!/usr/bin/env python3
"""Prototype LiT actor-animation probe (chunk 0x22 tags 0x25 / 0x3f / 0x28 / 0x0d). Spec: notes AnimRE.md.
Tag-0x25 blocks are Sony TOD (id 0x50) animations, played by FUN_004393e0 / applied by FUN_0044e400.
    anim.py --selftest
    anim.py stats BZE_DIR                    -> parse every tag 0x25/0x3f block of every LiT BZE, print stats
    anim.py dump LEVEL.bze ASSET_ID          -> print the TOD frames/packets of one asset
    anim.py pose LEVEL.bze ACTOR_SEC1_OFF ANIM_ID FRAME OUTDIR
                                             -> skeleton (role 4) + FRAME frames of ANIM_ID -> OBJ (+PNG)
Research use only; output is game-derived and must stay outside the repo."""
import collections
import glob
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import bze  # noqa: E402
import sec1  # noqa: E402
import tmd  # noqa: E402

# TOD packet types handled by FUN_0044e400 (word bits 16..19); 4/5/7 never occur in LiT data
PKT = {0: "attr", 1: "coord", 2: "tmd_id", 3: "parent", 6: "light?", 8: "objctl", 9: "bbox", 0xa: "attach"}


def parse_tod(d, o):
    """Return (resolution, [frames]); frame = (frame_no, [(part_id, type, flag, payload_bytes)])."""
    ident, ver, res, nf = struct.unpack_from("<BBHI", d, o)
    assert ident == 0x50 and ver == 0, f"not a TOD at {o:#x}"
    p, frames = o + 8, []
    for _ in range(nf):
        size, npk, fno = struct.unpack_from("<HHI", d, p)
        q, pk = p + 8, []
        for _ in range(npk):
            w = struct.unpack_from("<I", d, q)[0]
            n = w >> 24
            assert n >= 1, f"zero-length packet at {q:#x}"
            pk.append((w & 0xffff, w >> 16 & 15, w >> 20 & 15, d[q + 4:q + 4 * n]))
            q += 4 * n
        assert q - p == size * 4, f"frame size mismatch at {p:#x}"
        frames.append((fno, pk))
        p = q
    return res, frames


def level(path):
    """Return (sec4, assets{id: dict}, actors[(sec1_off, tags{tag: [payload]})]) of a LiT BZE."""
    kind, secs = bze.load(path)
    assert kind == "lit", path
    by = dict(secs)
    assets, actors = {}, []
    for off, k, t, tags in sec1.walk(by[1]):
        if k.startswith("unknown"):
            raise ValueError(f"{path}: {k}")
        if t == 0x22:
            a = dict(snd=[], evt=[])
            for tg, pl in tags:
                if tg == 0x27:
                    a["role"], a["id"] = struct.unpack("<HH", pl)
                elif tg in (0x24, 0x40, 0x25):
                    a["tag"], (a["off"], a["size"]) = tg, struct.unpack("<II", pl)
                elif tg == 0x3f:  # {u16 frames, u16 pad, u32 off, u32 size} -> {u8 sprite, u8 ticks}[frames]
                    a["tag"], a["frames"], _, a["off"], a["size"] = tg, *struct.unpack("<HHII", pl)
                elif tg == 0x28:  # sound event {u8 sound, u8 frame, u8 every-nth-pass}
                    a["snd"].append(tuple(pl))
                elif tg == 0x0d:  # logic event; bytes 6..11 = TestLogicCond/RunLogicAction record
                    a["evt"].append(struct.unpack("<HHh6B", pl))
            assets[a["id"]] = a
        elif t in (7, 8, 10):
            d = collections.defaultdict(list)
            for tg, pl in tags:
                d[tg].append(pl)
            actors.append((off, d))
    return by.get(4, b""), assets, actors


# ---------------------------------------------------------------- pose (FUN_0044e400 + FUN_0040a050 + FUN_00411350)
def rot_matrix(rx, ry, rz):
    """RotMatrix 0x407a50: M = Rx*Ry*Rz, angles 4.12 (0x1000 = 360 deg)."""
    def r(a):
        a *= 2 * math.pi / 4096
        return math.cos(a), math.sin(a)
    cx, sx = r(rx)
    cy, sy = r(ry)
    cz, sz = r(rz)
    X = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Y = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Z = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    return mul(X, mul(Y, Z))


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def apply(m, t, v):
    return [sum(m[i][k] * v[k] for k in range(3)) + t[i] for i in range(3)]


class Part:
    def __init__(self, pid):
        self.id, self.obj, self.parent = pid, None, 0
        self.rot, self.scale, self.trans = (0, 0, 0), (4096, 4096, 4096), (0, 0, 0)


def apply_frame(parts, pk):
    """One TOD frame onto the part list (list order = creation order; ids are 1-based list positions)."""
    for pid, typ, flag, pl in pk:
        if typ == 8:
            if flag & 1 == 0 and flag >> 1 == 0:  # create (subtypes 1..4 = light/flag nodes, not drawn)
                parts.append(Part(pid))
            elif flag & 1 == 0 and flag >> 1 == 5:
                pass  # clear 'not yet posed' flag 0x8000
            elif flag & 1 and flag >> 1 == 5:  # 0xa00000: zero param.scale + matrix, set 'unposed' 0x8000
                parts[pid - 1].scale = (0, 0, 0)  # invisible until a coord packet carries scale again
            elif flag & 1:  # kill
                parts.pop(pid - 1)
            else:
                parts.append(Part(pid))
            continue
        if pid == 0 or pid > len(parts):
            continue  # actor-level packets (bbox type 9)
        p = parts[pid - 1]
        if typ == 2:
            p.obj = struct.unpack_from("<I", pl)[0] & 0xffff or None  # 1-based TMD object index
        elif typ == 3:
            v = struct.unpack_from("<I", pl)[0]
            p.parent = 0 if v in (0, 0xffff) else v
        elif typ == 1:
            q = 0
            if flag & 2:
                p.rot = struct.unpack_from("<3h", pl, q)
                q += 8
            if flag & 4:
                p.scale = struct.unpack_from("<3h", pl, q)
                q += 8
            if flag & 8:
                p.trans = struct.unpack_from("<3i", pl, q)


def world_matrices(parts):
    """coord = RotMatrix(rot)*diag(scale/4096), t = trans; workm = parent.workm (x) coord (CompMatrixLV)."""
    out = {}

    def get(i):
        if i in out:
            return out[i]
        p = parts[i - 1]
        r = rot_matrix(*p.rot)
        m = [[r[a][b] * p.scale[b] / 4096 for b in range(3)] for a in range(3)]
        t = list(p.trans)
        if p.parent:
            pm, pt = get(p.parent)
            m, t = mul(pm, m), apply(pm, pt, t)
        out[i] = (m, t)
        return m, t
    for i in range(1, len(parts) + 1):
        get(i)
    return out


def pose_mesh(s4, model_off, parts):
    """Return (slot positions, faces[(idx...)]). Vertex word 3: low 15 bits = slot in the actor's shared
    vertex buffer, bit 0x8000 = average with the slot's previous value (FUN_00411350, factor 0.5)."""
    import export_level as el  # noqa: E402  (POLY index table; needs Pillow)
    objs = list(tmd.objects(s4, model_off))
    mats = world_matrices(parts)
    buf, faces = {}, []
    for i, p in enumerate(parts, 1):
        m, t = mats[i]
        if p.obj is None or not any(any(r) for r in m[:2]):  # FUN_0040a050: zero workm rows -> not drawn
            continue
        o = objs[p.obj - 1]
        for j in range(o["nvert"]):
            x, y, z, w = struct.unpack_from("<3fI", s4, o["vert"] + 16 * j)
            v = apply(m, t, (x, y, z))
            s = w & 0x7fff
            buf[s] = [(a + b) * 0.5 for a, b in zip(buf[s], v)] if w & 0x8000 and s in buf else v
        for q, typ, cnt, stride in tmd.groups(s4, o["prim"], o["nprim"]):
            if typ in el.POLY:
                offs = el.POLY[typ][0]
                for r in range(cnt):
                    idx = [struct.unpack_from("<H", s4, q + r * stride + k)[0] for k in offs]
                    faces.append(idx if len(idx) == 3 else [idx[0], idx[1], idx[3], idx[2]])
    return buf, faces


def pose(path, actor_off, anim_id, frame, outdir):
    s4, assets, actors = level(path)
    d = dict(actors)[actor_off]
    mine = [assets[struct.unpack("<H", x)[0]] for x in d[0x0f]]
    by_role = {a["role"]: a for a in mine}
    model, skel, anim = by_role[7], by_role[4], by_role[anim_id]
    parts = []
    apply_frame(parts, parse_tod(s4, skel["off"])[1][0][1])  # 0x4393e0 first call: role-4 frame 0
    res, frames = parse_tod(s4, anim["off"])
    for k in range(frame + 1):
        apply_frame(parts, frames[k][1])
    buf, faces = pose_mesh(s4, model["off"], parts)
    os.makedirs(outdir, exist_ok=True)
    name = os.path.join(outdir, f"{os.path.basename(path)[:-4]}_{actor_off:x}_anim{anim_id:x}_f{frame}")
    order = sorted(buf)
    remap = {s: i + 1 for i, s in enumerate(order)}
    with open(name + ".obj", "w") as fp:
        fp.writelines(f"v {buf[s][0]:g} {-buf[s][1]:g} {buf[s][2]:g}\n" for s in order)  # +Y down -> up
        fp.writelines("f " + " ".join(str(remap[i]) for i in f) + "\n" for f in faces if all(i in buf for i in f))
    print(f"{name}.obj: {len(parts)} parts, {len(buf)} slots, {len(faces)} faces, TOD res {res} "
          f"({max(res >> 1, 1)} tick/frame @30Hz), {len(frames)} frames")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    except ImportError:
        return
    fig = plt.figure(figsize=(10, 5))
    pts = list(buf.values())
    for n, (el_, az) in enumerate(((0, -90), (0, 0))):
        ax = fig.add_subplot(1, 2, n + 1, projection="3d")
        polys = [[(buf[i][0], buf[i][2], -buf[i][1]) for i in f] for f in faces if all(i in buf for i in f)]
        ax.add_collection3d(Poly3DCollection(polys, facecolor="#c8c8e8", edgecolor="#303050", linewidth=0.2))
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        c = [(lo[i] + hi[i]) / 2 for i in range(3)]
        h = max(hi[i] - lo[i] for i in range(3)) / 2
        ax.set_xlim(c[0] - h, c[0] + h)
        ax.set_ylim(c[2] - h, c[2] + h)
        ax.set_zlim(-c[1] - h, -c[1] + h)
        ax.view_init(el_, az)
        ax.set_title(("front", "side")[n])
    fig.savefig(name + ".png", dpi=100)
    print(f"{name}.png")


# ---------------------------------------------------------------- stats over all levels
def stats(bze_dir):
    st = collections.Counter()
    res_h, pk_h, nfr_h, roles = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    for f in sorted(glob.glob(os.path.join(bze_dir, "*.bze"))):
        s4, assets, actors = level(f)
        st["levels"] += 1
        for a in assets.values():
            st[f"tag_{a.get('tag', 0):#x}"] += 1
            st["snd_events"] += len(a["snd"])
            st["logic_events"] += len(a["evt"])
            st["max_snd_per_asset"] = max(st["max_snd_per_asset"], len(a["snd"]))
            st["max_evt_per_asset"] = max(st["max_evt_per_asset"], len(a["evt"]))
            if a.get("tag") == 0x25:
                res, frames = parse_tod(s4, a["off"])
                st["tod"] += 1
                res_h[res] += 1
                nfr_h[min(len(frames), 64)] += 1
                roles[a["role"]] += 1
                st["frames"] += len(frames)
                if [fn for fn, _ in frames] != list(range(len(frames))):
                    st["tod_nonsequential_frame_no"] += 1
                for _, pk in frames:
                    for pid, typ, flag, pl in pk:
                        pk_h[(typ, flag, len(pl))] += 1
                        if typ == 1 and flag & 1:
                            st["coord_differential_flag"] += 1
                if a["role"] == 4 and any(t not in (2, 3, 8) for _, pk in frames for _, t, _, _ in pk):
                    st["role4_with_other_packets"] += 1
            elif a.get("tag") == 0x3f:
                raw = s4[a["off"]:a["off"] + 2 * a["frames"]]
                assert len(raw) == 2 * a["frames"] and a["size"] >= len(raw)
                st["flipbook_frames"] += a["frames"]
        for off, d in actors:
            rl = {assets[struct.unpack("<H", x)[0]]["role"] for x in d.get(0x0f, [])
                  if assets[struct.unpack("<H", x)[0]].get("tag") == 0x25}
            if rl:
                st["actors_with_tod"] += 1
                st["actors_tod_without_role4"] += 4 not in rl
        for a in assets.values():  # vertex-slot invariants of models referenced as role 7
            if a.get("role") == 7 and a.get("tag") == 0x24:
                ws = [struct.unpack_from("<I", s4, o["vert"] + 16 * j + 12)[0]
                      for o in tmd.objects(s4, a["off"]) for j in range(o["nvert"])]
                plain = [w for w in ws if not w & 0x8000]
                st["role7_models"] += 1
                st["role7_slots_not_sequential"] += plain != list(range(len(plain)))
                st["role7_blend_verts"] += len(ws) - len(plain)
                st["role7_blend_slot_out_of_range"] += sum((w & 0x7fff) >= len(plain) for w in ws if w & 0x8000)
    for k in sorted(st):
        print(f"{k}: {st[k]}")
    print("TOD resolution histogram:", dict(sorted(res_h.items())))
    print("frames-per-TOD histogram (64 = >=64):", dict(sorted(nfr_h.items())))
    print("packet (type, flag, payload bytes) histogram:", {f"{t:x}/{fl:x}/{n}": c for (t, fl, n), c in sorted(pk_h.items())})
    print("distinct TOD roles (anim ids):", len(roles), "top:", roles.most_common(12))


def selftest():
    # synthetic TOD: 1 frame, create part 1, bind TMD obj 1, parent root, coord rot+trans
    pk = [struct.pack("<I", 1 | 8 << 16 | 0 << 20 | 1 << 24),
          struct.pack("<II", 1 | 2 << 16 | 2 << 24, 1),
          struct.pack("<II", 1 | 3 << 16 | 2 << 24, 0),
          struct.pack("<I3hH3i", 1 | 1 << 16 | 0xa << 20 | 6 << 24, 0, 0x400, 0, 0, 10, 20, 30)]
    body = b"".join(pk)
    blob = struct.pack("<BBHI", 0x50, 0, 2, 1) + struct.pack("<HHI", 2 + len(body) // 4, len(pk), 0) + body
    res, frames = parse_tod(blob, 0)
    parts = []
    apply_frame(parts, frames[0][1])
    assert res == 2 and len(parts) == 1 and parts[0].obj == 1 and parts[0].trans == (10, 20, 30)
    m, t = world_matrices(parts)[1]
    v = apply(m, t, (100, 0, 0))  # Ry(90 deg): +X -> -Z
    assert all(abs(a - b) < 1e-9 for a, b in zip(v, (10, 20, -70))), v
    print("selftest ok")


def main():
    cmd = sys.argv[1]
    if cmd == "--selftest":
        selftest()
    elif cmd == "stats":
        stats(sys.argv[2])
    elif cmd == "dump":
        s4, assets, _ = level(sys.argv[2])
        a = assets[int(sys.argv[3], 0)]
        print({k: v for k, v in a.items()})
        if a.get("tag") == 0x25:
            res, frames = parse_tod(s4, a["off"])
            print(f"resolution {res}, {len(frames)} frames")
            for fn, pk in frames:
                print(f"frame {fn}: " + " ".join(f"[{pid} {PKT.get(t, t)} f{fl:x} {pl.hex()}]" for pid, t, fl, pl in pk))
    elif cmd == "pose":
        pose(sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0), int(sys.argv[5]), sys.argv[6])


if __name__ == "__main__":
    main()
