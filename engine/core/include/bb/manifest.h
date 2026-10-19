// Read-only inventory of an original install (LiT or TB): hashes, formats, BZE health.
#pragma once
#include "bb/bze.h"
#include "bb/scene.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace bb {

struct ManifestSection {
    BzeSection section;
    bool compressed = false;        // LZSS (LiT) vs stored (TB)
    uint64_t decodedSize = 0;       // payload size after decompression
    std::string error{};                  // decompression error, empty = OK
    std::optional<uint32_t> scriptItems{};  // section 1 only: items parsed when the level script parses fully
    std::string scriptError{};            // section 1 only: first level-script error
};

struct ManifestFile {
    std::string path;         // relative to install root, '/' separators
    uint64_t size = 0;
    std::string sha256;       // lowercase hex
    std::string md5;          // PE images only (version table uses MD5 for LiT)
    std::string format;       // bze, bmp, xa, exe-pe, dll-pe, exe-mz, ttf, wav, avi, mp4, cab, cab-is, pdf, unknown
    bool byMagic = false;     // false = format guessed from extension (headerless formats such as xa)
    std::optional<BzeVariant> bzeVariant;
    uint32_t tbBudget = 0;
    std::vector<ManifestSection> sections;
    std::string error;        // read / BZE parse error
};

struct Manifest {
    std::filesystem::path root;
    std::optional<GameId> game;
    std::string exePath, exeVersion;  // matched executable, empty if none matched
    std::vector<ManifestFile> files;  // sorted by path
};

// Walks `root` read-only. Never throws for per-file problems; they land in ManifestFile::error.
Manifest scanInstall(const std::filesystem::path& root);
std::string manifestJson(const Manifest& m);

} // namespace bb
