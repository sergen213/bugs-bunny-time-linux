#include "bb/bze.h"
#include "bb/lzss.h"

#include <cstring>
#include <format>
#include <fstream>

namespace bb {

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    if (size < 0) throw std::runtime_error("cannot size " + path.string());
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) throw std::runtime_error("cannot read " + path.string());
    return buf;
}

static uint32_t u32(std::span<const uint8_t> b, size_t off) {
    uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;  // little-endian host assumed (x86-64/aarch64 Linux)
}

uint32_t bzeChecksum(std::span<const uint8_t> header) {
    uint32_t sum = 0;
    for (size_t i = 0; i < BzeFile::kChecksumOffset; ++i) sum += uint32_t(int32_t(int8_t(header[i])));
    return sum;
}

BzeFile parseBze(std::vector<uint8_t> bytes) {
    BzeFile f;
    if (bytes.size() < BzeFile::kHeaderSize)
        throw FormatError(std::format("bze: file is {} bytes, header needs {}", bytes.size(), BzeFile::kHeaderSize));
    size_t entries;
    uint32_t count;
    switch (u32(bytes, 0)) {
    case 1:
        // PS1 TB (.BZZ): TB header with first word 1 = LZSS sections; LiT counts never reach a TB budget (≥ 1000).
        if (u32(bytes, 4) >= 1000) {
            f.variant = BzeVariant::TimeBusters, f.compressed = true, f.tbBudget = u32(bytes, 4);
            count = u32(bytes, 8), entries = 12;
        } else {
            f.variant = BzeVariant::LostInTime, f.compressed = true, count = u32(bytes, 4), entries = 8;
        }
        break;
    case 0: f.variant = BzeVariant::TimeBusters; f.tbBudget = u32(bytes, 4); count = u32(bytes, 8); entries = 12; break;
    default: throw FormatError(std::format("bze: unknown first header word 0x{:x} at offset 0", u32(bytes, 0)));
    }
    if (count == 0 || count > (BzeFile::kChecksumOffset - entries) / 12)
        throw FormatError(std::format("bze: section count {} at offset {} out of range", count, entries - 4));
    f.checksum = u32(bytes, BzeFile::kChecksumOffset);
    if (const uint32_t sum = bzeChecksum(bytes); sum != f.checksum)
        throw FormatError(std::format("bze: header checksum 0x{:x} at offset 0x7fc, computed 0x{:x}", f.checksum, sum));

    uint64_t off = BzeFile::kHeaderSize;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = entries + 12 * i;
        BzeSection s{u32(bytes, e), u32(bytes, e + 4), u32(bytes, e + 8), uint32_t(off)};
        if (s.fileSize % 0x800 || s.actualSize > s.fileSize)
            throw FormatError(std::format("bze: section entry at offset 0x{:x}: actual 0x{:x} / file 0x{:x} invalid", e,
                                          s.actualSize, s.fileSize));
        off += s.fileSize;
        if (off > bytes.size())
            throw FormatError(std::format("bze: section entry at offset 0x{:x} ends at 0x{:x}, file is 0x{:x}", e, off,
                                          bytes.size()));
        f.sections.push_back(s);
    }
    if (off != bytes.size())
        throw FormatError(std::format("bze: sections end at 0x{:x} but file is 0x{:x}", off, bytes.size()));
    f.bytes = std::move(bytes);
    return f;
}

std::span<const uint8_t> BzeFile::raw(size_t i) const {
    const BzeSection& s = sections.at(i);
    return std::span(bytes).subspan(s.offset, s.actualSize);
}

std::vector<uint8_t> BzeFile::payload(size_t i, LzssInfo* info) const {
    const auto r = raw(i);
    if (!compressed) return {r.begin(), r.end()};
    return lzssDecompress(r, info);
}

int BzeFile::find(uint32_t id) const {
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].id == id) return int(i);
    return -1;
}

} // namespace bb
