#!/usr/bin/env python3
"""Poll globals of a running original to CSV (default every 33 ms).

usage: trace.py lit|tb OUT.csv SECONDS name=EXPR:TYPE[*N] ... [--period 0.033]
  EXPR/TYPE as peek.py (pointer expressions are re-resolved every sample). Column t = seconds since start.
example: trace.py lit /tmp/p.csv 5 tick=0x4b24a4:u32 pos=[0x4b324c]+0xe4:s32*3 state=[0x4b324c]+0x17c:u16
Add the game's tick counter (LiT 0x4b24a4, TB 0x4d2708; +2 per tick) to align samples to ticks.
"""
import csv, struct, sys, time
from peek import FMT, Mem


def parse(spec):
    name, rest = spec.split('=', 1)
    expr, typ = rest.rsplit(':', 1)
    n = 1
    if '*' in typ:
        typ, n = typ.split('*')
        n = int(n)
    return name, expr, typ, n


def main(argv):
    period = 0.033
    if '--period' in argv:
        i = argv.index('--period')
        period = float(argv[i + 1])
        del argv[i:i + 2]
    game, out, secs, specs = argv[0], argv[1], float(argv[2]), [parse(s) for s in argv[3:]]
    m = Mem(game)
    cols = ['t'] + [f'{nm}_{i}' if n > 1 else nm for nm, _, _, n in specs for i in range(n)]
    with open(out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(cols)
        t0 = time.monotonic()
        nxt = t0
        while (now := time.monotonic()) - t0 < secs:
            spans = [(m.addr(e), struct.calcsize(FMT[t]) * n) for _, e, t, n in specs]
            row = [f'{now - t0:.4f}']
            for (_, _, t, n), raw in zip(specs, m.read_many(spans)):
                row += list(struct.unpack('<%d%s' % (n, FMT[t]), raw)) if len(raw) else [''] * n
            w.writerow(row)
            nxt += period
            time.sleep(max(0.0, nxt - time.monotonic()))
    print(out)


if __name__ == '__main__':
    main(sys.argv[1:])
