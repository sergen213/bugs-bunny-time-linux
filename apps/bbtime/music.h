// Music: collection-menu tracks and LiT / TB level tracks (docs/formats/AUDIO_XA.md, docs/re/GAMEPLAY_TB.md §6).
#pragma once
#include "bb/scene.h"

#include <filesystem>

namespace bb { class AudioDevice; }

namespace bbtime {

// Track for the collection menu while `game` is highlighted (gameRoot = that game's install root, contains Datas/).
std::filesystem::path menuTrack(bb::GameId game, const std::filesystem::path& gameRoot);
// LiT PlayMusic(n) 0x41d630: n = 0 silence, else Datas/track/Track<n>.xa (LitWorld::music / musicNext).
std::filesystem::path litLevelTrack(const std::filesystem::path& gameRoot, int n);
// TB PlayMusic(n) 0x4291e0: n <= 1 silence, else Datas/TRACK/Track(n-1).xa, Track14.xa when that file is missing.
std::filesystem::path tbLevelTrack(const std::filesystem::path& gameRoot, int n);

// Call once per frame with the wanted track (empty = silence). A new path restarts; the same path keeps streaming.
// At its end `next` follows (looping); empty or equal `next` = the track loops. paused: the stream holds its position
// (LiT 0x41d5c0). The OpenAL device opens on the first non-empty track, so --mute (always empty) stays silent.
void playMusic(const std::filesystem::path& track, const std::filesystem::path& next = {}, bool paused = false);
// The shared device (opened on demand, nullptr if OpenAL has no device) for sound effects.
bb::AudioDevice* audioDevice();

} // namespace bbtime
