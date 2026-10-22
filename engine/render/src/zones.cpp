#include "bb/render/zones.h"

#include <algorithm>
#include <array>
#include <span>

namespace bb::render {
namespace {

// 0x40fe90 rejects a portal when all its corners lie outside the same frustum plane: near = the eye plane (view
// z <= 1, 0x45c314 = 1.0), no bottom-edge test (only "any y below the top"), as in the exe.
bool outside(std::span<const Corner> pts, const glm::mat4& vp) {
    std::array<bool, 5> all{true, true, true, true, true};  // left, right, top, near, far
    for (const Corner& p : pts) {
        const glm::vec4 c = vp * glm::vec4(p[0], p[1], p[2], 1.0f);
        all[0] = all[0] && c.x < -c.w;
        all[1] = all[1] && c.x > c.w;
        all[2] = all[2] && c.y > c.w;
        all[3] = all[3] && c.w < -1.0f;
        all[4] = all[4] && c.z > c.w;
    }
    return std::ranges::any_of(all, [](bool b) { return b; });
}

} // namespace

bool walkZones(const std::vector<WorldZone>& zones, const glm::mat4& vp, const ZoneView& view, std::vector<ZoneEntry>& table,
               std::vector<uint8_t>& drawn) {
    table.clear();
    auto has = [&](int z) { return z >= 0 && size_t(z) < zones.size() && zones[size_t(z)].present; };
    // Camera actor +0x78 zone object; before the eye has a zone the exe falls back to the player's record (0x4318f0).
    const int16_t cam = has(view.camZone) ? view.camZone : view.playerZone;
    if (!has(cam)) return false;
    drawn.assign(zones.size(), 0);
    // One 0x40fe90 pass: draws `zone`; a pass at depth 0 restarts the table with entry 0 = the camera zone. Portals that
    // pass the tests add their zone once (flag = zone's depth limit +0xb5 below the walk depth; the A17 message-queue
    // exclusion 0x4b2480 is not ported). The exe's table holds 24 entries.
    auto walk = [&](int16_t zone, int depth, int maxDepth) {
        if (depth == 0) table.assign(1, {cam, false});
        drawn[size_t(zone)] = 1;
        for (const ZonePortal& p : zones[size_t(zone)].portals) {
            if (outside(p.quad, vp)) continue;
            if (std::ranges::any_of(table, [&](const ZoneEntry& e) { return e.zone == int16_t(p.to); })) continue;
            if (table.size() < 24) table.push_back({int16_t(p.to), maxDepth < depth + 1});
        }
    };
    walk(cam, 0, 1);
    int16_t skip = -1;  // sVar2: the player zone, -1 under a camera node
    if (!view.cameraNode) {
        skip = view.playerZone;
        if (view.playerZone != cam && view.playerZone != -1) {
            if (has(view.playerZone)) walk(view.playerZone, 0, zones[size_t(view.playerZone)].depth);
            table[0].zone = view.playerZone;
        }
    }
    // Breadth-first over the table: one depth generation per batch of entries added by the previous one.
    int gen = 1;
    for (size_t i = 1, snap = table.size(); i < table.size(); ++i) {
        if (i == snap) ++gen, snap = table.size();
        const ZoneEntry e = table[i];
        if (e.flagged || e.zone == skip || !has(e.zone)) continue;
        walk(e.zone, gen, zones[size_t(e.zone)].depth);
    }
    if (skip != cam && table.size() < 24) table.push_back({cam, false});
    return true;
}

} // namespace bb::render
