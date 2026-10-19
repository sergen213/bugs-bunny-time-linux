#!/usr/bin/env python3
"""Match functions between two ExportFunctionFeatures.java dumps (LiT bugs.exe vs TB bblit2.exe).

usage: match_functions.py <a.features.tsv> <b.features.tsv> [bugsdecomp funcs.csv] > pairs.tsv

Passes, each only accepting pairs unique on both sides:
  hash   identical mnemonic sequence (>= 8 instructions)
  hashord  same, for groups of k identical sequences on both sides: paired in address order
  str    identical set of referenced strings, mnemonic similarity >= 0.5
  const  identical (imports, immediates) signature with >= 2 items, mnemonic similarity >= 0.5
  call   callees of an already-matched pair, aligned by call order, mnemonic similarity >= 0.6
  fuzzy  remaining functions (>= 20 instr), best 4-gram similarity >= 0.8 and clearly ahead of runner-up
Output columns: a_entry b_entry method score a_ninstr b_ninstr bugsdecomp_name
"""
import csv
import sys
from collections import Counter, defaultdict
from difflib import SequenceMatcher

csv.field_size_limit(1 << 30)


def load(path):
    fns = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            r["ninstr"] = int(r["ninstr"])
            r["calls"] = [c for c in r["calls"].split(",") if c]
            r["mn"] = r["mnems"].split()
            r["strset"] = frozenset(s for s in r["strings"].split("|") if len(s) >= 4)
            r["sig"] = (r["imports"], r["scalars"])
            fns[r["entry"]] = r
    return fns


def unique_pairs(a, b, key, ok):
    ka, kb = defaultdict(list), defaultdict(list)
    for e, r in a.items():
        if ok(r): ka[key(r)].append(e)
    for e, r in b.items():
        if ok(r): kb[key(r)].append(e)
    return [(ka[k][0], kb[k][0]) for k in ka if len(ka[k]) == 1 and len(kb.get(k, ())) == 1]


def sim(x, y):
    return SequenceMatcher(None, x, y, autojunk=False).ratio()


def grams(mn, n=4):
    return Counter(tuple(mn[i:i + n]) for i in range(max(0, len(mn) - n + 1)))


def gram_sim(g1, g2):
    inter = sum((g1 & g2).values())
    return 2 * inter / (sum(g1.values()) + sum(g2.values()) or 1)


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    names = {}
    if len(sys.argv) > 3:
        for r in csv.DictReader(open(sys.argv[3])):
            addr = r["PC v1.6 addr"].strip("*")  # '*' marks functions merged by the linker
            if addr.startswith("0x"):
                names.setdefault("%08x" % int(addr, 16), []).append(r["name"])
    pairs, used_a, used_b = {}, set(), set()

    def accept(ea, eb, how, score):
        if ea in used_a or eb in used_b: return
        pairs[ea] = (eb, how, score)
        used_a.add(ea); used_b.add(eb)

    free = lambda d, used: {e: r for e, r in d.items() if e not in used}
    for ea, eb in unique_pairs(a, b, lambda r: r["mnemhash"], lambda r: r["ninstr"] >= 8):
        accept(ea, eb, "hash", 1.0)
    ga, gbh = defaultdict(list), defaultdict(list)
    for d, g in ((a, ga), (b, gbh)):
        for e, r in d.items():
            if r["ninstr"] >= 8: g[r["mnemhash"]].append(e)
    for h, la in ga.items():  # linker keeps object-file order, so equal groups pair up in order
        if len(la) > 1 and len(la) == len(gbh.get(h, ())):
            for ea, eb in zip(sorted(la), sorted(gbh[h])):
                accept(ea, eb, "hashord", 1.0)
    for ea, eb in unique_pairs(free(a, used_a), free(b, used_b), lambda r: r["strset"], lambda r: r["strset"]):
        if (s := sim(a[ea]["mn"], b[eb]["mn"])) >= 0.5: accept(ea, eb, "str", round(s, 2))
    for ea, eb in unique_pairs(free(a, used_a), free(b, used_b), lambda r: r["sig"],
                               lambda r: len([x for x in (r["imports"] + "," + r["scalars"]).split(",") if x]) >= 2):
        if (s := sim(a[ea]["mn"], b[eb]["mn"])) >= 0.5: accept(ea, eb, "const", round(s, 2))

    for _round in range(3):  # fuzzy matches seed further call propagation
        changed = True
        while changed:  # propagate through call order of matched pairs
            changed = False
            for ea, (eb, _, _) in list(pairs.items()):
                ca, cb = a[ea]["calls"], b[eb]["calls"]
                if len(ca) != len(cb): continue
                for x, y in zip(ca, cb):
                    if x in used_a or y in used_b or x not in a or y not in b: continue
                    s = sim(a[x]["mn"], b[y]["mn"])
                    if s >= 0.6:
                        accept(x, y, "call", round(s, 2)); changed = True

        fa, fb = free(a, used_a), free(b, used_b)
        gb = {e: grams(r["mn"]) for e, r in fb.items() if r["ninstr"] >= 20}
        for ea, ra in sorted(fa.items(), key=lambda kv: -kv[1]["ninstr"]):
            if ra["ninstr"] < 20: continue
            g = grams(ra["mn"])
            scored = sorted(((gram_sim(g, gb[eb]), eb) for eb in gb
                             if eb not in used_b and 0.6 <= fb[eb]["ninstr"] / ra["ninstr"] <= 1.6), reverse=True)
            if scored and scored[0][0] >= 0.8 and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.1):
                accept(ea, scored[0][1], "fuzzy", round(scored[0][0], 2))

    print("a_entry\tb_entry\tmethod\tscore\ta_ninstr\tb_ninstr\tbugsdecomp_name")
    for ea in sorted(pairs):
        eb, how, s = pairs[ea]
        print(f"{ea}\t{eb}\t{how}\t{s}\t{a[ea]['ninstr']}\t{b[eb]['ninstr']}\t{'/'.join(names.get(ea, []))}")
    print(f"# matched {len(pairs)} of {len(a)} (a) / {len(b)} (b)", file=sys.stderr)
    print("# by method: " + str(Counter(v[1] for v in pairs.values())), file=sys.stderr)


if __name__ == "__main__":
    main()
