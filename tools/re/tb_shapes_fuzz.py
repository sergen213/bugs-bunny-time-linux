#!/usr/bin/env python3
"""Differential check of the TB collision primitives (engine/game/src/tb_shapes.cpp) against bblit2.exe itself:
random records go through the exe's function (unicorn) and through ours (built here into a throwaway .so); the
record bytes and return codes must agree. Needs `pip install unicorn` and the exe under BBTIME_DATA (default
/mnt/1st4TB/BugsBunny/original). Usage: tb_shapes_fuzz.py [cases per function, default 20000] [function name ...]"""
import ctypes, os, random, struct, subprocess, sys
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(os.environ.get('BBTIME_DATA', '/mnt/1st4TB/BugsBunny/original'), 'tb/Datas/BIN/bblit2.exe')

# name: (exe address, record size, field kinds by byte offset: v = s16 vector, r = s32 radius, h = s16 radius,
#        m = s16 tmax, returns: 'code' exact, 'bool' nonzero, 'int', None)
FUNCS = {
    'sphereCast': (0x43cb60, 0x28, {0: 'v', 8: 'v', 0x10: 'r'}, 'code'),
    'cylinderCast': (0x43c420, 0x28, {0: 'v', 8: 'v', 0x10: 'v', 0x24: 'r'}, 'code'),
    'capsulePush': (0x449750, 0x1c, {0: 'v', 8: 'v', 0x10: 'r'}, 'bool'),
    'capsuleCast': (0x449960, 0x30, {0: 'v', 8: 'v', 0x10: 'v', 0x18: 'h', 0x1a: 'm'}, 'bool'),
    'boxCast': (0x43cdf0, 0x30, {0: 'e', 8: 'v', 0x10: 'v', 0x18: 'h'}, 'bool'),
    'boxPush': (0x449bb0, 0x1c, {0: 'v', 8: 'e', 0x10: 'r'}, 'bool'),
    'lineNearest': (0x43bfb0, 0x24, {0: 'v', 8: 'v', 0x10: 'v'}, 'int'),
    'slabCast': (0x43c7e0, 0x38, {0: 'v', 8: 'v', 0x10: 'v', 0x18: 'v', 0x20: 'r'}, 'code'),
    'segmentGap': (0x43cfa0, 0x24, {0: 'v', 8: 'v', 0x10: 'v'}, None),
    'segmentPush': (0x44a1a0, 0x24, {0: 'v', 8: 'v', 0x10: 'v', 0x18: 'r'}, 'bool'),
    'segmentCast': (0x44a290, 0x38, {0: 'v', 8: 'v', 0x10: 'v', 0x18: 'v', 0x20: 'h', 0x22: 'm'}, 'bool'),
    'segmentReach': (0x447240, 0x14, {0: 'v', 8: 'v', 0x10: 'r'}, 'bool'),
    'boxSweep': (0x449d20, 0x30, {0: 'v', 8: 'e', 0x10: 'v', 0x18: 'h', 0x1a: 'm'}, 'bool'),
    'boxCapsulePush': (0x44a9b0, 0x24, {0: 'e', 8: 'v', 0x10: 'v', 0x18: 'r'}, 'bool'),
    'boxCapsuleCast': (0x44b010, 0x38, {0: 'e', 8: 'v', 0x10: 'v', 0x18: 'v', 0x20: 'h', 0x22: 'm'}, 'bool'),
    'lineGap': (0x43d830, 0x2c, {0: 'v', 8: 'v', 0x10: 'v'}, 'int'),
}
# Sweep tests (engine/game/src/tb_sweep.cpp): (exe address, our name) called as f(work, shape record, scratch).
SWEEPS = {
    'sweepSphere': 0x44c840, 'sweepCapsule': 0x44c9b0, 'sweepBox': 0x44cb50,
    'moverSphere': 0x440c10, 'moverCapsule': 0x440e70, 'moverBox': 0x441080,
    'boxSphere': 0x43d9f0, 'boxCapsule': 0x43dc50, 'boxBox': 0x43eaf0,
}
BOX_MOVER = ('boxSphere', 'boxCapsule', 'boxBox')
# Work fields: centre +0x34, motion +0x54, capA +0x8c, capB +0x94 (vectors), t +4, mover radius +0x14, radius +0xf0,
# best +0xf4. Record: centre +0x10, radius +0x18, capStart +0x1c, capAxis +0x2c, capRadius +0x34, half +0x58,
# rot +0x60, inv +0x80.
WORK_SPEC = {0x34: 'v', 0x54: 'v', 0x8c: 'v', 0x94: 'v', 4: 'm', 0x14: 'h', 0xf0: 'h'}
REC_SPEC = {0x10: 'v', 0x18: 'r', 0x1c: 'v', 0x2c: 'v', 0x34: 'r', 0x58: 'e'}
WORK, SHAPE = 0x3e02000, 0x3e03000
REC, SCRATCH, STACK, RET = 0x3e00000, 0x3e01000, 0x3f00000, 0x3ff0000

# The primitives' named scratch words, loaded from the exe's scratch block (`scr`, as it was before the exe's call)
# at their place in it: both sides start from the same stale words, whatever earlier (or faulted) calls left.
SCRATCH_ARGS = {
    'boxCast': ('int16_t a = s16At(scr, 0x1e);', 'a'),
    'slabCast': ('int32_t a = s32At(scr, 4);', 'a'),
    'segmentPush': ('SVec a;', 'a'),
    'segmentCast': ('int32_t a = s32At(scr, 0x3c);', 'a'),
    'boxSweep': ('BoxSweepScratch a{s32At(scr, 0x34), s16At(scr, 0x1e)};', 'a'),
    'boxCapsuleCast': ('int32_t a = s32At(scr, 0x8c);', 'a'),
}
SHIM = """#include "tb_shapes.h"
#include <cstring>
using namespace bb::game::tbm;
namespace {
int32_t s32At(const uint8_t* p, int o) { int32_t v; std::memcpy(&v, p + o, 4); return v; }
int16_t s16At(const uint8_t* p, int o) { int16_t v; std::memcpy(&v, p + o, 2); return v; }
}
extern "C" int call(int w, void* r, const uint8_t* scr) {
  switch (w) {
""" + ''.join(
    f'  case {i}: {{ {SCRATCH_ARGS.get(n, ("", ""))[0]} return int({n}(*static_cast<{n[0].upper() + n[1:]}*>(r)'
    f'{", " + SCRATCH_ARGS[n][1] if n in SCRATCH_ARGS else ""})); }}\n' for i, n in enumerate(FUNCS)) + '  }\n  return 0;\n}\n'
# The work block and the shape record keep the exe's layout for their numeric fields; their links follow it.
SHIM += """#include "tb_sweep.h"
namespace {
Sweep fromExe(const void* p) { Sweep s; std::memcpy(static_cast<void*>(&s), p, kSweepExeSize); return s; }
void toExe(const Sweep& s, void* p) { std::memcpy(p, static_cast<const void*>(&s), kSweepExeSize); }
}
extern "C" int sweep(int w, void* work, void* rec, const uint8_t* scr) {
  Sweep s = fromExe(work);
  s.scratch.camBox = {s32At(scr, 0x54), s16At(scr, 0x3e)};
  s.scratch.boxSphere = {s32At(scr, 0x74), s16At(scr, 0x5e)};
  s.scratch.capsuleSlabT = s32At(scr, 0x8c), s.scratch.boxSlabT = s32At(scr, 0xdc), s.scratch.boxCapSlabT = s32At(scr, 0xd4);
  ShapeRec r;
  std::memcpy(static_cast<void*>(&r), rec, 0xa0);
  int out = 0;
  switch (w) {
""" + ''.join(f'  case {i}: out = int({n}(s, r)); break;\n' for i, n in enumerate(SWEEPS)) + """  }
  toExe(s, work), std::memcpy(rec, static_cast<const void*>(&r), 0xa0);
  return out;
}
// header: the exe's collision header (32-bit addresses), slots: 8 (polygon, edge distances) pairs.
extern "C" int polyop(int w, void* work, const uint32_t* slots, const uint32_t* header, const uint8_t* scr) {
  auto at = [](uint32_t a) { return reinterpret_cast<uint8_t*>(uintptr_t(a)); };
  Collision c;
  c.surfaces = reinterpret_cast<const Surface*>(at(header[1])), c.nodes = at(header[2]), c.polys = at(header[3]);
  c.vertices = reinterpret_cast<const SVec4*>(at(header[5]));
  const PolyWorld world{c};
  PolySlot s[8];
  for (int i = 0; i < 8; ++i) s[i] = {reinterpret_cast<const Poly*>(at(slots[2 * i])), reinterpret_cast<const int32_t*>(at(slots[2 * i + 1]))};
  if (w == 2) return int(polyPush(*static_cast<PolyPush*>(work), s[0], world));
  Sweep sw = fromExe(work);
  BoxPolyScratch bp;  // 0x43f060's words: num +0x68, den +0x6c, normal +0xe4
  bp.num = s32At(scr, 0x68), bp.den = s32At(scr, 0x6c);
  std::memcpy(&bp.normal, scr + 0xe4, 6);
  int out = 0;
  switch (w) {
  case 0: out = int(polySweep(sw, s[0], world)); break;
  case 1: applyPush(sw, s, world); break;
  case 3: out = int(pointPoly(sw, s[0], world)); break;
  case 4: out = int(capsulePolyPush(sw, s[0], world)); break;
  case 5: out = int(capsulePoly(sw, s[0], world)); break;
  case 6: out = int(boxPoly(sw, s[0], world, bp)); break;
  }
  toExe(sw, work);
  return out;
}
"""
# Polygon tests: (exe address, polyop index, args) with a synthetic collision block at FIX in both processes.
POLYS = {'polySweep': (0x44bfd0, 0), 'applyPush': (0x443bf0, 1), 'polyPush': (0x44bcc0, 2), 'pointPoly': (0x44b640, 3),
         'capsulePolyPush': (0x4412b0, 4), 'capsulePoly': (0x4418a0, 5), 'boxPoly': (0x43f060, 6)}
FIX = 0x3e20000


def build():
    src, lib = '/tmp/tb_shapes_fuzz.cpp', '/tmp/tb_shapes_fuzz.so'
    open(src, 'w').write(SHIM.replace('int(segmentGap(*static_cast<SegmentGap*>(r)))', '(segmentGap(*static_cast<SegmentGap*>(r)), 0)'))
    subprocess.check_call(['g++', '-std=c++2c', '-O1', '-fwrapv', '-shared', '-fPIC', '-I', f'{ROOT}/engine/game/include', '-I',
                           f'{ROOT}/engine/game/src', f'{ROOT}/engine/game/src/tb_shapes.cpp',
                           f'{ROOT}/engine/game/src/tb_sweep.cpp', src, '-o', lib])
    return ctypes.CDLL(lib)


def exe():
    d = open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    ns, osz = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(0x400000, 0x4000000)
    for i in range(ns):
        vs, va, rs, ra = struct.unpack_from('<IIII', d, pe + 24 + osz + 40 * i + 8)
        mu.mem_write(base + va, d[ra:ra + (min(rs, vs) if vs else rs)])
    return mu


def value(kind, rng):
    big = rng.random() < 0.1
    if kind == 'r':
        return rng.randint(1, 6000 if big else 900)
    if kind == 'h':
        return rng.randint(1, 900)
    if kind == 'm':
        return rng.choice([0x1000, rng.randint(0, 0x1000)])
    lim = 20000 if big else 3000
    return rng.randint(-lim, lim)


def record(spec, size, rng):
    b = bytearray(size)
    vecs = []
    for off, kind in sorted(spec.items()):
        if kind in 've':
            r = rng.random()
            if vecs and r < 0.15:  # parallel / opposite / equal to an earlier vector
                k = rng.choice([1, -1, 2, -2])
                v = [max(-32768, min(32767, c * k)) for c in rng.choice(vecs)]
            elif r < 0.2:
                v = [0, 0, 0]
            elif r < 0.3:  # axis aligned
                v = [0, 0, 0]
                v[rng.randrange(3)] = value(kind, rng)
            else:
                v = [value(kind, rng) for _ in range(3)]
            if kind == 'e':
                v = [min(abs(c), 32767) for c in v]
            vecs.append(v)
            struct.pack_into('<3h', b, off, *v)
        elif kind == 'r':
            struct.pack_into('<i', b, off, value(kind, rng))
        else:
            struct.pack_into('<h', b, off, value(kind, rng))
    return bytes(b)


def matrix(rng):
    return struct.pack('<9hh3i', *[rng.randint(-4096, 4096) for _ in range(9)], 0, 0, 0, 0)


def rotation(rng):
    """A Q12 rotation (or, sometimes, any matrix) and its transpose, as PsyQ MATRIX bytes."""
    import math
    if rng.random() < 0.2:
        m = [rng.randint(-4096, 4096) for _ in range(9)]
    else:
        a, b, c = (rng.uniform(-math.pi, math.pi) for _ in range(3))
        ca, sa, cb, sb, cc, sc = math.cos(a), math.sin(a), math.cos(b), math.sin(b), math.cos(c), math.sin(c)
        r = [[cb * cc, -cb * sc, sb], [sa * sb * cc + ca * sc, -sa * sb * sc + ca * cc, -sa * cb],
             [-ca * sb * cc + sa * sc, ca * sb * sc + sa * cc, ca * cb]]
        m = [int(round(r[i][j] * 4096)) for i in range(3) for j in range(3)]
    t = [m[3 * j + i] for i in range(3) for j in range(3)]
    return m, struct.pack('<9hh3i', *m, 0, 0, 0, 0), struct.pack('<9hh3i', *t, 0, 0, 0, 0)


def box_mover(work, rng):
    """The box mover's frame (+0xf8 / +0x118), half extents (+0x14) and scaled world axes (+0xcc) in a work block."""
    m, rot, inv = rotation(rng)
    half = [rng.randint(2, 900 if rng.random() < 0.9 else 4000) for _ in range(3)]
    struct.pack_into('<3h', work, 0x14, *half)
    work[0xf8:0x118], work[0x118:0x138] = rot, inv
    for i in range(3):
        struct.pack_into('<3h', work, 0xcc + 8 * i, *[max(-32768, min(32767, m[3 * r + i] * half[i] >> 12)) for r in range(3)])
    struct.pack_into('<h', work, 0xf0, int(sum(h * h for h in half) ** 0.5) + 1)


def sweep_case(rng, name):
    work = bytearray(record(WORK_SPEC, 0x194, rng))
    struct.pack_into('<i', work, 0xf4, rng.choice([0, 0x7fffffff, rng.randint(1, 1 << 24)]))
    shape = bytearray(record(REC_SPEC, 0xa0, rng))
    shape[0x60:0x80], shape[0x80:0xa0] = matrix(rng), matrix(rng)
    if name in BOX_MOVER:
        box_mover(work, rng)
        _, shape[0x60:0x80], shape[0x80:0xa0] = rotation(rng)
    return bytes(work), bytes(shape)


def run_sweeps(lib, mu, rng, n, names):
    bad_total = 0
    for name in names:
        bad = ran = 0
        mu.mem_write(SCRATCH, bytes(0x400))
        for _ in range(n):
            work, shape = sweep_case(rng, name)
            mu.mem_write(WORK, work), mu.mem_write(SHAPE, shape)
            mu.mem_write(STACK, struct.pack('<IIII', RET, WORK, SHAPE, SCRATCH))
            mu.reg_write(UC_X86_REG_ESP, STACK)
            scr = bytes(mu.mem_read(SCRATCH, 0x400))
            try:
                mu.emu_start(SWEEPS[name], RET)
            except UcError:
                continue
            if mu.reg_read(UC_X86_REG_EIP) != RET:
                continue
            ran += 1
            want, eax = bytes(mu.mem_read(WORK, 0x194)), mu.reg_read(UC_X86_REG_EAX)
            w, r = ctypes.create_string_buffer(work, 0x194), ctypes.create_string_buffer(shape, 0xa0)
            got = lib.sweep(list(SWEEPS).index(name), w, r, scr)
            if w.raw != want or (got != 0) != (eax != 0):
                bad += 1
                if bad <= 3:
                    diff = [hex(i) for i in range(0x194) if w.raw[i] != want[i]]
                    print(f'  {name} work {work.hex()}\n    shape {shape.hex()}\n    differ at {diff[:12]} exe {eax:#x} ours {got:#x}')
        print(f'{name}: {bad} of {ran} differ')
        bad_total += bad
    return bad_total


def unit(v, scale):
    l = sum(c * c for c in v) ** 0.5 or 1
    return [int(round(c * scale / l)) for c in v]


def polygon_block(rng, npolys):
    """A collision block at FIX (header, 4 surfaces, vertices, polygons, edge distances) and its slots."""
    b = bytearray(0xc00)
    struct.pack_into('<6I', b, 0, 0, FIX + 0x100, 0, FIX + 0x400, 0, FIX + 0x200)
    for i in range(4):
        struct.pack_into('<I', b, 0x100 + 12 * i + 8, rng.choice([0, 0, 0x200, 0x10, 0x20]))
    slots, vi = [], 0
    for p in range(npolys):
        n = rng.choice([3, 3, 4])
        c = [rng.randint(-1500, 1500) for _ in range(3)]
        u, w = unit([rng.uniform(-1, 1) for _ in range(3)], 1), unit([rng.uniform(-1, 1) for _ in range(3)], 1)
        verts = []
        for k in range(n):
            a = 2 * 3.14159 * k / n
            r = rng.randint(300, 2500)
            import math
            verts.append([int(c[j] + r * (math.cos(a) * u[j] + math.sin(a) * w[j])) for j in range(3)])
        e1 = [verts[1][j] - verts[0][j] for j in range(3)]
        e2 = [verts[2][j] - verts[0][j] for j in range(3)]
        nrm = unit([e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]], 16384)
        d = -sum(nrm[j] * verts[0][j] for j in range(3))
        poly = FIX + 0x400 + 0x80 * p
        struct.pack_into('<3hhi', b, poly - FIX, *nrm, rng.randrange(4), d)
        struct.pack_into('<h', b, poly - FIX + 0x16, n)
        cen = [sum(v[j] for v in verts) / n for j in range(3)]
        for k in range(n):
            va, vb = verts[k], verts[(k + 1) % n]
            e = [vb[j] - va[j] for j in range(3)]
            en = unit([e[1] * nrm[2] - e[2] * nrm[1], e[2] * nrm[0] - e[0] * nrm[2], e[0] * nrm[1] - e[1] * nrm[0]], 16384)
            if sum(en[j] * (cen[j] - va[j]) for j in range(3)) > 0:
                en = [-x for x in en]
            struct.pack_into('<3hh', b, poly - FIX + 0x18 + 8 * k, *en, vi)
            struct.pack_into('<3h', b, 0x200 + 8 * vi, *va)
            struct.pack_into('<i', b, 0x800 + 0x40 * p + 4 * k, -sum(en[j] * va[j] for j in range(3)))
            vi += 1
        slots.append((poly, FIX + 0x800 + 0x40 * p))
    return bytes(b), slots


def run_polys(lib, mu, rng, n, names):
    libc = ctypes.CDLL(None)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    if libc.mmap(FIX, 0x1000, 3, 0x22 | 0x100000, -1, 0) != FIX:
        sys.exit('cannot map the polygon block')
    bad_total = 0
    for name in names:
        addr, op = POLYS[name]
        bad = ran = 0
        mu.mem_write(SCRATCH, bytes(0x400))
        for _ in range(n):
            npolys = rng.randint(1, 4)
            block, slots = polygon_block(rng, npolys)
            slot_bytes = b''.join(struct.pack('<II', *s) for s in slots) + bytes(8 * (8 - npolys))
            work = bytearray(record(WORK_SPEC, 0x194, rng))
            if rng.random() < 0.5:  # the end near the start's motion
                struct.pack_into('<3h', work, 0x3c, *[max(-32768, min(32767, a + b)) for a, b in
                                                      zip(struct.unpack_from('<3h', work, 0x34), struct.unpack_from('<3h', work, 0x54))])
            struct.pack_into('<hh', work, 4, rng.randint(0, 0x1000), 0x1000)
            struct.pack_into('<hh', work, 0xf0, rng.randint(10, 700), rng.randint(0, 300))
            struct.pack_into('<i', work, 0xf4, rng.choice([0, 0x7fffffff]))
            struct.pack_into('<i', work, 8, rng.choice([0, 8]))
            struct.pack_into('<3h', work, 0x44, *[rng.choice([0, rng.randint(-300, 300)]) for _ in range(3)])
            struct.pack_into('<3h', work, 0x74, *[rng.randint(-600, 600) for _ in range(3)])
            struct.pack_into('<h', work, 0x148, npolys)
            if name == 'boxPoly':
                box_mover(work, rng)
            if name == 'polyPush':
                work = bytearray(0x194)
                struct.pack_into('<3h', work, 0, *[rng.randint(-3000, 3000) for _ in range(3)])
                struct.pack_into('<h', work, 0x10, rng.randint(10, 900))
            mu.mem_write(FIX, block), mu.mem_write(0x4d87c0, slot_bytes), mu.mem_write(0x4d83b0, struct.pack('<I', FIX))
            mu.mem_write(WORK, bytes(work))
            args = (WORK, 0x4d87c0, SCRATCH) if op != 1 else (0, WORK, 0, SCRATCH)
            mu.mem_write(STACK, struct.pack('<I', RET) + b''.join(struct.pack('<I', a) for a in args))
            mu.reg_write(UC_X86_REG_ESP, STACK)
            scr = bytes(mu.mem_read(SCRATCH, 0x400))
            try:
                mu.emu_start(addr, RET)
            except UcError:
                continue
            if mu.reg_read(UC_X86_REG_EIP) != RET:
                continue
            ran += 1
            want, eax = bytes(mu.mem_read(WORK, 0x194)), mu.reg_read(UC_X86_REG_EAX)
            ctypes.memmove(FIX, block, len(block))
            w, sl = ctypes.create_string_buffer(bytes(work), 0x194), ctypes.create_string_buffer(slot_bytes, len(slot_bytes))
            got = lib.polyop(op, w, sl, ctypes.c_void_p(FIX), scr)
            same_ret = op == 1 or (got == eax if op in (2, 4) else (got != 0) == (eax & 0xffff != 0))
            if w.raw != want or not same_ret:
                bad += 1
                if bad <= 3:
                    diff = [hex(i) for i in range(0x194) if w.raw[i] != want[i]]
                    print(f'  {name} differ at {diff[:12]} exe {eax:#x} ours {got:#x}')
        print(f'{name}: {bad} of {ran} differ')
        bad_total += bad
    return bad_total


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 20000
    names = [a for a in sys.argv[1:] if not a.isdigit()] or list(FUNCS) + list(SWEEPS) + list(POLYS)
    lib, mu, rng = build(), exe(), random.Random(14)
    bad_total = run_sweeps(lib, mu, rng, n, [a for a in names if a in SWEEPS])
    bad_total += run_polys(lib, mu, rng, n, [a for a in names if a in POLYS])
    for name in [a for a in names if a in FUNCS]:
        addr, size, spec, ret = FUNCS[name]
        which = list(FUNCS).index(name)
        bad = ran = 0
        mu.mem_write(SCRATCH, bytes(0x400))  # kept between calls, as the exe's block (stale words are read from it)
        for _ in range(n):
            rec = record(spec, size, rng)
            mu.mem_write(REC, rec)
            mu.mem_write(STACK, struct.pack('<III', RET, REC, SCRATCH))
            mu.reg_write(UC_X86_REG_ESP, STACK)
            scr = bytes(mu.mem_read(SCRATCH, 0x400))
            try:
                mu.emu_start(addr, RET)
            except UcError:
                continue  # the exe faults (division by zero): no defined result
            if mu.reg_read(UC_X86_REG_EIP) != RET:
                continue  # stopped on a fault without raising
            ran += 1
            want, eax = bytes(mu.mem_read(REC, size)), mu.reg_read(UC_X86_REG_EAX)
            buf = ctypes.create_string_buffer(rec, size)
            got = lib.call(which, buf, scr) & 0xffffffff
            ok = buf.raw == want and (ret is None or (got == eax if ret != 'bool' else (got != 0) == (eax != 0)))
            if not ok:
                bad += 1
                if bad <= 3:
                    print(f'  {name} in  {rec.hex()}\n    exe {want.hex()} {eax:#x}\n    ours {buf.raw.hex()} {got:#x}')
        print(f'{name}: {bad} of {ran} differ')
        bad_total += bad
    sys.exit(1 if bad_total else 0)


if __name__ == '__main__':
    main()
