#include "bb/game/heightfield.h"

#include "bb/bze.h"

#include <array>
#include <cstring>
#include <format>

namespace bb::game {
namespace {

constexpr int32_t kCell = 320, kSub = 40;  // GAMEPLAY §4.2

template <class T> T rd(std::span<const uint8_t> b, size_t o) {
    if (o + sizeof(T) > b.size()) throw FormatError(std::format("heightfield: read past end at 0x{:x}", o));
    T v;
    std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}

bool inside(const Zone& r, const V3& p) {
    return r.yTop < p.y && p.y <= r.yBottom && r.minX <= p.x && p.x < r.minX + int32_t(r.sizeX) && r.minZ <= p.z &&
           p.z < r.minZ + int32_t(r.sizeZ);
}

// Fills r from the sub-cell byte b of zone z (GroundAt 0x436c20's tail).
void fill(const Zone& z, int b, uint16_t cellFlags, Ground& r) {
    r.rec = &z;
    r.zone = int16_t(z.id);
    r.cellFlags = cellFlags;
    r.ceiling = z.yTop;
    r.raw = 0;
    r.hoffPrev = r.hoff = 0;
    r.groundY = z.yBottom;
    if (b == 0x7e) { r.kind = KindOpen; return; }
    if (b == 0x7f) { r.kind = KindWall; return; }
    r.raw = int16_t(b);
    r.kind = KindFloor;
    if (b > 0) { b = -b; r.kind |= KindSlide; }
    r.hoff = r.hoffPrev = int16_t(int16_t(z.hscale) * b);
    r.groundY = r.hoff + z.yBottom;
}

} // namespace

Heightfield Heightfield::parse(std::span<const uint8_t> b) {
    Heightfield h;
    h.first_.fill(-1);
    const uint32_t n = rd<uint32_t>(b, 0);
    if (n > 0x10000) throw FormatError("heightfield: implausible zone count");
    for (uint32_t i = 0; i < n; ++i) {
        const size_t o = 4 + size_t(i) * 0x44;
        Zone z{rd<uint16_t>(b, o), rd<uint16_t>(b, o + 2), rd<uint16_t>(b, o + 4), rd<uint16_t>(b, o + 6),
               rd<uint16_t>(b, o + 8), rd<uint16_t>(b, o + 0xa), rd<int32_t>(b, o + 0xc), rd<int32_t>(b, o + 0x10),
               rd<int32_t>(b, o + 0x14), rd<int32_t>(b, o + 0x18), rd<uint32_t>(b, o + 0x1c), rd<uint32_t>(b, o + 0x20)};
        if (z.id >= 0x3c) throw FormatError(std::format("heightfield: zone id {} >= 0x3c", z.id));
        if (size_t(z.cellmap) + size_t(z.cols) * z.rows * 2 > b.size() || z.tiles > b.size())
            throw FormatError(std::format("heightfield: zone {} offsets outside the block", i));
        if (h.first_[z.id] < 0) h.first_[z.id] = int(i);
        h.zones_.push_back(z);
    }
    h.blk_.assign(b.begin(), b.end());
    // Every cell's tile must lie in the pool.
    for (const Zone& z : h.zones_)
        for (size_t c = 0; c < size_t(z.cols) * z.rows; ++c)
            if (z.tiles + size_t(rd<uint16_t>(b, z.cellmap + c * 2) & 0xfff) * 64 + 64 > b.size())
                throw FormatError("heightfield: tile index outside the pool");
    return h;
}

const Zone* Heightfield::findZone(const V3& p, int hint) const {
    if (hint >= 0 && hint < 0x3c && first_[size_t(hint)] >= 0)
        for (size_t i = size_t(first_[size_t(hint)]); i < zones_.size() && zones_[i].id == hint; ++i)
            if (inside(zones_[i], p)) return &zones_[i];
    for (const Zone& z : zones_)
        if (inside(z, p)) return &z;
    return nullptr;
}

int Heightfield::subcell(const Zone& z, int32_t x, int32_t zz, uint16_t* flags) const {
    const int32_t lx = x - z.minX, lz = zz - z.minZ;
    uint16_t w;
    std::memcpy(&w, blk_.data() + z.cellmap + (size_t(lz / kCell) * z.cols + size_t(lx / kCell)) * 2, 2);
    if (flags) *flags = w & 0xf000;
    return int8_t(blk_[z.tiles + size_t(w & 0xfff) * 64 + size_t(lz % kCell / kSub) * 8 + size_t(lx % kCell / kSub)]);
}

void Heightfield::groundAt(V3& p, Ground& r) const {
    const Zone* z = findZone(p, r.zone);
    if (!z) {
        r = Ground{.raw = 1, .kind = KindNoZone, .zone = -1};
        return;
    }
    uint16_t fl = 0;
    const int b = subcell(*z, p.x, p.z, &fl);
    p.y = z->yBottom;
    const int32_t ceiling = r.ceiling;
    fill(*z, b, fl, r);
    r.ceiling = ceiling;  // GroundAt leaves +0xc untouched
}

// TraceGround 0x434cd0, transcribed: the exe's scratch (param_4) fields keep their roles — a 2-slot zone copy (slot 1 is
// rewritten on every zone change, so history entries made in an earlier slot-1 zone read the new one, as in the exe), a
// 16-entry sub-cell history (bytes + slot, folded to its last four at 15), cell walk then sub-cell walk per axis
// branch, the diagonal one choosing the nearer of the next x / z sub-boundary (Q12 slopes, round(sqrt) lengths). Cell
// indices use the zone-relative position, sub-cells the absolute position % 320 (C remainders), as the exe.
void Heightfield::trace(V3& p, int32_t& dx, int32_t& dz, Ground& r) const {
    struct Slot { int16_t id = 0, hscale = 0; uint16_t cols = 0, sizeX = 0, sizeZ = 0; int32_t minX = 0, minZ = 0, y = 0, top = 0; const Zone* src = nullptr; };
    const auto copy = [](const Zone& z) {
        return Slot{int16_t(z.id), int16_t(z.hscale), z.cols, z.sizeX, z.sizeZ, z.minX, z.minZ, z.yBottom, z.yTop, &z};
    };
    const auto cellWord = [&](const Zone& z, int32_t i) -> uint16_t {
        const int64_t o = int64_t(z.cellmap) + int64_t(i) * 2;
        if (o < 0 || size_t(o) + 2 > blk_.size()) return 0;
        uint16_t w;
        std::memcpy(&w, blk_.data() + o, 2);
        return w;
    };
    const auto tileByte = [&](const Zone& z, int32_t i) -> int16_t {
        const int64_t o = int64_t(z.tiles) + i;
        return o < 0 || size_t(o) >= blk_.size() ? 0 : int16_t(int8_t(blk_[size_t(o)]));
    };
    const int32_t ox = p.x, oy = p.y, oz = p.z;
    const int16_t hint = r.zone;
    const Zone* zr = findZone(p, hint);
    r.rec = zr;
    if (!zr) {
        r.raw = 1, r.kind = 1, r.hoffPrev = 0, r.hoff = 0, r.cellFlags = 0, r.zone = -1;
        return;
    }
    std::array<Slot, 2> slots{copy(*zr), {}};
    int cur = 0;
    std::array<int16_t, 16> hist{};
    std::array<int, 16> histS{};
    int16_t n = 0, nm1 = 0;
    int16_t zid = int16_t(zr->id);  // sVar5: the current zone's id
    int32_t cx = ox, cz = oz, gx = ox, gz = oz;  // +0x60 walk point / cell origin, +0x70 last passable point
    uint16_t tile = 0;
    const auto neg = [](int16_t s) { return s == 0x7e ? int16_t(0) : s > 0 && s < 0x7e ? int16_t(-s) : s; };
    const auto push = [&](int32_t idx) {  // next history entry; returns its byte as the height test sees it
        hist[size_t(++n)] = tileByte(*zr, idx);
        histS[size_t(n)] = cur;
        const int16_t s = neg(hist[size_t(n)]);
        if (n == 15) {
            for (size_t k = 0; k < 4; ++k) hist[k] = hist[12 + k], histS[k] = histS[12 + k];
            n = 3;
        }
        return s;
    };
    const auto blocked = [&](int16_t s) {
        const Slot& S = slots[size_t(cur)];
        return (s == 0x7f && n > 1) || int32_t(s) * int32_t(uint16_t(S.hscale)) + S.y - oy < -100;
    };
    const auto enterCell = [&](int32_t idx) {
        const uint16_t w = cellWord(*zr, idx);
        r.cellFlags = w & 0xf000;
        tile = w & 0xfff;
    };
    const auto relook = [&]() {  // FUN_00436b60 at the walk point (y = start y, the caller's zone hint)
        const Zone* z = findZone({cx, oy, cz}, hint);
        if (!z) return false;
        zr = z, cur = 1, slots[1] = copy(*z), zid = int16_t(z->id);
        return true;
    };
    const auto finish = [&](const Slot& H) { r.zone = H.id, r.ceiling = H.top, r.rec = H.src; };
    // Leaving every zone: stop at the last passable point (kind 2; heights from the last two entries; the x branch
    // reports 8 on an open entry, the diagonal / z branches keep 2).
    const auto noZone = [&](bool xKind8) {
        const Slot& S = slots[size_t(cur)];
        nm1 = n > 0 ? int16_t(n - 1) : n;
        p.y = S.y;
        r.raw = hist[0], r.kind = 2;
        int16_t t = hist[size_t(nm1)];
        if (t == 0x7e) { t = 0; if (xKind8) r.kind = 8; }
        else if (t > 0) t = int16_t(-t);
        r.hoffPrev = int16_t(t * S.hscale);
        t = hist[size_t(n)];
        if (t == 0x7e) { t = 0; if (xKind8) r.kind = 8; }
        else if (t > 0) t = int16_t(-t), r.kind |= 0x20;
        r.hoff = int16_t(t * S.hscale);
        r.zone = zid, r.ceiling = S.top, r.rec = nullptr;
    };
    // Blocked: back to the previous entry; kind `k` (2 when that is entry 14), `k7e` on an open entry.
    const auto block = [&](uint16_t k, uint16_t k7e) {
        if (n > 0) --n;
        const Slot& H = slots[size_t(histS[size_t(n)])];
        p.y = H.y;
        r.raw = hist[0];
        r.kind = n == 14 ? 2 : k;
        int16_t t = hist[size_t(n)];
        if (t == 0x7e) t = 0, r.kind = k7e;
        else if (t > 0) t = int16_t(-t), r.kind |= 0x20;
        r.hoff = r.hoffPrev = int16_t(H.hscale * t);
        finish(H);
    };
    // The move done: heights of the last two entries, kind 8 when the last is open.
    const auto done = [&]() {
        nm1 = n > 0 ? int16_t(n - 1) : n;
        const Slot& H = slots[size_t(histS[size_t(n)])];
        p.y = H.y;
        r.raw = hist[0];
        if (hist[size_t(n)] == 0x7e) {
            r.kind = 8, r.hoffPrev = 0, r.hoff = 0;
        } else {
            int16_t t = hist[size_t(nm1)];
            if (t == 0x7e) t = 0, r.kind = 8;
            else { r.kind = 2; if (t > 0) t = int16_t(-t); }
            r.hoffPrev = int16_t(t * H.hscale);
            t = hist[size_t(n)];
            if (t > 0) t = int16_t(-t), r.kind |= 0x20;
            r.hoff = int16_t(t * H.hscale);
        }
        finish(H);
    };
    const auto within = [](int32_t m, int32_t t) { return m < 1 ? m < t : t < m; };  // not yet at the move's end
    int32_t mx = dx, mz = dz;

    if (mx == 0 && mz == 0) {  // LAB_00436991: the start sub-cell only
        const Slot& S = slots[0];
        enterCell((cx - S.minX) / 320 + ((cz - S.minZ) / 320) * int32_t(S.cols));
        const int32_t lx = cx % 320, lz = cz % 320;
        hist[size_t(++n)] = tileByte(*zr, lx / 40 + (lz / 40 + int32_t(tile) * 8) * 8);
        int16_t s = hist[size_t(n)];
        if (s > 0 && s < 0x7e) s = int16_t(-s);
        p.y = S.y;
        r.raw = hist[0];
        r.hoffPrev = r.hoff = 0;
        if (s == 0x7e) r.kind = 8;
        else if (s == 0x7f) r.kind = 6;
        else {
            r.kind = 2;
            int16_t t = hist[size_t(n)];
            if (t > 0) t = int16_t(-t), r.kind |= 0x20;
            r.hoff = r.hoffPrev = int16_t(t * S.hscale);
        }
        finish(S);
        return;
    }
    const int16_t stepCx = mx > 0 ? 320 : -320, stepSx = mx > 0 ? 40 : -40;
    const int16_t stepCz = mz > 0 ? 320 : -320, stepSz = mz > 0 ? 40 : -40;
    int32_t nbx = mx > 0 ? ox - ox % 320 + 320 : ox - ox % 320 - 1;  // next cell boundary +0xa0 / +0xa4
    int32_t nbz = mz > 0 ? oz - oz % 320 + 320 : oz - oz % 320 - 1;
    const auto subStart = [](int32_t l, int16_t step) { return step < 1 ? l - l % 40 - 1 : l - l % 40 + 40; };

    if (mx != 0 && mz != 0) {  // diagonal
        const int32_t kzx = (mz << 12) / mx, kxz = (mx << 12) / mz;  // +0xc8 dz/dx, +0xcc dx/dz (Q12)
        int32_t czx = ((nbz - oz) * kxz >> 12) + ox, czz = nbz;      // next cell z boundary crossing
        int32_t cxx = nbx, cxz = ((nbx - ox) * kzx >> 12) + oz;      // next cell x boundary crossing
        int16_t tx = 0, tz = 0;                                      // +0xa8 / +0xaa: walked so far
        for (;;) {
            const Slot* S = &slots[size_t(cur)];
            if (cx < S->minX || S->minX + int32_t(S->sizeX) <= cx || cz < S->minZ || S->minZ + int32_t(S->sizeZ) <= cz) {
                if (!relook()) {
                    noZone(false);
                    dx = gx - ox, dz = gz - oz;
                    return;
                }
                S = &slots[1];
            }
            enterCell((cx - S->minX) / 320 + ((cz - S->minZ) / 320) * int32_t(S->cols));
            int32_t lx = cx % 320, lz = cz % 320;
            cx -= lx, cz -= lz;
            int32_t sbx = subStart(lx, stepSx), sbz = subStart(lz, stepSz);
            int32_t zbx = ((sbz - lz) * kxz >> 12) + lx, zbz = sbz;  // next sub z boundary crossing (+0x20 / +0x28)
            int32_t xbx = sbx, xbz = ((sbx - lx) * kzx >> 12) + lz;  // next sub x boundary crossing (+0x30 / +0x38)
            for (;;) {
                const int16_t s = push(lx / 40 + (lz / 40 + int32_t(tile) * 8) * 8);
                if (blocked(s)) {
                    const int32_t gdx = gx - ox, gdz = gz - oz;
                    if (mx < 1 ? mx < gdx : gdx < mx) mx = gdx;
                    if (mz < 1 ? mz < gdz : gdz < mz) mz = gdz;
                    dx = mx, dz = mz;
                    block(0x16, 0x1c);
                    return;
                }
                gx = cx + lx, gz = cz + lz;
                tz = int16_t(int16_t(cz) - int16_t(oz) + int16_t(lz));
                tx = int16_t(int16_t(cx) - int16_t(ox) + int16_t(lx));
                const int32_t d0x = lx - zbx, d0z = lz - zbz, d1x = lx - xbx, d1z = lz - xbz;
                if (isqrt(d0x * d0x + d0z * d0z) < isqrt(d1x * d1x + d1z * d1z)) {
                    if (within(mz, tz)) {
                        lx = zbx, lz = zbz;
                        sbz += stepSz, zbz = sbz, zbx += kxz * stepSz >> 12;
                    } else {
                        tx = int16_t(mx);
                    }
                } else if (within(mx, tx)) {
                    lx = xbx, lz = xbz;
                    sbx += stepSx, xbx = sbx, xbz = (kzx * stepSx >> 12) + xbz;
                } else {
                    tz = int16_t(mz);
                }
                const int16_t ex = int16_t(int16_t(ox) + int16_t(mx)), px = int16_t(int16_t(cx) + int16_t(lx));
                const int16_t ez = int16_t(int16_t(mz) + int16_t(oz)), pz = int16_t(int16_t(cz) + int16_t(lz));
                if (mx < 0 ? px < ex : ex < px) lx += ex - px;  // never past the move's end
                if (mz < 0 ? pz < ez : ez < pz) lz += ez - pz;
                if (lx < 0 || lx > 0x13f || lz < 0 || lz > 0x13f) break;
                if (!within(mx, tx) && !within(mz, tz)) break;
            }
            // LAB_0043552b: on to the nearer next cell crossing.
            const int32_t c0x = ox - czx, c0z = oz - czz, c1x = ox - cxx, c1z = oz - cxz;
            if (isqrt(c0x * c0x + c0z * c0z) < isqrt(c1x * c1x + c1z * c1z)) {
                cx = czx, cz = czz;
                nbz += stepCz, czz = nbz, czx += kxz * stepCz >> 12;
            } else {
                cx = cxx, cz = cxz;
                nbx += stepCx, cxx = nbx, cxz += kzx * stepCx >> 12;
            }
            if (!within(mx, tx) && !within(mz, tz)) { done(); return; }
        }
    }
    if (mx != 0) {  // x only
        int16_t row = int16_t(int16_t((cz - slots[size_t(cur)].minZ) / 320) * int16_t(slots[size_t(cur)].cols));
        const int16_t subRow = int16_t(cz % 320 / 40 * 8);
        int16_t tx = 0;
        for (;;) {
            const Slot* S = &slots[size_t(cur)];
            if (cx < S->minX || S->minX + int32_t(S->sizeX) <= cx) {
                if (!relook()) {
                    noZone(true);
                    dx = gx - ox;
                    return;
                }
                S = &slots[1];
                row = int16_t(int16_t((cz - S->minZ) / 320) * int16_t(S->cols));
            }
            enterCell((cx - S->minX) / 320 + row);
            int32_t lx = cx % 320;
            cx -= lx;
            int32_t sbx = subStart(lx, stepSx);
            for (;;) {
                const int16_t s = push(lx / 40 + int32_t(tile) * 0x40 + subRow);
                if (blocked(s)) {
                    dx = gx - ox;
                    block(6, 0xc);
                    return;
                }
                tx = int16_t(int16_t(cx) - int16_t(ox) + int16_t(lx));
                gx = cx + lx;
                lx = sbx, sbx += stepSx;
                if (lx < 0 || lx > 0x13f || !within(mx, tx)) break;
            }
            cx = nbx, nbx += stepCx;
            if (!within(mx, tx)) { done(); return; }
        }
    }
    // z only
    int16_t col = int16_t((cx - slots[size_t(cur)].minX) / 320);
    const int16_t subCol = int16_t(cx % 320 / 40);
    int16_t tz = 0;
    for (;;) {
        const Slot* S = &slots[size_t(cur)];
        if (cz < S->minZ || S->minZ + int32_t(S->sizeZ) <= cz) {
            if (!relook()) {
                noZone(false);
                dz = gz - oz;
                return;
            }
            S = &slots[1];
            col = int16_t((cx - S->minX) / 320);
        }
        enterCell(((cz - S->minZ) / 320) * int32_t(S->cols) + col);
        int32_t lz = cz % 320;
        cz -= lz;
        int32_t sbz = subStart(lz, stepSz);
        for (;;) {
            const int16_t s = push(subCol + (lz / 40 + int32_t(tile) * 8) * 8);
            if (blocked(s)) {
                dz = gz - oz;
                block(6, 0xc);
                return;
            }
            tz = int16_t(int16_t(cz) - int16_t(oz) + int16_t(lz));
            gz = cz + lz;
            lz = sbz, sbz += stepSz;
            if (lz < 0 || lz > 0x13f || !within(mz, tz)) break;
        }
        cz = nbz, nbz += stepCz;
        if (!within(mz, tz)) { done(); return; }
    }
}

} // namespace bb::game
