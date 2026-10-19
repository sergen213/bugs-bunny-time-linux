#include "bb/bze.h"
#include "bb/lzss.h"

#include <cassert>
#include <random>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

static bool throws(const Bytes& in) {
    try {
        bb::lzssDecompress(in);
    } catch (const bb::FormatError&) {
        return true;
    }
    return false;
}

int main() {
    // mode 0x0b: osize 3 -> 12-bit offset, 4-bit length; step 1; LUT 3..10, then 12,14,..,26.
    // 4 literals + pair(offset 3, len idx 5 = 8 bytes, overlapping copy). 5 items -> stored 4.
    const Bytes overlap = {0x0b, 0, 0, 4, 0x0f, 0xde, 0xad, 0xbe, 0xef, 0x00, 0x35};
    const Bytes overlapOut = {0xde, 0xad, 0xbe, 0xef, 0xad, 0xbe, 0xef, 0xad, 0xbe, 0xef, 0xad, 0xbe};
    bb::LzssInfo info;
    assert(bb::lzssDecompress(overlap, &info) == overlapOut);
    assert(info.items == 5 && info.consumed == overlap.size() && !info.tailDropped);

    // Same stream but count stored as the real item count: the extra final item is padding.
    Bytes realCount = overlap;
    realCount[3] = 5;
    assert(bb::lzssDecompress(realCount, &info) == overlapOut && info.tailDropped && info.items == 5);
    realCount.insert(realCount.end(), {0, 0, 0});  // zero padding decodes as a distance-0 pair
    assert(bb::lzssDecompress(realCount, &info) == overlapOut && info.tailDropped && info.consumed == overlap.size());

    // LUT above threshold: idx 8 -> 3 + (1 << step) + 7. step 1 (0x0b) = 12, step 2 (0x13) = 14.
    assert(bb::lzssDecompress(Bytes{0x0b, 0, 0, 1, 0x01, 'a', 0x00, 0x18}) == Bytes(13, 'a'));
    assert(bb::lzssDecompress(Bytes{0x13, 0, 0, 1, 0x01, 'a', 0x00, 0x18}) == Bytes(15, 'a'));
    // osize 0: 9-bit offset, 7-bit length, LUT >= 32 -> threshold 19; idx 20, step 0 -> 23.
    assert(bb::lzssDecompress(Bytes{0x00, 0, 0, 1, 0x01, 'x', 0x00, 0x80 | 20}) == Bytes(24, 'x'));
    // Flag groups of 8: 9 literals need two flag bytes.
    assert(bb::lzssDecompress(Bytes{0x0b, 0, 0, 8, 0xff, 1, 2, 3, 4, 5, 6, 7, 8, 0x01, 9}) ==
           (Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9}));

    // Malformed input.
    assert(throws({0x0b, 0, 0}));                                  // short header
    assert(throws({0x0f, 0, 0, 0, 0x01, 'a'}));                    // osize 7
    assert(throws({0x2b, 0, 0, 0, 0x01, 'a'}));                    // reserved bits
    assert(throws({0x0b, 0, 0, 1, 0x00, 0x00, 0x15, 0x01, 'a'}));  // back-reference into nothing, not final
    assert(throws({0x0b, 0, 0, 1, 0x02, 0x00, 0x00, 'a'}));        // distance 0, not final
    assert(throws({0x0b, 0, 0, 3, 0x0f, 'a', 'b'}));               // truncated mid-stream
    assert(throws({0x0b, 0, 0, 2, 0x03, 'a'}));                    // truncated before the final item

    // Random garbage must either decode or throw FormatError, never read/write out of bounds (ASan).
    std::mt19937 rng(1234);
    for (int i = 0; i < 20000; ++i) {
        Bytes b(4 + rng() % 64);
        for (auto& c : b) c = uint8_t(rng());
        b[0] &= 0x1f;
        b[1] = b[2] = 0;
        try {
            bb::lzssDecompress(b);
        } catch (const bb::FormatError&) {
        }
    }
    return 0;
}
