#include "bb/game/tb_collision.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bb::game {
namespace {

struct Rd {
    std::span<const uint8_t> b;
    void need(size_t p, size_t n) const {
        if (p > b.size() || b.size() - p < n)
            throw FormatError(std::format("tb collision: {} bytes at 0x{:x} past end 0x{:x}", n, p, b.size()));
    }
    uint16_t u16(size_t p) const { need(p, 2); return uint16_t(b[p] | b[p + 1] << 8); }
    int16_t s16(size_t p) const { return int16_t(u16(p)); }
    uint32_t u32(size_t p) const { return u16(p) | uint32_t(u16(p + 2)) << 16; }
};

int64_t tdiv(int64_t a, int64_t b) { return a / b; }  // C++ truncates toward zero (tb_ground.py tdiv)
int64_t dot3(const std::array<int16_t, 3>& n, int64_t x, int64_t y, int64_t z) { return n[0] * x + n[1] * y + n[2] * z; }

} // namespace

bool tbQueryPasses(uint32_t f, uint32_t q) {
    if ((f & 0xf136) == 0) return true;
    return (!(f & 0x100) || (q & 0x400)) && (!(f & 0x10) || (q & 2)) && (!(f & 0x20) || (q & 4)) &&
           (!(f & 4) || (q & 8)) && (!(f & 2) || (q & 0x100)) && (!(f & 0xf000) || (q & (f >> 8) & 0xf0));
}

bool tbMoverPasses(uint32_t f, uint32_t actor10) {
    if (f & 0x100) return false;
    if (f & 0xf000) return (actor10 >> 16) & f & 0xf000;
    return !(f & 4);
}

TbCollision TbCollision::parse(std::span<const uint8_t> block) {
    TbCollision m;
    m.b_.assign(block.begin(), block.end());
    const Rd in{m.b_};
    const uint32_t soff = in.u32(4), ns = in.u32(8), voff = in.u32(0xc), poff = in.u32(0x10);
    const int32_t h = int32_t(in.u32(0x1c) >> 1);
    m.half = h;
    m.centre = {in.s16(0x14) + h, in.s16(0x16) - h, in.s16(0x18) + h};  // 0x445d00: corner + (h, -h, h)
    m.poff_ = poff;
    m.soff_ = soff;
    if (voff < poff || soff < voff) throw FormatError("tb collision: table order");
    for (uint32_t i = 0; i < ns; ++i) {
        const size_t p = soff + 12 * i;
        m.surfaces.push_back({in.u16(p), in.u16(p + 2), in.u16(p + 4), in.u16(p + 6), in.u32(p + 8)});
    }
    for (uint32_t p = voff; p + 8 <= soff; p += 8) m.verts.push_back({in.s16(p), in.s16(p + 2), in.s16(p + 4)});
    m.keyPoly_.assign((voff - poff) / 4 + 1, -1);
    for (uint32_t p = poff; p < voff;) {
        TbPoly pl;
        pl.n = {in.s16(p), in.s16(p + 2), in.s16(p + 4)};
        pl.surf = in.u16(p + 6);
        pl.d = int32_t(in.u32(p + 8));
        pl.c = {in.s16(p + 0xc), in.s16(p + 0xe), in.s16(p + 0x10)};
        pl.r = in.s16(p + 0x12);
        pl.nv = in.u16(p + 0x16) & 0x7fff;
        pl.edge = uint32_t(m.edges.size());
        pl.off = p;
        if (pl.surf >= ns) throw FormatError(std::format("tb collision: polygon at 0x{:x} surface {} >= {}", p, pl.surf, ns));
        for (uint16_t k = 0; k < pl.nv; ++k) {
            const size_t q = p + 0x18 + 8 * k;
            TbEdge e{{in.s16(q), in.s16(q + 2), in.s16(q + 4)}, in.u16(q + 6)};
            if (e.v >= m.verts.size()) throw FormatError(std::format("tb collision: edge vertex {} out of range", e.v));
            m.edges.push_back(e);
        }
        m.keyPoly_[(p - poff) / 4] = int32_t(m.polys.size());
        m.polys.push_back(pl);
        p += 0x18 + 8 * uint32_t(pl.nv);
    }
    return m;
}

TbCollision TbCollision::fromLevel(const BzeFile& bze) {
    const int i1 = bze.find(1);
    if (i1 < 0) throw FormatError("tb collision: no section 1");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    for (const TbChunk& ch : parseTbScript(s1).chunks) {
        const auto t = ch.tag(0x36);
        if (ch.type != 0x35 || t.empty()) continue;
        const int is = bze.find(ch.section);
        if (is < 0) throw FormatError(std::format("tb collision: section {} absent", ch.section));
        const std::vector<uint8_t> sec = bze.payload(size_t(is));
        const Rd r{t};
        const uint32_t off = r.u32(0), size = r.u32(4);
        if (off > sec.size() || sec.size() - off < size) throw FormatError("tb collision: block outside its section");
        return parse(std::span<const uint8_t>(sec).subspan(off, size));
    }
    throw FormatError("tb collision: no chunk 0x35");
}

std::span<const uint8_t> TbCollision::leaf(int32_t node) const {
    const Rd in{b_};
    const int16_t n = in.s16(size_t(node) + 4);
    in.need(size_t(node) + 6, size_t(std::max<int16_t>(n, 0)) * 2);
    return std::span<const uint8_t>(b_).subspan(size_t(node) + 6, size_t(std::max<int16_t>(n, 0)) * 2);
}

TbCube TbCollision::locate(const V3& p) const {
    int32_t h = half;
    std::array<int32_t, 3> c = centre;
    const int32_t pv[3] = {p.x, p.y, p.z};
    for (int i = 0; i < 3; ++i)
        if (std::abs(int64_t(pv[i]) - c[size_t(i)]) > h) return {};
    const Rd in{b_};
    size_t off = 0x20;
    for (int depth = 0; in.need(off, 0x14), b_[off + 2] & 1; ++depth) {
        if (depth > 32) throw FormatError("tb collision: octree too deep");
        h >>= 1;
        int o = 0;
        if (p.x < c[0]) c[0] -= h; else c[0] += h, o |= 1;
        if (p.z < c[2]) c[2] -= h; else c[2] += h, o |= 2;
        if (p.y < c[1]) c[1] -= h, o |= 4; else c[1] += h;
        const int32_t child = int32_t(in.s16(off + 4 + 2 * size_t(o))) * 4;
        if (child == 0) return {-1, c, h};  // empty octant: the caller steps on (0x445ed0)
        off = 0x20 + size_t(child);
    }
    return {int32_t(off), c, h};
}

std::optional<TbCube> TbCollision::nextLeaf(TbCube cube, const V3& p0, const V3& dv) const {
    const int64_t P[3] = {p0.x, p0.y, p0.z}, D[3] = {dv.x, dv.y, dv.z};
    for (int guard = 0; guard < 4096; ++guard) {
        int64_t best = 0x7fffffff, comp = 0;
        for (int i : {0, 2, 1}) {  // x, z, y; ties keep the earlier axis
            const int64_t t = D[i] == 0 ? 0x7fffffff
                                        : tdiv(((D[i] < 0 ? cube.c[size_t(i)] - cube.h : cube.c[size_t(i)] + cube.h) - P[i]) * 0x1000, D[i]);
            if (t < best) best = t, comp = D[i];
        }
        if (best > 0x1000) return std::nullopt;
        const int64_t t = best + 0x14000 / std::abs(comp);
        const V3 q{int32_t(((D[0] * t) >> 12) + P[0]), int32_t(((D[1] * t) >> 12) + P[1]), int32_t(((D[2] * t) >> 12) + P[2])};
        cube = locate(q);
        if (cube.node >= 0) return cube;
        if (cube.h == 0) return std::nullopt;
    }
    return std::nullopt;
}

std::optional<TbHit> TbCollision::query(const V3& p0, const V3& dv, int32_t radius, uint32_t q, std::optional<uint32_t> mover) const {
    TbCube cube = locate(p0);
    if (cube.node < 0) {
        if (auto r = nextLeaf(cube, p0, dv)) cube = *r;
    }
    const int64_t P0[3] = {p0.x, p0.y, p0.z}, D[3] = {dv.x, dv.y, dv.z}, P1[3] = {P0[0] + D[0], P0[1] + D[1], P0[2] + D[2]};
    std::vector<bool> seen(polys.size());
    std::optional<TbHit> best;
    int64_t bestDist = 0;
    while (cube.node >= 0) {
        const auto lst = leaf(cube.node);
        for (size_t k = 0; k + 1 < lst.size(); k += 2) {
            const int32_t pi = at(uint16_t(lst[k] | lst[k + 1] << 8));
            if (pi < 0) throw FormatError("tb collision: leaf references no polygon");
            if (seen[size_t(pi)]) continue;
            seen[size_t(pi)] = true;
            const TbPoly& P = polys[size_t(pi)];
            const uint32_t f = surfaces[P.surf].flags;
            if (!(mover ? tbMoverPasses(f, *mover) : tbQueryPasses(f, q))) continue;
            const int64_t s0 = (P.d + dot3(P.n, P0[0], P0[1], P0[2])) >> 14;
            const int64_t s1 = (P.d + dot3(P.n, P1[0], P1[1], P1[2])) >> 14;
            if (!(s0 >= 0 && s1 <= 0 && s1 < s0)) continue;
            const int64_t rel[3] = {tdiv(D[0] * s0, s0 - s1), tdiv(D[1] * s0, s0 - s1), tdiv(D[2] * s0, s0 - s1)};
            const int64_t dist = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2];
            if (best && dist > bestDist) continue;
            const int64_t hit[3] = {P0[0] + rel[0], P0[1] + rel[1], P0[2] + rel[2]};
            bool inside = true;
            for (uint32_t e = P.edge; inside && e < P.edge + P.nv; ++e) {
                const TbEdge& E = edges[e];
                const V3& V = verts[E.v];
                inside = dot3(E.e, hit[0], hit[1], hit[2]) - dot3(E.e, V.x, V.y, V.z) <= int64_t(radius) << 14;
            }
            if (!inside) continue;
            bestDist = dist;
            best = TbHit{{int32_t(hit[0]), int32_t(hit[1]), int32_t(hit[2])}, pi, P.n};
        }
        if (best) break;
        if (auto r = nextLeaf(cube, p0, dv)) cube = *r;
        else break;
    }
    return best;
}

} // namespace bb::game
