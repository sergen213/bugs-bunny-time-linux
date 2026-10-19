// Integration test against original installs: $BBTIME_DATA/{lit,tb}. Exits 77 (ctest SKIP) without data.
#include "bb/bze.h"
#include "bb/levelscript.h"
#include "bb/lzss.h"
#include "bb/manifest.h"

#include <cstdlib>
#include <filesystem>
#include <print>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ++failures;                                                       \
            std::println(stderr, "FAIL {}:{}: {}", __FILE__, __LINE__, #cond); \
            std::println(stderr, __VA_ARGS__);                                \
        }                                                                     \
    } while (0)

static void checkGame(const fs::path& root, bb::GameId game) {
    const bool lit = game == bb::GameId::LostInTime;
    const bb::Manifest m = bb::scanInstall(root);
    CHECK(m.game == game, "{}: game detection", root.string());
    CHECK(!m.exeVersion.empty(), "{}: no known executable hash", root.string());

    size_t bzes = 0, sections = 0, dropped = 0;
    for (const bb::ManifestFile& mf : m.files) {
        CHECK(mf.error.empty(), "{}: {}", mf.path, mf.error);
        if (mf.format != "bze") continue;
        ++bzes;
        const bb::BzeFile f = bb::loadBze(root / mf.path);
        CHECK(f.variant == (lit ? bb::BzeVariant::LostInTime : bb::BzeVariant::TimeBusters), "{}: variant", mf.path);
        for (size_t i = 0; i < f.sections.size(); ++i) {
            ++sections;
            const bb::BzeSection& s = f.sections[i];
            bb::LzssInfo li;
            if (lit) {
                try {
                    const auto payload = f.payload(i, &li);
                    // A genuine stream fills its section up to the 4-byte rounding of actualSize.
                    CHECK(li.consumed + 3 >= s.actualSize, "{} section {}: stream ends at 0x{:x} of 0x{:x}", mf.path, s.id,
                          li.consumed, s.actualSize);
                    dropped += li.tailDropped;
                    if (s.id == 1) {
                        const bb::LevelScript ls = bb::parseLevelScript(payload);
                        CHECK(ls.end == payload.size(), "{}: script ends at 0x{:x} of 0x{:x}", mf.path, ls.end,
                              payload.size());
                    }
                } catch (const bb::FormatError& e) {
                    CHECK(false, "{} section {}: {}", mf.path, s.id, e.what());
                }
            } else {
                // TB stores sections raw: no section may look like a full LZSS stream.
                bool looksLzss = false;
                try {
                    bb::lzssDecompress(f.raw(i), &li);
                    looksLzss = li.consumed + 3 >= s.actualSize;
                } catch (const bb::FormatError&) {
                }
                CHECK(!looksLzss, "{} section {}: raw bytes decode as a full LZSS stream", mf.path, s.id);
            }
        }
    }
    CHECK(bzes > 0, "{}: no BZE files found", root.string());
    std::println("{}: {} files, {} ({}), {} BZE, {} sections, {} with count==items (final item dropped)", root.string(),
                 m.files.size(), m.exePath, m.exeVersion, bzes, sections, dropped);
}

int main() {
    const char* data = std::getenv("BBTIME_DATA");
    if (!data || !fs::is_directory(fs::path(data) / "lit") || !fs::is_directory(fs::path(data) / "tb")) {
        std::println("BBTIME_DATA unset or missing lit/ tb/: skipping");
        return 77;
    }
    checkGame(fs::path(data) / "lit", bb::GameId::LostInTime);
    checkGame(fs::path(data) / "tb", bb::GameId::TimeBusters);
    return failures ? 1 : 0;
}
