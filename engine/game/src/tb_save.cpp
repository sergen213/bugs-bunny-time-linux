// TB in-game save writer 0x401b60 / 0x428f70 / 0x428a50 / 0x428b00 / 0x428d70; see tb_save.h, SAVE_TB.md.
#include "bb/game/tb_save.h"

#include "bb/bze.h"
#include "bb/game/lit_save.h"
#include "bb/game/tb_world.h"

#include <algorithm>
#include <format>

namespace fs = std::filesystem;

namespace bb::game {

int tbSavePercent(std::span<const uint8_t> gd) { return std::min(100, (gd[0x5a] | gd[0x5b] << 8) * 100 / 0x177a); }

void tbSealSave(std::span<uint8_t> gd) {
    gd[0x14b] = uint8_t(tbSavePercent(gd));
    int32_t sum = 0;
    for (size_t i = 0; i < kTbSaveSize; ++i) sum += int8_t(gd[i]);
    for (size_t i = 0; i < 4; ++i) gd[4 + i] = uint8_t(uint32_t(sum) >> (8 * i));
}

std::string tbSaveText(const std::tm& t, int percent) {
    return std::format("{:02}/{:02}/{:02}, {:02}:{:02}:{:02}, {}%", t.tm_mon + 1, t.tm_mday, t.tm_year % 100, t.tm_hour,
                       t.tm_min, t.tm_sec, percent);
}

void tbSaveRecord(std::vector<TbSaveSlot>& slots, int slot, int level, std::string_view text) {
    auto it = std::ranges::find(slots, slot, &TbSaveSlot::slot);
    if (it == slots.end()) {
        TbSaveSlot n;
        n.slot = slot;
        n.raw.fill(0xcd);  // malloc'd (0x46b0c0, debug-heap fill), +0x50 next = 0
        n.raw[0x50] = n.raw[0x51] = n.raw[0x52] = n.raw[0x53] = 0;
        it = slots.insert(slots.end(), n);
    }
    it->level = level, it->text = std::string(text.substr(0, 0x4a));
    // The in-memory record (sprintf into +1): the bytes past the new NUL keep the old text's tail.
    std::ranges::copy(it->text, it->raw.begin() + 1);
    it->raw[1 + it->text.size()] = 0;
}

std::string tbSaveRequest(TbWorld& w, int& slot, const fs::path& dir, const std::tm& now) {
    uint8_t& req = w.S[0x29];
    if (req != 6) return req = 0, "";
    req = 0;
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::vector<TbSaveSlot> slots;
    try {
        slots = parseTbSavedata(readFile(dir / "Savedata.dat"));
    } catch (const std::exception&) {  // missing / unreadable: an empty list, as 0x428e50
    }
    if (w.S[0x27] == 2 || slot < 0) slot = int(slots.size());  // 0x428f70: a new file
    tbSealSave(w.GD);
    const std::span<const uint8_t> file(w.GD.data(), kTbSaveSize);
    std::string err = replaceSaveFile(dir / std::format("Savegame{}.dat", slot), file);
    if (err.empty()) {
        const int pct = tbSavePercent(w.GD);
        tbSaveRecord(slots, slot, int(w.GD[0] | w.GD[1] << 8 | w.GD[2] << 16 | uint32_t(w.GD[3]) << 24), tbSaveText(now, pct));
        err = replaceSaveFile(dir / "Savedata.dat", writeTbSavedata(slots));
    }
    w.S[0x27] = err.empty() ? 0x0e : 0x0f;
    return err;
}

}  // namespace bb::game
