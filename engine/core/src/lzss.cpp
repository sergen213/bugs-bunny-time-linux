#include "bb/lzss.h"
#include "bb/bze.h"

#include <format>
#include <string>

namespace bb {

std::vector<uint8_t> lzssDecompress(std::span<const uint8_t> in, LzssInfo* info) {
    if (in.size() < 4) throw FormatError("lzss: input shorter than 4-byte header");
    const unsigned osize = in[0] & 7, step = (in[0] >> 3) & 3;
    if (osize == 7) throw FormatError("lzss: osize 7 leaves no length bits");
    if (in[0] & 0xe0) throw FormatError(std::format("lzss: reserved header bits set (0x{:02x})", in[0]));
    const unsigned offBits = 9 + osize, lenBits = 7 - osize;
    const uint32_t stored = uint32_t(in[1]) << 16 | uint32_t(in[2]) << 8 | in[3];

    unsigned lut[128];
    const unsigned lutSize = 1u << lenBits;
    const unsigned thres = lutSize >= 32 ? 19 : lutSize / 2 - 1;
    for (unsigned i = 0; i < lutSize; ++i) lut[i] = i > thres ? 3 + ((i - thres) << step) + thres : 3 + i;

    LzssInfo st{.mode = in[0], .storedCount = stored};
    std::vector<uint8_t> out;
    out.reserve(in.size() * 2);
    size_t p = 4;
    uint8_t flags = 0;
    // Decodes one item; returns an error message instead of throwing so the final item can be dropped.
    auto item = [&](uint32_t n) -> std::string {
        if ((n & 7) == 0) {
            if (p >= in.size()) return std::format("truncated at input offset {} (item {})", p, n);
            flags = in[p++];
        }
        if (flags >> (n & 7) & 1) {
            if (p >= in.size()) return std::format("truncated at input offset {} (item {})", p, n);
            out.push_back(in[p++]);
            return {};
        }
        if (in.size() - p < 2) return std::format("truncated at input offset {} (item {})", p, n);
        const unsigned v = unsigned(in[p]) << 8 | in[p + 1];
        const size_t off = (v >> lenBits) & ((1u << offBits) - 1);
        const unsigned len = lut[v & (lutSize - 1)];
        if (off == 0 || off > out.size())
            return std::format("back-reference {} before output start at input offset {} (item {})", off, p, n);
        p += 2;
        for (unsigned i = 0; i < len; ++i) {  // overlapping copies repeat bytes: copy one at a time
            const uint8_t b = out[out.size() - off];
            out.push_back(b);
        }
        return {};
    };
    for (uint32_t n = 0; n <= stored; ++n) {
        const size_t p0 = p, out0 = out.size();
        if (std::string err = item(n); !err.empty()) {
            if (n != stored) throw FormatError("lzss: " + err);
            p = p0;  // undecodable final item: padding, not data
            out.resize(out0);
            st.tailDropped = true;
            break;
        }
        st.items = n + 1;
    }
    st.consumed = p;
    if (info) *info = st;
    return out;
}

} // namespace bb
