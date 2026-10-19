#include "music.h"

#include "bb/audio.h"

#include <cctype>
#include <format>
#include <memory>
#include <print>
#include <string>

namespace fs = std::filesystem;

namespace bbtime {
namespace {

// Install trees mix case (LiT Datas/track/TRACK25.xa, TB Datas/TRACK/track14.xa): match components ignoring case.
fs::path find(fs::path p, std::initializer_list<std::string> parts) {
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(uint8_t(c)));
        return s;
    };
    for (const std::string& part : parts) {
        std::error_code ec;
        fs::path next;
        for (const auto& e : fs::directory_iterator(p, ec))
            if (lower(e.path().filename().string()) == lower(part)) { next = e.path(); break; }
        if (next.empty()) return {};
        p = next;
    }
    return p;
}

std::unique_ptr<bb::AudioDevice> dev;

} // namespace

// LiT: title.bze section-1 music table (chunk 0x05, tag 0x06 {zone, track, track}) = {1,25,25},{2,25,25};
//      bugs.exe 0x41d630 opens ..\track\Track%d.xa with that number -> TRACK25.xa. Verified (code + data).
// TB:  the original front end plays NO music (only bong.wav volume test; docs/re/GAMEPLAY_TB.md §6).
//      Our collection menu uses Track2.xa, the Granny hub track (level music via logic action 126 ->
//      Track(n-1).xa). Design choice, not original behaviour.
fs::path menuTrack(bb::GameId g, const fs::path& gameRoot) {
    return g == bb::GameId::LostInTime ? find(gameRoot, {"Datas", "track", "TRACK25.xa"})
                                       : find(gameRoot, {"Datas", "TRACK", "track2.xa"});
}

fs::path tbLevelTrack(const fs::path& gameRoot, int n) {
    if (n <= 1) return {};
    fs::path t = find(gameRoot, {"Datas", "TRACK", std::format("Track{}.xa", n - 1)});
    return t.empty() ? find(gameRoot, {"Datas", "TRACK", "Track14.xa"}) : t;
}

bb::AudioDevice* audioDevice() {
    if (!dev) {
        dev = std::make_unique<bb::AudioDevice>();
        if (!dev->ok()) std::println(stderr, "bbtime: no OpenAL device, audio disabled");
    }
    return dev->ok() ? dev.get() : nullptr;
}

void playMusic(const fs::path& track) {
    static fs::path current;
    if (track != current) {
        current = track;
        if (dev) dev->stopMusic();
        if (!track.empty())
            if (bb::AudioDevice* d = audioDevice()) {
                try {
                    d->playMusic(track);
                    std::println("bbtime: music {}", track.string());
                } catch (const std::exception& e) {
                    std::println(stderr, "bbtime: music {}: {}", track.string(), e.what());
                }
            }
    }
    if (dev) dev->update();
}

} // namespace bbtime
