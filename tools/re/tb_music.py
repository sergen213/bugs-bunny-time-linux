#!/usr/bin/env python3
"""TB per-level music: logic action 126 (0x432db0, `music = low byte of arg1`) found in section-1 'S' tags
(0x53: u16 action id, s16 arg1, s16 arg2; encoder 0x433ab0). PlayMusic 0x4291e0 opens ..\\track\\Track(n-1).xa,
falls back to Track14.xa when that file is missing; n = 0 stops, n = 1 -> Track0 = stop (iVar5 == 0).

  tb_music.py BZE_DIR [TRACK_DIR]   table level -> n -> file (marks missing files / fallback)
  tb_music.py                       self-check
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tb_sec1  # noqa: E402

ACTION_MUSIC = 126
FALLBACK = 14  # 0x4291e0: second attempt opens Track14.xa


def music_actions(sec1):
    """[(chunk type, entity id or None, logic block id, n)] for every action-126 'S' tag, in file order."""
    out = []
    for _, kind, t, tags in tb_sec1.walk(sec1):
        if kind != "chunk":
            continue
        ent = blk = None
        for _, tg, pl in tags:
            if tg == 0x27:
                ent = struct.unpack_from("<H", pl)[0]
            elif tg == 0x4D:
                blk = struct.unpack("<H", pl)[0]
            elif tg == 0x53:
                act, arg1, _ = struct.unpack("<Hhh", pl)
                if act == ACTION_MUSIC:
                    out.append((t, ent, blk, arg1 & 0xFF))
    return out


def track_file(n, track_dir=None):
    """PlayMusic(n) -> file name actually opened (None = silence)."""
    if n <= 1:
        return None
    name = f"Track{n - 1}.xa"
    if track_dir and not os.path.exists(os.path.join(track_dir, name.lower())) \
            and not os.path.exists(os.path.join(track_dir, name)):
        return f"Track{FALLBACK}.xa (fallback; {name} missing)"
    return name


def main(bze_dir, track_dir=None):
    for f in sorted(x for x in os.listdir(bze_dir) if x.lower().endswith(".bze")):
        acts = music_actions(tb_sec1.section1(os.path.join(bze_dir, f)))
        ns = sorted({a[3] for a in acts})
        files = ", ".join(f"{n} -> {track_file(n, track_dir)}" for n in ns) or "silent (no action 126)"
        src = sorted({f"chunk {t:02x} ent {e} blk {b}" for t, e, b, _ in acts})
        print(f"{f:14} {files:60} [{'; '.join(src)}]")


def selftest():
    sec1 = bytes([0x2C, 0x09, 0x4D]) + struct.pack("<H", 10) + bytes([0x53]) + struct.pack("<Hhh", 126, 26, 0) \
        + bytes([0x2D, 0x2E])
    assert music_actions(sec1) == [(0x09, None, 10, 26)]
    assert track_file(26) == "Track25.xa" and track_file(0) is None and track_file(1) is None
    print("selftest ok")


if __name__ == "__main__":
    selftest() if len(sys.argv) < 2 else main(*sys.argv[1:3])
