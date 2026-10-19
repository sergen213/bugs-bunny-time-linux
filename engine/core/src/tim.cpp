#include "bb/tim.h"

#include <format>

namespace bb {
namespace {

struct In {
    std::span<const uint8_t> d;
    void need(size_t p, size_t n) const {
        if (p > d.size() || d.size() - p < n)
            throw FormatError(std::format("tim: {} bytes at offset 0x{:x} past end (0x{:x})", n, p, d.size()));
    }
    uint16_t u16(size_t p) const { need(p, 2); return uint16_t(d[p] | d[p + 1] << 8); }
    uint32_t u32(size_t p) const { need(p, 4); return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
};

void put(uint8_t* px, uint16_t c) {
    px[0] = uint8_t((c & 0x1f) << 3);
    px[1] = uint8_t((c >> 5 & 0x1f) << 3);
    px[2] = uint8_t((c >> 10 & 0x1f) << 3);
    px[3] = c == 0 ? 0 : 255;
}

} // namespace

Texture decodeTim(std::span<const uint8_t> data, size_t p, std::string name) {
    const In in{data};
    if (in.u32(p) != 0x10) throw FormatError(std::format("tim: no TIM id at offset 0x{:x}", p));
    const uint32_t flags = in.u32(p + 4), mode = flags & 7;
    if (mode > 2) throw FormatError(std::format("tim: unsupported pixel mode {} at offset 0x{:x}", mode, p));
    size_t q = p + 8;
    size_t clutAt = 0, clutN = 0;
    if (flags & 8) {
        const uint32_t size = in.u32(q);
        clutN = in.u16(q + 8);
        if (size < 12 + clutN * 2 || in.u16(q + 10) == 0)
            throw FormatError(std::format("tim: CLUT block size {} too small at offset 0x{:x}", size, q));
        in.need(q, size);
        clutAt = q + 12;
        q += size;
    }
    const size_t need = mode == 0 ? 16 : mode == 1 ? 256 : 0;
    if (clutN < need) throw FormatError(std::format("tim: mode {} needs {} CLUT colours, has {} (offset 0x{:x})", mode, need, clutN, p));

    const uint32_t iw = in.u16(q + 8), h = in.u16(q + 10);
    const uint32_t w = iw * (mode == 0 ? 4 : mode == 1 ? 2 : 1);
    if (w == 0 || h == 0) throw FormatError(std::format("tim: empty image at offset 0x{:x}", q));
    const size_t px = q + 12;
    in.need(px, size_t(iw) * h * 2);

    Texture t{std::move(name), w, h, std::vector<uint8_t>(size_t(w) * h * 4), false};
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        uint16_t c;
        if (mode == 2) c = in.u16(px + i * 2);
        else {
            const unsigned idx = mode == 1 ? data[px + i] : data[px + i / 2] >> (i & 1) * 4 & 15;  // low nibble = left
            c = in.u16(clutAt + idx * 2);
        }
        put(&t.rgba[i * 4], c);
        t.hasAlpha |= c == 0;
    }
    return t;
}

} // namespace bb
