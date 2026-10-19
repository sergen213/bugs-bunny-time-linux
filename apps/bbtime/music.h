// Music: collection-menu tracks and TB level tracks (docs/formats/AUDIO_XA.md, docs/re/GAMEPLAY_TB.md §6).
#pragma once
#include "bb/scene.h"

#include <filesystem>

namespace bb { class AudioDevice; }

namespace bbtime {

// Track for the collection menu while `game` is highlighted (gameRoot = that game's install root, contains Datas/).
std::filesystem::path menuTrack(bb::GameId game, const std::filesystem::path& gameRoot);
// TB PlayMusic(n) 0x4291e0: n <= 1 silence, else Datas/TRACK/Track(n-1).xa, Track14.xa when that file is missing.
std::filesystem::path tbLevelTrack(const std::filesystem::path& gameRoot, int n);

// Call once per frame with the wanted track (empty = silence). A new path restarts; the same path keeps streaming and
// loops at the end. The OpenAL device opens on the first non-empty track, so --mute (always empty) stays silent.
void playMusic(const std::filesystem::path& track);
// The shared device (opened on demand, nullptr if OpenAL has no device) for sound effects.
bb::AudioDevice* audioDevice();

} // namespace bbtime
