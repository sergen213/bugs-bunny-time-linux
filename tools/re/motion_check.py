#!/usr/bin/env python3
"""Smoothness check for Enhanced interpolation (idea: BBLIT-Remaster tools/trace_motion.py, MIT).

Input: a BBTIME_MOTION="N:file" trace (main.cpp App::MotionTrace): per frame and traced actor / shadow
`tick sub id cx cy cz rms flow verts`, N frames per 30 Hz tick at alpha k/N.

Per moving (id, tick): evenness = largest frame move / the tick's total move (1/N = even glide, 1.0 = one step),
collapse = smallest mid-tick rms / smallest rms at the tick ends (< 0.9: the mesh shrinks between ticks, the
chord collapse naive vertex lerps give spinning vortices). Shadows (id 20000 + owner): the spread of the
shadow-to-owner offset inside a tick (0 = the shadow moves with its owner).

usage: motion_check.py TRACE [--min 2] [--top 8]
"""
import argparse
import collections
import math


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--min", type=float, default=2.0, help="least total move in a tick to count it (world units)")
    ap.add_argument("--top", type=int, default=8)
    a = ap.parse_args()
    rows = collections.defaultdict(dict)  # (id, tick) -> sub -> (c, rms, flow)
    steps = 0
    for line in open(a.trace):
        t, s, i, x, y, z, rms, flow, _ = line.split()
        rows[(int(i), int(t))][int(s)] = ((float(x), float(y), float(z)), float(rms), float(flow))
        steps = max(steps, int(s))
    per = collections.defaultdict(lambda: [0, 0.0, 0.0, 0, 0, 1.0])  # n, sum even, worst even, snaps, collapses, worst collapse
    for (i, t), subs in rows.items():
        if len(subs) != steps or (i, t - 1) not in rows or steps not in rows[(i, t - 1)]:
            continue
        flows = [subs[k][2] for k in range(1, steps + 1)]
        if min(flows) < 0 or sum(flows) < a.min:
            continue
        even = max(flows) / sum(flows)
        ends = min(rows[(i, t - 1)][steps][1], subs[steps][1])
        mid = min(subs[k][1] for k in range(1, steps))
        coll = mid / ends if ends > 1 else 1.0
        p = per[i]
        p[0] += 1; p[1] += even; p[2] = max(p[2], even); p[3] += even > 0.6; p[4] += coll < 0.9; p[5] = min(p[5], coll)
    n = sum(p[0] for p in per.values())
    if not n:
        print("no moving actors")
        return
    print(f"{steps} frames/tick, {n} moving actor-ticks over {len(per)} actors: mean evenness "
          f"{sum(p[1] for p in per.values()) / n:.3f} (even glide {1 / steps:.3f}, stepped 1.000), "
          f"snaps(>0.6) {sum(p[3] for p in per.values())}, collapses(<0.9) {sum(p[4] for p in per.values())}")
    for i, p in sorted(per.items(), key=lambda kv: -kv[1][2])[: a.top]:
        print(f"  id {i:6d}: ticks {p[0]:4d} mean {p[1] / p[0]:.3f} worst {p[2]:.3f} snaps {p[3]} collapses {p[4]} min rms ratio {p[5]:.3f}")
    # shadows vs owners
    sh = []
    for (i, t), subs in rows.items():
        if i < 19999 or len(subs) != steps:
            continue
        o = (i - 20000, t)
        if o not in rows or len(rows[o]) != steps or steps not in rows.get((o[0], t - 1), {}):
            continue
        offs = [tuple(subs[k][0][j] - rows[o][k][0][j] for j in (0, 2)) for k in range(1, steps + 1)]
        mx = max(math.dist(u, v) for u in offs for v in offs)
        mv = math.dist(rows[(o[0], t - 1)][steps][0], rows[o][steps][0])
        if mv > a.min:
            sh.append((mx, mv, i - 20000, t))
    if sh:
        sh.sort(reverse=True)
        print(f"shadows: {len(sh)} moving owner-ticks, worst offset spread {sh[0][0]:.1f} (owner {sh[0][2]} tick {sh[0][3]} "
              f"moving {sh[0][1]:.1f}), mean {sum(s[0] for s in sh) / len(sh):.2f}")


if __name__ == "__main__":
    main()
