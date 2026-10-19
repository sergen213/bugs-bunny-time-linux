// PS1 TIM decoder on synthetic images: 4/8/15-bit, colour/transparency rule, strict truncation errors.
#include "bb/tim.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

struct W {
    std::vector<uint8_t> b;
    void u16(uint32_t v) { b.push_back(uint8_t(v)), b.push_back(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(v & 0xffff), u16(v >> 16); }
};

// TIM with optional 1-row CLUT; `pix` are raw u16 image words, image width in 16-bit units = wWords.
std::vector<uint8_t> tim(uint32_t mode, const std::vector<uint16_t>& clut, uint16_t wWords, uint16_t h,
                         const std::vector<uint16_t>& pix) {
    W w;
    w.u32(0x10);
    w.u32(mode | (clut.empty() ? 0 : 8));
    if (!clut.empty()) {
        w.u32(12 + uint32_t(clut.size()) * 2);
        w.u16(0), w.u16(0), w.u16(uint16_t(clut.size())), w.u16(1);
        for (uint16_t c : clut) w.u16(c);
    }
    w.u32(12 + uint32_t(pix.size()) * 2);
    w.u16(0), w.u16(0), w.u16(wWords), w.u16(h);
    for (uint16_t p : pix) w.u16(p);
    return w.b;
}

bool throws(const std::vector<uint8_t>& d, size_t off = 0) {
    try {
        bb::decodeTim(d, off, "t");
    } catch (const bb::FormatError&) {
        return true;
    }
    return false;
}

const uint8_t* px(const bb::Texture& t, uint32_t x, uint32_t y) { return &t.rgba[(size_t(y) * t.width + x) * 4]; }

} // namespace

int main() {
    // 4-bit: 16-colour CLUT; low nibble = left pixel. Entry 0 = 0x0000 (transparent), 1 = red, 2 = 0x8000 (opaque black).
    std::vector<uint16_t> clut16(16, 0x7fff);
    clut16[0] = 0x0000, clut16[1] = 0x001f, clut16[2] = 0x8000, clut16[3] = 0x7c00;
    const auto t4 = tim(0, clut16, 1, 2, {0x3210, 0x0001});
    const bb::Texture a = bb::decodeTim(t4, 0, "a");
    assert(a.width == 4 && a.height == 2 && a.hasAlpha && a.rgba.size() == 32);
    assert(px(a, 0, 0)[3] == 0 && px(a, 0, 0)[0] == 0);                                     // 0x0000 -> transparent
    assert(px(a, 1, 0)[0] == 248 && px(a, 1, 0)[1] == 0 && px(a, 1, 0)[3] == 255);         // red, << 3 no replication
    assert(px(a, 2, 0)[0] == 0 && px(a, 2, 0)[2] == 0 && px(a, 2, 0)[3] == 255);           // STP black is opaque
    assert(px(a, 3, 0)[2] == 248 && px(a, 3, 0)[0] == 0);                                  // blue = bits 10-14
    assert(px(a, 0, 1)[0] == 248 && px(a, 1, 1)[3] == 0);

    // 8-bit: 256-colour CLUT, two pixels per word (low byte = left).
    std::vector<uint16_t> clut256(256, 0x03e0);  // green
    clut256[7] = 0x0000;
    const bb::Texture b = bb::decodeTim(tim(1, clut256, 1, 1, {0x0705}), 0, "b");
    assert(b.width == 2 && b.height == 1 && b.hasAlpha);
    assert(px(b, 0, 0)[1] == 248 && px(b, 0, 0)[3] == 255 && px(b, 1, 0)[3] == 0);

    // 15-bit direct, no CLUT, at a non-zero offset.
    std::vector<uint8_t> d(6, 0xee);
    const auto t16 = tim(2, {}, 2, 1, {0x7fff, 0x8421});
    d.insert(d.end(), t16.begin(), t16.end());
    const bb::Texture c = bb::decodeTim(d, 6, "c");
    assert(c.width == 2 && !c.hasAlpha && px(c, 0, 0)[0] == 248 && px(c, 1, 0)[0] == 8 && px(c, 1, 0)[3] == 255);

    // Errors: every truncation of a valid file, bad id, 24-bit mode, short CLUT, empty image, offset past end.
    for (const auto* full : {&t4, &t16})
        for (size_t n = 0; n < full->size(); ++n) assert(throws({full->begin(), full->begin() + long(n)}));
    auto bad = t16;
    bad[0] = 0x11;
    assert(throws(bad));
    assert(throws(tim(3, {}, 3, 1, {0, 0, 0})));
    assert(throws(tim(0, std::vector<uint16_t>(15, 1), 1, 1, {0})));
    assert(throws(tim(1, std::vector<uint16_t>(16, 1), 1, 1, {0})));
    assert(throws(tim(2, {}, 0, 1, {})));
    assert(throws(t16, t16.size()) && throws(t16, size_t(-1)));
    return 0;
}
