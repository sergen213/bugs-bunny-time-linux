#undef NDEBUG
#include "bb/render/bmp.h"

#include <cassert>
#include <cstdlib>
#include <print>
#include <vector>

using namespace bb::render;
using Bytes = std::vector<uint8_t>;

// noinline: GCC 16 -O2 reports a bogus -Wstringop-overflow on the inlined vector copy + write.
[[gnu::noinline]] static void put32(Bytes& b, size_t o, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + i] = uint8_t(v >> (8 * i));
}

// Builds a BITMAPINFOHEADER file with the given pixel rows (already padded) and optional palette.
static Bytes make(int32_t w, int32_t h, uint16_t bpp, const Bytes& pal, const Bytes& pixels) {
    Bytes b(54 + pal.size());
    b[0] = 'B'; b[1] = 'M';
    put32(b, 10, uint32_t(b.size()));
    put32(b, 14, 40);
    put32(b, 18, uint32_t(w));
    put32(b, 22, uint32_t(h));
    b[26] = 1;
    b[28] = uint8_t(bpp);
    put32(b, 46, uint32_t(pal.size() / 4));
    std::copy(pal.begin(), pal.end(), b.begin() + 54);
    b.insert(b.end(), pixels.begin(), pixels.end());
    return b;
}

int main() {
    // 8-bit, 2x2, bottom-up. Palette (BGRA): 0 = red, 1 = blue. Rows padded to 4 bytes.
    const Bytes pal = {0, 0, 255, 0, 255, 0, 0, 0};
    Bytes ok8 = make(2, 2, 8, pal, {1, 0, 0, 0, /*top row:*/ 0, 1, 0, 0});
    auto t = decodeBmp(ok8);
    assert(t && t->width == 2 && t->height == 2);
    assert(t->rgba[0] == 255 && t->rgba[2] == 0 && t->rgba[3] == 255); // top-left red
    assert(t->rgba[4] == 0 && t->rgba[6] == 255);                      // top-right blue
    assert(t->rgba[8] == 0 && t->rgba[10] == 255);                     // bottom-left blue

    // 24-bit, 3x1 top-down (negative height); stride 9 -> 12.
    auto t24 = decodeBmp(make(3, -1, 24, {}, {10, 20, 30, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    assert(t24 && t24->rgba[0] == 30 && t24->rgba[1] == 20 && t24->rgba[2] == 10);

    // Malformed input: each must be rejected, never crash or over-read.
    auto bad = [](Bytes b) { return !decodeBmp(b).has_value(); };
    assert(bad({}));
    assert(bad(Bytes(ok8.begin(), ok8.begin() + 30)));                 // truncated header
    assert(bad(Bytes(ok8.begin(), ok8.end() - 1)));                    // truncated pixels
    { Bytes b = ok8; b[0] = 'X'; assert(bad(b)); }                     // signature
    { Bytes b = ok8; put32(b, 30, 1); assert(bad(b)); }                // BI_RLE8
    { Bytes b = ok8; b[28] = 4; assert(bad(b)); }                      // 4-bit
    { Bytes b = ok8; put32(b, 22, 0); assert(bad(b)); }                // zero height
    { Bytes b = ok8; put32(b, 22, 0x80000000u); assert(bad(b)); }      // INT32_MIN height
    { Bytes b = ok8; put32(b, 18, 0x7fffffffu); assert(bad(b)); }      // huge width
    { Bytes b = ok8; put32(b, 10, 0xfffffff0u); assert(bad(b)); }      // data offset past EOF
    { Bytes b = ok8; put32(b, 14, 2000); assert(bad(b)); }             // DIB header size
    { Bytes b = ok8; put32(b, 46, 300); assert(bad(b)); }              // palette too large
    { Bytes b = ok8; b[54 + 8] = 7; assert(bad(b)); }                  // index >= palette size
    assert(bad(make(2, 2, 8, Bytes(4 * 256, 0), {})));                 // palette but no pixels

    // Real data (optional): the title backdrops used by the collection menu.
    if (const char* root = std::getenv("BBTIME_DATA")) {
        auto lit = loadBmp(std::string(root) + "/lit/Datas/ScrnBgEn.bmp");
        auto tb = loadBmp(std::string(root) + "/tb/Datas/data/english.bmp");
        assert(lit && lit->width == 800 && lit->height == 600);
        assert(tb && tb->width == 640 && tb->height == 480);
    } else {
        std::println("BBTIME_DATA unset: real-data checks skipped");
    }
    return 0;
}
