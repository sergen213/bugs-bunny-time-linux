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
// size > 0 plays only bytes [offset, offset + size) of `track`, once (a LiT speech clip, LitWorld::speechClip).
void playMusic(const std::filesystem::path& track, const std::filesystem::path& next = {}, bool paused = false,
               uint64_t offset = 0, uint64_t size = 0);
// LiT speech file 0x41e1e0: Datas/speeches/Speech<NN>.xa of config language NN (empty when this disc lacks it).
std::filesystem::path litSpeechFile(const std::filesystem::path& gameRoot, int language);
// TB speech file 0x42a030: Datas/SPEECH/<GD+0x93 as %.2d>/Speech<clip>.xa (empty when this disc lacks it: the exe then
// times the clip instead, TbWorld::speechAudio false).
std::filesystem::path tbSpeechFile(const std::filesystem::path& gameRoot, int language, int clip);
// The shared device (opened on demand, nullptr if OpenAL has no device) for sound effects.
bb::AudioDevice* audioDevice();

} // namespace bbtime
