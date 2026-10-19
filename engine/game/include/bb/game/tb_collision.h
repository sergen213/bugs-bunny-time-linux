// Time Busters collision ("Plancher", GAMEPLAY_TB.md §1): polygon mesh + octree from section-1 chunk 0x35 tag 0x36.
// locate / nextLeaf / query are bit-exact ports of tools/re/tb_ground.py (0x445d90, 0x445ed0, 0x4463e0) including
// the first-hit-leaf quirk (§1.4 step 3); gather follows 0x446ea0. The swept-shape mover (0x4450c0 response
// 0x443f40) is an approximation: see TbCollision::sweep.
#pragma once
#include "bb/bze.h"
#include "bb/game/litmath.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bb::game {

inline constexpr int32_t kTbFloorNy = -11000;  // §1.5 step 5: contact normal.y (Q14) below this = floor

struct TbSurface { uint16_t friction = 4096, b = 4096, id = 255, d = 0; uint32_t flags = 0; };  // §1.2, 12 B
struct TbEdge { std::array<int16_t, 3> e{}; uint16_t v = 0; };  // outward edge normal (Q14) + start vertex
struct TbPoly {
    std::array<int16_t, 3> n{};  // plane normal Q14
    uint16_t surf = 0;
    int32_t d = 0;               // n.P + d = 0 (units * 0x4000)
    std::array<int16_t, 3> c{};  // bounding sphere
    int16_t r = 0;
    uint32_t edge = 0;           // first TbEdge
    uint16_t nv = 0;
};
struct TbCube { int32_t node = -1; std::array<int32_t, 3> c{}; int32_t h = 0; };  // node = leaf offset, -1 none
struct TbHit { V3 p; int32_t poly = -1; std::array<int16_t, 3> n{}; };

// 0x4463e0 query surface filter (tb_ground.py passes) and 0x446ea0 mover filter (mover_passes).
bool tbQueryPasses(uint32_t surfFlags, uint32_t q);
bool tbMoverPasses(uint32_t surfFlags, uint32_t actor10);

class TbCollision {
public:
    static TbCollision parse(std::span<const uint8_t> block);  // throws FormatError
    static TbCollision fromLevel(const BzeFile& bze);           // chunk 0x35 tag 0x36; throws FormatError

    TbCube locate(const V3& p) const;                           // 0x445d90
    std::optional<TbCube> nextLeaf(TbCube cube, const V3& p0, const V3& dv) const;  // 0x445ed0
    // Nearest polygon hit by p0 -> p0 + dv (front -> back), polygons widened by radius; filter = query flags q,
    // or the mover rule when `mover` (actor flags +0x10) is given. Actors are not tested (query flag 1).
    std::optional<TbHit> query(const V3& p0, const V3& dv, int32_t radius = 5, uint32_t q = 0,
                               std::optional<uint32_t> mover = std::nullopt) const;
    // 0x446ea0: polygons within R of c (plane distance, bounding sphere, every edge line) from the <= 15 leaves
    // touched by the sphere's box, mover-filtered, `skip` excluded; appended to out (indices into polys).
    void gather(const V3& c, int32_t R, uint32_t actor10, int32_t skip, std::vector<int32_t>& out) const;

    struct Shape { int32_t r = 10; int32_t half = 0; };  // sphere (half 0) or vertical capsule segment ±half
    struct Contact { std::array<int16_t, 3> n{}; int32_t poly = -1; };
    // Approximate swept-shape slide (§1.5 steps 4-5): de-penetration, then up to 4 earliest-contact iterations
    // removing the normal component of the remaining motion; a motion into two contact normals stops. `centre`
    // is moved in place; contacts receive every polygon touched. Not bit-exact (per-mode sweeps not transcribed).
    void sweep(V3& centre, V3 motion, Shape shape, uint32_t actor10, std::vector<Contact>& contacts) const;

    std::vector<TbSurface> surfaces;
    std::vector<TbPoly> polys;
    std::vector<TbEdge> edges;
    std::vector<V3> verts;
    std::array<int32_t, 3> centre{};
    int32_t half = 0;

private:
    std::vector<uint8_t> b_;
    uint32_t poff_ = 0;
    std::vector<int32_t> keyPoly_;  // polygon key (offset / 4) -> polys index
    std::span<const uint8_t> leaf(int32_t node) const;
    int32_t at(uint16_t key) const { return key < keyPoly_.size() ? keyPoly_[key] : -1; }
};

} // namespace bb::game
