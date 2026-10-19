#!/usr/bin/env python3
"""Prototype exporter: LiT level -> textures (PNG) + model geometry (OBJ/MTL); TB level -> textures only.

Specs: docs/formats/TEXTURES.md, docs/formats/MODELS.md. Research use only; output is
game-derived and must stay outside the repo.
    export_level.py LEVEL.bze OUTDIR
Needs Pillow (PNG). Writes OUTDIR/tex/<id>.png, OUTDIR/atlas.png, OUTDIR/world.obj+mtl,
OUTDIR/models/<sec4-offset>.obj, OUTDIR/world_top.png (top-down wireframe), summary.txt.
"""
import argparse
import math
import os
import struct
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
import bze  # noqa: E402
import sec1  # noqa: E402
import tmd  # noqa: E402


# ---------------------------------------------------------------- textures (PS1 TIM)
def bgr555_to_rgba(c):
    # FUN_00420d10: 0x0000 -> transparent, everything else opaque in the ABR-less (mode 4) variant
    return ((c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3, 0 if c == 0 else 255)


def decode_tim(d, p):
    """Return (Image RGBA, info dict). Supports 4/8/16-bit TIMs (the game rejects mode 3)."""
    ident, flags = struct.unpack_from("<II", d, p)
    assert ident == 0x10, f"no TIM at {p:#x}"
    mode = flags & 7
    q = p + 8
    clut = None
    info = dict(mode=mode)
    if flags & 8:
        bnum, cx, cy, cw, ch = struct.unpack_from("<I4H", d, q)
        clut = [bgr555_to_rgba(c) for c in struct.unpack_from(f"<{cw}H", d, q + 12)]  # row 0 only
        info.update(clut_xy=(cx, cy), clut_wh=(cw, ch))
        q += bnum
    bnum, ix, iy, iw, ih = struct.unpack_from("<I4H", d, q)
    raw = d[q + 12:q + 12 + iw * ih * 2]
    info.update(img_xy=(ix, iy), img_wh16=(iw, ih), end=q + bnum)
    if mode == 0:
        w = iw * 4
        px = [clut[b >> s & 15] for b in raw for s in (0, 4)]
    elif mode == 1:
        w = iw * 2
        px = [clut[b] for b in raw]
    elif mode == 2:
        w = iw
        px = [bgr555_to_rgba(c) for c in struct.unpack(f"<{iw * ih}H", raw)]
    else:
        raise ValueError(f"unsupported TIM mode {mode}")
    im = Image.new("RGBA", (w, ih))
    im.putdata(px)
    info["size"] = (w, ih)
    return im, info


# ---------------------------------------------------------------- geometry
# per primitive type: (vertex-index byte offsets, uv byte offsets or None, tex-id offset or None)
# Offsets from the descriptor tables at 0x4692b8.., 0x469510.., 0x469920.. (see MODELS.md).
POLY = {
    0x34: ((20, 22, 14), (4, 8, 12), 6),
    0x38: ((20, 22, 24, 26), (4, 8, 12, 14), 6),
    0x3c: ((28, 30, 14), (4, 8, 12), 6),
    0x40: ((32, 34, 36, 38), (4, 8, 12, 14), 6),
    0x48: ((16, 18, 20), None, None),
    0x4a: ((20, 22, 24), None, 4),  # textured, implicit UVs (emitter defaults), tex id at +4
    0x4c: ((20, 22, 24, 26), None, None),
    0x4e: ((24, 26, 28, 30), None, 4),
}


def read_verts(d, p, n, stride):
    return [struct.unpack_from("<3f", d, p + stride * i) for i in range(n)]


def model_faces(d, base, stats):
    """Yield (positions, uvs or None, tex_id or None, flags) per polygon of the model at `base`.
    Quads are emitted in PS1 'Z' vertex order v0,v1,v2,v3 -> outline v0,v1,v3,v2.
    Indices address the concatenation of all object vertex tables (multi-object actor models
    index across objects), or the most recent 0x44 block inside world cells."""
    objs = list(tmd.objects(d, base))
    objv = [v for o in objs for v in read_verts(d, o["vert"], o["nvert"], 16)]
    for o in objs:
        cur = objv
        for p, t, cnt, stride in tmd.groups(d, o["prim"], o["nprim"]):
            stats[t] = stats.get(t, 0) + cnt
            if t == 0x44:
                cur = read_verts(d, p + 4, cnt, 12)
                continue
            if t not in POLY:
                continue
            idx_off, uv_off, tex_off = POLY[t]
            for r in range(cnt):
                q = p + r * stride
                idx = [struct.unpack_from("<H", d, q + k)[0] for k in idx_off]
                if max(idx) >= len(cur):
                    stats["bad_index"] = stats.get("bad_index", 0) + 1
                    continue
                pos = [cur[i] for i in idx]
                uv = [(d[q + k], d[q + k + 1]) for k in uv_off] if uv_off else None
                tex = struct.unpack_from("<H", d, q + tex_off)[0] if tex_off is not None else None
                yield pos, uv, tex, d[q + 2]


def transform(pos, tr, scale):
    # ponytail: rotation ignored (all LiT world chunks use 0x1000 = 360 deg); add RotMatrix if needed
    return tuple(pos[i] * scale + tr[i] for i in range(3))


def write_obj(path, faces, mtl_name=None, texsizes=None):
    vs, vts, lines = [], [], []
    cur_mtl = None
    for pos, uv, tex, flags in faces:
        m = f"tex{tex}" if tex is not None else "untex"
        if m != cur_mtl:
            lines.append(f"usemtl {m}")
            cur_mtl = m
        order = [0, 1, 3, 2] if len(pos) == 4 else [0, 1, 2]
        f = []
        for k in order:
            x, y, z = pos[k]
            vs.append(f"v {x:g} {-y:g} {z:g}")  # PS1 +Y is down -> flip for OBJ
            if uv:
                u, v = uv[k]
                vts.append(f"vt {u / 255:.5f} {1 - v / 255:.5f}")  # UV byte / 255 over the whole TIM
                f.append(f"{len(vs)}/{len(vts)}")
            else:
                f.append(f"{len(vs)}")
        lines.append("f " + " ".join(f))
    with open(path, "w") as fp:
        if mtl_name:
            fp.write(f"mtllib {mtl_name}\n")
        fp.write("\n".join(vs + vts + lines) + "\n")
    return len(vs)


# ---------------------------------------------------------------- level driver
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bze")
    ap.add_argument("out")
    a = ap.parse_args()
    kind, secs = bze.load(a.bze)
    by_id = {sid: data for sid, data in secs}
    s1, s3, s4 = by_id[1], by_id.get(3, b""), by_id.get(4, b"")
    os.makedirs(os.path.join(a.out, "tex"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "models"), exist_ok=True)
    summary = [f"{a.bze}: {kind}, sections {[sid for sid, _ in secs]}"]

    tex_refs, world, actors = [], [], []
    if kind == "tb":
        # ponytail: TB tag lengths are not tabulated yet; locate the texture chunk '2c 28' + tags '29'
        # (bblit2 FUN_004347c0) by pattern. Geometry uses another format (MODELS.md §10) -> textures only.
        p = s1.find(b"\x2c\x28\x29") + 2
        while p > 1 and s1[p] == 0x29:
            o, v = struct.unpack_from("<II", s1, p + 1)
            tex_refs.append((o, v & 0xfff))
            p += 9
    for off, k, t, tags in (sec1.walk(s1) if kind == "lit" else []):
        if k.startswith("unknown"):
            summary.append(f"sec1 walk stopped: {k} at {off:#x}")
            break
        if t == 0x29:
            tex_refs += [struct.unpack("<II", pl) for tg, pl in tags if tg == 0x2a]
        elif t == 0x20:
            d = dict(tags)
            o, n = struct.unpack("<II", d[0x24])
            tr = struct.unpack("<3i", d.get(0x10, bytes(12)))
            sc = struct.unpack("<i", d.get(0x12, b"\0\x10\0\0")[:4])[0] / 4096
            rot = struct.unpack("<3h", d.get(0x11, b"\0\x10" * 3))
            world.append((off, o, n, tr, sc, rot))
        elif t == 0x22:
            for tg, pl in tags:
                if tg in (0x24, 0x40):
                    actors.append((off, tg) + struct.unpack("<II", pl))

    # textures
    texsizes, thumbs, modes = {}, [], {}
    for o, tid in tex_refs:
        try:
            im, info = decode_tim(s3, o)
        except ValueError as e:  # mode 3 (24-bit): FUN_00420d10 rejects it too
            summary.append(f"texture {tid} @sec3+{o:#x}: {e}")
            continue
        key = (info["mode"], info.get("clut_wh"))
        modes[key] = modes.get(key, 0) + 1
        texsizes[tid] = im.size
        im.save(os.path.join(a.out, "tex", f"{tid:04d}.png"))
        thumbs.append((tid, im))
    summary.append(f"textures: {len(tex_refs)} TIM refs, (mode, clut w/h) histogram: {modes}")
    if thumbs:
        cols, cell = 24, 66
        rows = math.ceil(len(thumbs) / cols)
        atlas = Image.new("RGBA", (cols * cell, rows * cell), (40, 40, 40, 255))
        for i, (tid, im) in enumerate(thumbs):
            t = im.copy()
            t.thumbnail((64, 64))
            atlas.alpha_composite(t, ((i % cols) * cell + 1, (i // cols) * cell + 1))
        atlas.save(os.path.join(a.out, "atlas.png"))

    with open(os.path.join(a.out, "world.mtl"), "w") as fp:
        fp.write("newmtl untex\nKd 0.7 0.7 0.7\n")
        for tid in sorted(texsizes):
            fp.write(f"newmtl tex{tid}\nKd 1 1 1\nmap_Kd tex/{tid:04d}.png\nmap_d tex/{tid:04d}.png\n")

    # world geometry (chunk 0x20)
    stats, faces = {}, []
    for off, o, n, tr, sc, rot in world:
        for pos, uv, tex, fl in model_faces(s4, o, stats):
            faces.append(([transform(v, tr, sc) for v in pos], uv, tex, fl))
        summary.append(f"world chunk @sec1+{off:#x}: sec4 {o:#x}+{n:#x} pos={tr} scale={sc} rot={rot}")
    if faces:
        nv = write_obj(os.path.join(a.out, "world.obj"), faces, "world.mtl", texsizes)
        missing = sorted({f[2] for f in faces if f[2] is not None and f[2] not in texsizes})
        xs, ys, zs = ([v[i] for f in faces for v in f[0]] for i in range(3))
        summary.append(f"world: {len(faces)} faces, {nv} obj verts, prim counts {{{', '.join(f'{k:#x}: {c}' if isinstance(k, int) else f'{k}: {c}' for k, c in stats.items())}}}")
        summary.append(f"  bounds x[{min(xs):g},{max(xs):g}] y[{min(ys):g},{max(ys):g}] z[{min(zs):g},{max(zs):g}]")
        summary.append(f"  face tex ids missing from section-3 TIM list: {missing}")
        fl = {}
        for f in faces:
            fl[f[3]] = fl.get(f[3], 0) + 1
        summary.append(f"  prim flag byte (+2) histogram: { {hex(k): v for k, v in sorted(fl.items())} }")
        # top-down wireframe (x right, z up)
        W = 1600
        sx = (W - 20) / max(max(xs) - min(xs), max(zs) - min(zs), 1)
        img = Image.new("RGB", (W, W), (0, 0, 0))
        dr = ImageDraw.Draw(img)
        for pos, uv, tex, _ in faces:
            order = [0, 1, 3, 2] if len(pos) == 4 else [0, 1, 2]
            pts = [(10 + (pos[k][0] - min(xs)) * sx, W - 10 - (pos[k][2] - min(zs)) * sx) for k in order]
            shade = int(255 - (pos[0][1] - min(ys)) / max(1, max(ys) - min(ys)) * 200)
            dr.polygon(pts, outline=(shade, shade, 80))
        img.save(os.path.join(a.out, "world_top.png"))

    # actor / prop models (chunk 0x22)
    astats, done = {}, set()
    for off, tg, o, n in actors:
        if o in done:
            continue
        done.add(o)
        try:
            fs = list(model_faces(s4, o, astats))
        except (AssertionError, ValueError, struct.error) as e:
            summary.append(f"actor model sec4 {o:#x} (tag {tg:#x}) failed: {e}")
            continue
        write_obj(os.path.join(a.out, "models", f"{o:06x}.obj"), fs, "../world.mtl")
    summary.append(f"actor models: {len(done)} unique, prim histogram {astats}")
    open(os.path.join(a.out, "summary.txt"), "w").write("\n".join(summary) + "\n")
    print("\n".join(summary))


if __name__ == "__main__":
    main()
