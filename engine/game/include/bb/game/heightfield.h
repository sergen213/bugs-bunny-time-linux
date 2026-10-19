// LiT collision heightfield: section-1 chunk 0x36 tag 0x37 -> block in decompressed section 4 (GAMEPLAY.md §4).
// 320-unit cells, shared pool of 8x8 tiles of 40-unit sub-cells; byte <= 0 floor, 1..125 slide floor, 0x7e open
// (zone below), 0x7f wall. Heights are piecewise constant per sub-cell: ground_y = y_bottom - |b| * hscale.
#pragma once
#include "bb/game/litmath.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bb::game {

struct Zone {  // 0x44-byte record (§4.2)
    uint16_t id = 0, hscale = 0, cols = 0, rows = 0, sizeX = 0, sizeZ = 0;
    int32_t minX = 0, minZ = 0, yBottom = 0, yTop = 0;
    uint32_t cellmap = 0, tiles = 0;  // block-relative
};

// Result record of GroundAt 0x436c20 / TraceGround 0x434cd0 (short[] in the exe, §4.3).
enum : uint16_t { KindNoZone = 1, KindFloor = 2, KindBlocked = 4, KindOpen = 8, KindDiagonal = 0x10, KindSlide = 0x20,
                  KindWall = 6 };
struct Ground {
    int16_t raw = 0;
    uint16_t kind = KindNoZone;
    int16_t hoffPrev = 0, hoff = 0;  // [2] / [3]: height offsets (<= 0) relative to y_bottom
    uint16_t cellFlags = 0;          // cell word & 0xf000
    int16_t zone = -1;               // in: lookup hint; out: zone id (-1 = none)
    int32_t ceiling = 0;             // +0xc zone y_top (TraceGround only)
    int32_t groundY = 0;             // +0x10 absolute ground y (GroundAt only)
    const Zone* rec = nullptr;       // +0x14
};

class Heightfield {
public:
    // Throws FormatError on a malformed block (zone id >= 0x3c traps in the exe, offsets outside the block).
    static Heightfield parse(std::span<const uint8_t> block);
    bool empty() const { return zones_.empty(); }
    const std::vector<Zone>& zones() const { return zones_; }

    // FindZone 0x436b60: y_top < y <= y_bottom, min <= x,z < min + size; hinted id's group first, then file order.
    const Zone* findZone(const V3& p, int hint) const;
    // GroundAt 0x436c20. Like the exe it overwrites p.y with the zone's y_bottom (open/wall/floor).
    void groundAt(V3& p, Ground& r) const;
    // TraceGround 0x434cd0 (XZ move by dx/dz from p, y = p.y). Clamps dx/dz when blocked; overwrites p.y with
    // the end zone's y_bottom. Implemented (approximation): sub-cell walk with the documented block rules, not
    // the exe's DDA with its 15-entry history buffer.
    void trace(V3& p, int32_t& dx, int32_t& dz, Ground& r) const;

    // Raw signed sub-cell byte at (x, z) in zone z (caller guarantees inside); cell word flags out.
    int subcell(const Zone& z, int32_t x, int32_t zz, uint16_t* flags = nullptr) const;

private:
    std::vector<uint8_t> blk_;
    std::vector<Zone> zones_;
    std::array<int, 0x3c> first_{};  // first record of each id (DAT_004b3c60), -1 = none
};

} // namespace bb::game
