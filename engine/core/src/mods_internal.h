// Shared between mods.cpp and mods_gltf.cpp.
#pragma once
#include "bb/mods.h"

#include <functional>

namespace bb::mods {

std::string textureKey(const Scene& s, const Texture& t);  // "textures/<game>/<level>/<id>", empty if not moddable
void applyModels(Scene& s, const ModSet& m);                 // models, swaps, animations, world meshes
// Checks a models/ or animations/ glTF (p = key split on '/'), against the level's originals when level() decodes one.
void validateGltf(const fs::path& file, const std::vector<std::string>& p, const std::string& rel, const Info& mod,
                  const std::function<const Scene*(const std::string& game, const std::string& level)>& level,
                  std::vector<Issue>& out);

} // namespace bb::mods
