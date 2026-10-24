#!/usr/bin/env python3
"""Read-only code-layout check of the oracle executables (adapted from BBLIT-Remaster tools/verify_exe.py, MIT,
InLineR495). Proves a bugs.exe / bblit2.exe (a Wine prefix copy, another disc) has the code layout the docs/re
addresses and the Ghidra exports assume: image base, entry, .text CRC32 and byte signatures at documented sites.

usage: verify_exe.py [EXE ...]   (default: $BBTIME_DATA or ../original, lit/Datas/bin/bugs.exe, tb/Datas/BIN/bblit2.exe)
Exit 1 on any mismatch. Never writes.
"""
import hashlib
import os
import pathlib
import struct
import sys
import zlib

# LiT bugs.exe 1.6. Sites with "bblit" come from BBLIT-Remaster src/addr.h (its patched hook sites); the others are
# bbtime's own (docs/re/RUNTIME_REFERENCE.md §37).
LIT = {
    # .text CRC32: BBLIT's reference 1.6 and this project's disc (same sites and version marker, other code bytes).
    'base': 0x400000, 'text_va': 0x401000, 'text_size': 0x5a400,
    'text_crc': {0xecffb7c9: 'BBLIT-Remaster reference 1.6', 0xc1d00325: 'bbtime oracle disc (original/, local/wine/)'},
    'version': (0x45ca00, 0x3fe5555555555555),  # double 2/3 marker of 1.6 (bblit)
    'sites': [
        ('frame tick 0x4065e0 (bblit)', 0x4065e0, '55 8b ec 83 ec 20'),
        ('QPC pacer 0x406ee0 (bblit)', 0x406ee0, '83 ec 08 53 55'),
        ('level load 0x42e7a0 (bblit)', 0x42e7a0, '81 ec 5c 03 00 00'),
        ('WM_PAINT draw slot call (bblit)', 0x40e323, 'a1 70 1a 4b 00 3b c3 74 02 ff d0'),
        ('frame build 0x40ceb0 (bblit)', 0x40ceb0, '83 ec 6c 8b 44 24 70'),
        ('actor spawn 0x448d40 (bblit)', 0x448d40, '66 8b 44 24 04'),
        ('actor free 0x447a50 (bblit)', 0x447a50, 'a1 b8 39 4b 00'),
        ('renderer reinit 0x40e790 (bblit)', 0x40e790, '8b 0d 44 f3 45 00'),
        ('quad split test 0x414c0a (bblit)', 0x414c0a, 'f6 c1 02'),
        ('actor cull x >= -0x100 (bblit)', 0x4481c7, '81 ff 00 ff ff ff'),
        ('actor cull x <= 0x100 (bblit)', 0x4481d1, '81 fa 00 01 00 00'),
        ('part cull x >= -0x100 (bblit)', 0x4484b7, '66 3d 00 ff'),
        ('part cull x <= 0x100 (bblit)', 0x4484bd, '66 3d 00 01'),
        ('41st shadow jge (bblit)', 0x44c981, '0f 8d f0 01 00 00'),
        ('41st shadow target: jmp $', 0x44cb77, 'eb fe'),
        ('fog writer reads fogDist 0x9ca83c (bblit)', 0x40cc80, 'a1 3c a8 9c 00'),
        ('activation distance cmp +0x3e (bblit)', 0x447f3a, '66 8b 51 3e 3b c2'),
        ('vertical cull h/2 0x4efb4e #1 (bblit)', 0x410070, '66 a1 4e fb 4e 00'),
        ('vertical cull h/2 0x4efb4e #2 (bblit)', 0x4100ba, '66 a1 4e fb 4e 00'),
        ('vertical cull h/2 0x4efb4e #3 (bblit)', 0x411c7d, '66 8b 0d 4e fb 4e 00'),
        ('portal top 0x62161c (bblit)', 0x410c15, 'db 05 1c 16 62 00'),
        ('portal +w/2 0x621614 (bblit)', 0x410b62, 'db 05 14 16 62 00'),
        ('portal -w/2 0x621610 (bblit)', 0x410bbe, 'db 05 10 16 62 00'),
        ('backface test 0x40fdc0 (bblit)', 0x40fdc0, '55 8b ec 83 ec 10'),
        ('PadUpdate call 0x40660b (bblit)', 0x40660b, 'e8 b0 2b 04 00'),
        ('MapJoystickInput call 0x449223 (bblit)', 0x449223, 'e8 e8 9d fb ff'),
        ('camera manual step reads 0x4b3940 (bblit)', 0x4323ce, '8b 1d 40 39 4b 00'),
    ],
}
TB = {'base': 0x400000, 'text_va': 0x401000, 'text_size': 0x80000, 'text_crc': {0xd87e2a8f: 'bbtime oracle disc (original/, local/wine/)'},
      'sites': []}  # bblit2.exe: the Ghidra export (ghidra/diff/out/bblit2.exe.c) is of this build


def pe(d):
    o = struct.unpack_from('<I', d, 0x3c)[0]
    n, opt = struct.unpack_from('<H', d, o + 6)[0], struct.unpack_from('<H', d, o + 20)[0]
    base = struct.unpack_from('<I', d, o + 24 + 28)[0]
    entry = base + struct.unpack_from('<I', d, o + 24 + 16)[0]
    secs = []
    for i in range(n):
        s = o + 24 + opt + 40 * i
        name = d[s:s + 8].rstrip(b'\0').decode(errors='replace')
        vs, rva, rs, ra = struct.unpack_from('<IIII', d, s + 8)
        secs.append((name, vs, rva, rs, ra))
    return base, entry, secs


def check(path):
    d = path.read_bytes()
    base, entry, secs = pe(d)
    lit = path.name.lower() == 'bugs.exe'
    ref = LIT if lit else TB
    print(f'{path}: {len(d)} bytes, sha256 {hashlib.sha256(d).hexdigest()}')
    print(f'  image base {base:#010x}, entry {entry:#010x}')

    def off(va):
        for _, vs, rva, rs, ra in secs:
            if base + rva <= va < base + rva + max(vs, rs):
                return ra + va - base - rva
        raise ValueError(hex(va))

    bad = int(base != ref['base'])
    if lit:
        va, want = LIT['version']
        ver = struct.unpack_from('<Q', d, off(va))[0]
        bad += ver != want
        print(('  ok   ' if ver == want else '  FAIL ') + f'version qword {ver:#x}')
    t = off(ref['text_va'])
    crc = zlib.crc32(d[t:t + ref['text_size']])
    known = ref['text_crc'].get(crc)
    bad += not known
    print(('  ok   ' if known else '  FAIL ') + f'.text crc32 {crc:#010x}: {known or "unknown build"}')
    for name, va, hexs in ref['sites']:
        want = bytes.fromhex(hexs)
        have = d[off(va):off(va) + len(want)]
        bad += have != want
        print(('  ok   ' if have == want else '  FAIL ') + f'{va:#x} {name}: {have.hex(" ")}')
    return bad


def main():
    root = pathlib.Path(os.environ.get('BBTIME_DATA') or pathlib.Path(__file__).resolve().parents[3] / 'original')
    paths = [pathlib.Path(p) for p in sys.argv[1:]] or [root / 'lit/Datas/bin/bugs.exe', root / 'tb/Datas/BIN/bblit2.exe']
    bad = sum(check(p) for p in paths)
    print('RESULT:', 'all checks passed' if not bad else f'{bad} mismatches')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
