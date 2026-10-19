#!/usr/bin/env python3
"""TB (bblit2.exe) player move graph: Bugs (entity id 2) / Taz (id 3) state records (tag 0x2f), event
transitions (tag 0x31) and the state logic blocks (tags 'M'..'`') that raise the events, plus the integer
jump/gravity model of the generic character mover. RE notes: local tb_player.md (state record layout,
FUN_00453de0 / FUN_004450c0 / FUN_0044d990 / VM FUN_0045d7a0 / encoder FUN_00433ab0).

Usage:
    tb_moves.py FILE.bze [--blocks]   per character: states (decoded fields), the pad/flag conditions of the
                                      state's logic blocks and the event -> state sequence they raise
    tb_moves.py --all DIR             parse every level, print per-level counts (self-check over all files)
    tb_moves.py                       self-check (jump integrator, decoders, ARCA01 facts)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_sec1  # noqa: E402
from tb_players import PAD, bits  # noqa: E402

# tag 0x2f payload (48 B) -> runtime record (loader FUN_004347c0 case 0x2f). (name, file off, fmt, rec off)
STATE = [("id", 0x00, "H", 0x06), ("anim", 0x02, "H", 0x08), ("mode", 0x04, "H", 0x04),
         ("logicA", 0x08, "H", 0x0a), ("logicB", 0x0c, "H", 0x0c), ("flagsA", 0x10, "I", 0x20),
         ("flagsB", 0x14, "I", 0x24), ("flagsC", 0x18, "I", 0x28), ("fwd", 0x1c, "h", 0x0e),
         ("grip", 0x1e, "h", 0x10), ("dmg", 0x20, "h", 0x12), ("vert", 0x22, "h", 0x14),
         ("target", 0x24, "h", 0x16), ("side", 0x26, "h", 0x1c), ("cap", 0x28, "h", 0x1e),
         ("hitTrans", 0x2a, "h", 0x18), ("ownHurt", 0x2c, "h", 0x1a)]
COND_TAGS = {0x4f: "O", 0x50: "P", 0x51: "Q", 0x52: "R", 0x54: "T", 0x57: "W", 0x58: "X", 0x5a: "Z",
             0x5b: "[", 0x5c: "\\", 0x5d: "]", 0x5e: "^", 0x60: "`"}  # FUN_00433900: carry 0x38 markers
ACT_TAGS = {0x4e: "N", 0x53: "S", 0x55: "U", 0x56: "V", 0x59: "Y", 0x5f: "_"}  # FUN_00433a10: no markers
FLAGWORD = {0x4f: "+0x20", 0x50: "+0x24", 0x51: "+0x28"}  # VM ops 4/5/6 test actor flag words


def state(pl):
    return {k: struct.unpack_from("<" + f, pl, o)[0] for k, o, f, _ in STATE}


def speed(v, dt=2):
    """Speed field (fwd/vert/...) as used by FUN_00453de0: v > 0 with bits 0x6000 -> s8 table 0x4d24a0[v &
    ~0x6000] (runtime logic variable, unknown statically -> None)."""
    return None if v > 0 and v & 0x6000 else v * dt


def isqrt_round(n):  # FUN_004040b0
    r = int(n ** 0.5)
    while r * r > n:
        r -= 1
    return r + 1 if n - r * r > r else r


def jump_vy0(h):
    """Initial vy (units/tick, +Y down) of a state with flagsA 0x10, FUN_0044d990: actor+0x72 =
    -0x50 - ((isqrt(H*0x50 + 100) >> 1) << 4), stored x16."""
    return (-0x50 - ((isqrt_round(h * 0x50 + 100) >> 1) << 4)) >> 4


def jump_arc(h, dt=2):
    """Flat-ground ballistic arc of FUN_004450c0 with k_y = 0 (no y smoothing): v += 5*dt, y += v.
    Returns (apex height, apex tick, landing tick)."""
    v, y, t, apex, at = jump_vy0(h), 0, 0, 0, 0
    while True:
        v += 5 * dt
        y += v
        t += 1
        if -y > apex:
            apex, at = -y, t
        if y >= 0:
            return apex, at, t


def xcond(pl):
    a, b = struct.unpack("<II", pl)
    src = {0x100: "raw", 0x200: "held", 0x400: "new", 0x300: "raw", 0x500: "raw&~e"}.get(a & 0x700, "pad")
    who = {0x800: "P1", 0x1000: "P2", 0x2000: "P3", 0x4000: "P4"}.get(a & 0x7800, "")
    req, anyb = b & 0xffff, b >> 16
    s = f"{src}{who}{'/cam' if a & 0x8000 else ''}"
    s += f"[{bits(req)}" + ("" if anyb == 0xffff else f" any {bits(anyb)}") + "]"
    return a, s


# VM op 8 ('R'): {u16 flags, u8 pad, u8 cond index, s16 X, s16 Y}; TB condition table 0x4cd560 (decoded slots)
RCOND = {0x01: "S[{y:#x}]=={x}", 0x05: "S[{y:#x}]>={x}", 0x07: "S[{y:#x}]<{x}",
         0x10: "stateTicks>{t}", 0x18: "S[{y:#x}]&{x:#x}", 0x19: "GD[0x58+{y:#x}]&{x:#x}",
         0x49: "depth+0x1a2 in [{x},{y}]"}


def rcond(pl):
    _, ci, x, y = struct.unpack("<HHhh", pl)
    ci &= 0xff
    fmt = RCOND.get(ci)
    return fmt.format(x=x, y=y, t=(y & 0xffff) << 8 | (x & 0xffff)) if fmt else f"R{ci:#x}({x},{y})"


def logic(tg, pl):
    """(marker, chain, text). marker: 'IF' (0x08), 'IF+' (0x10), 'SUB' (0x20), '' ; chain: '', AND, OR + NOT."""
    a = struct.unpack_from("<I", pl)[0] if len(pl) >= 4 else struct.unpack_from("<H", pl)[0]
    if tg == 0x58:
        a, txt = xcond(pl)
    elif tg == 0x4e:
        return "", "", f"-> ev {a:#x}"
    elif tg in FLAGWORD:
        txt = f"{FLAGWORD[tg]}&{struct.unpack_from('<I', pl, 4)[0]:#x}"
    elif tg in ACT_TAGS:
        return "", "", f"{ACT_TAGS[tg]}({pl.hex()})"
    elif tg == 0x5c:
        txt = "ELSE"
    elif tg == 0x52:
        txt = rcond(pl)
    else:
        txt = f"{COND_TAGS[tg]}({pl.hex()})"
    mark = "IF" if a & 8 else "IF+" if a & 0x10 else "SUB" if a & 0x20 else ""
    chain = ("AND" if a & 1 else "OR" if a & 2 else "") + (" NOT" if a & 4 else "")
    return mark, chain.strip(), txt


def chunks(path):
    for off, kind, t, tags in tb_sec1.walk(tb_sec1.section1(path)):
        if kind == "chunk" and t in (7, 8, 0x0a, 0x63):
            ids = [struct.unpack("<HH", pl) for _, tg, pl in tags if tg == 0x27]
            if ids:
                yield off, t, ids[0], tags


def blocks(tags):
    out, cur = {}, None
    for _, tg, pl in tags:
        if tg == 0x4d:
            cur = struct.unpack("<H", pl)[0]
            out.setdefault(cur, [])
        elif 0x4e <= tg <= 0x60 and cur is not None:
            out[cur].append((tg, pl))
        else:
            cur = None
    return out


def character(path, eid):
    """Merged (template then own chunk; own records win on equal ids) states, transitions, logic blocks."""
    cs = list(chunks(path))
    tmpl = {i[0]: tags for _, t, i, tags in cs if t == 8}
    own = next((tags for _, t, i, tags in cs if t != 8 and i[0] == eid), None)
    if own is None:
        return None
    tid = next(i[1] for _, t, i, tags in cs if t != 8 and i[0] == eid)
    st, tr, bl = {}, {}, {}
    for tags in (tmpl.get(tid, []), own):
        for _, tg, pl in tags:
            if tg == 0x2f:
                s = state(pl)
                st[s["id"]] = s
            elif tg == 0x31:
                ev, *tg5 = struct.unpack("<6H", pl)
                tr[ev] = [x for x in tg5 if x not in (0, 0xfff0)]
        bl.update(blocks(tags))
    return {"template": tid, "states": st, "trans": tr, "blocks": bl}


def edges(block):
    """Flatten a logic block into (conditions, event) pairs in priority order. Structure (VM FUN_0045d7a0,
    encoder FUN_00433900): IF (0x08) starts a top-level statement, the first true IF ends the block (op 0
    returns when the previous chain was true); SUB (0x20) chains inside an IF body are if / else-if / ELSE
    ('\\\\' 0x20); 'N' returns its event immediately."""
    out, top, sub = [], [], []
    for tg, pl in block:
        mark, chain, txt = logic(tg, pl)
        if tg == 0x4e:
            out.append((tuple(top + sub), struct.unpack("<I", pl)[0]))
            continue
        if tg not in COND_TAGS:
            continue
        c = f"{chain + ' ' if chain else ''}{txt}"
        if mark in ("IF", "IF+"):
            top, sub = [c], []
        elif mark == "SUB":
            sub = ["else"] if txt == "ELSE" else [c]
        elif sub:
            sub.append(c)
        else:
            top.append(c)
    return out


def describe(path, eid, show_blocks=False):
    c = character(path, eid)
    if c is None:
        return
    name = {2: "Bugs", 3: "Taz"}[eid]
    print(f"== {name} (id {eid}, template {c['template']}): {len(c['states'])} states, "
          f"{len(c['trans'])} transitions, {len(c['blocks'])} logic blocks")
    start = c["trans"].get(2) or c["trans"].get(1)
    print(f"   start (event 2) -> {start}, default (event 1) -> {c['trans'].get(1)}")
    for sid, s in sorted(c["states"].items()):
        jump = ""
        if s["flagsA"] & 0x10 and speed(s["vert"]) is not None:
            apex, at, land = jump_arc(s["vert"])
            jump = f" jumpH {s['vert']} vy0 {jump_vy0(s['vert'])} apex {apex}@{at} land {land}"
        fwd = speed(s["fwd"])
        print(f"  S{sid:<4} anim {s['anim']:<4} mode {s['mode']:#x} A {s['flagsA']:#010x} B {s['flagsB']:#010x} "
              f"C {s['flagsC']:#x} fwd {'S[%#x]' % (s['fwd'] & 0x9fff) if fwd is None else fwd} grip {s['grip']}"
              f" dmg {s['dmg']} vert {s['vert']}{jump} logic {s['logicA']}/{s['logicB']}")
    used = sorted({b for s in c["states"].values() for b in (s["logicA"], s["logicB"]) if b})
    for bid in used:
        users = [sid for sid, s in sorted(c["states"].items()) if bid in (s["logicA"], s["logicB"])]
        print(f"  block M{bid} (states {users}):")
        for conds, ev in edges(c["blocks"].get(bid, [])):
            tgt = c["trans"].get(ev) if ev < 10000 else None
            print(f"      {' & '.join(conds) or 'always'} -> ev {ev:#x}" + (f" -> {tgt}" if tgt else ""))
        if show_blocks:
            print("      raw: " + " | ".join(" ".join(x for x in logic(tg, pl) if x)
                                           for tg, pl in c["blocks"].get(bid, [])))


def all_levels(folder):
    n = 0
    for f in sorted(os.listdir(folder), key=str.upper):
        if not f.lower().endswith(".bze"):
            continue
        row = []
        for eid in (2, 3):
            c = character(os.path.join(folder, f), eid)
            if c is None:
                row.append("-")
                continue
            ne = sum(len(edges(b)) for b in c["blocks"].values())
            hs = sorted({s["vert"] for s in c["states"].values() if s["flagsA"] & 0x10 and speed(s["vert"])})
            row.append(f"{len(c['states'])}st {len(c['trans'])}tr {len(c['blocks'])}blk {ne}ev jumpH {hs}")
        print(f"{f:14} Bugs {row[0]} | Taz {row[1]}")
        n += 1
    return n


def selfcheck():
    assert isqrt_round(43940) == 210 and isqrt_round(2) == 1 and isqrt_round(3) == 2
    assert jump_vy0(0x224) == -110  # -0x50 - ((210 >> 1) << 4) = -1760 -> /16
    apex, at, land = jump_arc(0x224)
    assert (apex, at, land) == (550, 10, 21), (apex, at, land)
    assert speed(0x4066) is None and speed(13) == 26
    assert logic(0x58, struct.pack("<II", 0x0008, 0xffff0040)) == ("IF", "", "pad[X]")
    assert logic(0x58, struct.pack("<II", 0x0005, 0x000c0000))[1:] == ("AND NOT", "pad[- any L1+R1]")
    arca = os.path.join(os.path.dirname(__file__), "../../../original/tb/Datas/BZE/ARCA01.bze")
    if os.path.exists(arca):
        b, t = character(arca, 2), character(arca, 3)
        assert b["template"] == 181 and t["template"] == 182
        assert b["trans"][19] == [36, 37] and b["states"][36]["flagsB"] & 0x4000000  # walk/run split
        assert b["trans"][2] == [3]
    print("selfcheck ok")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        selfcheck()
    elif sys.argv[1] == "--all":
        print(f"{all_levels(sys.argv[2])} levels")
    else:
        for eid in (2, 3):
            describe(sys.argv[1], eid, "--blocks" in sys.argv)
