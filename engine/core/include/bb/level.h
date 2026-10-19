// LiT level BZE -> bb::Scene. Specs: docs/formats/{TEXTURES,MODELS}.md. TB: bb/tb_level.h.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <string>

namespace bb {

// LiT: section-3 textures (chunk 0x29), world models (chunk 0x20, one mesh group per cell/texture/blend),
// placed static props (entity tag 0x0f -> chunk 0x22 tag 0x24/0x40 model; placement Inferred) and the player
// start (entity with tag 0x13 == 1). Missing texture ids are kept as untextured faces and counted in
// Scene::stats. Throws FormatError on malformed data.
Scene buildLitScene(const BzeFile& bze, const std::string& level);

} // namespace bb
