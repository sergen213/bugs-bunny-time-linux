// LiT save format (docs/formats/SAVE_LIT.md): synthetic checksum/validation/summary checks, then the real
// Wine-produced saves (BBTIME_SAVES, default $BBTIME_DATA/../local/saves/lit; skipped without them):
// byte-identical parse, the native writer reproducing the original's Savegame/Savedata bytes, import + .bak.
#include "bb/bze.h"
#include "bb/game/lit_save.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace bb;
using namespace bb::game;
namespace fs = std::filesystem;

namespace {

template <class F> bool throws(F f) {
    try { f(); } catch (const FormatError&) { return true; }
    return false;
}

void write(const fs::path& p, std::span<const uint8_t> b) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

void testSynthetic(const fs::path& tmp) {
    LitSaveGame g = LitSaveGame::newGame(3);
    assert(g.G()[0] == 3 && g.G()[1] == 6 && g.G()[2] == 6 && g.G()[0x0e] == 0x38 && g.slot() == 3);
    assert(g.startLevel() == 0x38);
    g.setLevel(4);
    g.G()[7] = 0x4d, g.G()[0xfc] = 1, g.G()[5] = 124, g.G()[0x80] = 0xf0;
    g.seal();
    // 4 + 3 + 6 + 6 + 0x38 + 3 + 0x4d + 1 + 124 + (s8)0xf0 + 2*0xff + 2*0x7f, each byte as a signed char
    const int32_t first = 4 + 3 + 6 + 6 + 0x38 + 3 + 0x4d + 1 + 124 - 16 + 2 * -1 + 2 * 0x7f;
    assert(g.checksum() == first);
    g.seal();  // the old checksum is part of the sum (0x4074f0 never clears it)
    assert(g.checksum() == first + int8_t(uint8_t(first)) + int8_t(uint8_t(first >> 8)));

    const LitSaveGame p = parseLitSaveGame(g.raw, 3);
    assert(p.raw == g.raw && p.level() == 4 && p.startLevel() == 4);
    assert(throws([&] { parseLitSaveGame(std::span(g.raw).first(0x13f), 3); }));       // truncated
    assert(throws([&] { parseLitSaveGame(std::vector<uint8_t>(0x141), 3); }));         // oversized
    assert(throws([&] { parseLitSaveGame(g.raw, 2); }));                               // slot byte != file number
    LitSaveGame bad = g;
    bad.setLevel(111);
    assert(throws([&] { parseLitSaveGame(bad.raw, 3); }));                              // level out of range
    assert(throws([&] { parseLitSaveData(std::vector<uint8_t>(0x167)); }));

    LitSaveData d;
    d.record(g);
    assert(d.mask() == 4 && d.golden(3) == 333 && d.clocks(3) == 124);
    d.record(LitSaveGame::newGame(0));  // slot 0 (level started without a slot): bit 31, no summary
    assert(d.mask() == 0x80000004u);

    const fs::path dir = tmp / "synthetic";
    fs::remove_all(dir);
    assert(writeLitSave(dir, g).empty());
    const auto first_file = readFile(litSaveGamePath(dir, 3));
    assert(parseLitSaveGame(first_file, 3).raw == g.raw);
    assert(readLitSaveData(dir).golden(3) == 333);
    assert(writeLitSave(dir, g).empty());  // second write backs the first one up
    assert(readFile(litSaveGamePath(dir, 3).string() + ".bak") == first_file);
    write(litSaveDataPath(dir), std::vector<uint8_t>(10));  // damaged summary: refuse, leave the slot file alone
    const auto before = readFile(litSaveGamePath(dir, 3));
    assert(!writeLitSave(dir, g).empty() && readFile(litSaveGamePath(dir, 3)) == before);
    std::printf("synthetic: ok\n");
}

LitSaveGame load(const fs::path& d) { return parseLitSaveGame(readFile(d / "Savegame2.dat"), 2); }

// Real saves, all slot 2 of one session (RUNTIME_REFERENCE.md "LiT saves"): save1 new game in MERLIN (51),
// save2 same slot after carrot cheats (G[3] 99, G[8] 0x1f), save3 after the 333/124 cheat (level 71, CCEND).
void testReal(const fs::path& saves, const fs::path& tmp) {
    const fs::path s1 = saves / "save1_merlin_new", s2 = saves / "save2_merlin_cheats", s3 = saves / "save3_gc333_credits";
    const LitSaveGame g1 = load(s1), g2 = load(s2), g3 = load(s3);
    for (const fs::path& d : {s1, s2, s3}) {
        assert(std::ranges::equal(load(d).raw, readFile(d / "Savegame2.dat")));  // parse -> bytes unchanged
        assert(std::ranges::equal(parseLitSaveData(readFile(d / "Savedata.dat")).raw, readFile(d / "Savedata.dat")));
    }
    assert(g1.level() == 51 && g1.checksum() == 0x17a && g1.G()[3] == 0 && g1.slot() == 2);
    assert(g2.level() == 51 && g2.checksum() == 637 && g2.G()[3] == 99 && g2.G()[8] == 0x1f);
    assert(g3.level() == 71 && g3.G()[7] == 0x4d && g3.G()[0xfc] == 1 && g3.G()[5] == 124);
    const LitSaveData d3 = parseLitSaveData(readFile(s3 / "Savedata.dat"));
    assert(d3.mask() == 2 && d3.golden(2) == 333 && d3.clocks(2) == 124);

    // Native writer == original writer: replay each save from the state just before it (+4 = previous checksum).
    struct Step { const fs::path& dir; const fs::path* prevData; int32_t prevSum; };
    const Step steps[] = {{s1, nullptr, 0}, {s2, &s1, g1.checksum()}, {s3, &s2, g2.checksum()}};
    for (const Step& st : steps) {
        const fs::path out = tmp / "replay";
        fs::remove_all(out);
        fs::create_directories(out);
        if (st.prevData) fs::copy_file(*st.prevData / "Savedata.dat", out / "Savedata.dat");
        LitSaveGame g = load(st.dir);
        for (int i = 0; i < 4; ++i) g.raw[size_t(4 + i)] = uint8_t(uint32_t(st.prevSum) >> (8 * i));
        assert(writeLitSave(out, g).empty());
        assert(readFile(out / "Savegame2.dat") == readFile(st.dir / "Savegame2.dat"));
        assert(readFile(out / "Savedata.dat") == readFile(st.dir / "Savedata.dat"));
    }

    // Import: valid slot files + Savedata copied byte-identically; a second import keeps .bak copies.
    const fs::path src = tmp / "orig_bin", dst = tmp / "imported";
    fs::remove_all(src);
    fs::remove_all(dst);
    fs::create_directories(src);
    fs::copy_file(s3 / "Savegame2.dat", src / "Savegame2.dat");
    fs::copy_file(s3 / "Savedata.dat", src / "Savedata.dat");
    write(src / "Savegame1.dat", std::vector<uint8_t>(0x140));  // shipped blank: slot byte 0, rejected
    write(src / "Savegame3.dat", std::vector<uint8_t>(100));    // truncated, rejected
    const std::string r = importLitSaves(src, dst);
    std::printf("import: %s\n", r.c_str());
    assert(readFile(dst / "Savegame2.dat") == readFile(s3 / "Savegame2.dat"));
    assert(readFile(dst / "Savedata.dat") == readFile(s3 / "Savedata.dat"));
    assert(!fs::exists(dst / "Savegame1.dat") && !fs::exists(dst / "Savegame3.dat"));
    importLitSaves(src, dst);
    assert(readFile(dst / "Savegame2.dat.bak") == readFile(s3 / "Savegame2.dat"));
    std::printf("real saves: ok\n");
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "bbtime_test_lit_save";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    testSynthetic(tmp);
    fs::path saves;
    if (const char* e = std::getenv("BBTIME_SAVES"); e && *e) saves = e;
    else if (const char* d = std::getenv("BBTIME_DATA"); d && *d) saves = fs::path(d) / "../local/saves/lit";
    if (saves.empty() || !fs::exists(saves / "save1_merlin_new/Savegame2.dat")) {
        std::printf("no real LiT saves (BBTIME_SAVES / BBTIME_DATA/../local/saves/lit): skipped\n");
    } else {
        testReal(saves, tmp);
    }
    fs::remove_all(tmp);
    return 0;
}
