#!/usr/bin/env python3
"""TB (bblit2.exe) player-entity probe: the two player characters, their class templates, state records
(tag 0x2f), event->state transitions (tag 0x31) and logic pad conditions (tag 'X' = 0x58).

Player = entity whose tag-0x16 flag word +8 has bit 0x4000000 (loader FUN_004347c0 case 0x16); tag-0x16
word +0xc bit 0x400 -> controlled by pad record 0 (P1, global 0x4d236c), bit 0x800 -> pad record 1 (P2,
0x4d2368). Tag 0x27 u16[0] = entity id (+0x34: 2 = Bugs, 3 = Taz), u16[1] = +0x50 (class template id).

Usage:
    tb_players.py FILE.bze             players + template summary (states, transitions, pad conditions)
    tb_players.py FILE.bze --states    also dump every decoded state / transition record
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_sec1  # noqa: E402

PAD = {0x0001: "L2", 0x0002: "R2", 0x0004: "L1", 0x0008: "R1", 0x0010: "Tri", 0x0020: "O", 0x0040: "X",
       0x0080: "Sq", 0x0100: "Sel", 0x0200: "L3", 0x0400: "R3", 0x0800: "Start", 0x1000: "Up",
       0x2000: "Right", 0x4000: "Down", 0x8000: "Left"}


def bits(v):
    return "+".join(n for b, n in PAD.items() if v & b) or "-"


# state record = tag 0x2f payload (48 B) -> 0x2c-byte runtime record (FUN_004347c0 case 0x2f)
STATE = [("id", 0x00, "H"), ("anim", 0x02, "H"), ("f4", 0x04, "H"), ("logicA", 0x08, "H"), ("logicB", 0x0c, "H"),
         ("f20", 0x10, "I"), ("f24", 0x14, "I"), ("f28", 0x18, "I"), ("fwd", 0x1c, "h"), ("r10", 0x1e, "h"),
         ("dmg", 0x20, "h"), ("vert", 0x22, "h"), ("target", 0x24, "h"), ("r1c", 0x26, "h"), ("r1e", 0x28, "h"),
         ("hitState", 0x2a, "h"), ("fallback", 0x2c, "h")]


def state(pl):
    return {k: struct.unpack_from("<" + f, pl, o)[0] for k, o, f in STATE}


def transition(pl):
    ev, *targets = struct.unpack("<6H", pl)
    return ev, [t for t in targets if t]


def padcond(pl):
    a, b = struct.unpack("<II", pl)
    return a & 0xffff, b & 0xffff, b >> 16  # VM flags word, required mask, any-of mask (0xffff = none)


def entities(path):
    for off, kind, t, tags in tb_sec1.walk(tb_sec1.section1(path)):
        if kind != "chunk" or t not in (7, 8):
            continue
        d = {}
        for _, tg, pl in tags:
            d.setdefault(tg, []).append(pl)
        if 0x27 in d:
            yield off, t, d


def players(path):
    ents = list(entities(path))
    templates = {struct.unpack_from("<H", d[0x27][0])[0]: d for _, t, d in ents if t == 8}
    out = []
    for off, t, d in ents:
        if t != 7 or 0x16 not in d:
            continue
        f8, fc = struct.unpack_from("<II", d[0x16][0])
        if not f8 & 0x4000000:
            continue
        eid, tmpl = struct.unpack_from("<HH", d[0x27][0])
        out.append({"off": off, "id": eid, "template": tmpl, "type": d[0x13][0][0], "pad": (fc >> 10) & 3,
                    "own": d, "tmpl": templates.get(tmpl, {})})
    return out


def main(argv):
    for p in players(argv[1]):
        who = {2: "Bugs", 3: "Taz"}.get(p["id"], "?")
        pad = {1: "P1", 2: "P2", 0: "none", 3: "both"}[p["pad"]]
        print(f"entity @{p['off']:#x} id {p['id']} ({who}) type {p['type']} pad {pad} template {p['template']}")
        for name, d in (("own", p["own"]), ("template", p["tmpl"])):
            xs = [padcond(x) for x in d.get(0x58, [])]
            print(f"  {name}: {len(d.get(0x2f, []))} states, {len(d.get(0x31, []))} transitions, "
                  f"{len(xs)} pad conds")
            for fl, req, anyb in sorted(set(xs)):
                print(f"    X flags={fl:#06x} req={bits(req)} any={'none' if anyb == 0xffff else bits(anyb)}")
            if "--states" in argv:
                for pl in d.get(0x2f, []):
                    print("    S", " ".join(f"{k}={v:#x}" for k, v in state(pl).items()))
                for pl in d.get(0x31, []):
                    ev, tg = transition(pl)
                    print(f"    T event {ev} -> {tg}")


def selfcheck():
    assert bits(0x0048) == "R1+X" and padcond(struct.pack("<II", 0x208, 0xffff0080)) == (0x208, 0x80, 0xffff)
    assert transition(struct.pack("<6H", 7, 3, 0, 0, 0, 0)) == (7, [3])
    granny = os.path.join(os.path.dirname(__file__), "../../../original/tb/Datas/BZE/Granny.bze")
    if os.path.exists(granny):
        ps = {p["id"]: p for p in players(granny)}
        assert ps[2]["pad"] == 1 and ps[3]["pad"] == 2 and ps[2]["type"] == ps[3]["type"] == 14
    print("selfcheck ok")


if __name__ == "__main__":
    selfcheck() if len(sys.argv) < 2 else main(sys.argv)
