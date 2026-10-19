#!/usr/bin/env python3
"""Side-by-side of an original capture and a bbtime render of the same camera, with pixel statistics.

usage: compare.py ORIG.png BBTIME.png OUT.png [label]     (compare.py --selftest)
OUT = [original | bbtime | |diff| x4]; prints mean abs diff (0..255, RGB) overall and per channel, mean RGB of
each image and the least-squares gain original ~ k * bbtime per channel (brightness rule check). Needs Pillow+numpy.
"""
import sys

import numpy as np
from PIL import Image


def stats(o, b):
    d = np.abs(o - b)
    k = (o * b).sum((0, 1)) / np.maximum((b * b).sum((0, 1)), 1)
    return d.mean(), d.mean((0, 1)), o.mean((0, 1)), b.mean((0, 1)), k


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float64)


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        a = np.full((4, 4, 3), 100.0)
        mad, _, _, _, k = stats(a, a * 2)
        assert mad == 100 and np.allclose(k, 0.5)
        print('compare selftest ok')
        sys.exit()
    o, b = load(sys.argv[1]), load(sys.argv[2])
    if o.shape != b.shape:
        sys.exit(f'size mismatch {o.shape} vs {b.shape}')
    mad, madc, mo, mb, k = stats(o, b)
    f = lambda v: ' '.join(f'{x:.1f}' for x in v)
    print(f'{sys.argv[-1] if len(sys.argv) > 4 else sys.argv[3]}: mean abs diff {mad:.2f} (R G B {f(madc)}); '
          f'mean orig {f(mo)} bbtime {f(mb)}; gain orig/bbtime {" ".join(f"{x:.3f}" for x in k)}')
    diff = np.clip(np.abs(o - b) * 4, 0, 255)
    Image.fromarray(np.concatenate([o, b, diff], axis=1).astype(np.uint8)).save(sys.argv[3])
