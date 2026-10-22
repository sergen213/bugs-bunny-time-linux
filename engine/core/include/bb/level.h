// LiT level BZE -> bb::Scene. Specs: docs/formats/{TEXTURES,MODELS}.md, docs/re/GAMEPLAY.md §6. TB: bb/tb_level.h.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bb {

// LiT: section-3 textures (chunk 0x29), world models (chunk 0x20, one mesh group per cell/texture/blend),
// TOD-animated actors (entity tag 0x0f lists a role-7 model and a role-4 skeleton: Scene::actorModels /
// Scene::actors, posed by bb/actor.h), other placed static props (single-object model; placement Inferred) and
// the player start (entity with tag 0x13 == 1). Face texture ids absent from the TIM list resolve through texture
// links (Scene::textureAnims, TEXTURE_GAP.md); ids that are neither are not drawn and counted in Scene::stats.
// Throws FormatError on malformed data.
Scene buildLitScene(const BzeFile& bze, const std::string& level);

// One 16-byte type-0x64 record of a sprite-sequence asset (chunk 0x22 tag 0x40, MODELS.md §8): flags (8 = semi-
// transparent), tpage (ABR bits 5-6), texel origin u, v, texture id, texel size w x h. Records in object/group order:
// the frames the sprite paths (0x438770 / 0x438830) index.
struct SpriteRecord {
    uint8_t flags = 0;
    uint16_t tpage = 0;
    uint8_t u = 0, v = 0;
    uint16_t tex = 0;
    int16_t w = 0, h = 0;
};
std::vector<SpriteRecord> spriteRecords(std::span<const uint8_t> sec4, uint32_t off, uint32_t size);

} // namespace bb
