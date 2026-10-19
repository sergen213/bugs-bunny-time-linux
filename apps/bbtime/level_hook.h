// Single seam between the collection app and the format decoders.
// LiT and TB: decoded level (Scene::placeholder = false). Missing file or decode error: synthetic
// placeholder scene with the reason in Scene::notes.
#pragma once
#include "bb/scene.h"

#include <filesystem>
#include <string>

namespace bb {

// dataRoot = directory containing lit/ and tb/ install trees; level = BZE file stem (any case, e.g. "L01A").
Scene loadLevelScene(GameId game, const std::filesystem::path& dataRoot, const std::string& level);

} // namespace bb
