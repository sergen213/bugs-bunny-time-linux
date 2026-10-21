// LiT save files (docs/formats/SAVE_LIT.md, GAMEPLAY.md §8.1): `..\bin\Savegame%d.dat` = raw GAME_DATA[0..0x13f]
// (current level, checksum, pad words, G[256]); `..\bin\Savedata.dat` = 0x168-B title summary (slot mask + per-slot
// golden carrots / clocks). Same bytes natively: original files import unchanged and round-trip byte-identically.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace bb::game {

struct LitSaveGame {
    static constexpr size_t kSize = 0x140;
    std::array<uint8_t, kSize> raw{};

    int level() const { return raw[0] | raw[1] << 8 | raw[2] << 16 | raw[3] << 24; }  // GD+0; 0 = none
    void setLevel(int id) { for (int i = 0; i < 4; ++i) raw[size_t(i)] = uint8_t(uint32_t(id) >> (8 * i)); }
    // Level the load request (0x407190 case 5) continues in: saved level, or 0x38 (CCINTRO) when it is 0.
    int startLevel() const { return level() ? raw[0] : 0x38; }
    int32_t checksum() const { return int32_t(raw[4] | raw[5] << 8 | raw[6] << 16 | uint32_t(raw[7]) << 24); }
    std::span<uint8_t, 256> G() { return std::span<uint8_t, 256>(raw.data() + 0x40, 256); }
    std::span<const uint8_t, 256> G() const { return std::span<const uint8_t, 256>(raw.data() + 0x40, 256); }
    int slot() const { return raw[0x40 + 0x9f]; }  // G[0x9f], the %d of the file name
    // Writer 0x4074f0: +4 = sum of all 320 bytes as signed chars, the old +4 dword included (not verified on load).
    void seal();
    // New-game request (0x407190 case 4): G[] = 0 except health/max 6, G[0] 3, slot and language; analog calibration.
    static LitSaveGame newGame(int slot, uint8_t language = 0);
};

struct LitSaveData {
    static constexpr size_t kSize = 0x168;
    std::array<uint8_t, kSize> raw{};

    uint32_t mask() const { return raw[4] | raw[5] << 8 | raw[6] << 16 | uint32_t(raw[7]) << 24; }  // bit slot-1
    // Slot summary at +8 + 0x2c * (slot - 1): u16 golden carrots, u16 clocks (title menu, 0x4076f0).
    int golden(int slot) const;
    int clocks(int slot) const;
    // 0x4074f0 after a slot write: mask |= bit, summary = {G[7] | G[0xfc] << 8, G[5]}.
    void record(const LitSaveGame& g);
};

// Validate original bytes (throw FormatError): exact size; Savegame slot byte G[0x9f] = file number (the writer names
// the file after it), level id < 111 (exe level table). The checksum cannot be checked: it includes the previous one.
LitSaveGame parseLitSaveGame(std::span<const uint8_t> file, int slot);
LitSaveData parseLitSaveData(std::span<const uint8_t> file);

std::filesystem::path litSaveGamePath(const std::filesystem::path& dir, int slot);  // dir/Savegame<slot>.dat
std::filesystem::path litSaveDataPath(const std::filesystem::path& dir);            // dir/Savedata.dat
// Reads dir/Savedata.dat; a missing file is the zeroed summary the original creates (0x4076f0). Throws on bad data.
LitSaveData readLitSaveData(const std::filesystem::path& dir);
// The save request (S[0x0a] = 6 -> 0x4074f0): seals g, writes Savegame<slot>.dat and the updated Savedata.dat.
// Existing files are first copied to <name>.bak. Returns "" or an error.
std::string writeLitSave(const std::filesystem::path& dir, LitSaveGame& g);
// Copies every valid Savegame0..6.dat and Savedata.dat from an original install's Datas/bin to dir (with .bak
// backups of replaced files). Returns a one-line report; invalid files are skipped and named in it.
std::string importLitSaves(const std::filesystem::path& from, const std::filesystem::path& dir);

}  // namespace bb::game
