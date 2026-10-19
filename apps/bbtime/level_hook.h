// Single seam between the collection app and the format decoders.
// LiT: decoded level (Scene::placeholder = false). TB: real textures shown as a labelled gallery
// (placeholder = true; TB geometry is Research Required). Missing file or decode error: synthetic
// placeholder scene with the reason in Scene::notes.
#pragma once
#include "bb/scene.h"

#include <filesystem>
#include <string>

namespace bb {

// dataRoot = directory containing lit/ and tb/ install trees; level = BZE file stem (any case, e.g. "L01A").
Scene loadLevelScene(GameId game, const std::filesystem::path& dataRoot, const std::string& level);

} // namespace bb
