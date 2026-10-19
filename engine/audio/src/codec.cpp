// .xa IMA-ADPCM and AIFF decoding. Spec + evidence: docs/formats/AUDIO_XA.md.
#include "bb/audio.h"
#include "bb/bze.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <stdexcept>
#include <string_view>

namespace bb {
namespace {

// bugs.exe v1.6 tables: step @0x45c0c0 (89 × s32), index adjust @0x45c080 (16 × s32 = these 8, twice).
constexpr int16_t kStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int8_t kAdjust[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

uint32_t be32(std::span<const uint8_t> d, size_t o) {
    return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
uint16_t be16(std::span<const uint8_t> d, size_t o) { return uint16_t(d[o] << 8 | d[o + 1]); }
uint32_t le32(std::span<const uint8_t> d, size_t o) {
    return uint32_t(d[o]) | uint32_t(d[o + 1]) << 8 | uint32_t(d[o + 2]) << 16 | uint32_t(d[o + 3]) << 24;
}

[[noreturn]] void fail(size_t off, std::string_view what) {
    throw FormatError(std::format("AIFF: {} at offset {:#x}", what, off));
}

bool isAiff(std::span<const uint8_t> d) {
    return d.size() >= 12 && std::string_view(reinterpret_cast<const char*>(d.data()), 4) == "FORM" &&
           std::string_view(reinterpret_cast<const char*>(d.data() + 8), 4) == "AIFF";
}

} // namespace

int16_t ImaState::decode(uint8_t n) {
    const int step = kStep[index];
    index = uint8_t(std::clamp(index + kAdjust[n & 7], 0, 88));
    int delta = step >> 3;
    if (n & 4) delta += step;
    if (n & 2) delta += step >> 1;
    if (n & 1) delta += step >> 2;
    sample = int16_t(std::clamp(sample + ((n & 8) ? -delta : delta), -32768, 32767));
    return sample;
}

XaLayout xaLayoutFor(const std::filesystem::path& p) {
    std::string gp = p.parent_path().parent_path().filename().string();
    std::ranges::transform(gp, gp.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return gp == "speech" ? XaLayout::Mono : XaLayout::Stereo;
}

void decodeXa(std::span<const uint8_t> in, XaLayout layout, ImaState (&st)[2], std::vector<int16_t>& out) {
    out.reserve(out.size() + in.size() * 2);
    for (uint8_t b : in) {
        if (layout == XaLayout::Stereo) {
            out.push_back(st[0].decode(b & 15));
            out.push_back(st[1].decode(b >> 4));
        } else {
            out.push_back(st[0].decode(b & 15));
            out.push_back(st[0].decode(b >> 4));
        }
    }
}

XaStream::XaStream(const std::filesystem::path& p, XaLayout layout)
    : f_(p, std::ios::binary | std::ios::ate), layout_(layout) {
    if (!f_) throw std::runtime_error("cannot open " + p.string());
    bytes_ = uint64_t(f_.tellg());
    f_.seekg(0);
}

size_t XaStream::read(size_t maxBytes, std::vector<int16_t>& out) {
    buf_.resize(maxBytes);
    f_.read(reinterpret_cast<char*>(buf_.data()), std::streamsize(maxBytes));
    const size_t n = size_t(f_.gcount());
    decodeXa(std::span(buf_).first(n), layout_, st_, out);
    return n;
}

void XaStream::rewind() {
    f_.clear();
    f_.seekg(0);
    st_[0] = st_[1] = {};
}

AiffSound parseAiff(std::span<const uint8_t> d, BzeVariant game) {
    if (!isAiff(d)) fail(0, "missing FORM/AIFF header");
    const uint64_t formEnd = 8 + uint64_t(be32(d, 4));
    if (formEnd > d.size()) fail(4, "FORM size past end of data");
    AiffSound s;
    uint32_t frames = 0;
    std::span<const uint8_t> ssnd;
    bool comm = false;
    for (size_t p = 12; p + 8 <= formEnd;) {
        const std::string_view id(reinterpret_cast<const char*>(d.data() + p), 4);
        uint64_t size = be32(d, p + 4);
        const uint64_t body = p + 8;
        if (body + size > formEnd) {
            // 4 TB sounds (ARBOSS01) have an SSND 8 bytes longer than the FORM; the game reads it anyway.
            if (id != "SSND") fail(p, std::format("chunk '{}' overruns FORM", id));
            size = formEnd - body;
            s.truncated = true;
        }
        if (id == "COMM") {
            if (size < 18) fail(p, "short COMM");
            s.channels = be16(d, body);
            frames = be32(d, body + 2);
            s.bits = be16(d, body + 6);
            // 80-bit IEEE extended sample rate: exponent (bias 16383), 64-bit mantissa with explicit 1.
            const int exp = (be16(d, body + 8) & 0x7fff) - 16383;
            const uint64_t mant = uint64_t(be32(d, body + 10)) << 32 | be32(d, body + 14);
            if (exp < 0 || exp > 31) fail(body + 8, "unsupported sample rate");
            s.rate = uint32_t(mant >> (63 - exp));
            comm = true;
        } else if (id == "SSND") {
            if (size < 8) fail(p, "short SSND");
            const uint64_t off = be32(d, body);
            if (8 + off > size) fail(p, "SSND offset past chunk");
            ssnd = d.subspan(size_t(body + 8 + off), size_t(size - 8 - off));
        }
        p = size_t(body + size + (size & 1));
    }
    if (!comm) fail(12, "no COMM chunk");
    if (ssnd.data() == nullptr) fail(12, "no SSND chunk");
    if (s.channels < 1 || s.channels > 2 || (s.bits != 8 && s.bits != 16) || s.rate == 0 ||
        (game == BzeVariant::TimeBusters && s.bits != 16))
        fail(12, std::format("unsupported format {} ch / {} bit / {} Hz", s.channels, s.bits, s.rate));
    if (game == BzeVariant::TimeBusters) {
        const XaLayout layout = s.channels == 2 ? XaLayout::Stereo : XaLayout::Mono;
        ImaState st[2]{};
        decodeXa(ssnd, layout, st, s.pcm);
        // Odd mono frame counts pad the last byte's high nibble; the game decodes every byte.
        s.truncated |= frames > xaFrames(ssnd.size(), layout) + (layout == XaLayout::Mono);
        return s;
    }
    // Some LiT sounds declare more frames than SSND holds (bugs.exe 0x41e950 then reads past the chunk into the
    // next blob); keep only the real data.
    const size_t bytesPer = s.bits / 8, have = ssnd.size() / (bytesPer * s.channels);
    s.truncated |= frames > have;
    const size_t count = std::min<size_t>(frames, have) * s.channels;
    s.pcm.resize(count);
    for (size_t i = 0; i < count; ++i)
        s.pcm[i] = bytesPer == 2 ? int16_t(be16(ssnd, i * 2)) : int16_t(int8_t(ssnd[i]) * 256);
    return s;
}

std::vector<std::span<const uint8_t>> sfxBank(std::span<const uint8_t> sec) {
    std::vector<std::span<const uint8_t>> out;
    if (sec.size() < 4) return out;
    const uint64_t n = le32(sec, 0);
    if (n == 0 || 4 + n * 4 > sec.size()) return out;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t o = le32(sec, 4 + i * 4);
        auto blob = sec.subspan(std::min<size_t>(o, sec.size()));
        if (!isAiff(blob)) return {};
        out.push_back(blob.first(std::min<size_t>(blob.size(), 8 + uint64_t(be32(blob, 4)))));
    }
    return out;
}

} // namespace bb
