// TB save format (docs/formats/SAVE_TB.md): synthetic percent / text / record checks, then the Wine-produced saves
// (BBTIME_SAVES, default $BBTIME_DATA/../local/saves/tb; skipped without them): three GRANNY pause-menu saves of
// one session (new file 0, "same file" 0 again, "no" -> new file 1) with the GAME_DATA dumps taken just before. The
// native writer must reproduce the original's Savegame bytes and Savedata bytes (bar the heap pointer the exe
// leaves in a non-last record's +0x50), and the request handler must pick the same slots.
#include "bb/bze.h"
#include "bb/game/tb_save.h"
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace bb;
using namespace bb::game;
namespace fs = std::filesystem;

namespace {

std::array<uint8_t, 0x300> dump(const fs::path& p) {  // tools/wine/peek.py "addr: b b b ..." of GD (688 bytes)
    std::ifstream f(p);
    std::string line;
    std::getline(f, line);
    std::istringstream s(line.substr(line.find(':') + 1));
    std::array<uint8_t, 0x300> gd{};
    int v = 0;
    for (size_t i = 0; s >> v; ++i) gd.at(i) = uint8_t(v);
    return gd;
}

std::tm at(int h, int m, int s) {  // 2026-10-21 h:m:s
    std::tm t{};
    t.tm_sec = s, t.tm_min = m, t.tm_hour = h, t.tm_mday = 21, t.tm_mon = 9, t.tm_year = 126;
    return t;
}

void synthetic() {
    std::array<uint8_t, 0x300> gd{};
    gd[0x5a] = 0x77, gd[0x5b] = 0x0b;  // 2935 / 6010 -> 48 %
    assert(tbSavePercent(gd) == 48);
    gd[0x5a] = 0xff, gd[0x5b] = 0xff;
    assert(tbSavePercent(gd) == 100);
    assert(tbSaveText(at(9, 3, 8), 7) == "10/21/26, 09:03:08, 7%");
    std::vector<TbSaveSlot> slots;
    tbSaveRecord(slots, 0, 4, "x");
    const std::vector<uint8_t> b = writeTbSavedata(slots);
    assert(b.size() == 9 + 0x54 && b[9] == 0 && b[10] == 'x' && b[11] == 0 && b[12] == 0xcd && b[9 + 0x4b] == 0xcd);
    assert(b[9 + 0x4c] == 4 && b[9 + 0x50] == 0);
    tbSaveRecord(slots, 0, 5, "");  // an update keeps the old bytes after the new NUL
    const std::vector<uint8_t> c = writeTbSavedata(slots);
    assert(c[10] == 0 && c[11] == 0 && c[12] == 0xcd && c[9 + 0x4c] == 5 && slots.size() == 1);
    std::puts("tb_save synthetic ok");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    synthetic();
    const char* env = std::getenv("BBTIME_SAVES");
    const char* data = std::getenv("BBTIME_DATA");
    const fs::path dir = env ? fs::path(env) : data ? fs::path(data) / "../local/saves/tb" : fs::path();
    if (dir.empty() || !fs::exists(dir / "save3_newslot/Savegame1.dat")) {
        std::puts("no Wine-made TB saves (BBTIME_SAVES / $BBTIME_DATA/../local/saves/tb): real part skipped");
        return 0;
    }
    const fs::path s1 = dir / "save1_granny", s2 = dir / "save2_granny", s3 = dir / "save3_newslot";

    // Savedata: save 1 is a fresh record in slot 0 (level 4 GRANNY, 0xcd fill); save 2 rewrites its text in place;
    // save 3 appends slot 1 (the exe's +0x50 of record 0 then holds its heap pointer: masked).
    const std::vector<uint8_t> d1 = readFile(s1 / "Savedata.dat"), d2 = readFile(s2 / "Savedata.dat"),
                               d3 = readFile(s3 / "Savedata.dat");
    std::vector<TbSaveSlot> slots;
    tbSaveRecord(slots, 0, 4, tbSaveText(at(19, 3, 38), 0));
    assert(writeTbSavedata(slots) == d1);
    slots = parseTbSavedata(d1);
    assert(slots.size() == 1 && slots[0].text == "10/21/26, 19:03:38, 0%" && slots[0].level == 4);
    tbSaveRecord(slots, 0, 4, tbSaveText(at(19, 4, 41), 0));
    assert(writeTbSavedata(slots) == d2);
    slots = parseTbSavedata(d2);
    tbSaveRecord(slots, 1, 4, tbSaveText(at(19, 5, 24), 0));
    std::vector<uint8_t> mine = writeTbSavedata(slots), want = d3;
    assert(want.size() == 9 + 2 * 0x54);
    for (size_t k = 0; k < 4; ++k) want[9 + 0x50 + k] = 0;
    assert(mine == want);

    // Savegame: GAME_DATA dumped while the prompt waited, with the pad words of the confirming tick (+8..+0xf,
    // the only other difference) and the previous checksum still at +4, sealed = the file. Then the request
    // handler on a copy of the save-2 directory: "no" (S[0x27] = 2) writes Savegame1 and the slot-1 record.
    for (const auto& [d, gdFile, game] : {std::tuple{s2, "gd_before3.txt", "Savegame0.dat"}, {s3, "gd_before4.txt", "Savegame1.dat"}}) {
        const std::vector<uint8_t> file = readFile(d / game);
        std::array<uint8_t, 0x300> gd = dump(d / gdFile);
        std::copy_n(file.begin() + 8, 8, gd.begin() + 8);
        tbSealSave(gd);
        assert(std::equal(file.begin(), file.end(), gd.begin()) && file.size() == kTbSaveSize);
    }
    const fs::path tmp = fs::temp_directory_path() / "bbtime_test_tb_save";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    for (const char* f : {"Savedata.dat", "Savegame0.dat"}) fs::copy_file(s2 / f, tmp / f);
    TbWorld w;
    w.GD = dump(s3 / "gd_before4.txt");
    std::copy_n(readFile(s3 / "Savegame1.dat").begin() + 8, 8, w.GD.begin() + 8);
    w.S[0x29] = 6, w.S[0x27] = 2;
    int slot = 0;
    assert(tbSaveRequest(w, slot, tmp, at(19, 5, 24)).empty());
    assert(slot == 1 && w.S[0x27] == 0x0e && w.S[0x29] == 0);
    assert(readFile(tmp / "Savegame1.dat") == readFile(s3 / "Savegame1.dat"));
    assert(readFile(tmp / "Savedata.dat") == want && readFile(tmp / "Savedata.dat.bak") == d2);
    w.S[0x29] = 1;  // anything but 6 is just cleared
    assert(tbSaveRequest(w, slot, tmp, at(0, 0, 0)).empty() && w.S[0x29] == 0 && slot == 1);
    fs::remove_all(tmp);
    std::puts("tb_save Wine saves ok: Savegame 0/1 and Savedata of 3 saves reproduced");
    return 0;
}
