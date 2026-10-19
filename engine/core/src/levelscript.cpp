#include "bb/levelscript.h"
#include "bb/bze.h"

#include <cstring>
#include <format>
#include <initializer_list>
#include <utility>

namespace bb {
namespace {

// Tag operand lengths per chunk type, from BugsDecomp doc/bze.md "Chunk Details" (LiT PC v1.6).
using Table = std::array<int8_t, 256>;  // -1 = tag not valid in this chunk type

Table table(std::initializer_list<std::pair<uint8_t, int8_t>> tags) {
    Table t;
    t.fill(-1);
    for (auto [id, len] : tags) t[id] = len;
    return t;
}

const Table* tableFor(uint8_t type) {
    static const Table t00 = table({{0x01, 8}, {0x02, 8}, {0x04, 8}, {0x44, 12}, {0x4c, 48}, {0x4d, 48}});
    static const Table t05 = table({{0x06, 3}});
    static const Table tEnt = table({{0x0b, 4},  {0x0c, 4},  {0x0f, 2},  {0x10, 12}, {0x11, 6},  {0x12, 12}, {0x13, 1},
                                     {0x15, 1},  {0x16, 8},  {0x17, 36}, {0x18, 16}, {0x19, 4},  {0x1a, 4},  {0x1b, 6},
                                     {0x1c, 8},  {0x1d, 4},  {0x1e, 2},  {0x1f, 1},  {0x27, 4},  {0x30, 32}, {0x31, 32},
                                     {0x32, 12}, {0x34, 24}, {0x35, 40}, {0x38, 2},  {0x39, 2},  {0x3a, 4},  {0x41, 20},
                                     {0x42, 6},  {0x46, 2}});
    static const Table t09 = table({{0x10, 12}, {0x11, 6}, {0x16, 8}, {0x1c, 8}, {0x1f, 1}, {0x33, 32}});
    static const Table t20 = table({{0x10, 12}, {0x11, 6}, {0x12, 12}, {0x21, 4}, {0x24, 8}, {0x39, 2}, {0x43, 1}});
    static const Table t22 =
        table({{0x0d, 12}, {0x24, 8}, {0x25, 8}, {0x26, 4}, {0x27, 4}, {0x28, 3}, {0x3f, 12}, {0x40, 8}});
    static const Table t29 = table({{0x2a, 8}});
    static const Table t36 = table({{0x37, 8}});
    static const Table t3b = table({{0x2b, 12}, {0x3c, 4}, {0x3d, 6}, {0x3e, 28}});
    static const Table t47 = table({{0x48, 8}});
    static const Table t49 = table({{0x4a, 48}});
    static const Table tPseudo = table({{0x45, 1}});
    switch (type) {
    case 0x00: return &t00;
    case 0x05: return &t05;
    case 0x07: case 0x08: case 0x0a: return &tEnt;
    case 0x09: return &t09;
    case 0x20: return &t20;
    case 0x22: return &t22;
    case 0x29: return &t29;
    case 0x36: return &t36;
    case 0x3b: return &t3b;
    case 0x47: return &t47;
    case 0x49: return &t49;
    case 0x45: return &tPseudo;  // pseudo-chunk has no type byte; keyed by its only tag
    default: return nullptr;
    }
}

template <class T, size_t N> std::array<T, N> decode(const std::vector<uint8_t>& d) {
    std::array<T, N> v;
    std::memcpy(v.data(), d.data(), sizeof v);  // little-endian
    return v;
}

void decodeTags(ScriptItem& it) {
    const bool entity = it.code == 0x07 || it.code == 0x08 || it.code == 0x0a;
    if (!entity && it.code != 0x09 && it.code != 0x20) return;
    for (const ScriptTag& t : it.tags) {
        switch (t.id) {
        case 0x10: it.position = decode<int32_t, 3>(t.data); break;
        case 0x11: it.rotation = decode<int16_t, 3>(t.data); break;
        case 0x12: it.scale = decode<int32_t, 3>(t.data); break;
        case 0x13: if (entity) it.player = t.data[0] == 1; break;
        case 0x27: if (entity) it.entityId = uint16_t(t.data[0] | t.data[1] << 8); break;
        }
    }
}

} // namespace

LevelScript parseLevelScript(std::span<const uint8_t> s, BzeVariant variant) {
    // TB structural bytes are the LiT ones minus 1 (GhidraEngineDiff, docs/re/TIME_BUSTERS.md);
    // TB commands and tag tables are not known yet, so TB fails at its first chunk type.
    const bool tb = variant == BzeVariant::TimeBusters;
    const uint8_t kChunk = tb ? 0x2c : 0x2d, kChunkEnd = tb ? 0x2d : 0x2e, kEnd = tb ? 0x2e : 0x2f;
    LevelScript out;
    size_t p = 0;
    auto byte = [&]() -> uint8_t {
        if (p >= s.size()) throw FormatError(std::format("levelscript: truncated at offset 0x{:x}", p));
        return s[p++];
    };
    // Reads tags of `type` until the chunk terminator.
    auto chunk = [&](ScriptItem& it, const Table& tab) {
        for (;;) {
            const uint32_t at = uint32_t(p);
            const uint8_t id = byte();
            if (id == kChunkEnd) return;
            if (tab[id] < 0)
                throw FormatError(std::format("levelscript: unknown tag 0x{:02x} in chunk type 0x{:02x} at offset 0x{:x}", id,
                                              it.code, at));
            if (s.size() - p < size_t(tab[id]))
                throw FormatError(std::format("levelscript: tag 0x{:02x} at offset 0x{:x} truncated", id, at));
            it.tags.push_back({id, at, {s.begin() + p, s.begin() + p + tab[id]}});
            p += size_t(tab[id]);
        }
    };

    if (!tb && s.size() >= 2 && s[0] == 0x2d && s[1] == 0x45) {
        ScriptItem& it = out.items.emplace_back(ScriptItem{.kind = ScriptItem::Kind::PseudoChunk, .code = 0x45});
        p = 1;
        chunk(it, *tableFor(0x45));
    }
    for (;;) {
        const uint32_t at = uint32_t(p);
        const uint8_t cmd = byte();
        if (cmd == kChunk) {
            const uint8_t type = byte();
            const Table* tab = tb || type == 0x45 ? nullptr : tableFor(type);
            if (!tab)
                throw FormatError(std::format("levelscript: unknown chunk type 0x{:02x} at offset 0x{:x}", type, at + 1));
            ScriptItem& it = out.items.emplace_back(ScriptItem{.kind = ScriptItem::Kind::Chunk, .code = type, .offset = at});
            chunk(it, *tab);
            decodeTags(it);
        } else if (cmd == kEnd) {
            out.items.push_back({.kind = ScriptItem::Kind::End, .code = cmd, .offset = at});
            out.end = uint32_t(p);
            return out;
        } else if (!tb && (cmd == 0x2c || cmd == 0x4b)) {
            out.items.push_back({.kind = ScriptItem::Kind::Command, .code = cmd, .offset = at});
        } else {
            throw FormatError(std::format("levelscript: unknown command 0x{:02x} at offset 0x{:x}", cmd, at));
        }
    }
}

} // namespace bb
