#include "bb/render/bmp.h"

#include <fstream>
#include <iterator>
#include <vector>

namespace bb::render {
namespace {

uint32_t u32(std::span<const uint8_t> b, size_t o) {
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}
uint16_t u16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | b[o + 1] << 8); }

} // namespace

std::expected<Texture, std::string> decodeBmp(std::span<const uint8_t> f) {
    auto fail = [](const char* m) { return std::unexpected(std::string("bmp: ") + m); };
    if (f.size() < 14 + 40) return fail("file too small");
    if (f[0] != 'B' || f[1] != 'M') return fail("missing BM signature");
    const uint32_t dataOff = u32(f, 10);
    const uint32_t dibSize = u32(f, 14);
    if (dibSize < 40 || dibSize > 124 || 14 + size_t(dibSize) > f.size()) return fail("unsupported DIB header");
    const int32_t w = int32_t(u32(f, 18));
    const int32_t hRaw = int32_t(u32(f, 22));
    const uint16_t planes = u16(f, 26), bpp = u16(f, 28);
    const uint32_t compression = u32(f, 30);
    uint32_t colours = u32(f, 46);
    if (planes != 1) return fail("planes != 1");
    if (compression != 0) return fail("compressed BMP not supported");
    if (bpp != 8 && bpp != 24) return fail("only 8-bit and 24-bit supported");
    if (w <= 0 || hRaw == 0 || hRaw == INT32_MIN || w > 16384) return fail("bad dimensions");
    const bool topDown = hRaw < 0;
    const uint32_t h = uint32_t(topDown ? -hRaw : hRaw);
    if (h > 16384) return fail("bad dimensions");

    const size_t stride = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
    if (dataOff > f.size() || stride * h > f.size() - dataOff) return fail("pixel data truncated");

    std::span<const uint8_t> pal;
    if (bpp == 8) {
        if (colours == 0) colours = 256;
        if (colours > 256) return fail("palette too large");
        const size_t palOff = 14 + size_t(dibSize);
        if (palOff + size_t(colours) * 4 > dataOff) return fail("palette truncated");
        pal = f.subspan(palOff, size_t(colours) * 4);
    }

    Texture t;
    t.width = uint32_t(w);
    t.height = h;
    t.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = f.data() + dataOff + stride * (topDown ? y : h - 1 - y);
        uint8_t* dst = t.rgba.data() + size_t(y) * w * 4;
        for (int32_t x = 0; x < w; ++x, dst += 4) {
            const uint8_t* bgr = src + x * 3;
            if (bpp == 8) {
                if (src[x] >= colours) return fail("palette index out of range");
                bgr = pal.data() + src[x] * 4;
            }
            dst[0] = bgr[2]; dst[1] = bgr[1]; dst[2] = bgr[0]; dst[3] = 255;
        }
    }
    return t;
}

std::expected<Texture, std::string> loadBmp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected("bmp: cannot open " + path.string());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    auto t = decodeBmp(bytes);
    if (t) t->name = path.filename().string();
    return t;
}

} // namespace bb::render
