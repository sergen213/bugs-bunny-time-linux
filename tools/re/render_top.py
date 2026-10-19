#!/usr/bin/env python3
"""Validation renderer: textured orthographic top-down (or side) view of an exported OBJ, z-buffered.
Checks UV/texture association visually. numpy + Pillow.
    render_top.py EXPORTDIR/world.obj OUT.png [--size 1400] [--view top|front]"""
import argparse
import os

import numpy as np
from PIL import Image


def load_obj(path):
    v, vt, faces, mtl = [], [], [], None
    for line in open(path):
        s = line.split()
        if not s:
            continue
        if s[0] == "v":
            v.append([float(x) for x in s[1:4]])
        elif s[0] == "vt":
            vt.append([float(x) for x in s[1:3]])
        elif s[0] == "usemtl":
            mtl = s[1]
        elif s[0] == "f":
            ids = [p.split("/") for p in s[1:]]
            faces.append((mtl, [int(a[0]) - 1 for a in ids], [int(a[1]) - 1 if len(a) > 1 else -1 for a in ids]))
    return np.array(v), np.array(vt) if vt else np.zeros((0, 2)), faces


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=1400)
    ap.add_argument("--view", default="top")
    a = ap.parse_args()
    V, VT, F = load_obj(a.obj)
    texdir = os.path.join(os.path.dirname(os.path.abspath(a.obj)), "tex")
    if not os.path.isdir(texdir):  # models/<x>.obj -> ../tex
        texdir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(a.obj))), "tex")
    # screen axes: top = (x, -z) depth y (OBJ y is up); front = (x, -y) depth z
    if a.view == "top":
        P = np.stack([V[:, 0], -V[:, 2], V[:, 1]], 1)
    else:
        P = np.stack([V[:, 0], -V[:, 1], -V[:, 2]], 1)
    lo, hi = P[:, :2].min(0), P[:, :2].max(0)
    s = (a.size - 4) / (hi - lo).max()
    P[:, :2] = (P[:, :2] - lo) * s + 2
    W = H = a.size
    img = np.full((H, W, 3), 30, np.uint8)
    zb = np.full((H, W), -np.inf)
    cache = {}
    for mtl, vi, ti in F:
        tris = [(0, 1, 2)] if len(vi) == 3 else [(0, 1, 2), (0, 2, 3)]
        if mtl not in cache:
            p = os.path.join(texdir, f"{int(mtl[3:]):04d}.png") if mtl.startswith("tex") else None
            cache[mtl] = np.asarray(Image.open(p).convert("RGBA")) if p and os.path.exists(p) else None
        tex = cache[mtl]
        for t in tris:
            a_, b_, c_ = (P[vi[k]] for k in t)
            x0, x1 = int(max(min(a_[0], b_[0], c_[0]), 0)), int(min(max(a_[0], b_[0], c_[0]), W - 1))
            y0, y1 = int(max(min(a_[1], b_[1], c_[1]), 0)), int(min(max(a_[1], b_[1], c_[1]), H - 1))
            if x1 < x0 or y1 < y0:
                continue
            ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1] + 0.5
            d = (b_[1] - c_[1]) * (a_[0] - c_[0]) + (c_[0] - b_[0]) * (a_[1] - c_[1])
            if abs(d) < 1e-9:
                continue
            w0 = ((b_[1] - c_[1]) * (xs - c_[0]) + (c_[0] - b_[0]) * (ys - c_[1])) / d
            w1 = ((c_[1] - a_[1]) * (xs - c_[0]) + (a_[0] - c_[0]) * (ys - c_[1])) / d
            w2 = 1 - w0 - w1
            m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not m.any():
                continue
            z = w0 * a_[2] + w1 * b_[2] + w2 * c_[2]
            yy, xx = ys[m].astype(int), xs[m].astype(int)
            zz = z[m]
            if tex is not None and ti[0] >= 0:
                uv = [VT[ti[k]] for k in t]
                u = w0[m] * uv[0][0] + w1[m] * uv[1][0] + w2[m] * uv[2][0]
                v = w0[m] * uv[0][1] + w1[m] * uv[1][1] + w2[m] * uv[2][1]
                th, tw = tex.shape[:2]
                c = tex[np.clip(((1 - v) * (th - 1)).round().astype(int), 0, th - 1),
                        np.clip((u * (tw - 1)).round().astype(int), 0, tw - 1)]
                keep = c[:, 3] > 0
                yy, xx, zz, col = yy[keep], xx[keep], zz[keep], c[keep, :3]
            else:
                col = np.array([[150, 150, 150]] * len(yy), np.uint8)
            vis = zz > zb[yy, xx]
            zb[yy[vis], xx[vis]] = zz[vis]
            img[yy[vis], xx[vis]] = col[vis]
    Image.fromarray(img).save(a.out)


if __name__ == "__main__":
    main()
