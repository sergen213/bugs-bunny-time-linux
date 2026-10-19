#!/usr/bin/env python3
"""Print the running original's camera as a bbtime --camera argument (engine space, radians).

usage: cam.py lit|tb      (cam.py --selftest checks the conversion)
Live eye/look-at: LiT 0x4b38c0 / 0x4b38cc (GAMEPLAY.md §5), TB view-0 block 0x4d5120 +4 / +0x10 (GAMEPLAY_TB.md §4.3),
both s32 PS1 world units. PS1 -> engine (scene.h): (x, y, z) -> (x, -y, -z); yaw/pitch as apps/bbtime/play.cpp
camera(): yaw = atan2(-d.x, -d.z), pitch = atan2(d.y, |d.xz|).
"""
import math, struct, sys

EYE = {'lit': (0x4b38c0, 0x4b38cc), 'tb': (0x4d5124, 0x4d5130)}


def to_camera(eye, look):
    e = (eye[0], -eye[1], -eye[2])
    d = (look[0] - eye[0], -(look[1] - eye[1]), -(look[2] - eye[2]))
    yaw = math.atan2(-d[0], -d[2])
    pitch = math.atan2(d[1], math.hypot(d[0], d[2]))
    return f'{e[0]},{e[1]},{e[2]},{yaw:.5f},{pitch:.5f}'


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        # looking from (0,0,1000) toward the origin in PS1 space = engine (0,0,-1000) looking +Z -> yaw pi
        assert to_camera((0, 0, 1000), (0, 0, 0)) == '0,0,-1000,3.14159,0.00000'
        # look-at above the eye (PS1 -y) -> positive pitch
        assert to_camera((0, 0, 0), (0, -100, 100)).endswith(',0.78540')
        print('cam selftest ok')
        sys.exit()
    from peek import Mem
    m = Mem(sys.argv[1])
    ea, la = EYE[sys.argv[1]]
    eye, look = (struct.unpack('<3i', b) for b in m.read_many([(ea, 12), (la, 12)]))
    print(f'eye={eye} look={look}')
    print(f'--camera {to_camera(eye, look)}')
