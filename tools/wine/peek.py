#!/usr/bin/env python3
"""Read memory of a running original (started by run_original.sh) through its memsrv socket.

usage: peek.py lit|tb ADDR TYPE [COUNT]
  ADDR: 0x4b38c0 | [0x4b324c]+0xe4 (u32 pointer deref, then offset; nestable: [[A]+8]+4)
  TYPE: u8 s8 u16 s16 u32 s32 f32 f64 hex
peek.py --selftest  checks the address parser without a game.
"""
import re, socket, struct, sys

RUN = '/mnt/1st4TB/BugsBunny/local/wine/run'
FMT = {'u8': 'B', 's8': 'b', 'u16': 'H', 's16': 'h', 'u32': 'I', 's32': 'i', 'f32': 'f', 'f64': 'd', 'hex': 'B'}


class Mem:
    def __init__(self, game):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(f'{RUN}/{game}.sock')
        self.f = self.s.makefile('rwb')

    def _req(self, line):
        self.f.write(line.encode() + b'\n')
        self.f.flush()
        n, = struct.unpack('<I', self.f.read(4))
        return self.f.read(n)

    def pid(self):
        return struct.unpack('<I', self._req('P'))[0]

    def read_many(self, spans):
        """spans: [(addr, len)] -> list of bytes (one round trip)."""
        data = self._req('R ' + ' '.join(f'{a} {n}' for a, n in spans))
        out, o = [], 0
        for _, n in spans:
            out.append(data[o:o + n])
            o += n
        return out

    def read(self, addr, n):
        return self.read_many([(addr, n)])[0]

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def addr(self, expr):
        return resolve(expr, self.u32)


def resolve(expr, u32):
    """'[A]+off' style expression -> int address; u32(addr) reads a pointer."""
    expr = expr.replace(' ', '')
    while '[' in expr:
        m = re.search(r'\[([^\[\]]+)\]', expr)
        expr = expr[:m.start()] + hex(u32(resolve(m.group(1), u32))) + expr[m.end():]
    return eval(expr, {'__builtins__': {}})  # only hex/int literals and +/- remain


def decode(raw, typ, count):
    if typ == 'hex':
        return raw.hex(' ')
    vals = struct.unpack('<%d%s' % (count, FMT[typ]), raw)
    return ' '.join(f'{v:.6g}' if typ[0] == 'f' else str(v) for v in vals)


def selftest():
    mem = {0x1000: 0x2000, 0x2008: 0x3000}
    assert resolve('0x4b38c0', mem.get) == 0x4b38c0
    assert resolve('[0x1000]+0xe4', mem.get) == 0x20e4
    assert resolve('[[0x1000]+8]+4', mem.get) == 0x3004
    assert decode(struct.pack('<hh', -2, 7), 's16', 2) == '-2 7'
    print('peek selftest ok')


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        selftest()
        sys.exit()
    game, expr, typ = sys.argv[1:4]
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    m = Mem(game)
    a = m.addr(expr)
    size = struct.calcsize(FMT[typ]) * count
    print(f'{a:#x}: {decode(m.read(a, size), typ, count)}')
