// Synthetic vectors for the .xa IMA-ADPCM decoder, AIFF parser and section-3 sound bank table.
#include "bb/audio.h"
#include "bb/bze.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

using namespace bb;

static std::vector<int16_t> dec(std::vector<uint8_t> in, XaLayout l) {
    ImaState st[2]{};
    std::vector<int16_t> out;
    decodeXa(in, l, st, out);
    return out;
}

static bool throws(std::vector<uint8_t> d, BzeVariant v = BzeVariant::LostInTime) {
    try {
        parseAiff(d, v);
    } catch (const FormatError&) {
        return true;
    }
    return false;
}

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int s : {24, 16, 8, 0}) v.push_back(uint8_t(x >> s)); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }

// Minimal AIFF: COMM (ch, frames, bits, 44100 Hz) + SSND(data); optional odd-sized chunk before COMM.
static std::vector<uint8_t> aiff(uint16_t ch, uint32_t frames, uint16_t bits, std::vector<uint8_t> data, bool oddChunk) {
    std::vector<uint8_t> b;
    if (oddChunk) { b.insert(b.end(), {'A', 'P', 'P', 'L'}); put32(b, 3); b.insert(b.end(), {1, 2, 3, 0}); }
    b.insert(b.end(), {'C', 'O', 'M', 'M'}); put32(b, 18);
    put16(b, ch); put32(b, frames); put16(b, bits);
    b.insert(b.end(), {0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0});
    b.insert(b.end(), {'S', 'S', 'N', 'D'}); put32(b, uint32_t(8 + data.size()));
    put32(b, 0); put32(b, 0);
    b.insert(b.end(), data.begin(), data.end());
    std::vector<uint8_t> f = {'F', 'O', 'R', 'M'};
    put32(f, uint32_t(4 + b.size()));
    f.insert(f.end(), {'A', 'I', 'F', 'F'});
    f.insert(f.end(), b.begin(), b.end());
    return f;
}

int main() {
    // Hand-computed IMA steps from (0, 0): nibble 7 -> step 7: 0+7+3+1 = 11, index 8;
    // then nibble 0xf -> step 16: -(2+16+8+4) = -19, index 16.
    {
        ImaState s;
        assert(s.decode(7) == 11 && s.index == 8);
        assert(s.decode(0xf) == -19 && s.index == 16);
        assert(s.decode(0) == -15 && s.index == 15);  // step kStep[16] = 34: +34>>3 = +4
    }
    // Index floor: nibble 0 at index 0 adds 7>>3 = 0 and keeps index 0.
    {
        ImaState s;
        for (int i = 0; i < 10; ++i) assert(s.decode(0) == 0 && s.index == 0);
        assert(s.decode(8) == 0 && s.index == 0);  // sign bit with zero magnitude: -0
    }
    // Saturation: repeated max nibbles clamp sample to s16 and index to 88.
    {
        ImaState s;
        for (int i = 0; i < 200; ++i) s.decode(7);
        assert(s.sample == 32767 && s.index == 88);
        for (int i = 0; i < 200; ++i) s.decode(0xf);
        assert(s.sample == -32768 && s.index == 88);
    }
    // Layouts: stereo = low nibble left / high right; mono = low then high on one channel.
    assert((dec({0x7f}, XaLayout::Stereo) == std::vector<int16_t>{-11, 11}));
    assert((dec({0x7f}, XaLayout::Mono) == std::vector<int16_t>{-11, 19}));
    assert(xaFrames(10, XaLayout::Stereo) == 10 && xaFrames(10, XaLayout::Mono) == 20);
    assert(xaLayoutFor("tb/Datas/SPEECH/00/Speech0.xa") == XaLayout::Mono);
    assert(xaLayoutFor("lit/Datas/speeches/speech00.xa") == XaLayout::Stereo);
    assert(xaLayoutFor("tb/Datas/TRACK/track1.xa") == XaLayout::Stereo);
    // Truncated input: no header/blocks, so any prefix decodes to the prefix of the full decode.
    {
        std::vector<uint8_t> in(1001);
        for (size_t i = 0; i < in.size(); ++i) in[i] = uint8_t(i * 37 + 11);
        const auto full = dec(in, XaLayout::Stereo);
        assert(full.size() == 2002);
        const auto part = dec({in.begin(), in.begin() + 333}, XaLayout::Stereo);
        assert(std::equal(part.begin(), part.end(), full.begin()));
        assert(dec({}, XaLayout::Mono).empty());

        // XaStream: chunked reads equal one-shot decode; rewind restarts with fresh state.
        const auto path = std::filesystem::temp_directory_path() / "bb_test_codec.xa";
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(in.data()), std::streamsize(in.size()));
        XaStream xs(path, XaLayout::Stereo);
        assert(xs.bytes() == 1001 && xs.frames() == 1001);
        std::vector<int16_t> got;
        while (xs.read(64, got) > 0) {}
        assert(got == full);
        xs.rewind();
        got.clear();
        assert(xs.read(5000, got) == 1001 && got == full);
        std::filesystem::remove(path);
    }
    // AIFF: 16-bit big-endian, odd chunk padding, 8-bit, and malformed/truncated input.
    {
        const auto a = parseAiff(aiff(1, 3, 16, {0x00, 0x01, 0xff, 0xfe, 0x80, 0x00}, true));
        assert(a.channels == 1 && a.bits == 16 && a.rate == 44100);
        assert((a.pcm == std::vector<int16_t>{1, -2, -32768}));
        const auto b = parseAiff(aiff(2, 1, 8, {0x7f, 0x80}, false));
        assert(b.channels == 2 && (b.pcm == std::vector<int16_t>{127 * 256, -128 * 256}));
        auto good = aiff(1, 3, 16, {0, 1, 0, 2, 0, 3}, false);
        assert(throws({good.begin(), good.end() - 1}));                       // FORM size past end
        const auto t = parseAiff(aiff(1, 4, 16, {0, 1, 0, 2, 0, 3, 9}, false));  // frames exceed SSND
        assert(t.truncated && (t.pcm == std::vector<int16_t>{1, 2, 3}));
        // TB: SSND is IMA ADPCM, length from SSND size (mono: 2 frames per byte, low nibble first).
        const auto m = parseAiff(aiff(1, 2, 16, {0x7f}, false), BzeVariant::TimeBusters);
        assert(!m.truncated && (m.pcm == std::vector<int16_t>{-11, 19}));
        const auto st = parseAiff(aiff(2, 2, 16, {0x7f}, false), BzeVariant::TimeBusters);
        assert(st.truncated && (st.pcm == std::vector<int16_t>{-11, 11}));
        assert(throws(aiff(1, 1, 8, {0}, false), BzeVariant::TimeBusters));
        assert(throws(aiff(3, 1, 16, {0, 0, 0, 0, 0, 0}, false)));           // unsupported channels
        assert(throws({'F', 'O', 'R', 'M', 0, 0, 0, 4, 'A', 'I', 'F', 'C'})); // not AIFF
        assert(throws({}));
    }
    // Section-3 sound bank table vs a TIM-first section.
    {
        const auto s1 = aiff(1, 1, 16, {0, 5}, false), s2 = aiff(1, 2, 16, {0, 6, 0, 7}, false);
        std::vector<uint8_t> sec = {2, 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0};
        const uint32_t o2 = uint32_t(12 + s1.size());
        for (int i = 0; i < 4; ++i) sec[8 + i] = uint8_t(o2 >> (8 * i));
        sec.insert(sec.end(), s1.begin(), s1.end());
        sec.insert(sec.end(), s2.begin(), s2.end());
        sec.insert(sec.end(), {0x10, 0, 0, 0});  // trailing TIM
        const auto bank = sfxBank(sec);
        assert(bank.size() == 2 && bank[0].size() == s1.size() && bank[1].size() == s2.size());
        assert(parseAiff(bank[1]).pcm == (std::vector<int16_t>{6, 7}));
        assert(sfxBank(std::vector<uint8_t>{0x10, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0}).empty());
        assert(sfxBank(std::vector<uint8_t>{0xff, 0xff, 0xff, 0xff}).empty());
    }
    std::println("audio.codec: ok");
    return 0;
}
