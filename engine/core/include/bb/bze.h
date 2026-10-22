// BZE level container (both games). Spec + evidence: docs/formats/BZE.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <vector>

namespace bb {

struct LzssInfo;

// Thrown for any malformed / truncated input. Message includes the offending offset.
struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::vector<uint8_t> readFile(const std::filesystem::path& path);

enum class BzeVariant : uint8_t {
    LostInTime,   // u32 version=1, u32 count, entries @0x08; sections LZSS-compressed
    TimeBusters,  // u32 0, u32 budget, u32 count, entries @0x0c; sections stored raw (PS1 .BZZ: u32 1 = LZSS)
};

struct BzeSection {
    uint32_t id = 0;
    uint32_t actualSize = 0;  // valid bytes (compressed size for LiT)
    uint32_t fileSize = 0;    // bytes occupied in the file, multiple of 0x800
    uint32_t offset = 0;      // absolute file offset
};

struct BzeFile {
    static constexpr uint32_t kHeaderSize = 0x800;
    static constexpr uint32_t kChecksumOffset = 0x7fc;

    BzeVariant variant = BzeVariant::LostInTime;
    uint32_t tbBudget = 0;  // TB only: 2nd header u32, always a multiple of 1000; meaning unverified
    bool compressed = false;  // sections LZSS-compressed: every LiT file and PS1 TB .BZZ files
    uint32_t checksum = 0;
    std::vector<BzeSection> sections;
    std::vector<uint8_t> bytes;  // whole file

    // actualSize bytes of section i as stored.
    std::span<const uint8_t> raw(size_t i) const;
    // Usable section contents: LZSS-decompressed (input bounded to actualSize) for LiT, raw copy for TB.
    std::vector<uint8_t> payload(size_t i, LzssInfo* info = nullptr) const;
    // Index of the first section with this id, or -1.
    int find(uint32_t id) const;
};

// Signed-byte sum of bytes [0, 0x7fc), as stored at 0x7fc.
uint32_t bzeChecksum(std::span<const uint8_t> header);

// Strict parse: header layout, checksum, contiguity and exact file length. Throws FormatError.
BzeFile parseBze(std::vector<uint8_t> bytes);
inline BzeFile loadBze(const std::filesystem::path& p) { return parseBze(readFile(p)); }

} // namespace bb
