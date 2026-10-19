// bbimport <install-root> [-o manifest.json] — read-only inventory of an original install as JSON.
#include "bb/manifest.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <print>
#include <string_view>

int main(int argc, char** argv) {
    const char* root = nullptr;
    const char* out = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "-o" && i + 1 < argc) out = argv[++i];
        else if (!root) root = argv[i];
        else root = nullptr, i = argc;
    }
    if (!root) {
        std::println(stderr, "usage: bbimport <install-root> [-o manifest.json]");
        return 2;
    }
    try {
        const bb::Manifest m = bb::scanInstall(root);
        const std::string json = bb::manifestJson(m);
        if (out) {
            std::ofstream f(out, std::ios::binary);
            if (!f.write(json.data(), std::streamsize(json.size()))) throw std::runtime_error(std::string("cannot write ") + out);
        } else {
            std::fwrite(json.data(), 1, json.size(), stdout);
        }
        size_t bad = 0;
        for (const bb::ManifestFile& f : m.files) {
            const bool sectionBad = std::ranges::any_of(f.sections, [](auto& s) { return !s.error.empty(); });
            if (!f.error.empty() || sectionBad) {
                ++bad;
                std::println(stderr, "FAIL {}: {}", f.path, f.error.empty() ? "section decode error" : f.error);
            }
        }
        std::println(stderr, "{}: {} files, game {}, exe {} ({}), {} failures", root, m.files.size(),
                     !m.game ? "unknown" : *m.game == bb::GameId::LostInTime ? "LostInTime" : "TimeBusters",
                     m.exePath.empty() ? "-" : m.exePath, m.exeVersion.empty() ? "unknown" : m.exeVersion, bad);
        return bad ? 1 : 0;
    } catch (const std::exception& e) {
        std::println(stderr, "bbimport: {}", e.what());
        return 1;
    }
}
