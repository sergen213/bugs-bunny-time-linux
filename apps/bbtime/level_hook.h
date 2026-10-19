// Single seam between the collection app and the format decoders.
// LiT and TB: decoded level (Scene::placeholder = false). Missing file or decode error: synthetic
// placeholder scene with the reason in Scene::notes.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <filesystem>
#include <string>

namespace bb {

// gameRoot = the game's install root (contains Datas/); level = BZE file stem (any case, e.g. "L01A").
// bzeOut (optional) receives the parsed container when the level decoded (play mode builds its simulation from it).
Scene loadLevelScene(GameId game, const std::filesystem::path& gameRoot, const std::string& level, BzeFile* bzeOut = nullptr);

} // namespace bb
