// Single seam between the collection app and the format decoders.
// Status: Designed. The current implementation returns a PLACEHOLDER scene (Scene::placeholder = true);
// the integrator replaces the body of loadLevelScene() with the real BZE decode once bb_core provides it.
#pragma once
#include "bb/scene.h"

#include <filesystem>
#include <string>

namespace bb {

// dataRoot = directory containing lit/ and tb/ install trees; level = BZE file stem (e.g. "L01A").
Scene loadLevelScene(GameId game, const std::filesystem::path& dataRoot, const std::string& level);

} // namespace bb
