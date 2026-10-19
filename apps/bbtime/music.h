// Collection-menu music. Owned by the audio work (docs/formats/AUDIO_XA.md, "Menu music").
#pragma once
#include "bb/scene.h"

#include <filesystem>
#include <optional>

namespace bbtime {

// Call once per frame. `game` = whose menu track should play (nullopt = silence; the audio device is only
// opened the first time a track is requested, so passing nullopt always keeps --mute fully silent).
void menuMusic(const std::filesystem::path& dataRoot, std::optional<bb::GameId> game);

} // namespace bbtime
