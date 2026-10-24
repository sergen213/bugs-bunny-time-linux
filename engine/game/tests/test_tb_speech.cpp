// TB speech (0x42a030 / 0x469770 mode 0x80 / action 62) against the Wine oracle. BBTIME_DATA real GRANNY:
// local/wine/tbintro/w13_speech_granny.sh (w13_speech_granny.csv, the exe with its DirectSound device): ✕ pages the
// "Welcome to Granwich!" box at counter 329, Granny's clip 2 (Datas/SPEECH/00/Speech2.xa, 61 s) asks at 411 (S[0x22] 2,
// 0x4cca40 2), plays at 413 (4); ✕ at 1899 skips it (S[0x22] 0, clip 0, 0x4cca40 -1). Counter c = tick (c - 1) / 2:
// the first entity tick after a load runs at counter 1 (local/wine/tbintro/w13_retry82.csv).
// (RETRY's Daffy clip 59 is checked in test_tb.cpp.)
#include "bb/game/tb_world.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace bb;
using namespace bb::game;
namespace fs = std::filesystem;

namespace {

fs::path findCi(fs::path base, std::initializer_list<std::string> parts) {
    for (std::string part : parts) {
        fs::path next;
        std::error_code ec;
        for (char& ch : part) ch = char(std::tolower(uint8_t(ch)));
        for (const auto& e : fs::directory_iterator(base, ec)) {
            std::string a = e.path().filename().string();
            for (char& ch : a) ch = char(std::tolower(uint8_t(ch)));
            if (a == part) { next = e.path(); break; }
        }
        if (next.empty()) return {};
        base = next;
    }
    return base;
}

PadInput keys(uint16_t b) { PadInput in; in.buttons = b; return in; }

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* data = std::getenv("BBTIME_DATA");
    const fs::path bze = data ? findCi(data, {"tb", "Datas", "BZE", "GRANNY.bze"}) : fs::path{};
    if (bze.empty()) {
        std::puts("BBTIME_DATA/tb GRANNY.bze not found: skipped");
        return 77;
    }
    const BzeFile f = loadBze(bze);
    Scene scene = buildTbScene(f, "GRANNY");
    addTbActors(scene, f);
    for (const bool audio : {true, false}) {
        TbWorld w = TbWorld::load(f, scene);
        w.GD[0] = 4;
        w.fadeIn(1, 200);
        w.speechAudio = audio;
        std::vector<int> st, clip;
        for (int i = 0; i < 960; ++i) {
            w.tick(keys(i == 164 || i == 949 ? PadCross : 0));
            st.push_back(w.S[0x22]), clip.push_back(w.speechClip);
        }
        const int two = int(std::ranges::find(st, 2) - st.begin());
        const int four = int(std::ranges::find(st, 4) - st.begin());
        const int zero = int(std::find(st.begin() + four, st.end(), 0) - st.begin());
        std::printf("  GRANNY speech (%s): clip %d, S[0x22] 2 @%d, 4 @%d, 0 @%d (clip then %d)\n", audio ? "audio" : "timed",
                    two < int(st.size()) ? clip[size_t(two)] : -1, two, four, zero, zero < int(st.size()) ? clip[size_t(zero)] : -1);
        assert(two == 205 && clip[205] == 2 && four == 206);
        // With audio the 61 s clip runs until ✕ skips it; timed, the exe ends it after 6000 * 1000 / 44100 ms.
        assert(audio ? zero == 949 && clip[949] == 0 : zero == 206 + 5);
    }
    std::puts("test_tb_speech: ok");
    return 0;
}
