#!/usr/bin/env python3
"""Prototype decoder for LiT/TB `.xa` music/speech: headerless 4-bit IMA ADPCM, interleaved
per byte (low nibble = left, high nibble = right), 44100 Hz stereo. See docs/formats/AUDIO_XA.md.
Decoder mirrors BugsDecomp track.c CvtStereoAdpcm (bugs.exe 0x401be0).
    xa2wav.py IN.xa OUT.wav [--mono] [--seconds N]"""
import argparse
import sys
import wave

import numpy as np

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
        66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
        408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
        2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
        8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
        29794, 32767]  # table @0x45c0c0 (89 entries)
ADJ = [-1, -1, -1, -1, 2, 4, 6, 8] * 2  # table @0x45c080


def decode(nibbles):
    out = np.empty(len(nibbles), np.int16)
    s, st = 0, 0
    for i, n in enumerate(nibbles):
        step = STEP[st]
        st = min(88, max(0, st + ADJ[n]))
        d = step >> 3
        if n & 4:
            d += step
        if n & 2:
            d += step >> 1
        if n & 1:
            d += step >> 2
        s = max(-32768, min(32767, s - d if n & 8 else s + d))
        out[i] = s
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--mono", action="store_true", help="file is mono (2 samples per byte, low first)")
    ap.add_argument("--seconds", type=float, default=0)
    a = ap.parse_args()
    raw = np.frombuffer(open(a.src, "rb").read(), np.uint8)
    if a.seconds:
        raw = raw[:int(a.seconds * 44100 / (2 if a.mono else 1))]
    lo, hi = (raw & 15).tolist(), (raw >> 4).tolist()
    if a.mono:
        pcm = decode([v for pair in zip(lo, hi) for v in pair])
        ch = 1
    else:
        pcm = np.stack([decode(lo), decode(hi)], 1).reshape(-1)
        ch = 2
    with wave.open(a.dst, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(pcm.tobytes())
    p = pcm.astype(np.float64)
    print(f"{a.src}: {len(raw)} bytes -> {len(pcm) // ch} frames, rms {np.sqrt((p * p).mean()):.0f}, "
          f"clipped {(np.abs(p) >= 32767).mean() * 100:.3f}%", file=sys.stderr)


if __name__ == "__main__":
    main()
