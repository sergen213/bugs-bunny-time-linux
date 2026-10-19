// Level BZE -> bb::Scene. Specs: docs/formats/{TEXTURES,MODELS}.md.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace bb {

// LiT: section-3 textures (chunk 0x29), world models (chunk 0x20, one mesh group per cell/texture/blend),
// placed static props (entity tag 0x0f -> chunk 0x22 tag 0x24/0x40 model; placement Inferred) and the player
// start (entity with tag 0x13 == 1). Missing texture ids are kept as untextured faces and counted in
// Scene::stats. Throws FormatError on malformed data.
Scene buildLitScene(const BzeFile& bze, const std::string& level);

// TB section-1 texture list (chunk 0x28 tag 0x29): (section-3 offset, texture id = low 12 bits).
std::vector<std::pair<uint32_t, uint32_t>> tbTextureRefs(std::span<const uint8_t> section1);

// All TB level textures, named "<level>/tex/<id>". Undecodable entries are counted in stats->textureErrors.
std::vector<Texture> decodeTbTextures(const BzeFile& bze, const std::string& level, SceneStats* stats = nullptr);

} // namespace bb
