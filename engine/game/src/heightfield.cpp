#include "bb/game/heightfield.h"

#include "bb/bze.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>

namespace bb::game {
namespace {

constexpr int32_t kCell = 320, kSub = 40;  // GAMEPLAY §4.2
constexpr int32_t kStepUp = 100;           // §4.3: blocked when ground > 100 above the start y

template <class T> T rd(std::span<const uint8_t> b, size_t o) {
    if (o + sizeof(T) > b.size()) throw FormatError(std::format("heightfield: read past end at 0x{:x}", o));
    T v;
    std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}

int32_t floorDiv(int32_t a, int32_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

bool inside(const Zone& r, const V3& p) {
    return r.yTop < p.y && p.y <= r.yBottom && r.minX <= p.x && p.x < r.minX + int32_t(r.sizeX) && r.minZ <= p.z &&
           p.z < r.minZ + int32_t(r.sizeZ);
}
bool insideXZ(const Zone& r, int32_t x, int32_t z) {
    return r.minX <= x && x < r.minX + int32_t(r.sizeX) && r.minZ <= z && z < r.minZ + int32_t(r.sizeZ);
}

// Byte -> height step used by the trace (§4.3): 0x7e counts as y_bottom, positive slide bytes as their negation,
// 0x7f stays +127 (only the wall rule blocks it).
int traceHeight(int b) { return b == 0x7e ? 0 : (b > 0 && b < 0x7e) ? -b : b; }

// Fills r from the sub-cell byte b of zone z (the shared tail of 0x436c20 / 0x434cd0).
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

void Heightfield::trace(V3& p, int32_t& dx, int32_t& dz, Ground& r) const {
    const Zone* cur = findZone(p, r.zone);
    if (!cur) {  // start outside every zone: delta unchanged (0x434cd0 returns 0 early)
        r = Ground{.raw = 1, .kind = KindNoZone, .zone = -1};
        return;
    }
    const int32_t x0 = p.x, z0 = p.z, y0 = p.y, ex = x0 + dx, ez = z0 + dz;
    const bool diag = dx != 0 && dz != 0;
    int32_t x = x0, z = z0, entryX = x0, entryZ = z0;
    int32_t cx = floorDiv(x0, kSub), cz = floorDiv(z0, kSub);  // current sub-cell (grid walk, monotonic)
    const Zone* passZone = cur;
    int passByte = 0;
    uint16_t passFlags = 0;
    bool first = true;
    for (;;) {
        if (!insideXZ(*cur, x, z)) {
            cur = findZone({x, y0, z}, cur->id);
            if (!cur) {  // the path leaves all zones: stop at the last passable sub-cell's entry point
                dx = entryX - x0;
                dz = entryZ - z0;
                p.y = passZone->yBottom;
                fill(*passZone, passByte, passFlags, r);
                if (r.kind == KindWall) r.kind = KindFloor;
                return;
            }
        }
        uint16_t fl = 0;
        const int b = subcell(*cur, x, z, &fl);
        const bool blocked = (b == 0x7f && !first) || traceHeight(b) * int32_t(cur->hscale) + cur->yBottom - y0 < -kStepUp;
        if (blocked && !first) {
            // §4.3: delta clamped to where the path entered the last passable sub-cell (never longer than asked).
            const int32_t nx = entryX - x0, nz = entryZ - z0;
            if (dx <= 0 ? dx < nx : nx < dx) dx = nx;
            if (dz <= 0 ? dz < nz : nz < dz) dz = nz;
            p.y = passZone->yBottom;
            fill(*passZone, passByte, passFlags, r);
            r.kind = uint16_t((diag ? KindDiagonal : 0) | KindBlocked | (passByte == 0x7e ? KindOpen : KindFloor) |
                              (passByte > 0 && passByte < 0x7e ? KindSlide : 0));
            return;
        }
        // ponytail: a start sub-cell > 100 above y0 (only possible mid-air) is accepted as passable here.
        entryX = x;
        entryZ = z;
        passZone = cur;
        passByte = b;
        passFlags = fl;
        first = false;
        // Next sub-cell along the segment (negative moves enter at boundary - 1, as the exe does). The entry point
        // uses the exe's Q12 slope and is clamped into the sub-cell so rounding never walks backwards.
        const int32_t bx = dx > 0 ? (cx + 1) * kSub : cx * kSub - 1;
        const int32_t bz = dz > 0 ? (cz + 1) * kSub : cz * kSub - 1;
        const bool hx = dx != 0 && (dx > 0 ? bx <= ex : bx >= ex);
        const bool hz = dz != 0 && (dz > 0 ? bz <= ez : bz >= ez);
        if (!hx && !hz) break;
        bool stepX = hx, stepZ = hz;
        if (hx && hz) {
            const int64_t tx = int64_t(std::abs(bx - x0)) * std::abs(dz), tz = int64_t(std::abs(bz - z0)) * std::abs(dx);
            stepX = tx <= tz;
            stepZ = tz <= tx;
        }
        if (stepX) cx += dx > 0 ? 1 : -1;
        if (stepZ) cz += dz > 0 ? 1 : -1;
        x = stepX ? bx : diag ? x0 + int32_t((int64_t(bz - z0) * ((int64_t(dx) << 12) / dz)) >> 12) : x;
        z = stepZ ? bz : diag ? z0 + int32_t((int64_t(bx - x0) * ((int64_t(dz) << 12) / dx)) >> 12) : z;
        x = std::clamp(x, cx * kSub, cx * kSub + kSub - 1);
        z = std::clamp(z, cz * kSub, cz * kSub + kSub - 1);
    }
    p.y = passZone->yBottom;
    fill(*passZone, passByte, passFlags, r);
}

} // namespace bb::game
