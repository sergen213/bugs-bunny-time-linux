// The exe's software GTE (bblit2.exe 0x401ba0..0x403640, registers at 0x63bfc0) and the s16 / s32 vector and
// matrix helpers (0x403b40..0x404bc0) the TB collision code (mover 0x4450c0, ray 0x4463e0, camera 0x440360) runs
// on. One register file, as in the exe: an op reads what the previous loads left, including the rotation diagonal
// the outer products overwrite. FLAG (0x63c108) and the colour FIFO are written but never read by these callers and
// are left out; BK (0x63c05c) and FC (0x63c0b8) are never loaded and stay 0.
#pragma once
#include "bb/game/litmath.h"

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>

namespace bb::game::tbm {

// PsyQ SVECTOR (the pad, where a caller has one, is not part of it).
struct SVec {
    int16_t x = 0, y = 0, z = 0;
    bool operator==(const SVec&) const = default;
    int16_t& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    int16_t operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
static_assert(sizeof(SVec) == 6);
static_assert(sizeof(Mat) == 0x20);  // PsyQ MATRIX: 9 s16, pad, translation s32 at +0x14

inline int32_t wrap(int64_t v) { return int32_t(uint32_t(uint64_t(v))); }  // the exe's 32-bit imul / add
inline int16_t s16(int32_t v) { return int16_t(uint16_t(uint32_t(v))); }

inline SVec operator+(SVec a, SVec b) { return {s16(a.x + b.x), s16(a.y + b.y), s16(a.z + b.z)}; }  // 0x403d40
inline SVec operator-(SVec a, SVec b) { return {s16(a.x - b.x), s16(a.y - b.y), s16(a.z - b.z)}; }  // 0x403e50
inline SVec& operator+=(SVec& a, SVec b) { return a = a + b; }                                      // 0x403c90
inline V3 widen(SVec a) { return {a.x, a.y, a.z}; }                                                // 0x403bd0
inline V3 widenSum(SVec a, SVec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }                   // 0x403cb0
inline SVec narrow(const V3& a) { return {s16(a.x), s16(a.y), s16(a.z)}; }                         // 0x403bb0
inline V3& addTo(V3& a, const V3& b) { return a = {wrap(int64_t(a.x) + b.x), wrap(int64_t(a.y) + b.y), wrap(int64_t(a.z) + b.z)}; }  // 0x403c10
inline V3& addTo(V3& a, SVec b) { return addTo(a, widen(b)); }                                     // 0x403c60
inline SVec& addTo(SVec& a, const V3& b) { return a = a + narrow(b); }                              // 0x403c40
inline int32_t dot(SVec a, SVec b) { return wrap(int64_t(a.x) * b.x + int64_t(a.y) * b.y + int64_t(a.z) * b.z); }
inline int32_t dot(const V3& a, const V3& b) { return wrap(int64_t(a.x) * b.x + int64_t(a.y) * b.y + int64_t(a.z) * b.z); }
inline int32_t absw(int32_t v) { return int32_t(uint32_t(v ^ (v >> 31)) - uint32_t(v >> 31)); }  // |INT_MIN| stays INT_MIN
inline int32_t mulDiv(int32_t a, int32_t b, int32_t c) { return wrap(int64_t(a) * b) / c; }      // imul, then idiv
// v · num / den per component (s16 results), the exe's usual projection step.
inline SVec scaled(SVec v, int32_t num, int32_t den) { return {s16(mulDiv(v.x, num, den)), s16(mulDiv(v.y, num, den)), s16(mulDiv(v.z, num, den))}; }
inline int32_t lenSq(SVec a) { return dot(a, a); }

// 0x4040b0: round(sqrt(v)) in x87 precision; a negative v (a product that wrapped) stores the NaN as the x87
// integer indefinite 0x80000000.
inline int32_t root(int32_t v) { return v < 0 ? INT32_MIN : int32_t(std::llrint(std::sqrt(static_cast<long double>(v)))); }
// 0x4040d0: round(atan2(y, x) * 4096 / 2pi) with the exe's scale (.rdata 0x4812e8); (0, 0) = 0.
inline int32_t angle(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    constexpr long double kScale = 651.8986469044033;  // 0x40845f306dcc48b9
    return int32_t(std::llrint(static_cast<long double>(double(std::atan2(static_cast<long double>(y), static_cast<long double>(x)) * kScale))));
}

// 0x4048b0: a·b, each element's s32 sum >> 12 stored s16. 0x404a50: b = a·b.
inline Mat mul(const Mat& a, const Mat& b) {
    Mat c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            c.m[size_t(i * 3 + j)] = s16((a.m[size_t(i * 3)] * b.m[size_t(j)] + a.m[size_t(i * 3 + 1)] * b.m[size_t(3 + j)] +
                                          a.m[size_t(i * 3 + 2)] * b.m[size_t(6 + j)]) >> 12);
    c.t = b.t;
    return c;
}
// 0x404bc0: the transposed rotation; its translation comes out 0 (the exe copies an unwritten stack block).
inline Mat transposed(const Mat& a) {
    Mat c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.m[size_t(i * 3 + j)] = a.m[size_t(j * 3 + i)];
    return c;
}

class Gte {
public:
    // Loads. 0x401de0 / 0x401e20 / 0x401e60 V0..V2; 0x401ea0 V0 from the low halves of an s32 vector.
    void loadV(int i, SVec v) { v_[size_t(i)] = widen(v); }
    void loadV0(const V3& v) { v_[0] = widen(narrow(v)); }
    void loadIr(SVec v) { ir = widen(v); }       // 0x401db0
    void loadIr(const V3& v) { ir = v; }         // 0x401f30
    void loadIr0(int32_t v) { ir0_ = v; }        // 0x401f90
    void setTrans(const V3& t) { tr_ = t; }      // 0x403240
    void setRot(const Mat& m) { load(rot_, m); }    // 0x403260
    void setLight(const Mat& m) { load(light_, m); }  // 0x403330
    void setColor(const Mat& m) { load(color_, m); }  // 0x403170
    void setRotRow0(SVec v) { row(rot_, 0, v); }      // 0x401cd0
    void setLightRow0(SVec v) { row(light_, 0, v); }  // 0x401d20
    void setLightRow2(SVec v) { row(light_, 2, v); }  // 0x401d70
    void setColorRow0(SVec v) { row(color_, 0, v); }  // 0x401c50
    void setColorRow2(SVec v) { row(color_, 2, v); }  // 0x401c90
    void setRotDiag(SVec v) { rot_[0] = v.x, rot_[4] = v.y, rot_[8] = v.z; }  // 0x401f50

    // Ops. MAC and IR are s32 registers; only SQR, GPF and OP saturate IR.
    void sqr() {  // 0x401ba0: MAC = IR², IR = MAC in [0, 0x7fff]
        mac = {wrap(int64_t(ir.x) * ir.x), wrap(int64_t(ir.y) * ir.y), wrap(int64_t(ir.z) * ir.z)};
        ir = {clamp(mac.x, 0), clamp(mac.y, 0), clamp(mac.z, 0)};
    }
    void rtv0() { mulSum(rot_, v_[0]); }    // 0x402950: (R·V0) >> 12
    void llv0() { mulSum(light_, v_[0]); }  // 0x402a70: (L·V0) >> 12
    void lcv0() { mulSum(color_, v_[0]); }  // 0x402b60: (LCM·V0) >> 12
    void rtir() {                           // 0x402860: R·IR, no shift
        const V3 i = ir;
        mac = {wrap(int64_t(rot_[2]) * i.z + int64_t(rot_[1]) * i.y + int64_t(rot_[0]) * i.x),
               wrap(int64_t(rot_[5]) * i.z + int64_t(rot_[4]) * i.y + int64_t(rot_[3]) * i.x),
               wrap(int64_t(rot_[7]) * i.y + int64_t(rot_[6]) * i.x + int64_t(rot_[8]) * i.z)};
        ir = mac;
    }
    void gpf(int shift) {  // 0x402c50 (shift 0) / 0x402da0 (12): MAC = IR·IR0 >> shift, IR saturated to s16
        mac = {wrap(int64_t(ir.x) * ir0_) >> shift, wrap(int64_t(ir.y) * ir0_) >> shift, wrap(int64_t(ir.z) * ir0_) >> shift};
        ir = {clamp(mac.x), clamp(mac.y), clamp(mac.z)};
    }
    void op() {  // 0x402ef0: rotation diagonal × IR, IR saturated to s16
        const int64_t d0 = rot_[0], d1 = rot_[4], d2 = rot_[8];
        mac = {wrap(d1 * ir.z - ir.y * d2), wrap(ir.x * d2 - d0 * ir.z), wrap(d0 * ir.y - d1 * ir.x)};
        ir = {clamp(mac.x), clamp(mac.y), clamp(mac.z)};
    }
    enum Matrix { kRot, kLight, kColor };
    enum Source { kV0, kV1, kV2, kIr };
    // 0x402090 MVMVA(sf, mx, v, cv; lm 0), cv: TR or none. sf 1 shifts each product (not the sum) and adds TR
    // unscaled; sf 0 adds TR << 12. IR = MAC.
    void mvmva(bool sf, Matrix mx, Source src, bool trans) {
        const auto& m = mx == kRot ? rot_ : mx == kLight ? light_ : color_;
        const V3 v = src == kIr ? ir : v_[size_t(src)];
        const V3 t = trans ? tr_ : V3{};
        const int32_t* tv = &t.x;
        int32_t out[3];
        for (size_t r = 0; r < 3; ++r) {
            const int32_t px = wrap(int64_t(v.x) * m[r * 3]), py = wrap(int64_t(v.y) * m[r * 3 + 1]), pz = wrap(int64_t(v.z) * m[r * 3 + 2]);
            out[r] = sf ? wrap(int64_t(px >> 12) + (py >> 12) + tv[r] + (pz >> 12)) : wrap(int64_t(pz) + py + px + int64_t(tv[r]) * 0x1000);
        }
        mac = {out[0], out[1], out[2]};
        ir = mac;
    }
    void rtir12() { mvmva(true, kRot, kIr, false); }  // 0x402840

    // Stores. 0x401fa0 IR as s16, 0x402060 MAC, 0x402080 MAC3.
    SVec irS() const { return narrow(ir); }

    V3 ir, mac;

private:
    using M9 = std::array<int32_t, 9>;  // [8] is an s32 register (R33 0x63bffc, L33 0x63c050, LCM33 0x63c0b0)
    static void load(M9& d, const Mat& m) { for (size_t i = 0; i < 9; ++i) d[i] = m.m[i]; }
    static void row(M9& d, size_t r, SVec v) { d[r * 3] = v.x, d[r * 3 + 1] = v.y, d[r * 3 + 2] = v.z; }
    static int32_t clamp(int32_t v, int32_t lo = -0x8000) { return v < lo ? lo : v > 0x7fff ? 0x7fff : v; }
    void mulSum(const M9& m, const V3& v) {
        mac = {wrap(int64_t(m[0]) * v.x + int64_t(m[1]) * v.y + int64_t(m[2]) * v.z) >> 12,
               wrap(int64_t(m[3]) * v.x + int64_t(m[4]) * v.y + int64_t(m[5]) * v.z) >> 12,
               wrap(int64_t(m[6]) * v.x + int64_t(m[7]) * v.y + int64_t(m[8]) * v.z) >> 12};
        ir = mac;
    }
    friend V3 cross(const V3& a, const V3& b);
    friend V3 cross(SVec a, SVec b);
    M9 rot_{}, light_{}, color_{};
    std::array<V3, 3> v_{};
    V3 tr_;
    int32_t ir0_ = 0;
};

inline Gte& gte() {
    static Gte g;
    return g;
}

// 0x4030e0: the components squared (SQR on an s32 vector; the MAC, unsaturated).
inline V3 squared(const V3& v) {
    Gte& g = gte();
    g.loadIr(v), g.sqr();
    return g.mac;
}
// 0x401db0, 0x401ba0, 0x402060: the squared length of an s16 vector through the GTE (the sums the callers add).
inline int32_t gteLenSq(SVec v) {
    Gte& g = gte();
    g.loadIr(v), g.sqr();
    return wrap(int64_t(g.mac.x) + g.mac.y + g.mac.z);
}
// 0x403470 / 0x404800: (m·v) >> 12 as s16 (loads m as the rotation).
inline SVec apply(const Mat& m, SVec v) {
    Gte& g = gte();
    g.setRot(m), g.loadV(0, v), g.rtv0();
    return g.irS();
}
// 0x403640: a × b of s16 vectors (the rotation diagonal becomes a, IR the saturated product), the s32 MAC.
inline V3 cross(SVec a, SVec b) {
    Gte& g = gte();
    g.setRotDiag(a), g.loadIr(b), g.op();
    return g.mac;
}
// 0x4034f0: a × b of s32 vectors; the exe loads a's x / y as s16 pairs (R11/R12, R22/R23) and z whole into R33.
inline V3 cross(const V3& a, const V3& b) {
    Gte& g = gte();
    g.rot_[0] = s16(a.x), g.rot_[1] = s16(a.x >> 16), g.rot_[4] = s16(a.y), g.rot_[5] = s16(a.y >> 16), g.rot_[8] = a.z;
    g.loadIr(b), g.op();
    return g.mac;
}

} // namespace bb::game::tbm
