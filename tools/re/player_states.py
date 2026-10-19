#!/usr/bin/env python3
"""Dump the LiT player state machine stored in a level BZE (section 1, entity chunk with tag 0x13 == 1).

The player controller FUN_0043a880 is data driven (docs: local PlayerRE notes / GAMEPLAY.md):
  tag 0x32 (12 B) state  : u16 id, 5 x s16 move ids (-16 = end)           -> actor+0xf4 list, node+4/+6
  tag 0x34 (24 B) move   : u16 id, u16 anim, s16 animMode, (2 B unused), u32 flagsA, u32 flagsB,
                           u16 link, s16 speed, s16 jumpH, s16 jumpStep   -> actor+0xf8 list, node
                           +4 id +6 anim +8 mode +0xc fA +0x10 fB +0xa link +0x14 spd +0x16 H +0x18 step
  tag 0x35 (40 B) transition: u16 group(=move.link), u16 buttonsAll, u16 buttonsAny, u16 heldCmd,
                           s16 nextState(-13 none), u8 cond,a1,a2, u8 act, u16 x, u16 y, u16 ?, u16 invert,
                           2 B unused, u32 heldMask, u32 parentMask, u32 flags14Mask, u32 flags18Mask
                           -> actor+0xfc list (loader 0x42e7a0 case '5'; evaluated at 0x43e270..0x43e3f0)
Usage: player_states.py LEVEL.bze   (or --selftest)"""
import struct
import sys

import bze
import sec1

BTN = {0x1000: "Up", 0x2000: "Right", 0x4000: "Down", 0x8000: "Left", 0x01: "L2", 0x02: "R2", 0x04: "L1",
       0x08: "R1", 0x10: "Tri", 0x20: "Circ", 0x40: "X", 0x80: "Sq", 0x100: "Sel", 0x800: "Start"}


def btn(m):
    if m in (0, 0xfff1):  # 0xfff1: both branches at 0x43e331 run the condition -> no button requirement
        return ""
    if m == 0xf000:
        return "anyDir"
    return "+".join(v for k, v in BTN.items() if m & k)


def parse(tags):
    states, moves, trans = [], {}, []
    for tg, pl in tags:
        if tg == 0x32:
            states.append((struct.unpack_from("<H", pl)[0], [x for x in struct.unpack_from("<5h", pl, 2) if x != -16]))
        elif tg == 0x34:
            mid, anim, mode = struct.unpack_from("<HHh", pl)
            fa, fb = struct.unpack_from("<II", pl, 8)
            link, spd, h, step = struct.unpack_from("<Hhhh", pl, 16)
            moves.setdefault(mid, dict(anim=anim, mode=mode, fA=fa, fB=fb, link=link, spd=spd, H=h, step=step))
        elif tg == 0x35:
            g, ball, bany, held, nxt = struct.unpack_from("<HHHHh", pl)
            cond, act = pl[10:13], pl[13]
            x, y, _, inv = struct.unpack_from("<HHHH", pl, 14)
            m = struct.unpack_from("<IIII", pl, 24)
            trans.append(dict(g=g, ball=ball, bany=bany, held=held, nxt=nxt, cond=tuple(cond), act=(act, x, y),
                              inv=inv, mHeld=m[0], mParent=m[1], m14=m[2], m18=m[3]))
    return states, moves, trans


def player_tags(path):
    _, secs = bze.load(path)
    for _, kind, t, tags in sec1.walk(dict(secs)[1]):
        if kind == "chunk" and t in (7, 8, 10) and any(tg == 0x13 and pl[0] == 1 for tg, pl in tags):
            return tags
    return None


def fmt_trans(t):
    s = []
    if btn(t["ball"]):
        s.append("all:" + btn(t["ball"]))
    if t["bany"]:
        s.append("any:" + btn(t["bany"]))
    for k in ("m14", "m18", "mHeld", "mParent"):
        if t[k]:
            s.append(f"{k}&{t[k]:#x}")
    if t["cond"][0]:
        s.append("cond%d(%d,%d)" % t["cond"])
    a = " / act%d(%d,%d)" % t["act"] if t["act"][0] else ""
    nxt = "-" if t["nxt"] == -13 else str(t["nxt"])
    held = f" held={t['held']:#x}" if t["held"] else ""
    return " & ".join(s) + a + " -> " + nxt + held


def dump(tags):
    states, moves, trans = parse(tags)
    out = []
    for sid, seq in states:
        out.append(f"STATE {sid}: {seq}")
        for mid in seq:
            m = moves.get(mid)
            if m is None:
                out.append(f"  move {mid}: MISSING")
                continue
            out.append(f"  move {mid}: anim {m['anim']} mode {m['mode']:#x} fA {m['fA']:08x} fB {m['fB']:08x} "
                       f"spd {m['spd']} jump {m['H']}/{m['step']} link {m['link']}")
            if m["link"]:
                out += ["      " + fmt_trans(t) for t in trans if t["g"] == m["link"]]
    return "\n".join(out)


def selftest():
    st = struct.pack("<H5h", 1, 4, -16, -16, -16, -16)
    mv = struct.pack("<HHh2xIIHhhh", 10, 15, 2, 0x4192, 0x800, 10, 16, 39, 70)
    tr = struct.pack("<HHHHh4BHHHH2xIIII", 10, 0x40, 0, 0, 7, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0x12, 0)
    assert len(st) == 12 and len(mv) == 24 and len(tr) == 40
    states, moves, trans = parse([(0x32, st), (0x34, mv), (0x35, tr)])
    assert states == [(1, [4])] and moves[10]["H"] == 39 and moves[10]["step"] == 70
    assert fmt_trans(trans[0]) == "all:X & m14&0x12 -> 7", fmt_trans(trans[0])
    print("selftest ok")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] == "--selftest":
        selftest()
    else:
        tags = player_tags(sys.argv[1])
        print(dump(tags) if tags else "no player chunk")
