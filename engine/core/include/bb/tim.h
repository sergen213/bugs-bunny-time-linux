// PlayStation TIM image -> bb::Texture (RGBA8). Spec + evidence: docs/formats/TEXTURES.md §2-3.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace bb {

// Decodes the TIM starting at data[offset]. 4-bit and 8-bit CLUT (row 0 only, block skipped by its size) and
// 15-bit direct. Colour rule of the PC port (bugs.exe FUN_00420d10, opaque variant): BGR555 << 3, no bit
// replication; 0x0000 -> fully transparent (rgba 0); every other value opaque, STP bit ignored (0x8000 = opaque
// black). Throws FormatError for a bad id, 24-bit mode (the game rejects it too), short CLUT, or truncation.
Texture decodeTim(std::span<const uint8_t> data, size_t offset, std::string name);

} // namespace bb
