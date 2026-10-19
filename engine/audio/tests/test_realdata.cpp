// Real-data audio checks (BBTIME_DATA=<dir with lit/ and tb/>; exit 77 = skip):
// every .xa of both games decodes to bytes→frames per docs/formats/AUDIO_XA.md, and every AIFF in the
// section-3 sound bank of every BZE parses as mono 16-bit 44100 Hz.
#include "bb/audio.h"
#include "bb/bze.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <print>
#include <string>

namespace fs = std::filesystem;
using namespace bb;

static std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

int main() {
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) {
        std::println("audio.realdata: BBTIME_DATA unset, skipping");
        return 77;
    }
    int failures = 0;
    for (const char* game : {"lit", "tb"}) {
        const fs::path root = fs::path(env) / game / "Datas";
        if (!fs::is_directory(root)) {
            std::println(stderr, "missing {}", root.string());
            return 1;
        }
        // .xa streams
        size_t files = 0, mono = 0;
        double seconds = 0;
        uint64_t clipped = 0, samples = 0;
        std::vector<int16_t> pcm;
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file() || lower(e.path().extension().string()) != ".xa") continue;
            XaStream xa(e.path());
            uint64_t got = 0;
            for (;;) {
                pcm.clear();
                if (xa.read(1 << 20, pcm) == 0) break;
                got += pcm.size();
                for (int16_t s : pcm) clipped += s == 32767 || s == -32768;
            }
            const uint64_t expect = xa.frames() * uint64_t(xaChannels(xa.layout()));
            if (got != expect) {
                std::println(stderr, "{}: {} samples, expected {}", e.path().string(), got, expect);
                ++failures;
            }
            samples += got;
            seconds += double(xa.frames()) / kXaRate;
            mono += xa.layout() == XaLayout::Mono;
            ++files;
        }
        std::println("audio.realdata: {} .xa: {} files ({} mono), {:.1f} s = {:.2f} h decoded, clipped {:.4f}%", game,
                     files, mono, seconds, seconds / 3600, samples ? 100.0 * double(clipped) / double(samples) : 0.0);
        if (files == 0) ++failures;

        // Section-3 AIFF sound banks
        size_t levels = 0, banks = 0, sounds = 0, truncated = 0;
        double sfxSeconds = 0;
        uint64_t sfxClipped = 0, sfxSamples = 0;
        std::map<std::string, size_t> formats;
        for (const auto& e : fs::directory_iterator(root / (std::string(game) == "lit" ? "bze" : "BZE"))) {
            if (lower(e.path().extension().string()) != ".bze") continue;
            ++levels;
            const BzeFile bze = loadBze(e.path());
            const int i = bze.find(3);
            if (i < 0) continue;
            const auto sec = bze.payload(size_t(i));
            const auto bank = sfxBank(sec);
            banks += !bank.empty();
            for (auto blob : bank) {
                try {
                    const AiffSound s = parseAiff(blob, bze.variant);
                    ++formats[std::format("{}ch/{}bit/{}Hz", s.channels, s.bits, s.rate)];
                    truncated += s.truncated;
                    for (int16_t v : s.pcm) sfxClipped += v == 32767 || v == -32768;
                    sfxSamples += s.pcm.size();
                    sfxSeconds += double(s.pcm.size()) / s.channels / s.rate;
                    ++sounds;
                } catch (const FormatError& err) {
                    std::println(stderr, "{}: {}", e.path().string(), err.what());
                    ++failures;
                }
            }
        }
        std::println("audio.realdata: {} section-3 AIFF: {} levels, {} with a sound bank, {} sounds ({} truncated), "
                     "{:.1f} s, clipped {:.4f}%", game, levels, banks, sounds, truncated, sfxSeconds,
                     sfxSamples ? 100.0 * double(sfxClipped) / double(sfxSamples) : 0.0);
        for (const auto& [f, n] : formats) std::println("audio.realdata: {}   {} x {}", game, n, f);
        if (sounds == 0) ++failures;
    }
    if (failures) std::println(stderr, "audio.realdata: {} failures", failures);
    return failures ? 1 : 0;
}
