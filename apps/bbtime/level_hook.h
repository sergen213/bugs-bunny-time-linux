// Single seam between the collection app and the format decoders.
// LiT and TB: decoded level (Scene::placeholder = false). Missing file or decode error: synthetic
// placeholder scene with the reason in Scene::notes. Enabled mods are applied to decoded levels (mods::apply).
#pragma once
#include "bb/bze.h"
#include "bb/mods.h"
#include "bb/scene.h"

#include <filesystem>
#include <string>

namespace bb {

namespace game { class Sfx; }

// Mods (bb/mods.h mods::active(), docs/MODDING.md); every hook below is a no-op without enabled mods.
// Mod sounds: sounds/<game>/<level>/<index>.wav replaces bank blob <index> (0-based), <level>_speech/ the speech bank.
void applySoundMods(game::Sfx& sfx, GameId game, const std::string& level);
// Mod music: music/<game>/<track stem>.ogg|wav for an original .xa track path, else the track itself.
std::filesystem::path modMusic(GameId game, const std::filesystem::path& track);

// gameRoot = the game's install root (contains Datas/); level = BZE file stem (any case, e.g. "L01A").
// bzeOut (optional) receives the parsed container when the level decoded (play mode builds its simulation from it).
// tbBrightness: TB texture brightness (bb::tbAdjustColor, settings.ini tb_brightness).
Scene loadLevelScene(GameId game, const std::filesystem::path& gameRoot, const std::string& level, BzeFile* bzeOut = nullptr,
                     double tbBrightness = 1.0);
// LiT loading screen (0x430e60 loads it, 0x423bd0 draws it while the next level streams in): the loading BZE's
// section 3 is one 512x512 15-bit TIM. stem e.g. "L_SARUN_0" (table name + "_" + language) or "LOADING".
// Empty texture if the file is missing or does not decode.
Texture loadLitLoadingImage(const std::filesystem::path& gameRoot, const std::string& stem);

} // namespace bb
