// TB in-game saves (docs/formats/SAVE_TB.md): `..\bin\Savegame%d.dat` = raw GAME_DATA[0..0x2b0) with a checksum and
// the completion percent sealed in; `..\bin\Savedata.dat` = the slot list (TbSaveSlot, tb_menu.h). The pause menu's
// "save game" (template 0x1d logic) sets S[0x29] = 6, with S[0x27] = 2 for "no, a new savegame file".
#pragma once

#include "bb/game/tb_menu.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bb::game {

class TbWorld;

constexpr size_t kTbSaveSize = 0x2b0;
// 0x464050: GD+0x5a (u16) * 100 / 6010, at most 100.
int tbSavePercent(std::span<const uint8_t> gd);
// 0x428a50's seal: GD+0x14b = percent, then GD+4 = sum of GD[0..0x2b0) as signed chars, the old GD+4 included.
void tbSealSave(std::span<uint8_t> gd);
// "%s, %s, %d%%" of _strdate / _strtime (MM/DD/YY, HH:MM:SS) and the percent (0x428b00).
std::string tbSaveText(const std::tm& t, int percent);
// 0x428b00: the record of `slot` (appended when new: 0xcd heap fill, next 0) gets `level` and `text`.
void tbSaveRecord(std::vector<TbSaveSlot>& slots, int slot, int level, std::string_view text);
// 0x401b60 when S[0x29] == 6 (once per frame, before the world): 0x428f70 picks the slot (`slot` = exe 0x48840c,
// -1 = none yet): S[0x27] == 2 or none yet -> the record count (a new file), else the current one; 0x428a50 seals GD,
// writes dir/Savegame<slot>.dat, updates the record and rewrites dir/Savedata.dat (existing files copied to .bak
// first). S[0x27] = 0x0e written / 0x0f failed; S[0x29] = 0 whatever its value. Returns "" or the error.
std::string tbSaveRequest(TbWorld& w, int& slot, const std::filesystem::path& dir, const std::tm& now);

}  // namespace bb::game
