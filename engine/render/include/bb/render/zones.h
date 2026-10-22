// LiT zone/portal visibility (0x44c270 + walker 0x40fe90, MODELS.md §7). Pure CPU, shared by both backends.
#pragma once
#include "bb/scene.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace bb::render {

// Per-frame inputs of 0x44c270: camera pseudo-actor zone (0x4b39b4 +0x26, eye in FindZone 0x436b60), player zone
// (0x4b324c +0x26), camera-node mode (0x4b3940 & 0x300: the player-zone pass is skipped). Both < 0 = no zone view.
struct ZoneView {
    int16_t camZone = -1, playerZone = -1;
    bool cameraNode = false;
};

// Visible-zone table 0x4b3460 entry: zone id, flag word bit 0 (walk depth exceeded: neither drawn nor actor-visible).
struct ZoneEntry {
    int16_t zone = -1;
    bool flagged = false;
};

// Walks the zones from the view and fills `table` (0x4b3460 order) and `drawn` (per zone id: rendered this frame).
// viewProj = GL-style clip (z in [-w, w]) of the actual render camera, so wide / Hor+ FOVs reveal edge portals.
// Returns false (table empty) when the scene has no zones or the view names none: draw everything.
bool walkZones(const std::vector<WorldZone>& zones, const glm::mat4& viewProj, const ZoneView& view,
               std::vector<ZoneEntry>& table, std::vector<uint8_t>& drawn);

} // namespace bb::render
