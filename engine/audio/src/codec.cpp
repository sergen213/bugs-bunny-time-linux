// .xa IMA-ADPCM and AIFF decoding. Spec + evidence: docs/formats/AUDIO_XA.md.
#include "bb/audio.h"
#include "bb/bze.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <format>
#include <stdexcept>
#include <string_view>

extern "C" int stb_vorbis_decode_filename(const char* filename, int* channels, int* sample_rate, short** output);

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

XaStream::XaStream(const std::filesystem::path& p, XaLayout layout) : layout_(layout) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".wav" || ext == ".ogg") {
        pcm_ = loadPcm(p);
        layout_ = pcm_.channels == 1 ? XaLayout::Mono : XaLayout::Stereo;
        const uint64_t frames = pcm_.samples.size() / pcm_.channels;
        bytes_ = layout_ == XaLayout::Stereo ? frames : (frames + 1) / 2;
        return;
    }
    f_.open(p, std::ios::binary | std::ios::ate);
    if (!f_) throw std::runtime_error("cannot open " + p.string());
    bytes_ = uint64_t(f_.tellg());
    f_.seekg(0);
}

size_t XaStream::read(size_t maxBytes, std::vector<int16_t>& out) {
    if (pcm_.rate) {  // 1 "byte" = 1 stereo frame / 2 mono frames, as for .xa; extra channels are dropped
        const size_t ch = pcm_.channels, frames = layout_ == XaLayout::Stereo ? maxBytes : maxBytes * 2;
        const size_t total = pcm_.samples.size() / ch;
        size_t f = pcmPos_;
        for (; f < total && f < pcmPos_ + frames; ++f) {
            out.push_back(pcm_.samples[f * ch]);
            if (layout_ == XaLayout::Stereo) out.push_back(pcm_.samples[f * ch + 1]);
        }
        const size_t n = f - pcmPos_;
        pcmPos_ = f;
        return layout_ == XaLayout::Stereo ? n : (n + 1) / 2;
    }
    buf_.resize(maxBytes);
    f_.read(reinterpret_cast<char*>(buf_.data()), std::streamsize(maxBytes));
    const size_t n = size_t(f_.gcount());
    decodeXa(std::span(buf_).first(n), layout_, st_, out);
    return n;
}

void XaStream::rewind() {
    pcmPos_ = 0;
    if (pcm_.rate) return;
    f_.clear();
    f_.seekg(0);
    st_[0] = st_[1] = {};
}

Pcm loadPcm(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    Pcm out;
    if (ext == ".ogg") {
        int ch = 0, rate = 0;
        short* data = nullptr;
        const int frames = stb_vorbis_decode_filename(p.string().c_str(), &ch, &rate, &data);
        if (frames < 0 || !data || ch <= 0) throw std::runtime_error("cannot decode Ogg Vorbis " + p.string());
        out.channels = uint16_t(ch), out.rate = uint32_t(rate);
        out.samples.assign(data, data + size_t(frames) * size_t(ch));
        std::free(data);
        return out;
    }
    const std::vector<uint8_t> d = readFile(p);
    const std::span<const uint8_t> s(d);
    auto bad = [&](std::string_view what) { throw FormatError(std::format("WAV {}: {}", p.string(), what)); };
    if (d.size() < 12 || std::string_view(reinterpret_cast<const char*>(d.data()), 4) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(d.data() + 8), 4) != "WAVE")
        bad("not RIFF/WAVE");
    uint16_t fmt = 0, bits = 0;
    std::span<const uint8_t> data;
    for (size_t o = 12; o + 8 <= d.size();) {
        const std::string_view id(reinterpret_cast<const char*>(d.data() + o), 4);
        const size_t len = le32(s, o + 4);
        if (o + 8 + len > d.size()) bad("chunk past end of file");
        if (id == "fmt " && len >= 16) {
            fmt = uint16_t(d[o + 8] | d[o + 9] << 8), out.channels = uint16_t(d[o + 10] | d[o + 11] << 8);
            out.rate = le32(s, o + 12), bits = uint16_t(d[o + 22] | d[o + 23] << 8);
            if (fmt == 0xfffe && len >= 26) fmt = uint16_t(d[o + 32] | d[o + 33] << 8);  // WAVE_FORMAT_EXTENSIBLE subformat
        } else if (id == "data") {
            data = s.subspan(o + 8, len);
        } else if (id == "smpl" && len >= 36 + 24 && le32(s, o + 8 + 28) > 0) {
            out.loopStart = le32(s, o + 8 + 36 + 8), out.loopEnd = le32(s, o + 8 + 36 + 12) + 1;  // end is inclusive
        }
        o += 8 + len + (len & 1);
    }
    if (!out.channels || !out.rate) bad("no fmt chunk");
    const size_t bps = bits / 8;
    if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32)))
        bad(std::format("unsupported encoding (format {}, {} bits): save as 16-bit PCM", fmt, bits));
    for (size_t o = 0; o + bps <= data.size(); o += bps) {
        int32_t v = 0;
        if (fmt == 3) {
            float f;
            std::memcpy(&f, &data[o], 4);
            v = int32_t(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f));
        } else if (bits == 8) {
            v = (int32_t(data[o]) - 128) << 8;
        } else {
            v = int16_t(data[o + bps - 2] | data[o + bps - 1] << 8);  // the top 16 bits
        }
        out.samples.push_back(int16_t(v));
    }
    out.samples.resize(out.samples.size() / out.channels * out.channels);
    return out;
}

bool writeWav(const std::filesystem::path& p, uint16_t channels, uint32_t rate, std::span<const int16_t> samples, uint32_t loopStart,
              uint32_t loopEnd) {
    std::vector<uint8_t> o;
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k))); };
    auto u16 = [&](uint16_t v) { o.push_back(uint8_t(v)), o.push_back(uint8_t(v >> 8)); };
    auto tag = [&](const char* t) { o.insert(o.end(), t, t + 4); };
    const uint32_t bytes = uint32_t(samples.size() * 2);
    tag("RIFF"), u32(0), tag("WAVE"), tag("fmt "), u32(16), u16(1), u16(channels), u32(rate), u32(rate * channels * 2), u16(uint16_t(channels * 2)), u16(16);
    if (loopEnd > loopStart) {
        tag("smpl"), u32(36 + 24);
        for (int k = 0; k < 7; ++k) u32(k == 2 ? 1000000000u / rate : k == 3 ? 60 : 0);  // manufacturer..SMPTE offset
        u32(1), u32(0);                                                               // loops, sampler data
        u32(0), u32(0), u32(loopStart), u32(loopEnd - 1), u32(0), u32(0);              // id, type, start, end (incl.)
    }
    tag("data"), u32(bytes);
    for (const int16_t v : samples) u16(uint16_t(v));
    const uint32_t riff = uint32_t(o.size() - 8);
    for (int k = 0; k < 4; ++k) o[size_t(4 + k)] = uint8_t(riff >> (8 * k));
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size()));
    return bool(f);
}

AiffSound parseAiff(std::span<const uint8_t> d, BzeVariant game) {
    if (!isAiff(d)) fail(0, "missing FORM/AIFF header");
    const uint64_t formEnd = 8 + uint64_t(be32(d, 4));
    if (formEnd > d.size()) fail(4, "FORM size past end of data");
    AiffSound s;
    uint32_t frames = 0;
    std::span<const uint8_t> ssnd;
    bool comm = false;
    std::vector<std::pair<int16_t, uint32_t>> marks;  // MARK {id, position}
    int16_t loopBegin = 0, loopEnd = 0;               // INST sustain loop marker ids
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
        } else if (id == "MARK" && size >= 2) {
            for (size_t q = body + 2, n = be16(d, body); n-- && q + 7 <= body + size;) {
                marks.emplace_back(int16_t(be16(d, q)), be32(d, q + 2));
                q += 7 + d[q + 6] + ((d[q + 6] & 1) ^ 1);  // pstring padded to an even total
            }
        } else if (id == "INST" && size >= 14) {
            loopBegin = int16_t(be16(d, body + 10)), loopEnd = int16_t(be16(d, body + 12));
        }
        p = size_t(body + size + (size & 1));
    }
    if (!comm) fail(12, "no COMM chunk");
    if (ssnd.data() == nullptr) fail(12, "no SSND chunk");
    if (s.channels < 1 || s.channels > 2 || (s.bits != 8 && s.bits != 16) || s.rate == 0 ||
        (game == BzeVariant::TimeBusters && s.bits != 16))
        fail(12, std::format("unsupported format {} ch / {} bit / {} Hz", s.channels, s.bits, s.rate));
    auto setLoop = [&] {
        if (marks.size() < 2) return;
        auto at = [&](int16_t mid) {
            const auto m = std::ranges::find(marks, mid, &std::pair<int16_t, uint32_t>::first);
            return m == marks.end() ? 0u : m->second;
        };
        const uint32_t b = game == BzeVariant::TimeBusters ? marks[0].second : at(loopBegin);
        const uint32_t e = game == BzeVariant::TimeBusters ? marks[1].second : at(loopEnd);
        if (b < e && e <= s.frames()) s.loopStart = b, s.loopEnd = e;
    };
    if (game == BzeVariant::TimeBusters) {
        const XaLayout layout = s.channels == 2 ? XaLayout::Stereo : XaLayout::Mono;
        ImaState st[2]{};
        decodeXa(ssnd, layout, st, s.pcm);
        // Odd mono frame counts pad the last byte's high nibble; the game decodes every byte.
        s.truncated |= frames > xaFrames(ssnd.size(), layout) + (layout == XaLayout::Mono);
        setLoop();
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
    setLoop();
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
