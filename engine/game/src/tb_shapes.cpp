// TB collision shape primitives (bblit2.exe); see tb_shapes.h. The exe's fixed-point steps are kept as they are:
// the pre-scaling of large products by powers of 4, the halving loops before divisions and the 32-bit wrap of
// every multiply (tbm::wrap), since the traces compare positions to the unit.
#include "tb_shapes.h"

#include <algorithm>
#include <climits>

namespace bb::game::tbm {
namespace {
int32_t sq(int32_t v) { return wrap(int64_t(v) * v); }
int32_t lenSqW(const V3& v) { return wrap(int64_t(sq(v.z)) + sq(v.y) + sq(v.x)); }
V3 shr(const V3& v, int k) { return {v.x >> (k & 31), v.y >> (k & 31), v.z >> (k & 31)}; }
int32_t shl(int32_t v, int k) { return int32_t(uint32_t(v) << (k & 31)); }
} // namespace

uint32_t sphereCast(SphereCast& r) {
    r.travel = {};
    r.normal = {0, 0x4000, 0};
    const int32_t r2 = sq(r.radius);
    if (lenSq(r.p) < r2) return 4;
    // Distance of the line from the centre: |p × d|² / |d|², both sides pre-scaled by 4^k and 4^j.
    V3 c = cross(widen(r.p), widen(r.d));
    int k = 0;
    for (int32_t n = lenSqW(shr(c, 12)); n > 0x40; n >>= 2) ++k;
    c = shr(c, k);
    const int32_t dd = lenSq(r.d) >> ((2 * k) & 31);
    int j = 0;
    for (int32_t n = wrap(int64_t(r2 >> 12) * (dd >> 12)); n > 0x40; n >>= 2) ++j;
    const int32_t m = wrap(int64_t(r2 >> (j & 31)) * (dd >> (j & 31)));
    const int32_t q = lenSqW(c) >> ((2 * j) & 31);
    if (m == 0) return 0x10;
    if (m < q || q == 0) return 2;
    // The entry along d: -(p·d) - |d|·sqrt(r² - dist²) (scaled back), as a fraction of |d|².
    int32_t v = wrap(-int64_t(dot(r.d, r.p)) - shl(root(wrap(int64_t(m) - q)), k + j));
    if (v < 0) return 8;
    int32_t dd2 = lenSq(r.d);
    if (dd2 < v) return 0x10;
    for (; v > 200000; v >>= 1) dd2 >>= 1;
    r.travel = scaled(r.d, v, dd2);
    r.normal = {s16((int32_t(r.travel.x) + r.p.x) * 0x4000 / r.radius), s16((int32_t(r.travel.y) + r.p.y) * 0x4000 / r.radius),
                s16((int32_t(r.travel.z) + r.p.z) * 0x4000 / r.radius)};
    r.t = (v << 12) / dd2;
    return 1;
}

uint32_t cylinderCast(CylinderCast& r) {
    // In the plane across the axis: c1 = d × axis, c2 = p × axis (|c|² / |axis|² are squared distances).
    V3 c1 = cross(r.d, r.axis), c2 = cross(r.p, r.axis);
    r.travel = {};
    int32_t n = std::max(lenSqW(shr(c1, 12)), lenSqW(shr(c2, 12)));
    int32_t aa = lenSq(r.axis);
    int k = 0;
    for (; n > 0x20; n >>= 2) ++k;
    if (k) {
        const int32_t h = 1 << ((k - 1) & 31);
        auto round = [&](const V3& v) { return V3{wrap(int64_t(v.x) + h) >> k, wrap(int64_t(v.y) + h) >> k, wrap(int64_t(v.z) + h) >> k}; };
        c1 = round(c1), c2 = round(c2);
        aa = wrap(int64_t(1 << (k & 31)) + aa) >> ((2 * k) & 31);
    }
    const int32_t cc = lenSqW(c1), cd = dot(c2, c1), dd = lenSqW(c2), r2 = sq(r.radius);
    if (aa == 0) return 2;
    if (dd / aa < r2) return 4;
    if (cc == 0) return 2;
    if (cd >= 0) return 8;
    int32_t e = absw(wrap(int64_t(r.p.y) * c1.y + int64_t(r.p.z) * c1.z + int64_t(r.p.x) * c1.x));
    int s = 0;
    for (; e > 30000; e >>= 1) ++s;
    if (r2 < e * e / (cc >> ((2 * s) & 31))) return 2;
    // The quadratic's discriminant cd² - cc·(dd - r²·aa), each term brought to a common power of two.
    int32_t f = wrap(int64_t(r2 >> 8) * (aa >> 8));
    int k2 = 0;
    for (; f > 30000; f >>= 2) ++k2;
    const int32_t h2 = k2 ? 1 << ((k2 - 1) & 31) : 0;
    const int32_t g = shl(wrap(int64_t(dd >> ((2 * k2) & 31)) - wrap(int64_t(wrap(int64_t(r2) + h2) >> (k2 & 31)) * (wrap(int64_t(h2) + aa) >> (k2 & 31)))), 2 * k2);
    int u = 0;
    for (int32_t a = absw(g); a > 30000; a >>= 1) ++u;
    const int ug = u;
    int32_t cs = cc;
    for (; cs > 30000; cs >>= 1) ++u;
    int32_t prod = wrap(int64_t(cs) * (g >> (ug & 31)));
    int w = 0;
    int32_t a2 = absw(cd);
    for (; a2 > 30000; a2 >>= 1) ++w;
    int32_t sqd = a2 * a2;
    if (u < 2 * w) prod >>= ((2 * w - u) & 31), u = 2 * w;
    else if (2 * w < u) sqd >>= ((u - 2 * w) & 31);
    if (sqd < prod) return 2;
    int32_t disc = sqd - prod, rt = 0;
    int sh = 0;
    if (disc) {
        if (u & 1) {
            if (disc < 1000000000) --u, disc *= 2;
            else ++u, disc >>= 1;
        }
        rt = root(disc);
        for (sh = u >> 1; rt < 1000000000 && sh != 0; --sh) rt *= 2;
    }
    int32_t den = cc >> (sh & 31);
    const int32_t num = wrap(int64_t(cd >> (sh & 31)) + rt);
    int32_t t = wrap(-int64_t(num));
    if (t < 0) return 8;
    if (!(wrap(-int64_t(den)) == num || t < den)) return 0x10;
    if (num != -500000 && t > 499999)
        do t >>= 1, den >>= 1;
        while (t > 500000);
    r.travel = scaled(r.d, t, den);
    r.t = (t << 12) / den;
    return 1;
}

bool capsulePush(CapsulePush& r) {
    const SVec a = r.axis;
    int32_t proj = dot(r.p, a), aa = lenSq(a), dist;
    r.push = r.p;
    if (proj >= 0 && proj <= aa) {
        // Beside the segment: push = p minus its projection; the distance from p × axis.
        for (; proj > 200000; proj >>= 1) aa >>= 1;
        r.push = r.push - scaled(a, proj, aa);
        V3 c = cross(r.p, a);
        int k = 0;
        for (int32_t n = lenSqW(shr(c, 12)); n > 0x40; n >>= 2) ++k;
        c = shr(c, k);
        dist = lenSqW(c) / (lenSq(a) >> ((2 * k) & 31));
    } else {
        if (proj >= 0 || aa < proj) r.push = r.push - a;  // past the far end (the near end is the origin)
        dist = lenSq(r.push);
    }
    const int32_t R = r.radius;
    if (wrap(int64_t(R + 2) * R) <= dist) return false;
    const int32_t d = root(shl(dist, 8)) >> 4;
    if (R <= d || d < 1) return false;
    r.push = scaled(r.push, R + (2 - d), d);
    return true;
}

bool capsuleCast(CapsuleCast& r) {
    CylinderCast cyl;
    cyl.axis = r.axis, cyl.p = r.p, cyl.d = r.d, cyl.radius = r.radius;
    const uint32_t u = cylinderCast(cyl);
    if (u & 0x1a) return false;
    r.normal = r.p;
    if (u & 1) {
        if (r.tmax < cyl.t) return false;
        r.normal += cyl.travel;
    }
    const SVec a = r.axis;
    int32_t proj = dot(r.normal, a), aa = lenSq(a);
    if (proj >= 0 && proj <= aa) {
        // The contact lies on the side: the normal is the offset from the axis.
        for (; proj > 200000; proj >>= 1) aa >>= 1;
        r.normal = r.normal - scaled(a, proj, aa);
        const int32_t rad = r.radius;
        r.normal = {s16((int32_t(r.normal.x) << 14) / rad), s16((int32_t(r.normal.y) << 14) / rad), s16((int32_t(r.normal.z) << 14) / rad)};
        if (!(u & 1)) return false;
        r.hit = cyl.travel, r.t = cyl.t;
        return true;
    }
    if (proj >= 0 || aa < proj) r.p = r.p - a;  // the far cap: a sphere at axis
    SphereCast cap;
    cap.p = r.p, cap.d = r.d, cap.radius = r.radius;
    if (!(sphereCast(cap) & 1) || r.tmax < cap.t) return false;
    r.hit = cap.travel, r.normal = cap.normal, r.t = cap.t;
    return true;
}

bool boxCast(BoxCast& r, int16_t& deepAxis) {
    int32_t tIn = 0, tOut = 0x1000, deepest = -50000;
    int16_t num = 0, den = 0, axis = -1;
    for (int16_t i = 0; i < 3; ++i) {
        const int32_t p = r.p[i], d = r.d[i];
        int32_t slab = r.ext[i] + r.radius;
        const int32_t gap = absw(p) - slab;
        if (gap < 1) {
            if (deepest < gap && d != 0) deepest = gap, deepAxis = i;
        } else {
            if (int16_t(p ^ d) >= 0) return false;  // outside and moving away
            const int32_t ad = absw(d);
            if (ad == 0) return false;
            const int32_t t = gap * 0x1000 / ad;
            if (tIn < t) {
                tIn = t;
                if (tOut < t) return false;
                num = s16(gap), den = s16(ad), axis = i;
            }
        }
        if (d != 0) {
            if (d < 0) slab = -slab;
            const int32_t t = (slab - p) * 0x1000 / d;
            if (t < tOut && (tOut = t) < tIn) return false;
        }
    }
    if (axis == -1) num = 0, den = 1, axis = deepAxis;
    r.normal = {};
    r.normal[axis] = r.d[axis] < 1 ? 0x4000 : -0x4000;
    r.travel = {s16(r.d.x * num / den), s16(r.d.y * num / den), s16(r.d.z * num / den)};
    r.t = tIn;
    return true;
}

bool boxPush(BoxPush& r) {
    int outside = 0;
    int32_t sum = 0, maxGap = INT32_MIN;
    const int32_t r2 = sq(r.radius);
    int axis = 0;
    r.push = r.p;  // becomes the nearest point of the box
    for (int i = 0; i < 3; ++i) {
        const int16_t p = r.p[i], e = r.ext[i];
        const int32_t gap = absw(p) - e;
        if (gap > 0) {
            if (r.radius <= gap) return false;
            r.push[i] = p < 0 ? s16(-e) : e;
            ++outside;
            sum = wrap(int64_t(sum) + sq(gap));
            if (r2 < sum) return false;
        }
        if (maxGap < gap) axis = i, maxGap = gap;
    }
    if (outside < 2) {  // out through the face of the shallowest axis
        r.push = {};
        const int16_t v = s16(int16_t(r.radius) - int16_t(maxGap) + 1);
        r.push[axis] = r.p[axis] < 0 ? s16(-v) : v;
        return true;
    }
    r.push = r.p - r.push;  // past an edge or corner: along the offset from it
    const int32_t s = root(shl(sum, 8)) >> 4;
    r.push = scaled(r.push, r.radius - s + 1, s);
    return true;
}

namespace {
V3 rounded(const V3& v, int k) {  // (v + 2^(k-1)) >> k
    const int32_t h = 1 << ((k - 1) & 31);
    return {wrap(int64_t(v.x) + h) >> (k & 31), wrap(int64_t(v.y) + h) >> (k & 31), wrap(int64_t(v.z) + h) >> (k & 31)};
}
// c + b·num / den, rounded by `half` (den / 2) as 0x43bfb0 does.
SVec along(SVec c, SVec b, int32_t num, int32_t den, int32_t half) {
    auto one = [&](int16_t bi, int16_t ci) { return s16(s16(wrap(int64_t(bi) * num + half) / den) + ci); };
    return {one(b.x, c.x), one(b.y, c.y), one(b.z, c.z)};
}
} // namespace

int32_t lineNearest(LineNearest& r) {
    SVec v = r.a;
    int32_t vv = lenSq(v);
    if (vv > 1000000) {
        int k = 0;
        for (; vv > 1000000; vv >>= 2) ++k;
        const int32_t h = 1 << ((k - 1) & 31);
        v = {s16((v.x + h) >> k), s16((v.y + h) >> k), s16((v.z + h) >> k)};
        vv = lenSq(v);
    }
    V3 n = cross(r.b, v);
    if (n.x || n.y || n.z) {
        r.skew = 1;
        // out = c minus its projection on v; then s from (out × v)·n / |n|² (n = b × v).
        int32_t e = wrap(int64_t(r.c.y) * v.y + int64_t(r.c.z) * v.z + int64_t(r.c.x) * v.x);
        for (; e > 200000; e >>= 1) vv >>= 1;
        if (vv == 0) vv = 1;
        r.out = r.c - scaled(v, e, vv);
        V3 m = cross(r.out, v);
        const V3 n12 = shr(n, 12), m12 = shr(m, 12);
        int32_t big = lenSqW(n12);
        big = std::max(big, absw(wrap(int64_t(m12.x) * n12.x + int64_t(m12.y) * n12.y + int64_t(m12.z) * n12.z)));
        if (big > 0x40) {
            int k = 0;
            for (; big > 0x40; big >>= 2) ++k;
            m = rounded(m, k), n = rounded(n, k);
        }
        int32_t num = wrap(int64_t(n.y) * m.y + int64_t(n.x) * m.x + int64_t(n.z) * m.z);
        const int32_t nn = lenSqW(n);
        int32_t den = wrap(-int64_t(nn)), half;
        if (num < 0) num = wrap(-int64_t(num)), den = nn;
        while ((half = den >> 1), num > 200000) den = half, num >>= 1;
        r.out = along(r.c, r.b, num, den, half);
        return (num << 12) / den;
    }
    r.skew = 0;
    const int32_t e = wrap(int64_t(v.z) * r.c.z + int64_t(v.y) * r.c.y + int64_t(r.c.x) * v.x);
    if (e < 0) r.out = {};
    else if (e <= vv) {
        r.out = r.c;
        return 0;
    } else r.out = v;
    r.out = r.out - r.c;
    const SVec b = r.b;
    int32_t f = dot(r.out, b), bb = lenSq(b), half;
    if (f < 0) {
        r.out = r.c;
        return 0;
    }
    if (bb < f) {
        r.out = r.c + b;
        return 0x1000;
    }
    while ((half = bb >> 1), f > 200000) bb = half, f >>= 1;
    r.out = along(r.c, b, f, bb, half);
    return (f << 12) / bb;
}

uint32_t slabCast(SlabCast& r, int32_t& normalY) {
    V3 n = cross(r.b, r.a);
    r.travel = {};
    r.normal = {0, 0x4000, 0};
    if (!n.x && !n.y && !n.z) {  // a parallel to b: the cylinder around a
        CylinderCast cyl;
        cyl.axis = r.a, cyl.p = r.p, cyl.d = r.d, cyl.radius = r.radius;
        uint32_t u = cylinderCast(cyl);
        if (u & 1) {
            r.travel = cyl.travel, r.t = cyl.t;
            const SVec at = r.travel + r.p;
            r.normal = at;
            int32_t proj = dot(at, r.a), aa = lenSq(r.a);
            for (int32_t e = absw(proj); e > 200000; e = absw(proj >> 1), proj >>= 1) aa >>= 1;
            if (aa == 0) aa = 1;
            const SVec off = at - scaled(r.a, proj, aa);
            r.normal = {s16((int32_t(off.x) << 14) / r.radius), s16((int32_t(off.y) << 14) / r.radius), s16((int32_t(off.z) << 14) / r.radius)};
            u = 1;
        }
        return u;
    }
    normalY = n.y >> 12;
    int32_t big = lenSqW(shr(n, 12));
    if (big > 8) {
        int k = 0;
        for (; big > 8; big >>= 2) ++k;
        n = rounded(n, k);
    }
    const int32_t len = root(lenSqW(n));
    int32_t proj = wrap(int64_t(r.p.x) * n.x + int64_t(r.p.y) * n.y + int64_t(r.p.z) * n.z);
    if (absw(proj) / len < r.radius) {
        r.travel = {}, r.t = 0;
        return 4;
    }
    const int32_t reach = wrap(int64_t(r.radius) * len);
    int32_t rate = wrap(int64_t(r.d.x) * n.x + int64_t(r.d.y) * n.y + int64_t(r.d.z) * n.z);
    const bool against = rate < 0;
    if ((reach ^ proj) < 0) rate = wrap(-int64_t(rate)), proj = wrap(-int64_t(proj));
    int32_t gap = wrap(int64_t(reach) - proj);
    if (rate < 0) gap = wrap(-int64_t(gap)), rate = wrap(-int64_t(rate));
    if (gap < 0) return 8;
    if (rate < gap) return 0x10;
    for (; gap > 200000; gap >>= 1) rate >>= 1;
    if (rate == 0) return 2;
    r.travel = scaled(r.d, gap, rate);
    r.t = (gap << 12) / rate;
    r.normal = against ? narrow(n) : SVec{s16(-s16(n.x)), s16(-s16(n.y)), s16(-s16(n.z))};
    return 1;
}

namespace {
// |p × axis|² / |axis|² with both pre-scaled by the same power of 4.
int32_t perpSq(SVec p, SVec axis) {
    V3 m = cross(p, axis);
    int k = 0;
    for (int32_t n = lenSqW(shr(m, 12)); n > 0x40; n >>= 2) ++k;
    m = shr(m, k);
    return lenSqW(m) / (lenSq(axis) >> ((2 * k) & 31));
}
SVec neg(SVec v) { return {s16(-v.x), s16(-v.y), s16(-v.z)}; }
} // namespace

void segmentGap(SegmentGap& r) {
    auto set = [&](SVec v) { r.out = v, r.dist = lenSq(v); };
    // The A end `e` against segment B (`flip`: the clamped ends report -(e - c), the exe's second copy of this).
    auto endToB = [&](SVec e, bool flip) {
        SVec p = e - r.c;
        int32_t g = dot(p, r.b), bb = lenSq(r.b);
        if (g >= 0 && g <= bb) {
            for (; g > 200000; g >>= 1) bb >>= 1;
            r.out = scaled(r.b, g, bb) - p;
            r.dist = perpSq(p, r.b);
            return;
        }
        if (g > 0) p = p - r.b;
        set(flip ? neg(p) : p);
    };
    // The B end p against segment A, falling back to A's end against segment B.
    auto endToA = [&](SVec p) {
        int32_t e = dot(p, r.a), aa = lenSq(r.a);
        if (e >= 0 && e <= aa) {
            for (; e > 200000; e >>= 1) aa >>= 1;
            r.out = p - scaled(r.a, e, aa);
            r.dist = perpSq(p, r.a);
            return;
        }
        endToB(e >= 1 ? r.a : SVec{}, true);
    };
    LineNearest line;
    line.a = r.a, line.b = r.b, line.c = r.c;
    const int32_t s = lineNearest(line);
    const SVec q = line.out;
    int32_t e = dot(q, r.a), aa = lenSq(r.a);
    if (!line.skew) {
        if (e >= 0 && e <= aa) {
            for (; e > 200000; e >>= 1) aa >>= 1;
            r.out = q - scaled(r.a, e, aa);
            r.dist = perpSq(r.c, r.a);
        } else set(e >= 1 ? q - r.a : q);
        return;
    }
    V3 n = cross(r.a, r.b);
    int k = 0;
    for (int32_t m = lenSqW(shr(n, 12)); m > 0x40; m >>= 2) ++k;
    n = shr(n, k);
    if (s < 0) return endToA(r.c);
    if (s > 0x1000) return endToA(r.b + r.c);
    if (e < 0) return endToB({}, false);
    if (e > aa) return endToB(e >= 1 ? r.a : SVec{}, false);
    // Both nearest points inside: the lines' distance |c·n| / |n|.
    for (; e > 200000; e >>= 1) aa >>= 1;
    r.out = q - scaled(r.a, e, aa);
    int32_t f = absw(dot(widen(r.c), n)), nn = lenSqW(n);
    for (; f > 30000; f >>= 1) nn >>= 2;
    r.dist = f * f / nn;
}

bool segmentPush(SegmentPush& r, SVec& nearest) {
    r.push = r.c;
    SegmentGap g;
    g.a = r.a, g.b = r.b, g.c = r.c;
    segmentGap(g);
    int32_t d = g.dist, R = r.radius;
    if (d < 1) d = 1, R = 2, g.out = {0, 0, 1};
    nearest = g.out;
    if (wrap(int64_t(R) * R) < d) return false;
    const int32_t s = root(d);
    r.push = scaled(g.out, R + (2 - s), s);
    return true;
}

bool segmentCast(SegmentCast& r, int32_t& slabT) {
    SlabCast slab;
    int32_t slabNormalY = 0;  // its slab test's scratch word (+0x48), read by nothing here
    slab.a = r.a, slab.b = r.b, slab.p = r.c, slab.d = r.d, slab.radius = r.radius, slab.t = slabT;
    uint32_t u = slabCast(slab, slabNormalY);
    slabT = slab.t;
    if (u & 0x1a) return false;
    SVec p = r.c;
    if (u & 1) {
        if (r.tmax < slab.t) return false;
        p += slab.travel;
    }
    LineNearest line;
    line.a = r.a, line.b = r.b, line.c = p;
    const int32_t s = lineNearest(line);
    // The exe builds the line and both cylinder records in one scratch block: a cylinder reporting "inside" leaves
    // its t on the word before, the line's `skew` flag (or the first cylinder's t).
    int32_t staleT = line.skew;
    int32_t e, aa = lenSq(r.a);
    const bool pastEnd = s < 0 || s > 0x1000;
    if (pastEnd) {
        // Past an end of the moving segment: that end against the cylinder around a.
        CylinderCast cyl;
        cyl.axis = r.a, cyl.p = s > 0x1000 ? r.c + r.b : r.c, cyl.d = r.d, cyl.radius = r.radius, cyl.t = staleT;
        u = cylinderCast(cyl);
        staleT = cyl.t;
        if (u & 0x1a) return false;
        SVec at = cyl.p;
        if (u & 1) at += cyl.travel;
        e = dot(at, r.a);
        if (e >= 0 && e <= aa) {
            if (r.tmax < cyl.t) return false;
            for (; e > 200000; e >>= 1) aa >>= 1;
            r.hit = cyl.travel;
            const SVec n = at - scaled(r.a, e, aa);
            r.normal = {s16((int32_t(n.x) << 14) / r.radius), s16((int32_t(n.y) << 14) / r.radius), s16((int32_t(n.z) << 14) / r.radius)};
            r.t = cyl.t;
            return true;
        }
    } else {
        e = dot(line.out, r.a);
        if (e >= 0 && e <= aa) {
            r.hit = slab.travel, r.normal = slab.normal, r.t = slab.t;
            return true;
        }
    }
    // Past an end of a: that end moving by -d against the cylinder around the segment's b.
    CylinderCast cyl;
    cyl.axis = r.b, cyl.p = aa < e ? r.a - r.c : neg(r.c), cyl.d = neg(r.d), cyl.radius = r.radius, cyl.t = staleT;
    u = cylinderCast(cyl);
    if (u & 0x1a) return false;
    SVec at = cyl.p;
    if (u & 1) at += cyl.travel;
    int32_t f = dot(at, r.b), bb = lenSq(r.b);
    if (f < 0 || bb < f) {  // past both ends: the end points as spheres
        SphereCast cap;
        cap.p = bb < f ? neg(cyl.p) + r.b : neg(cyl.p), cap.d = r.d, cap.radius = r.radius;
        if (!(sphereCast(cap) & 1) || r.tmax < cap.t) return false;
        r.hit = cap.travel, r.normal = cap.normal, r.t = cap.t;
        return true;
    }
    if (pastEnd && r.tmax < cyl.t) return false;  // only this way in does the exe check the reach
    for (; f > 200000; f >>= 1) bb >>= 1;
    r.hit = neg(cyl.travel);
    const SVec n = scaled(r.b, f, bb) - at;
    r.normal = {s16((int32_t(n.x) << 14) / r.radius), s16((int32_t(n.y) << 14) / r.radius), s16((int32_t(n.z) << 14) / r.radius)};
    r.t = cyl.t;
    return true;
}

bool segmentReach(const SegmentReach& r) {
    int32_t e = dot(r.p, r.axis), aa = lenSq(r.axis);
    SVec q;  // the segment's point nearest p
    if (e < 0) q = {};
    else if (aa < e) q = r.axis;
    else {
        for (; e > 100000; e >>= 1) aa >>= 1;
        q = scaled(r.axis, e, aa);
    }
    return gteLenSq(q - r.p) <= sq(r.radius);
}

bool boxSweep(BoxSweep& r, BoxSweepScratch& s) {
    int32_t& faceAxis = s.faceAxis;
    int16_t& edgeAxis = s.edgeAxis;
    // The faces: the latest entry into the slabs grown by the radius.
    int32_t tBest = -1;
    r.hit = {};
    for (int i = 0; i < 3; ++i) {
        const int32_t gap = absw(r.p[i]) - r.ext[i] - r.radius;
        if (gap < 0) continue;
        const int16_t d = r.d[i];
        if (d == 0 || int16_t(d ^ r.p[i]) >= 0) return false;
        const int32_t ad = absw(d), t = gap * 0x1000 / ad;
        if (tBest < t) {
            if (r.tmax < t) return false;
            tBest = t;
            r.hit = {s16(r.d.x * gap / ad), s16(r.d.y * gap / ad), s16(r.d.z * gap / ad)};
            faceAxis = i;
        }
    }
    // The edges: where the entry point lies outside two slabs, the cylinder along the third axis.
    const SVec q = r.hit + r.p;
    int outside = 0;
    uint32_t last = 0;
    CylinderCast cyl;
    cyl.d = r.d, cyl.radius = r.radius;
    for (int i = 0; i < 3; ++i) {
        if (!(r.ext[i] < absw(q[i]))) continue;
        ++outside;
        for (int j = i + 1; j < 3; ++j) {
            if (!(r.ext[j] < absw(q[j]))) continue;
            const int16_t k = int16_t(3 - i - j);
            cyl.axis = {};
            cyl.axis[k] = r.ext[k];
            cyl.p = r.p;
            cyl.p[i] = s16(r.p[i] - (r.p[i] < 0 ? -r.ext[i] : r.ext[i]));
            cyl.p[j] = s16(r.p[j] - (r.p[j] < 0 ? -r.ext[j] : r.ext[j]));
            const uint32_t u = cylinderCast(cyl);
            if (!(u & 5)) return false;
            if (last == 0 || ((u & 1) && tBest < cyl.t)) {
                edgeAxis = k;
                if (!(u & 1)) r.hit = {};
                else {
                    tBest = cyl.t;
                    if (r.tmax < cyl.t) return false;
                    r.hit = cyl.travel;
                }
            }
            last = u;
        }
    }
    if (outside == 1) {  // through a face
        r.normal = {};
        r.normal[faceAxis] = q[faceAxis] < 0 ? -0x4000 : 0x4000;
        r.t = tBest;
        return true;
    }
    const SVec at = r.hit + r.p;
    if (r.ext[edgeAxis] < absw(at[edgeAxis])) {  // past the edge's end too: the corner sphere
        SphereCast corner;
        corner.p = r.p;
        for (int k = 0; k < 3; ++k) corner.p[k] = s16(corner.p[k] - (at[k] < 0 ? -r.ext[k] : r.ext[k]));
        corner.d = r.d, corner.radius = r.radius;
        if (!(sphereCast(corner) & 1) || r.tmax < corner.t) return false;
        r.t = corner.t, r.hit = corner.travel, r.normal = corner.normal;
        return true;
    }
    r.t = tBest;
    r.normal = {};
    for (int k = 0; k < 3; ++k)
        if (k != edgeAxis) r.normal[k] = s16(at[k] + (at[k] < 0 ? r.ext[k] : -r.ext[k]));
    r.normal = {s16((int32_t(r.normal.x) << 14) / r.radius), s16((int32_t(r.normal.y) << 14) / r.radius), s16((int32_t(r.normal.z) << 14) / r.radius)};
    return true;
}

bool boxCapsulePush(BoxCapsulePush& r) {
    bool pushed = false;
    r.push = {};
    // p inside the box: out through the nearest face first.
    // The exe keeps the running bests (gap, entry t) in s16 scratch words; their truncation decides cases.
    SVec q = r.p;
    int16_t best = -20000;
    int axis = 0, i = 0;
    for (; i < 3; ++i) {
        const int32_t gap = absw(q[i]) - r.ext[i];
        if (gap >= 0) break;
        if (best < gap) best = s16(gap), axis = i;
    }
    if (i == 3) {
        const int16_t v = q[axis];
        const int16_t out = v < 0 ? s16(-(r.ext[axis] + v)) : s16(r.ext[axis] - v);
        pushed = true;
        r.push[axis] = out;
        q[axis] = s16(q[axis] + out);
    }
    // Separating slabs; k = the axis with the least overlap.
    best = -20000;
    int k = 0;
    for (i = 0; i < 3; ++i) {
        const int32_t gap = absw(q[i]) - r.ext[i] - r.radius - absw(r.axis[i]);
        if (best < gap) {
            if (gap >= 0) return false;
            best = s16(gap), k = i;
        }
    }
    // The segment from its end on the far side of that face (start s, extent d).
    const bool toward = int16_t(r.axis[k] ^ q[k]) < 0;
    const SVec s = toward ? r.p + r.axis : r.p - r.axis;
    const SVec d = toward ? SVec{s16(r.axis.x * -2), s16(r.axis.y * -2), s16(r.axis.z * -2)}
                          : SVec{s16(r.axis.x << 1), s16(r.axis.y << 1), s16(r.axis.z << 1)};
    // Where it enters the other slabs last.
    int16_t tBest = -1;
    int j = 0;
    for (i = 0; i < 3; ++i) {
        if (i == k || !(r.ext[i] < absw(s[i]))) continue;
        int32_t num = s[i] <= 0 ? -s[i] - r.ext[i] : -s[i] + r.ext[i], den = d[i];
        if (num < 0) num = -num, den = -den;
        const int32_t t = den <= 0 ? 0 : (num << 12) / den;
        if (tBest < t) tBest = s16(t), j = i;
    }
    if (tBest == -1) {  // the whole segment within those slabs: straight out of face k
        int16_t v = s16(r.ext[k] + int16_t(r.radius));
        if (q[k] < 0) v = s16(-v);
        r.push[k] = s16(r.push[k] + s16(v - s[k]));
        return true;
    }
    const bool through = absw(wrap((int64_t(d[k]) * tBest >> 12) + s[k])) < r.ext[k];
    // The box edge along the third axis m (from e0, extent e1) against the segment.
    const int m = 3 - k - j;
    SVec e1, e0;
    e1[m] = s16(r.ext[m] << 1);
    e0[m] = s16(s[m] + r.ext[m]);
    e0[k] = q[k] < 1 ? s16(s[k] + r.ext[k]) : s16(s[k] - r.ext[k]);
    e0[j] = s[j] < 1 ? s16(r.ext[j] + s[j]) : s16(s[j] - r.ext[j]);
    LineNearest line;
    line.a = e1, line.b = d, line.c = e0;
    const int32_t at = lineNearest(line);
    const SVec x = at < 0 ? e0 : at <= 0x1000 ? line.out : e0 + d;
    int32_t f = dot(x, e1), ee = lenSq(e1);
    SVec y;
    if (f < 0) y = ee < f ? e1 : SVec{};
    else if (ee < f) y = e1;
    else {
        for (; f > 200000; f >>= 1) ee >>= 1;
        y = scaled(e1, f, ee);
    }
    int32_t g = dot(y - e0, d), dd = lenSq(d);
    SVec z;
    if (g < 0) z = g <= dd ? SVec{} : d;
    else if (g <= dd) {
        for (; g > 200000; g >>= 1) dd >>= 1;
        z = scaled(d, g, dd);
    } else z = d;
    z = z + e0 - y;
    const int32_t dist = lenSq(z);
    if (dist < sq(r.radius) || through) {
        int32_t len = root(shl(dist, 8)) >> 4;
        if (through) len = -len;
        pushed = true;
        r.push = r.push + scaled(z, r.radius + (2 - len), len);
    }
    return pushed;
}

bool boxCapsuleCast(BoxCapsuleCast& r, int32_t& slabT) {
    // Faces: the capsule end nearer each slab the centre is outside of; `edges` collects, per axis m, the box edge
    // along m (bit 4m+3) and which side of the other two axes it lies on (bits 4m+c).
    uint16_t edges = 0;
    bool face = false;
    for (int i = 0; i < 3; ++i) {
        if (absw(r.p[i]) < r.ext[i]) continue;
        const SVec end = int16_t(r.axis[i] ^ r.p[i]) < 0 ? r.axis + r.p : r.p - r.axis;
        SVec at = end;
        const int32_t gap = absw(end[i]) - r.radius - r.ext[i];
        if (gap >= 0) {
            const int16_t d = r.d[i];
            if (int16_t(d ^ end[i]) >= 0) return false;
            const int32_t ad = absw(d);
            if (ad == 0 || ad < gap) return false;
            r.t = gap * 0x1000 / ad;
            if (r.tmax < r.t) return false;
            r.hit = {s16(r.d.x * gap / ad), s16(r.d.y * gap / ad), s16(r.d.z * gap / ad)};
            at = r.hit + end;
            face = true;
        }
        uint32_t code = 0;
        for (int j = 0; j < 3; ++j) {
            if (j == i || !(r.ext[j] < absw(at[j]))) continue;
            code = 8;
            for (int c = 0; c < 3; ++c)
                if (at[c] < 0) code |= 1u << c;
            const int m = 3 - i - j;
            if (!(edges & (1u << (4 * m + 3)))) edges = uint16_t(edges | (code << (4 * m)));
        }
        if (code == 0 && face) {
            r.normal = {};
            r.normal[i] = at[i] < 0 ? -0x4000 : 0x4000;
            return true;
        }
    }
    if (!edges) return false;
    // Edges: the slab around each against the moving capsule axis; the latest entry wins.
    struct Best { SVec a, c, hit, normal; int16_t t = -1; uint16_t code = 0; } best;
    for (int i = 0; i < 3; ++i) {
        if (!(edges & (1u << (4 * i + 3)))) continue;
        SVec off, corner;
        for (int c = 0; c < 3; ++c) {
            off[c] = c == i ? s16(r.ext[c] << 1) : int16_t(0);
            corner[c] = c == i || (edges & (1u << (4 * i + c))) ? s16(r.p[c] + r.ext[c]) : s16(r.p[c] - r.ext[c]);
        }
        SlabCast slab;
        slab.a = off, slab.b = {s16(r.axis.x << 1), s16(r.axis.y << 1), s16(r.axis.z << 1)}, slab.p = corner - r.axis;
        slab.d = r.d, slab.radius = r.radius;
        const uint32_t u = slabCast(slab, slabT);  // its scratch word +4 is the segment test's slab t below
        if (!(u & 5)) return false;
        if ((u & 4) && best.code) continue;
        if (!(u & 1)) best.hit = {}, best.t = 0;
        else {
            if (slab.t <= best.t) continue;
            if (r.tmax < slab.t) return false;
            best.hit = slab.travel, best.t = s16(slab.t);
        }
        best.a = slab.a, best.c = slab.p, best.normal = slab.normal, best.code = uint16_t(u);
    }
    const SVec axis2{s16(r.axis.x << 1), s16(r.axis.y << 1), s16(r.axis.z << 1)};
    LineNearest line;
    line.a = best.a, line.b = axis2, line.c = best.c;
    const int32_t s = lineNearest(line);
    if (s >= 0 && s <= 0x1000) {
        const int32_t w = dot(line.out, line.a);
        if (w >= 0 && w <= lenSq(line.a)) {
            r.hit = best.hit, r.normal = best.normal, r.t = best.t;
            return true;
        }
    }
    SegmentCast seg;
    seg.a = best.a, seg.b = axis2, seg.c = best.c, seg.d = r.d, seg.radius = r.radius, seg.tmax = r.tmax;
    if (!segmentCast(seg, slabT)) return false;
    r.hit = seg.hit, r.normal = seg.normal, r.t = seg.t;
    return true;
}

int32_t lineGap(LineGap& r) {
    r.n = cross(r.a, r.b);
    if (!r.n.x && !r.n.y && !r.n.z) {  // parallel: the distance of c from the line along a
        V3 m = cross(r.a, r.c);
        int k = 0;
        for (int32_t q = lenSqW(shr(m, 12)); q > 0x40; q >>= 2) ++k;
        m = shr(m, k);
        r.len = 0;
        return root(lenSqW(m) / (lenSq(r.a) >> ((2 * k) & 31)));
    }
    const V3 n0 = r.n, n12 = shr(r.n, 12);
    int32_t big = lenSqW(n12);
    big = std::max(big, absw(wrap(int64_t(n12.x) * r.c.x + int64_t(n12.y) * r.c.y + int64_t(r.c.z) * n12.z) >> 12));
    int k = 0;
    for (; big > 0x40; big >>= 2) ++k;
    r.n = shr(r.n, k);
    r.len = root(lenSqW(r.n));
    return absw((wrap(int64_t(r.c.z) * n0.z + int64_t(r.c.y) * n0.y + int64_t(r.c.x) * n0.x) >> (k & 31)) / r.len);
}

} // namespace bb::game::tbm
