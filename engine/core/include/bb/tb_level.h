// Time Busters (bblit2.exe) level BZE -> bb::Scene. Specs: docs/formats/TB_LEVELS.md (section 1),
// docs/formats/MODELS.md §10 (octree world/sky models, actor models), TEXTURES.md.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bb {

struct TbTag {
    uint8_t id = 0;
    uint32_t offset = 0;             // of the tag byte within section 1
    std::span<const uint8_t> data;   // fixed-size payload; views the buffer given to parseTbScript
};

struct TbChunk {
    uint8_t type = 0;
    uint32_t offset = 0;   // of the leading 0x2c
    uint32_t section = 3;  // BZE section id that {offset, size} payloads refer to: 3, the language section
                           // after command 0x4a, 4 after command 0x2b (TB_LEVELS.md §2)
    std::vector<TbTag> tags;
    std::span<const uint8_t> tag(uint8_t id) const;  // payload of the first tag `id`, empty if absent
};

struct TbScript {
    std::vector<TbChunk> chunks;
    uint32_t end = 0;  // offset of the 0x2e end byte
};

// Strict walk of TB section 1 (TB_LEVELS.md §2-3): throws FormatError with the offset of any byte the game
// would silently skip (unknown top-level byte, chunk type or tag), a 0x2b before the 0x3a/0x3d pool sizes,
// or truncation. Tag 0x62 has no handler and no payload. `languageSection` = section id command 0x4a loads
// (configured language 0 -> 6).
TbScript parseTbScript(std::span<const uint8_t> section1, uint32_t languageSection = 6);

// Textures (chunk 0x28 tag 0x29, id = low 12 bits), world octrees (chunk 0x20; trees A+B drawn, far-LOD tree C
// only decoded, as at DETAILS 2), octree sky domes (chunk 0x22 tag 0x24, placed by the entity that lists them,
// else at the origin: Inferred), single-object actor props in rest pose (placement Inferred, as LiT) and the
// player start (entity id 2 = Bugs, identity Inferred). Multi-object actors are counted in stats.skippedActors,
// not drawn. Face ids without a TIM resolve through texture links (entity tag 0x0b -> Scene::textureAnims,
// Mesh::textureAnim; TEXTURE_GAP.md); ids that resolve to nothing are not drawn, as in the original.
// Throws FormatError. `brightness` = config.pc +0xe0 (tbAdjustColor).
Scene buildTbScene(const BzeFile& bze, const std::string& level, double brightness = 1.0);

// Texture load 0x42d140 / 0x42df70 -> 0x407d30 (0x408270 / 0x407fe0): every CLUT colour (TIM modes 0 / 1; direct
// colour TIMs are left alone) goes RGB/255 -> HSI, intensity × brightness (the Video page slider, 0.25 + 1.75 v,
// 1 by default) clamped to 1, hue / saturation unchanged (config +0xd0 / +0xd8 = 0 / 1), back to RGB clamped to
// 0..1, ×255 truncated, in the exe's double arithmetic (also at brightness 1, where most channels drop by 1).
void tbAdjustColor(uint8_t* rgb, double brightness);

} // namespace bb
