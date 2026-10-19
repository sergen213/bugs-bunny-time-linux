// LiT section compression (LZSS variant). Spec + evidence: docs/formats/BZE.md.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace bb {

struct LzssInfo {
    uint8_t mode = 0;          // header byte: bits 0-2 osize, bits 3-4 LUT step
    uint32_t storedCount = 0;  // 24-bit BE field
    uint32_t items = 0;        // items actually decoded
    size_t consumed = 0;       // input bytes consumed, including the 4-byte header
    bool tailDropped = false;  // final item was undecodable padding (see below)
};

// Decodes storedCount+1 items like the game (bugs.exe v1.6 FUN_00430ff0, which never bounds-checks).
// Some sections store the real item count instead of count-1; the game then decodes one extra item
// from padding. Here, if the final item is undecodable within `in` (pass actualSize bytes: truncated,
// or a back-reference of distance 0 / before the output start) it is dropped and tailDropped is set.
// Any such error before the final item throws FormatError.
std::vector<uint8_t> lzssDecompress(std::span<const uint8_t> in, LzssInfo* info = nullptr);

} // namespace bb
