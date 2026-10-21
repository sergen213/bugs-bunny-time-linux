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

// PsyQ MATRIX (Q12 s16 rows, TransMatrix t) with the exe's integer rotation builders: each product is >> 12 and
// truncated to s16 before the sum, as 0x407c30 (X) / 0x407da0 (Y) / 0x407f00 (Z) left-multiply the matrix.
struct Mat {
    std::array<int16_t, 9> m{4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
    V3 t;
};
namespace detail {
// rows (a, b) := (c·a - s·b, s·a + c·b) per column (sign -1: (c·a + s·b, c·b - s·a)), with s16 terms.
inline void rotRows(Mat& M, int32_t ang, int a, int b, int sign) {
    const int32_t c = rcos(ang), s = rsin(ang);
    for (int j = 0; j < 3; ++j) {
        const int32_t x = M.m[size_t(a * 3 + j)], y = M.m[size_t(b * 3 + j)];
        const int16_t sy = int16_t((s * y) >> 12), sx = int16_t((s * x) >> 12);
        M.m[size_t(a * 3 + j)] = int16_t(int16_t((c * x) >> 12) - sign * sy);
        M.m[size_t(b * 3 + j)] = int16_t(int16_t((c * y) >> 12) + sign * sx);
    }
}
} // namespace detail
inline void mulRotX(Mat& M, int32_t a) { detail::rotRows(M, a, 1, 2, 1); }
inline void mulRotY(Mat& M, int32_t a) { detail::rotRows(M, a, 0, 2, -1); }
inline void mulRotZ(Mat& M, int32_t a) { detail::rotRows(M, a, 0, 1, 1); }
// 0x407a50 RotMatrix: Rx·Ry·Rz (Z built first). 0x407af0: Ry·Rx·Rz. 0x407b90: Rz·Ry·Rx. Translation untouched.
inline void rotMatrixXYZ(Mat& M, int32_t x, int32_t y, int32_t z) {
    M.m = {int16_t(rcos(z)), int16_t(-rsin(z)), 0, int16_t(rsin(z)), int16_t(rcos(z)), 0, 0, 0, 4096};
    mulRotY(M, y);
    mulRotX(M, x);
}
inline void rotMatrixYXZ(Mat& M, int32_t x, int32_t y, int32_t z) {
    M.m = {int16_t(rcos(z)), int16_t(-rsin(z)), 0, int16_t(rsin(z)), int16_t(rcos(z)), 0, 0, 0, 4096};
    mulRotX(M, x);
    mulRotY(M, y);
}
inline void rotMatrixZYX(Mat& M, int32_t x, int32_t y, int32_t z) {
    M.m = {4096, 0, 0, 0, int16_t(rcos(x)), int16_t(-rsin(x)), 0, int16_t(rsin(x)), int16_t(rcos(x))};
    mulRotY(M, y);
    mulRotZ(M, z);
}
// 0x408a60 / 0x408120 ApplyMatrix: s16 vector, int32 row sums >> 12 (no translation).
inline V3 apply(const Mat& M, const V3& v) {
    const int32_t x = int16_t(v.x), y = int16_t(v.y), z = int16_t(v.z);
    const auto& m = M.m;
    return {(m[0] * x + m[1] * y + m[2] * z) >> 12, (m[3] * x + m[4] * y + m[5] * z) >> 12, (m[6] * x + m[7] * y + m[8] * z) >> 12};
}
// 0x408260 ApplyMatrixLV: int32 vector.
inline V3 applyLV(const Mat& M, const V3& v) {
    const auto& m = M.m;
    return {(m[0] * v.x + m[1] * v.y + m[2] * v.z) >> 12, (m[3] * v.x + m[4] * v.y + m[5] * v.z) >> 12,
            (m[6] * v.x + m[7] * v.y + m[8] * v.z) >> 12};
}
// 0x4081c0 ApplyRotMatrix / GTE MVMVA (0x408d50): as apply, results stored as s16.
inline V3 apply16(const Mat& M, const V3& v) {
    const V3 r = apply(M, v);
    return {int16_t(r.x), int16_t(r.y), int16_t(r.z)};
}

// MSVC rand() (thunk 0x450810 -> CRT): LCG 214013/2531011, 15-bit result. Seed 1 = CRT default (no srand
// call was looked for: Research Required).
struct MsvcRand {
    uint32_t seed = 1;
    int32_t operator()() { seed = seed * 214013u + 2531011u; return int32_t((seed >> 16) & 0x7fff); }
};

} // namespace bb::game
