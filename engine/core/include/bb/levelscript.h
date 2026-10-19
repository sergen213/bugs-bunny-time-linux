// LiT BZE section 1 "load instructions" tag stream. Spec + evidence: docs/formats/BZE.md.
#pragma once
#include "bb/bze.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bb {

struct ScriptTag {
    uint8_t id = 0;
    uint32_t offset = 0;        // of the tag byte within the section
    std::vector<uint8_t> data;  // fixed-length operand bytes
};

struct ScriptItem {
    enum class Kind : uint8_t { PseudoChunk, Chunk, Command, End };
    Kind kind = Kind::Chunk;
    uint8_t code = 0;     // chunk type byte, or command byte (0x2c / 0x4b / 0x2f)
    uint32_t offset = 0;  // of the leading 0x2d / command byte
    std::vector<ScriptTag> tags{};

    // Decoded from tags 0x10/0x11/0x12 (types 07/08/09/0a/20) and 0x13/0x27 (types 07/08/0a).
    std::optional<std::array<int32_t, 3>> position{};  // 20.12 fixed
    std::optional<std::array<int16_t, 3>> rotation{};  // 4.12, 0x1000 = 360 degrees
    std::optional<std::array<int32_t, 3>> scale{};     // 20.12 fixed
    bool player = false;                             // tag 0x13 byte == 1
    std::optional<uint16_t> entityId{};                // tag 0x27 short @0
};

struct LevelScript {
    std::vector<ScriptItem> items;
    uint32_t end = 0;  // offset just past the 0x2f end command
};

// Throws FormatError with the offset of the first unknown command / chunk type / tag, or truncation.
// TimeBusters: structural bytes only (see docs/formats/BZE.md); fails at the first chunk type.
LevelScript parseLevelScript(std::span<const uint8_t> section1, BzeVariant variant = BzeVariant::LostInTime);

} // namespace bb
