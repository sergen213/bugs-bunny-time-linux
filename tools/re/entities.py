#!/usr/bin/env python3
"""LiT (bugs.exe) section-1 entity lister + collectible statistics. Research prototype, stdlib only.

Entity chunks 0x07 (live), 0x08 (template), 0x0a (live) of section 1; tag -> field map from loader
FUN_0042e7a0 (docs/re/LOGIC.md §2). Mnemonics are read from the LOGIC.md condition/action tables.
Usage:
    entities.py FILE.bze [--type N]   list entities (chunk, behaviour type, id, pos, flags, states, steps, items)
    entities.py --stats DIR           per-level collectible counts (carrot / golden carrot / clock / health) + G[] bits
    entities.py --selftest
"""
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bze  # noqa: E402
import sec1  # noqa: E402

LOGIC_MD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../docs/re/LOGIC.md")
GC, CLOCKS, CARROTS, HEALTH = 7, 5, 3, 1  # G[] indices (LOGIC.md §1)


def mnemonics():
    """{('C'|'A', code): first word of the LOGIC.md semantics column}."""
    m = {}
    try:
        for line in open(LOGIC_MD, encoding="utf-8"):
            r = re.match(r"\| ([CA])(\d+) \| 0x[0-9a-f]+ \| 0x[0-9a-f]+ \| ([^|]*)\|", line)
            if r:
                word = re.sub(r"[`*:,(]", " ", r.group(3)).split()
                m[(r.group(1), int(r.group(2)))] = word[0] if word else "?"
    except OSError:
        pass
    return m


MN = mnemonics()


def parse(path):
    """-> list of entity dicts (only section-1 entity chunks 0x07/0x08/0x0a)."""
    _, secs = bze.load(path)
    out = []
    for off, kind, t, tags in sec1.walk(dict(secs)[1]):
        if kind.startswith("unknown"):
            raise ValueError(f"{path}: {kind} at {off:#x}")
        if t not in (0x07, 0x08, 0x0a):
            continue
        e = dict(off=off, chunk=t, type=None, id=None, pos=None, f8=0, fc=0, hp=None, t1d=None,
                 states=[], steps=[], items=[])
        for tg, pl in tags:
            if tg == 0x13:
                e["type"] = pl[0]
            elif tg == 0x27:
                e["id"] = struct.unpack_from("<H", pl)[0]
            elif tg == 0x10:
                e["pos"] = struct.unpack("<3i", pl)
            elif tg == 0x16:
                e["f8"], e["fc"] = struct.unpack("<II", pl)  # actor +0x08 / +0x0c
            elif tg == 0x1e:
                e["hp"] = struct.unpack("<h", pl)[0]  # actor +0x1e (type 14: hit points)
            elif tg == 0x1d:
                e["t1d"] = struct.unpack_from("<H", pl)[0]  # type 14/31: +0x196 turn shift
            elif tg == 0x32:
                e["states"].append(struct.unpack("<6H", pl))
            elif tg == 0x30:
                sid, anim, spd = struct.unpack_from("<HHh", pl)
                grp, = struct.unpack_from("<H", pl, 8)
                fa, fb = struct.unpack_from("<II", pl, 12)
                e["steps"].append(dict(id=sid, anim=anim, spd=spd, grp=grp, fa=fa, fb=fb,
                                       w=struct.unpack_from("<5h", pl, 20)))  # step +0x14..+0x1c
            elif tg == 0x31:
                g, nx, m8, mc = struct.unpack_from("<HHII", pl)
                c, a, b, act = pl[12:16]
                x, y, fl, p1, p2 = struct.unpack_from("<HHIiI", pl, 16)
                e["items"].append(dict(g=g, nx=nx, m8=m8, mc=mc, c=c, A=a, B=b, act=act, X=x, Y=y,
                                       fl=fl, p1=p1, p2=p2))
        out.append(e)
    return out


def item_str(it):
    s = f"g{it['g']}"
    if it["m8"] or it["mc"]:
        s += f" gate14={it['m8']:#x} gate18={it['mc']:#x}"
    if it["c"]:
        s += f" C{it['c']}:{MN.get(('C', it['c']), '?')}(A={it['A']},B={it['B']})"
    if it["act"]:
        s += f" A{it['act']}:{MN.get(('A', it['act']), '?')}(X={it['X']},Y={it['Y']})"
    s += f" fl={it['fl']:#x} P1={it['p1']} P2={it['p2']} ->state {it['nx']}"
    return s


def touches(it, idx):
    """Does item's action change G[idx]? (inc/dec/add/sub/incgg/movgi/gvcrt)."""
    a, x, y = it["act"], it["X"], it["Y"]
    if a in (4, 6, 19, 37, 40):
        return y == idx
    if a == 23:
        return idx in (x, y)
    return a == 48 and idx == CARROTS  # gvcrt: health+1, else G[Y]+1 (Y is 3 in all LiT data)


def classify(e, templates):
    """-> (kind, persistence bits [(X, Y)], level counter index or None)."""
    its = e["items"]
    kind = None
    if any(touches(i, GC) for i in its):
        kind = "golden_carrot"
    elif any(touches(i, CLOCKS) for i in its):
        kind = "clock"
    elif any(i["act"] == 48 for i in its):
        kind = "carrot_fx"
    elif any(i["act"] in (4, 19) and i["Y"] == HEALTH and i["act"] != 6 for i in its):
        kind = "health"
    else:
        for i in its:  # spawns a template that awards a carrot (A48)
            t = templates.get(i["p2"]) if i["fl"] & 0x40100 and not i["fl"] & 0x20000000 else None
            if t is not None and t is not e and any(j["act"] == 48 for j in t["items"]):
                kind = "carrot"
                break
    if kind is None:
        return None, [], None
    bits = sorted({(i["X"], i["Y"]) for i in its if i["act"] == 12})  # orgi G[Y] |= X (collected flag)
    counter = None
    for i in its:
        if i["act"] == 23 and kind in ("golden_carrot", "clock"):
            idx = GC if kind == "golden_carrot" else CLOCKS
            if idx in (i["X"], i["Y"]):
                counter = i["Y"] if i["X"] == idx else i["X"]
    return kind, bits, counter


def level_files(d):
    """Gameplay levels: L0x*/LBxx/LS01, skipping the duplicate '_8' builds."""
    return sorted(f for f in os.listdir(d) if f.lower().endswith(".bze") and re.match(r"(L0\d|LB\d|LS\d)", f)
                  and "_8" not in f)


def stats(d):
    tot = collections.Counter()
    per_counter = collections.Counter()
    for f in level_files(d):
        es = parse(os.path.join(d, f))
        templates = {e["id"]: e for e in es if e["chunk"] == 0x08}
        c = collections.Counter()
        bits = collections.defaultdict(set)
        for e in es:
            if e["type"] != 14:
                continue
            kind, b, counter = classify(e, templates)
            if kind is None:
                continue
            where = "tmpl" if e["chunk"] == 0x08 else "live"
            c[(kind, where)] += 1
            for x, y in b:
                bits[(kind, y)].add(x)
            if counter is not None and where == "live":
                per_counter[(kind, counter)] += 1
        tot.update(c)
        print(f"{f:14} " + " ".join(f"{k}/{w}={n}" for (k, w), n in sorted(c.items())))
        for (k, y), xs in sorted(bits.items()):
            print(f"{'':14}   {k} bits G[{y}] = {sorted(xs)}")
    print("TOTAL " + " ".join(f"{k}/{w}={n}" for (k, w), n in sorted(tot.items())))
    print("live golden carrots / clocks per level counter G[i]:")
    for (k, i), n in sorted(per_counter.items()):
        print(f"  {k} G[{i}] {n}")


def dump(path, only_type=None):
    es = parse(path)
    for e in es:
        if only_type is not None and e["type"] != only_type:
            continue
        print(f"@{e['off']:#07x} chunk {e['chunk']:#04x} type {e['type']} id {e['id']} pos {e['pos']} "
              f"f8={e['f8']:#x} fc={e['fc']:#x} hp={e['hp']} t1d={e['t1d']}")
        for s in e["states"]:
            print(f"    state {s[0]}: steps {[x for x in s[1:] if x != 0xfff0]}")
        for s in e["steps"]:
            print(f"    step {s['id']} anim {s['anim']} spd {s['spd']} grp {s['grp']} fa={s['fa']:#x} "
                  f"fb={s['fb']:#x} w={s['w']}")
        for it in e["items"]:
            print("    item " + item_str(it))


def selftest():
    it = dict(g=1, nx=0, m8=0x8000, mc=0, c=0, A=0, B=0, act=23, X=84, Y=7, fl=0x9000, p1=0, p2=0)
    e = dict(chunk=7, items=[it, dict(it, act=12, X=2, Y=55, fl=0)])
    assert classify(e, {}) == ("golden_carrot", [(2, 55)], 84)
    fx = dict(chunk=8, items=[dict(it, act=48, X=0, Y=3)])
    carrot = dict(chunk=7, items=[dict(it, act=12, X=1, Y=20, fl=0x10100, p2=37)])
    assert classify(carrot, {37: fx})[0] == "carrot"
    assert MN.get(("A", 23)) == "incgg", MN.get(("A", 23))
    print("selftest ok")


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "--selftest":
        selftest()
    elif a[0] == "--stats":
        stats(a[1])
    else:
        dump(a[0], int(a[2]) if len(a) > 2 and a[1] == "--type" else None)
