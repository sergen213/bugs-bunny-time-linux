#!/usr/bin/env python3
"""Texture-gap resolver (docs/formats/TEXTURE_GAP.md): face texture ids missing from a level's TIM list are
*texture links* owned by an entity (section-1 chunk 7/8/10, tag 0x0b = link id). At run time the entity's
sprite-sequence frames (0x64 records, tex id u16 @+10) are written into the id alias table
(LiT DAT_004e5e80 / TB DAT_004d038c) under the link id; unlinked ids stay 0 and are never drawn.

Usage: texgap.py lit|tb BZEDIR [--check]     per-level table: missing faces (world, actor) / resolved by links
       texgap.py lit|tb LEVEL.bze OUTDIR      contact sheet OUTDIR/<level>_links.png (link id -> frame TIMs)
--check asserts every missing *world* face id resolves to a link whose frames are all loaded TIMs.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bze  # noqa: E402
import sec1  # noqa: E402
import tb_world  # noqa: E402
import tmd  # noqa: E402
from export_level import decode_tim, model_faces  # noqa: E402


def lit(path):
    """Return (sec3, {tex id: sec3 offset}, world face ids Counter, actor face ids Counter,
    {link id: (entity type byte or None, start-frame params tag 0x42 or None, [frame tex ids])})."""
    _, secs = bze.load(path)
    S = dict(secs)
    tex, world, actor, assets, links = {}, collections.Counter(), collections.Counter(), {}, {}
    for _, k, t, tags in sec1.walk(S[1]):
        if k.startswith("unknown"):
            break
        d = dict(tags)
        if t == 0x29:
            tex.update((i, o) for o, i in (struct.unpack("<II", pl) for tg, pl in tags if tg == 0x2a))
        elif t == 0x20:
            world.update(tid for *_, tid, _ in model_faces(S[4], struct.unpack("<I", d[0x24][:4])[0], {})
                         if tid is not None)
        elif t == 0x22 and 0x27 in d:
            aid = struct.unpack_from("<H", d[0x27], 2)[0]  # tag 0x27: u16 class (+0x4e), u16 asset id (+0xc)
            for tg, pl in tags:
                o = struct.unpack("<I", pl[:4])[0] if tg in (0x24, 0x40) else 0
                if tg == 0x40:  # sprite sequence (asset kind 0x100): 0x64 records, tex id @+10
                    assets[aid] = [struct.unpack_from("<H", S[4], p + 16 * r + 10)[0]
                                   for ob in tmd.objects(S[4], o)
                                   for p, ty, n, _ in tmd.groups(S[4], ob["prim"], ob["nprim"]) if ty == 0x64
                                   for r in range(n)]
                elif tg == 0x24:
                    try:
                        actor.update(tid for *_, tid, _ in model_faces(S[4], o, {}) if tid is not None)
                    except (AssertionError, ValueError, struct.error):
                        pass
        elif t in (7, 8, 10):
            _link(tags, assets, links)
    return S[3], tex, world, actor, links


def _link(tags, assets, links):
    # loader case 0x0f keeps the latest kind-0x100 asset; case 0x0b passes it to FUN_004247a0/FUN_00408410
    cur = None
    for tg, pl in tags:
        if tg == 0x0f:
            cur = assets.get(struct.unpack("<H", pl[:2])[0], cur)
        elif tg == 0x0b and cur:
            d = dict(tags)
            links[struct.unpack("<H", pl[:2])[0]] = (d.get(0x13, b"?")[0], d.get(0x42), cur)


def tb(path):
    S, chunks = tb_world.level(path)
    tex, world, actor, assets, links = {}, collections.Counter(), collections.Counter(), {}, {}
    for t, d, tags, sec in chunks:
        if t == 0x28:
            tex.update(((v & 0xfff), (sec, o)) for o, v in (struct.unpack("<II", pl) for _, pl in tags))
        elif t == 0x20:
            off, size = struct.unpack("<II", d[0x24])
            for *_, tid, _, _ in tb_world.octree_faces(S[sec][off:off + size], collections.Counter()):
                world[tid] += 1
        elif t == 0x22 and 0x27 in d:
            aid = struct.unpack_from("<H", d[0x27], 2)[0]
            for tg, pl in tags:
                if tg not in (0x24, 0x3f):
                    continue
                off, size = struct.unpack("<II", pl)
                m = S[sec][off:off + size]
                if tg == 0x3f:  # MODELS.md §10.3: nD 16-byte 0x64 records at prim block + 0xc
                    ph = struct.unpack_from("<I", m, 4)[0]
                    nd = struct.unpack_from("<H", m, ph + 2)[0]
                    assets[aid] = [struct.unpack_from("<H", m, ph + 0xc + 16 * i + 10)[0] & 0xfff for i in range(nd)]
                elif tg == 0x24:
                    try:
                        fs = (tb_world.octree_faces if tb_world.is_octree(m) else tb_world.actor_faces)(m, collections.Counter())
                        for *_, tid, _, _ in fs:
                            actor[tid] += 1
                    except (ValueError, struct.error):
                        pass
        elif t in (7, 8, 10):
            _link(tags, assets, links)
    return S, tex, world, actor, links


def table(game, folder, check):
    tot = collections.Counter()
    print("level  world_missing world_resolved  actor_missing actor_resolved  unresolved_ids")
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(".bze"):
            continue
        try:
            _, tex, world, actor, links = (lit if game == "lit" else tb)(os.path.join(folder, f))
        except (KeyError, AssertionError, ValueError, struct.error) as e:
            print(f, "skipped:", e)
            continue
        mw = {t: c for t, c in world.items() if t not in tex}
        ma = {t: c for t, c in actor.items() if t not in tex}
        rw = sum(c for t, c in mw.items() if t in links)
        ra = sum(c for t, c in ma.items() if t in links)
        if check:
            for t in mw:
                assert t in links and all(fr in tex for fr in links[t][2]), (f, t)
        row = (sum(mw.values()), rw, sum(ma.values()), ra)
        tot.update(dict(zip(("w", "rw", "a", "ra"), row)))
        if row[0] or row[2]:
            print(f, *row, sorted(t for t in set(mw) | set(ma) if t not in links))
    print("total", tot["w"], tot["rw"], tot["a"], tot["ra"])


def sheet(game, path, out):
    from PIL import Image, ImageDraw
    src, tex, world, actor, links = (lit if game == "lit" else tb)(path)
    rows = [(lid, world[lid] + actor[lid], fr) for lid, (_, _, fr) in sorted(links.items()) if lid not in tex]
    cell = 72
    img = Image.new("RGBA", (cell * (1 + max([len(r[2]) for r in rows] + [1])), cell * max(len(rows), 1)), (60, 60, 60, 255))
    draw = ImageDraw.Draw(img)
    for y, (lid, n, frames) in enumerate(rows):
        draw.text((4, y * cell + 20), f"{lid}\n{n} faces", fill=(255, 255, 0, 255))
        for x, fr in enumerate(frames):
            data, off = (src, tex[fr]) if game == "lit" else (src[tex[fr][0]], tex[fr][1])
            im = decode_tim(data, off)[0]
            im.thumbnail((cell - 4, cell - 4))
            img.alpha_composite(im, ((x + 1) * cell + 2, y * cell + 2))
    os.makedirs(out, exist_ok=True)
    img.save(os.path.join(out, os.path.splitext(os.path.basename(path))[0] + "_links.png"))


if __name__ == "__main__":
    game, target = sys.argv[1], sys.argv[2]
    if os.path.isdir(target):
        table(game, target, "--check" in sys.argv)
    else:
        sheet(game, target, sys.argv[3])
