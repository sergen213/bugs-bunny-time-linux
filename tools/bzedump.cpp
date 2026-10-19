// bzedump <file.bze> [--script] [--section ID --out FILE]
// Prints header + section table (with LZSS stream stats for LiT); --script lists section-1 items;
// --section/--out writes that section's decoded payload.
#include "bb/bze.h"
#include "bb/levelscript.h"
#include "bb/lzss.h"

#include <cstdio>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    const char* in = nullptr;
    const char* out = nullptr;
    long sectionId = -1;
    bool script = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--script") script = true;
        else if (a == "--section" && i + 1 < argc) sectionId = std::stol(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (!in && !a.starts_with("-")) in = argv[i];
        else { in = nullptr; break; }
    }
    if (!in || (out != nullptr) != (sectionId >= 0)) {
        std::println(stderr, "usage: bzedump <file.bze> [--script] [--section ID --out FILE]");
        return 2;
    }
    try {
        const bb::BzeFile f = bb::loadBze(in);
        const bool lit = f.variant == bb::BzeVariant::LostInTime;
        std::println("variant {}  size 0x{:x}  checksum 0x{:x}  sections {}{}", lit ? "LostInTime" : "TimeBusters",
                     f.bytes.size(), f.checksum, f.sections.size(), lit ? "" : std::format("  tbBudget {}", f.tbBudget));
        int rc = 0;
        for (size_t i = 0; i < f.sections.size(); ++i) {
            const bb::BzeSection& s = f.sections[i];
            std::print("  [{}] id {:2}  off 0x{:07x}  actual 0x{:06x}  file 0x{:06x}", i, s.id, s.offset, s.actualSize,
                       s.fileSize);
            if (lit) {
                bb::LzssInfo li;
                try {
                    const auto d = f.payload(i, &li);
                    std::print("  lzss mode 0x{:02x} stored {} items {} end 0x{:x} ({:+} vs actual){} decoded 0x{:x}", li.mode,
                               li.storedCount, li.items, li.consumed, int64_t(li.consumed) - s.actualSize,
                               li.tailDropped ? " TAIL-DROPPED" : "", d.size());
                } catch (const bb::FormatError& e) {
                    std::print("  LZSS ERROR: {}", e.what());
                    rc = 1;
                }
            }
            std::println("");
        }
        if (script) {
            const int i = f.find(1);
            if (i < 0) throw bb::FormatError("no section 1");
            const auto payload = f.payload(size_t(i));
            const bb::LevelScript ls = bb::parseLevelScript(payload, f.variant);
            for (const bb::ScriptItem& it : ls.items) {
                static constexpr const char* kinds[] = {"pseudo", "chunk", "cmd", "end"};
                std::print("0x{:05x} {:6} 0x{:02x}", it.offset, kinds[int(it.kind)], it.code);
                for (const bb::ScriptTag& t : it.tags) std::print(" {:02x}", t.id);
                if (it.position) std::print("  pos ({:.3f} {:.3f} {:.3f})", (*it.position)[0] / 4096.0,
                                            (*it.position)[1] / 4096.0, (*it.position)[2] / 4096.0);
                if (it.rotation) std::print("  rot ({} {} {})", (*it.rotation)[0], (*it.rotation)[1], (*it.rotation)[2]);
                if (it.scale) std::print("  scale ({:.3f} {:.3f} {:.3f})", (*it.scale)[0] / 4096.0, (*it.scale)[1] / 4096.0,
                                         (*it.scale)[2] / 4096.0);
                if (it.entityId) std::print("  id {}", *it.entityId);
                if (it.player) std::print("  PLAYER");
                std::println("");
            }
            std::println("script end 0x{:x} of 0x{:x}", ls.end, payload.size());
        }
        if (out) {
            const int i = f.find(uint32_t(sectionId));
            if (i < 0) throw bb::FormatError(std::format("no section id {}", sectionId));
            const auto payload = f.payload(size_t(i));
            std::ofstream o(out, std::ios::binary);
            if (!o.write(reinterpret_cast<const char*>(payload.data()), std::streamsize(payload.size())))
                throw std::runtime_error(std::format("cannot write {}", out));
            std::println("wrote {} bytes to {}", payload.size(), out);
        }
        return rc;
    } catch (const std::exception& e) {
        std::println(stderr, "bzedump: {}: {}", in, e.what());
        return 1;
    }
}
