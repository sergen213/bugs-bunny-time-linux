// LiT fixed-point helpers with the exe's exact integer behaviour (GAMEPLAY.md conventions): Q12, angles 4.12,
// PS1 axes (+Y down). Tables reproduce the exe's rsin 0x45f3f0 / rcos 0x4633f0 (truncated trig·4096; the test
// compares them with bugs.exe bytes).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>

namespace bb::game {

struct V3 {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const V3&) const = default;
};

namespace detail {
template <bool Cos> const std::array<int32_t, 4096>& table() {
    static const std::array<int32_t, 4096> t = [] {
        std::array<int32_t, 4096> a{};
        for (int i = 0; i < 4096; ++i) {
            const double r = i * (2.0 * 3.14159265358979323846 / 4096.0);
            a[size_t(i)] = int32_t((Cos ? std::cos(r) : std::sin(r)) * 4096.0);
        }
        return a;
    }();
    return t;
}
} // namespace detail

// 0x407a30: sin table[(a + 0x1000) & 0xfff] (same as a & 0xfff in two's complement).
inline int32_t rsin(int32_t a) { return detail::table<false>()[size_t(uint32_t(a) & 0xfff)]; }
// 0x407a10: cos table[|a| & 0xfff].
inline int32_t rcos(int32_t a) { return detail::table<true>()[size_t(uint32_t(a < 0 ? -a : a) & 0xfff)]; }
// 0x4079c0: round(sqrt(v)), x87 round-to-nearest-even.
inline int32_t isqrt(int32_t v) { return int32_t(std::nearbyint(std::sqrt(double(v)))); }
// 0x4079e0: round(atan2(a, b) * 4096 / 2pi).
inline int32_t ratan2(int32_t a, int32_t b) {
    return int32_t(std::nearbyint(std::atan2(double(a), double(b)) * (4096.0 / (2.0 * 3.14159265358979323846))));
}
// (|a| & 0xfff) with a's sign: the exe's "angle mod 0x1000" idiom.
inline int32_t wrapSigned(int32_t a) { return a < 0 ? -((-a) & 0xfff) : (a & 0xfff); }
// wrapSigned, then into [0, 0x1000).
inline int32_t wrap0(int32_t a) { a = wrapSigned(a); return a < 0 ? a + 0x1000 : a; }
// Difference b - a of two angles as the exe computes it (both wrapped to [0, 0x1000), result in (-0x800, 0x800]).
inline int32_t angleDiff(int32_t to, int32_t from) {
    int32_t d = wrap0(to) - wrap0(from);
    if (d < 0) d += 0x1000;
    if (d > 0x800) d -= 0x1000;
    return d;
}

// RotMatrix (0x407a50) with only a yaw (x and z rotation 0 give exact identities in Q12) applied to a short
// vector by ApplyMatrix (0x408a60/0x408120): rows (c,0,s) (0,1,0) (-s,0,c), each row sum >> 12.
inline V3 rotY(int32_t yaw, int32_t x, int32_t y, int32_t z) {
    const int32_t c = int16_t(rcos(yaw)), s = int16_t(rsin(yaw));
    const int32_t sx = int16_t(x), sy = int16_t(y), sz = int16_t(z);
    return {(c * sx + s * sz) >> 12, (0x1000 * sy) >> 12, (-s * sx + c * sz) >> 12};
}

// MSVC rand() (thunk 0x450810 -> CRT): LCG 214013/2531011, 15-bit result. Seed 1 = CRT default (no srand
// call was looked for: Research Required).
struct MsvcRand {
    uint32_t seed = 1;
    int32_t operator()() { seed = seed * 214013u + 2531011u; return int32_t((seed >> 16) & 0x7fff); }
};

} // namespace bb::game
