#include "bb/game/lit_save.h"

#include "bb/bze.h"
#include "bb/game/lit_world.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace fs = std::filesystem;

namespace bb::game {

void LitSaveGame::seal() {
    int32_t sum = 0;
    for (uint8_t b : raw) sum += int8_t(b);
    for (int i = 0; i < 4; ++i) raw[size_t(4 + i)] = uint8_t(uint32_t(sum) >> (8 * i));
}

LitSaveGame LitSaveGame::newGame(int slot, uint8_t language) {
    LitSaveGame g;
    auto G = g.G();
    G[0] = 3;
    G[1] = G[2] = 6;
    G[0x0e] = 0x38;  // next level CCINTRO
    G[0x10] = language;
    G[0x9f] = uint8_t(slot);
    // Analog calibration u16s +0x16..+0x20: min 0, max 0xff, centre 0x7f (X then Y).
    for (size_t o : {0x18u, 0x1eu}) g.raw[o] = 0xff;
    for (size_t o : {0x1au, 0x20u}) g.raw[o] = 0x7f;
    return g;
}

int LitSaveData::golden(int slot) const {
    const size_t o = 8 + 0x2c * size_t(slot - 1);
    return raw[o] | raw[o + 1] << 8;
}

int LitSaveData::clocks(int slot) const {
    const size_t o = 8 + 0x2c * size_t(slot - 1);
    return raw[o + 2] | raw[o + 3] << 8;
}

void LitSaveData::record(const LitSaveGame& g) {
    const int slot = g.slot();
    raw[4 + size_t(((slot - 1) & 31) >> 3)] |= uint8_t(1 << ((slot - 1) & 7));  // 1 << (slot - 1 & 0x1f)
    if (slot < 1 || slot > 6) return;  // the summary switch covers slots 1..6 only
    const size_t o = 8 + 0x2c * size_t(slot - 1);
    const auto G = g.G();
    raw[o] = G[7];
    raw[o + 1] = G[0xfc];
    raw[o + 2] = G[5];
    raw[o + 3] = 0;
}

LitSaveGame parseLitSaveGame(std::span<const uint8_t> file, int slot) {
    if (file.size() != LitSaveGame::kSize)
        throw FormatError(std::format("Savegame{}.dat: {} bytes, expected {}", slot, file.size(), LitSaveGame::kSize));
    LitSaveGame g;
    std::ranges::copy(file, g.raw.begin());
    if (g.slot() != slot) throw FormatError(std::format("Savegame{}.dat: slot byte {} != {}", slot, g.slot(), slot));
    if (g.level() < 0 || LitWorld::levelName(g.level()).empty())
        throw FormatError(std::format("Savegame{}.dat: level {} out of range", slot, g.level()));
    return g;
}

LitSaveData parseLitSaveData(std::span<const uint8_t> file) {
    if (file.size() != LitSaveData::kSize)
        throw FormatError(std::format("Savedata.dat: {} bytes, expected {}", file.size(), LitSaveData::kSize));
    LitSaveData d;
    std::ranges::copy(file, d.raw.begin());
    return d;
}

fs::path litSaveGamePath(const fs::path& dir, int slot) { return dir / std::format("Savegame{}.dat", slot); }
fs::path litSaveDataPath(const fs::path& dir) { return dir / "Savedata.dat"; }

LitSaveData readLitSaveData(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(litSaveDataPath(dir), ec) ? parseLitSaveData(readFile(litSaveDataPath(dir))) : LitSaveData{};
}

namespace {

// Backs up an existing file to <name>.bak, then replaces it via a temporary (no torn file on a failed write).
std::string replaceFile(const fs::path& path, std::span<const uint8_t> bytes) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::copy_file(path, path.string() + ".bak", fs::copy_options::overwrite_existing, ec);
        if (ec) return std::format("{}: backup failed: {}", path.string(), ec.message());
    }
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f.flush()) return std::format("{}: write failed", tmp.string());
    }
    fs::rename(tmp, path, ec);
    return ec ? std::format("{}: {}", path.string(), ec.message()) : "";
}

}  // namespace

std::string writeLitSave(const fs::path& dir, LitSaveGame& g) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    LitSaveData d;
    try {
        d = readLitSaveData(dir);
    } catch (const std::exception& e) {
        return e.what();  // keep a damaged summary untouched rather than overwrite it
    }
    g.seal();
    if (std::string e = replaceFile(litSaveGamePath(dir, g.slot()), g.raw); !e.empty()) return e;
    d.record(g);
    return replaceFile(litSaveDataPath(dir), d.raw);
}

std::string importLitSaves(const fs::path& from, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string imported, skipped;
    auto take = [&](const fs::path& src, std::span<const uint8_t> bytes) {
        if (std::string e = replaceFile(dir / src.filename(), bytes); !e.empty()) skipped += " " + e;
        else imported += " " + src.filename().string();
    };
    for (int slot = 0; slot <= 6; ++slot) {
        const fs::path src = litSaveGamePath(from, slot);
        if (!fs::exists(src, ec)) continue;
        try {
            const std::vector<uint8_t> bytes = readFile(src);
            const LitSaveGame g = parseLitSaveGame(bytes, slot);
            take(src, g.raw);
        } catch (const std::exception& e) {
            skipped += std::format(" [{}]", e.what());
        }
    }
    if (fs::exists(litSaveDataPath(from), ec)) {
        try {
            take(litSaveDataPath(from), parseLitSaveData(readFile(litSaveDataPath(from))).raw);
        } catch (const std::exception& e) {
            skipped += std::format(" [{}]", e.what());
        }
    }
    return std::format("imported:{} | skipped:{}", imported.empty() ? " none" : imported,
                       skipped.empty() ? " none" : skipped);
}

}  // namespace bb::game
