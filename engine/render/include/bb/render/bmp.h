// Minimal Windows BMP reader for the original games' menu/title bitmaps.
// Supports uncompressed (BI_RGB) 8-bit paletted and 24-bit images with a BITMAPINFOHEADER or later
// (40/52/56/108/124-byte) DIB header, bottom-up or top-down. That covers every .bmp shipped with
// LiT and TB (checked with file(1) on the install trees). Anything else is rejected, never guessed.
#pragma once
#include "bb/scene.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace bb::render {

// Returns an RGBA8 top-down texture (alpha 255) or an error message.
std::expected<Texture, std::string> decodeBmp(std::span<const uint8_t> file);
std::expected<Texture, std::string> loadBmp(const std::filesystem::path& path);

} // namespace bb::render
