// TB collision sweep (bblit2.exe): the per-shape and per-polygon tests of the camera sphere and the four movers,
// their shape-list loops and the mover passes.
#include "tb_sweep.h"

#include <algorithm>
#include <cstring>

namespace bb::game::tbm {
namespace {
// A contact found by the motion test: its reach becomes the pass's `t`, the nearest by squared travel is kept.
SVec neg(SVec v) { return {s16(-v.x), s16(-v.y), s16(-v.z)}; }
void keepNearest(Sweep& w, SVec travel, SVec normal) {
    const int32_t d = lenSq(travel);
    if (d < w.best) w.normal = normal, w.best = d, w.contact = travel;
}
// V0 = v through one GTE matrix ((M·v) >> 12, IR as s16): 0x402950 rotation, 0x402a70 light, 0x402b60 colour.
SVec rtv(SVec v) { return gte().loadV(0, v), gte().rtv0(), gte().irS(); }
SVec llv(SVec v) { return gte().loadV(0, v), gte().llv0(), gte().irS(); }
SVec lcv(SVec v) { return gte().loadV(0, v), gte().lcv0(), gte().irS(); }
// 0x402090 MVMVA(sf 1, rotation, V0) of v (the box mover's per-vertex transform).
SVec rtvSf(SVec v) { return gte().loadV(0, v), gte().mvmva(true, Gte::kRot, Gte::kV0, false), gte().irS(); }
Mat rows(SVec a, SVec b, SVec c) {
    Mat m;
    m.m = {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z};
    return m;
}
Mat rows(const Axes& t) { return rows(t[0].v, t[1].v, t[2].v); }
V3 shr(const V3& v, int k) { return {v.x >> k, v.y >> k, v.z >> k}; }
SVec shr16(const V3& v, int k) { return {s16(v.x >> k), s16(v.y >> k), s16(v.z >> k)}; }
int32_t sumAbs(const V3& v) { return absw(v.x) + absw(v.y) + absw(v.z); }
int32_t macSum() { return gte().mac.x + gte().mac.y + gte().mac.z; }
} // namespace

bool sweepSphere(Sweep& w, const ShapeRec& rec) {
    SVec off = w.centre - rec.centre;
    const int32_t R = w.radius + rec.radius, dist = lenSq(off);
    if (dist < wrap(int64_t(R) * R)) {  // overlapping: out along the offset to R + 1
        int32_t s = root(wrap(int64_t(dist) << 8)) >> 4;
        const int32_t k = R - s + 1;
        s += s == 0;
        w.push = {s16(mulDiv(off.x, k, s)), s16(mulDiv(off.y, k, s)), s16(mulDiv(off.z, k, s))};
        off += w.push;
        w.pushed = 1;
    }
    if (!w.best) return false;
    SphereCast c;
    c.p = off, c.d = w.motion, c.radius = R;
    if (!(sphereCast(c) & 1) || w.t < c.t) return false;
    w.t = s16(c.t);
    keepNearest(w, c.travel, c.normal);
    return true;
}

bool sweepCapsule(Sweep& w, const ShapeRec& rec) {
    SVec at = w.centre;
    CapsulePush in;
    in.p = w.centre - rec.capStart, in.axis = rec.capAxis, in.radius = w.radius + rec.capRadius;
    if (capsulePush(in)) at += in.push, w.push = in.push, w.pushed = 1;
    if (!w.best) return false;
    CapsuleCast c;
    c.tmax = w.t, c.p = at - rec.capStart, c.axis = rec.capAxis;
    c.radius = s16(w.radius + int16_t(rec.capRadius)), c.d = w.motion;
    if (!capsuleCast(c)) return false;
    w.t = s16(c.t);
    keepNearest(w, c.hit, c.normal);
    return true;
}

bool sweepBox(Sweep& w, const ShapeRec& rec) {
    // Into the box's frame through the GTE light matrix, back out through the colour matrix, as the exe does.
    Gte& g = gte();
    g.setLight(rec.inv), g.setColor(rec.rot);
    auto toBox = [&](SVec v) { return g.loadV(0, v), g.llv0(), g.irS(); };
    auto toWorld = [&](SVec v) { return g.loadV(0, v), g.lcv0(), g.irS(); };
    SVec off = toBox(w.centre - rec.centre);
    const SVec m = toBox(w.motion);
    BoxPush in;
    in.p = off, in.ext = rec.half, in.radius = w.radius;
    if (boxPush(in)) off += in.push, w.push = toWorld(in.push), w.pushed = 1;
    if (!w.best) return false;
    BoxSweep c;
    c.p = off, c.ext = rec.half, c.d = m, c.radius = w.radius, c.tmax = w.t;
    if (!boxSweep(c, w.scratch.camBox)) return false;
    const SVec normal = toWorld(c.normal);
    if (const int32_t d = lenSq(c.hit); d < w.best) w.contact = toWorld(c.hit), w.normal = normal, w.best = d;
    w.t = s16(c.t);
    return true;
}

bool moverSphere(Sweep& w, const ShapeRec& rec) {
    SVec at = w.centre;
    const SVec axis = w.capB - w.capA;
    CapsulePush in;
    in.p = rec.centre - w.capA - w.centre, in.axis = axis, in.radius = w.half.x + rec.radius;
    if (capsulePush(in)) at = at - in.push, w.push = neg(in.push), w.pushed = 1;
    if (!w.best) return false;
    CapsuleCast c;
    c.tmax = w.t, c.p = rec.centre - w.capA - at, c.axis = axis;
    c.radius = s16(w.half.x + int16_t(rec.radius)), c.d = neg(w.motion);
    if (!capsuleCast(c)) return false;
    w.t = s16(c.t);
    if (const int32_t d = lenSq(c.hit); d < w.best) w.normal = neg(c.normal), w.contact = neg(c.hit), w.best = d;
    return true;
}

bool moverCapsule(Sweep& w, const ShapeRec& rec) {
    SVec at = w.centre;
    const SVec b2{s16(w.capB.x << 1), s16(w.capB.y << 1), s16(w.capB.z << 1)};
    SegmentPush in;
    in.a = rec.capAxis, in.b = b2, in.c = w.centre - rec.capStart + w.capA, in.radius = w.half.x + rec.capRadius;
    SVec nearest;
    if (segmentPush(in, nearest)) at += in.push, w.push = in.push, w.pushed = 1;
    // The push's nearest-point offset and the segment test's slab t share a scratch word (+0x8c): its low half.
    int32_t& slabT = w.scratch.capsuleSlabT;
    slabT = int32_t((uint32_t(slabT) & 0xffff0000u) | uint16_t(nearest.z));
    if (!w.best) return false;
    SegmentCast c;
    c.tmax = w.t, c.c = at - rec.capStart + w.capA, c.a = rec.capAxis, c.d = w.motion, c.b = b2;
    c.radius = s16(w.half.x + int16_t(rec.capRadius));
    if (!segmentCast(c, slabT)) return false;
    w.t = s16(c.t);
    keepNearest(w, c.hit, c.normal);
    return true;
}

bool moverBox(Sweep& w, const ShapeRec& rec) {
    Gte& g = gte();
    g.setLight(rec.inv), g.setColor(rec.rot);
    auto toBox = [&](SVec v) { return g.loadV(0, v), g.llv0(), g.irS(); };
    auto toWorld = [&](SVec v) { return g.loadV(0, v), g.lcv0(), g.irS(); };
    SVec off = toBox(w.centre - rec.centre);
    const SVec b = toBox(w.capB);
    BoxCapsulePush in;
    in.ext = rec.half, in.axis = b, in.p = off, in.radius = w.half.x;
    if (boxCapsulePush(in)) off += in.push, w.push = toWorld(in.push), w.pushed = 1;
    if (!w.best) return false;
    BoxCapsuleCast c;
    c.tmax = w.t, c.p = off, c.d = toBox(w.motion), c.axis = b, c.ext = rec.half, c.radius = w.half.x;
    if (!boxCapsuleCast(c, w.scratch.boxSlabT)) return false;
    const SVec normal = toWorld(c.normal);
    if (const int32_t d = lenSq(c.hit); d < w.best) w.normal = normal, w.contact = toWorld(c.hit), w.best = d;
    w.t = s16(c.t);
    return true;
}

// The box mover's tests run in its frame: light = world -> box (`inv`), colour = box -> world (`rot`).
bool boxSphere(Sweep& w, const ShapeRec& rec) {
    gte().setLight(w.inv), gte().setColor(w.rot);
    SVec off = llv(rec.centre - w.centre);
    const SVec d = neg(llv(w.motion));
    BoxPush in;
    in.p = off, in.ext = w.half, in.radius = rec.radius;
    if (boxPush(in)) off += in.push, w.push = neg(lcv(in.push)), w.pushed = 1;
    if (!w.best) return false;
    BoxSweep c;
    c.p = off, c.ext = w.half, c.d = d, c.radius = s16(rec.radius), c.tmax = w.t;
    if (!boxSweep(c, w.scratch.boxSphere)) return false;
    w.t = s16(c.t);
    if (const int32_t dd = lenSq(c.hit); dd < w.best) {
        w.normal = neg(lcv(c.normal));
        w.contact = neg(lcv(c.hit));
        w.best = dd;
    }
    return true;
}

bool boxCapsule(Sweep& w, const ShapeRec& rec) {
    gte().setLight(w.inv), gte().setColor(w.rot);
    SVec off = llv(rec.centre - w.centre);
    const SVec axis = llv({s16(rec.capAxis.x >> 1), s16(rec.capAxis.y >> 1), s16(rec.capAxis.z >> 1)});
    BoxCapsulePush in;
    in.ext = w.half, in.axis = axis, in.p = off, in.radius = rec.capRadius;
    if (boxCapsulePush(in)) off += in.push, w.push = neg(lcv(in.push)), w.pushed = 1;
    if (!w.best) return false;
    BoxCapsuleCast c;
    c.tmax = w.t, c.p = off, c.d = neg(llv(w.motion)), c.axis = axis, c.ext = w.half, c.radius = s16(rec.capRadius);
    if (!boxCapsuleCast(c, w.scratch.boxCapSlabT)) return false;
    w.t = s16(c.t);
    if (const int32_t dd = lenSq(c.hit); dd < w.best) {
        w.normal = neg(lcv(c.normal));
        w.contact = neg(lcv(c.hit));
        w.best = dd;
    }
    return true;
}

namespace {
int16_t abs16(int16_t v) { return s16(v < 0 ? -v : v); }

// 0x43def0: the box `ext` at the origin against a box at p whose axes (scaled by its half extents) are `t`: 2 =
// apart along one of the first box's axes; else the shallowest of them gives `push` / `depth` (1 + the overlap).
bool slabPush(SVec p, SVec ext, const Axes& t, SVec& push, int32_t& depth) {
    depth = 10000;
    for (int i = 0; i < 3; ++i) {
        const int32_t gap = absw(p[i]) - ext[i] - absw(t[2].v[i]) - absw(t[1].v[i]) - absw(t[0].v[i]);
        if (gap >= 0) return false;
        if (int32_t dd = 1 - gap; dd < depth) {
            depth = dd, push = {};
            push[i] = s16(p[i] < 0 ? -dd : dd);
        }
    }
    return true;
}

// 0x43dfb0: two boxes (world axes `a` and `b`, scaled; the second at p) against each other along the nine edge
// cross products: 2 = apart; else the axis of least squared overlap gives `push` (+3) / `depth`.
bool edgePush(SVec p, const Axes& a, const Axes& b, SVec& push, int32_t& depth) {
    Gte& g = gte();
    g.setColor(rows(a)), g.setLight(rows(b));
    depth = 10000;
    int32_t best = INT32_MAX;
    V3 kept;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            g.setRotDiag(a[size_t(i)].v), g.loadIr(b[size_t(j)].v), g.op();
            V3 c = g.mac;
            g.loadIr(shr16(c, 8)), g.sqr();
            int32_t n = macSum(), k = 0;
            for (; n > 20000; n >>= 2) ++k;
            c = shr(c, k);
            g.loadIr(c), g.sqr();
            n = macSum();
            if (n <= 0x18) continue;
            g.loadV0(c), g.setRotRow0(p), g.mvmva(false, Gte::kRot, Gte::kV0, false);
            int32_t pc = g.mac.x;
            g.mvmva(false, Gte::kLight, Gte::kV0, false);
            const int32_t e1 = sumAbs(g.mac);
            g.mvmva(false, Gte::kColor, Gte::kV0, false);
            if (pc < 0) c = {-c.x, -c.y, -c.z}, pc = -pc;
            const int32_t gap = pc - sumAbs(g.mac);
            if (e1 < gap) return false;
            int32_t over = e1 - gap;
            for (; over > 32000; over >>= 1) n >>= 2;
            if (n != 0)
                if (const int32_t m = over * over / n; m < best) best = m, kept = c;
        }
    if (best < INT32_MAX) {
        const int32_t r = root(best) + 3;
        depth = r;
        g.loadIr(kept), g.sqr();
        const int32_t l = root(macSum());
        push = {s16(kept.x * r / l), s16(kept.y * r / l), s16(kept.z * r / l)};
    }
    return true;
}

// 0x43e330: the box mover overlapping a box shape: the shallowest of its face, the shape's face and the edge axes
// is added to `push`. `t` = the axis tables of 0x43eaf0.
bool boxBoxPush(Sweep& w, const ShapeRec& rec, const std::array<Axes, 4>& t) {
    Gte& g = gte();
    const SVec c = w.centre;
    SVec best{}, push;
    int16_t bestDepth = 10000;
    int32_t depth;
    g.setLight(rec.inv);
    if (!slabPush(llv(c - rec.centre), rec.half, t[0], push, depth)) return false;
    if (depth < bestDepth) bestDepth = s16(depth), g.setLight(rec.rot), best = llv(push);
    g.setLight(w.inv);
    if (!slabPush(llv(rec.centre - c), w.half, t[1], push, depth)) return false;
    if (depth < bestDepth) bestDepth = s16(depth), g.setLight(w.rot), best = neg(llv(push));
    if (!edgePush(c - rec.centre, t[2], t[3], push, depth)) return false;
    if (depth < bestDepth) best = push;
    w.push += best;
    return true;
}

// The motion's entry / exit fractions (Q12) of 0x43e570 / 0x43e700 and the entry's normal, as num / den of the motion.
struct Range {
    int16_t tIn = -10000, tOut = 10000;
    SVec normal;
    int16_t num = 0, den = 0;
};

// 0x43e570: a point at p moving by d against the box `ext` grown by the projection of the axes `t`: 2 = apart
// and not moving on an axis, 0 = moving away or not reaching it, 0x10 = entering after `tmax`; 1 = `r` narrowed.
uint32_t slabRange(SVec p, SVec ext, SVec d, const Axes& t, int16_t tmax, Range& r) {
    for (int i = 0; i < 3; ++i) {
        const int16_t di = d[i], e = ext[i], pi = p[i];
        const int16_t grow = s16(abs16(t[2].v[i]) + abs16(t[1].v[i]) + abs16(t[0].v[i]));
        const int16_t gap = s16(abs16(pi) - grow - e);
        if (di == 0) {
            if (gap >= 0) return 2;
            continue;
        }
        if (gap >= 0) {
            if (int16_t(di ^ pi) >= 0) return 0;
            const int32_t ad = absw(di);
            if (ad < gap) return 0;
            const int32_t tt = (int32_t(gap) << 12) / ad;
            if (tmax < tt) return 0x10;
            if (r.tIn < tt) {
                r.normal = {};
                r.normal[i] = di < 1 ? 0x4000 : -0x4000;
                r.tIn = s16(tt), r.den = s16(ad), r.num = gap;
            }
        }
        const int16_t v = di < 0 ? s16(-(grow + pi + e)) : s16(grow - pi + e);
        if (const int32_t tt = (int32_t(v) << 12) / di; tt < r.tOut) r.tOut = s16(tt);
    }
    return 1;
}

// 0x43e700: a box (world axes `a`) at p moving by d against the box with world axes `b` at the origin, along the
// nine edge cross products: codes as slabRange; `r` restarts at [-10000, 10000].
uint32_t edgeRange(SVec p, SVec d, const Axes& a, const Axes& b, int16_t tmax, Range& r) {
    Gte& g = gte();
    g.setColor(rows(a)), g.setLight(rows(b));
    r.tIn = -10000, r.tOut = 10000;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            g.setRotDiag(a[size_t(i)].v), g.loadIr(b[size_t(j)].v), g.op();
            V3 c = g.mac;
            g.loadIr(shr16(c, 8)), g.sqr();
            int32_t n = macSum(), k = 0;
            for (; n > 32000; n >>= 2) ++k;
            c = shr(c, k);
            g.loadIr(c), g.sqr();
            n = macSum();
            if (n <= 0x18) continue;
            g.loadV0(c), g.mvmva(false, Gte::kLight, Gte::kV0, false);
            const int32_t e1 = sumAbs(g.mac);
            g.mvmva(false, Gte::kColor, Gte::kV0, false);
            const int32_t e2 = sumAbs(g.mac);
            g.setRotRow0(p), g.mvmva(false, Gte::kRot, Gte::kV0, false);
            int32_t pc = g.mac.x;
            g.setRotRow0(d), g.mvmva(false, Gte::kRot, Gte::kV0, false);
            int32_t dc = g.mac.x;
            if (dc > 0) dc = -dc, pc = -pc, c = {-c.x, -c.y, -c.z};
            int32_t gap = absw(pc) - e1 - e2;
            if (dc == 0) {
                if (gap >= 0) return 2;
                continue;
            }
            bool entry = true;
            if (gap < 0) {  // overlapping along it: a contact only when the overlap is within the axis length
                int32_t o = -gap, m = n;
                if (gap != -32000 && o > 31999)
                    do o >>= 1, m >>= 2;
                    while (o > 32000);
                if (o * o <= m) gap = 0;
                else entry = false;
            }
            if (entry) {
                if (pc < 0) return 2;
                int32_t ad = absw(dc);
                if (ad < gap) return 0x10;
                for (; ad > 32000; ad >>= 1) gap >>= 1;
                const int32_t tt = (gap << 12) / ad;
                if (r.tIn < tt) {
                    if (tmax < tt) return 0x10;
                    r.tIn = s16(tt), r.num = s16(gap), r.den = s16(ad), r.normal = narrow(c);
                }
            }
            int32_t e = e1 + e2 + pc, od = -dc;
            if (e <= od) {
                for (; e > 32000; e >>= 1) od >>= 1;
                if (const int32_t tt = (e << 12) / od; tt < r.tOut) r.tOut = s16(tt);
            }
        }
    return 1;
}
} // namespace

bool boxBox(Sweep& w, const ShapeRec& rec) {
    Gte& g = gte();
    // t[0] the mover's axes in the shape's frame, t[1] the shape's in the mover's, t[2] / t[3] both in the world.
    std::array<Axes, 4> t;
    auto axesOf = [&](SVec half, Axes& world, Axes& other) {
        for (int i = 0; i < 3; ++i) {
            SVec e{};
            e[i] = half[i];
            g.loadV(0, e), g.llv0(), world[size_t(i)].v = g.irS();
            g.rtir12(), other[size_t(i)].v = g.irS();
        }
    };
    g.setRot(rec.inv), g.setLight(w.rot), axesOf(w.half, t[2], t[0]);
    g.setRot(w.inv), g.setLight(rec.rot), axesOf(rec.half, t[3], t[1]);
    SVec at = w.centre;
    if (boxBoxPush(w, rec, t)) at += w.push, w.pushed = 1;
    if (!w.best) return false;
    int16_t tIn = -10000, tOut = 10000, num = 0, den = 0;
    SVec normal;
    Range r;
    // The shape's faces (in its frame), then the mover's (in its own), then the edges.
    g.setRot(rec.inv);
    SVec p = rtv(at - rec.centre), d = rtv(w.motion);
    r.tIn = tIn, r.tOut = tOut;
    if (!(slabRange(p, rec.half, d, t[0], w.t, r) & 1)) return false;
    if (tIn < r.tIn) tIn = r.tIn, g.setRot(rec.rot), normal = rtv(r.normal), num = r.num, den = r.den;
    tOut = std::min(tOut, r.tOut);
    if (tIn > tOut) return false;
    g.setRot(w.inv);
    p = rtv(rec.centre - at), d = rtv(neg(w.motion));
    r.tIn = tIn, r.tOut = tOut;
    if (!(slabRange(p, w.half, d, t[1], w.t, r) & 1)) return false;
    if (tIn < r.tIn) tIn = r.tIn, g.setRot(w.rot), normal = rtv(neg(r.normal)), num = r.num, den = r.den;
    tOut = std::min(tOut, r.tOut);
    if (tIn > tOut) return false;
    if (!(edgeRange(at - rec.centre, w.motion, t[2], t[3], w.t, r) & 1)) return false;
    if (tIn < r.tIn) tIn = r.tIn, normal = r.normal, num = r.num, den = r.den;
    tOut = std::min(tOut, r.tOut);
    if (tIn > tOut || tIn < 0 || tIn > w.t) return false;
    w.t = tIn;
    const SVec hit = scaled(w.motion, num, den);
    if (const int32_t dd = lenSq(hit); dd < w.best) w.best = dd, w.contact = hit, w.normal = normal;
    return true;
}

bool boxPoly(Sweep& w, const PolySlot& slot, const PolyWorld& world, BoxPolyScratch& s) {
    const Poly& poly = *slot.poly;
    const SVec n = poly.n;
    const int cnt = poly.edgeCount;
    int16_t pen = 30000;  // the shallowest overlap of a face (or the plane)
    bool inside = true;
    int32_t tIn = -10000, tOut = w.t + 1;
    const int32_t dEnd = (w.end.z * n.z + w.end.y * n.y + poly.d + w.end.x * n.x) >> 14;
    const int32_t dStart = (w.centre.z * n.z + w.centre.x * n.x + poly.d + w.centre.y * n.y) >> 14;
    if (w.radius <= dEnd || (dEnd < -w.radius && dStart < -w.radius)) return false;
    if (dStart <= dEnd && (world.surfaceFlags(poly) & 0x200)) return false;
    // The plane: the box's extent along n (axes through the colour matrix), the centre's and the motion's distance
    // (light rows: centre, motion and the third axis the colour copy left).
    Gte& g = gte();
    g.setColor(rows(w.axes)), g.setLight(rows(w.centre, w.motion, w.axes[2].v));
    g.loadV(1, n), g.mvmva(false, Gte::kColor, Gte::kV1, false);
    const int32_t ext = sumAbs(g.mac);
    g.mvmva(false, Gte::kLight, Gte::kV1, false);
    const int32_t dist = g.mac.x + poly.d, mn = g.mac.y;
    SVec push, pushN;
    if (absw(dist) < absw(ext)) {  // across the plane: out along n, leaving by the far side
        const int32_t v = (dist < 0 ? -ext : ext) - dist + 0x2000, k = v >> 14;
        pen = s16(v < 0 ? -k : k);
        push = {s16((n.x * k + 0x2000) >> 14), s16((n.y * k + 0x2000) >> 14), s16((n.z * k + 0x2000) >> 14)};
        pushN = n;
        if (mn != 0) {
            int32_t e = dist, m = mn;
            if (m < 0) m = -m;
            else e = -e;
            for (e += ext; e > 200000; e >>= 1) m >>= 1;
            if (m != 0)
                if (const int32_t t = (e << 12) / m; t < tOut) tOut = t;
        }
    } else {
        inside = false;
        if (mn == 0) return false;
        int32_t dd = dist, m = mn, e = ext;
        if (dd < 0) dd = -dd;
        else m = -m;
        if (m < dd - e) return false;
        for (; dd + e > 200000; dd >>= 1) e >>= 1, m >>= 1;
        if (m == 0) return false;
        s.num = dd - e, s.den = m;
        tIn = (dd - e) * 0x1000 / m, tOut = (dd + e) * 0x1000 / m;
        s.normal = mn < 0 ? n : neg(n);
    }
    if (!inside && (tOut < tIn || !w.best)) return false;
    // The polygon's bounds in the box's frame against its faces.
    SVec lo{10000, 10000, 10000}, hi{-10000, -10000, -10000};
    g.setRot(w.inv);
    const SVec md = rtvSf(w.motion);
    for (int e = 0; e < cnt; ++e) {
        const SVec q = rtvSf(world.vertex(poly, e) - w.centre);
        for (int k = 0; k < 3; ++k) {
            if (q[k] < lo[k]) lo[k] = q[k];
            if (hi[k] < q[k]) hi[k] = q[k];
        }
    }
    int face = -1;
    int32_t faceGap = 0;
    g.setRot(w.rot);
    auto faceNormal = [&](int i, int16_t v) {
        SVec e{};
        e[i] = v;
        return rtvSf(e);
    };
    for (int i = 0; i < 3; ++i) {
        const int32_t m = md[i], h = w.half[i];
        if (const int32_t gp = h - lo[i]; gp < 1) {
            inside = false;
            if (m < 1) return false;
            if (const int32_t t = gp * 0x1000 / -m; tIn < t) s.num = gp, s.den = -m, tIn = t, s.normal = faceNormal(i, -0x4000);
        } else if (gp < pen) pen = s16(gp), face = i, faceGap = gp;
        if (const int32_t gp = hi[i] + h; gp > 0) {
            if (gp < pen) pen = s16(gp), face = i, faceGap = -gp;
        } else {
            inside = false;
            if (m >= 0) return false;
            if (const int32_t t = gp * -0x1000 / -m; tIn < t) s.num = -gp, s.den = -m, tIn = t, s.normal = faceNormal(i, 0x4000);
        }
        if (m != 0)
            if (const int32_t t = ((m < 1 ? lo[i] - h : hi[i] + h) << 12) / m; t < tOut) tOut = t;
    }
    if (face != -1 && inside) {  // out through the shallowest face: the rotated push (IR of the first MVMVA), its normal
        SVec e{};
        e[face] = s16(faceGap * -0x40);
        g.setRot(w.rot), g.loadV(0, e), g.mvmva(true, Gte::kRot, Gte::kV0, false);
        e = {}, e[face] = faceGap >= 0 ? -0x4000 : 0x4000;
        g.loadV(0, e);
        const SVec r = g.irS();
        g.mvmva(true, Gte::kRot, Gte::kV0, false);
        push = {s16((r.x + 0x20) >> 6), s16((r.y + 0x20) >> 6), s16((r.z + 0x20) >> 6)};
        pushN = g.irS();
    }
    if (!inside && (tOut < tIn || !w.best)) return false;
    // The polygon's edges against the box's axes (the cross products as separating axes).
    std::array<SVec, 32> v;
    for (int e = 0; e < cnt; ++e) v[size_t(e)] = world.vertex(poly, e) - w.centre;
    int32_t best = int32_t(pen) * pen, bestN = 0;
    bool edgePushed = false;
    for (int e = 0; e < cnt; ++e) {
        const int e1 = e + 1 == cnt ? 0 : e + 1, e2 = e1 + 1 == cnt ? 0 : e1 + 1;
        g.setLightRow2(v[size_t(e)]), g.setRotDiag(v[size_t(e1)] - v[size_t(e)]);
        const SVec next = v[size_t(e2)] - v[size_t(e1)];
        for (int k = 0; k < 3; ++k) {
            g.loadIr(w.axes[size_t(k)].v), g.op();
            V3 c = g.mac;
            if (c.x == 0 && c.y == 0 && c.z == 0) continue;
            g.loadIr(shr16(c, 12)), g.sqr();
            int32_t q = macSum(), sh = 0;
            for (; q > 0x32; q >>= 2) ++sh;
            c = shr(c, sh);
            if (next.z * c.z + next.x * c.x + next.y * c.y > 0) c = {-c.x, -c.y, -c.z};
            g.loadV(0, narrow(c)), g.mvmva(false, Gte::kColor, Gte::kV0, false);
            V3 a = g.mac;
            (k == 0 ? a.x : k == 1 ? a.y : a.z) = 0;  // the axis itself lies in the plane of c
            const int32_t ext2 = sumAbs(a);
            g.mvmva(false, Gte::kLight, Gte::kV0, false);
            const int32_t vc = -g.mac.z, m = -g.mac.y;
            int32_t gap = vc - ext2;
            if (gap < 0) {
                if (inside) {
                    int32_t o = -gap;
                    g.loadIr(c), g.sqr();
                    int32_t cn = macSum();
                    if (gap != -32000 && o > 31999)
                        do c = shr(c, 1), o >>= 1, cn >>= 2;
                        while (o > 32000);
                    const int32_t mm = o * o / cn;
                    if (best <= mm) continue;
                    best = mm, bestN = cn, pushN = narrow(c), edgePushed = true;
                }
                if (m < 0) {
                    int32_t num = ext2 - vc, den = -m;
                    for (; num > 200000; num >>= 1) den >>= 1;
                    if (den != 0) {
                        if (const int32_t t = (num << 12) / den; t < tOut) tOut = t;
                        if (tOut < tIn && !inside) return false;
                    }
                }
            } else {
                if (m < gap || m == 0) return false;
                int32_t mm = m;
                for (; gap > 200000; gap >>= 1) mm >>= 1;
                if (const int32_t t = (gap << 12) / mm; tIn < t) s.num = gap, s.den = mm, tIn = t, s.normal = narrow(c);
                inside = false;
            }
        }
    }
    if (edgePushed && inside) {
        const int32_t a = root(best), b = root(bestN);
        push = {s16(pushN.x * a / b), s16(pushN.y * a / b), s16(pushN.z * a / b)};
    }
    if (inside) {  // still overlapping: pushed out, a contact at once when moving into it
        w.pushed = 1, w.push = push;
        if (dot(pushN, w.motion) >= 0) return false;
        w.best = 0, w.t = 0, w.contact = {}, w.normal = pushN;
        return true;
    }
    if (!(tIn <= tOut && tIn <= w.t)) return false;
    w.t = s16(tIn);
    const SVec hit = {s16(mulDiv(w.motion.x, s.num, s.den)), s16(mulDiv(w.motion.y, s.num, s.den)), s16(mulDiv(w.motion.z, s.num, s.den))};
    if (const int32_t dd = lenSq(hit); dd < w.best) w.best = dd, w.contact = hit, w.normal = s.normal;
    return true;
}

uint32_t polyPush(PolyPush& r, const PolySlot& slot, const PolyWorld& world) {
    const Poly& poly = *slot.poly;
    const int32_t* edgeD = slot.edgeD;
    const int32_t R = r.radius;
    const int32_t depth = (wrap(int64_t(poly.n.z) * r.p.z + int64_t(poly.n.y) * r.p.y + int64_t(poly.n.x) * r.p.x + poly.d) >> 14) - R;
    if (depth >= 0 || depth < -R) return 2;
    auto outOfFace = [&] {  // straight out along the plane normal to the surface + 1
        const int32_t k = 1 - depth;
        r.push = {s16(wrap(int64_t(poly.n.x) * k + 0x2000) >> 14), s16(wrap(int64_t(poly.n.y) * k + 0x2000) >> 14),
                  s16(wrap(int64_t(poly.n.z) * k + 0x2000) >> 14)};
        return 4u;
    };
    const int n = poly.edgeCount;
    int32_t worst = 0;
    int e = 0;
    for (int i = 0; i < n; ++i) {  // outside an edge plane by up to the radius: near that edge
        const SVec en = poly.edge(i).n;
        const int32_t v = wrap(int64_t(en.x) * r.p.x + int64_t(en.z) * r.p.z + int64_t(en.y) * r.p.y + edgeD[i]) >> 14;
        if (R < v) return 2;
        if (worst < v) e = i, worst = v;
    }
    if (worst == 0) return outOfFace();
    const SVec a = world.vertex(poly, e), edge = world.vertex(poly, e + 1 < n ? e + 1 : 0) - a;
    r.p = r.p - a;
    int32_t f = wrap(int64_t(r.p.z) * edge.z + int64_t(r.p.x) * edge.x + int64_t(r.p.y) * edge.y), ee = lenSq(edge);
    if (ee < f) r.p = r.p - edge;
    else if (f >= 1) {
        for (; f > 200000; f >>= 1) ee >>= 1;
        r.p = r.p - scaled(edge, f, ee);
    }
    const int32_t dist = lenSq(r.p);
    if (wrap(int64_t(R) * R) <= dist) return 2;
    const int32_t s = root(wrap(int64_t(dist) << 8)) >> 4;
    r.push = scaled(r.p, R - s + 1, s);
    return 4;
}

bool polySweep(Sweep& w, const PolySlot& slot, const PolyWorld& world) {
    const Poly& poly = *slot.poly;
    const int32_t* edgeD = slot.edgeD;
    const int32_t R = w.radius;
    // Plane distances of the start and the end through the GTE (light rows 0 / 2 against the normal in V2).
    Gte& g = gte();
    g.setLightRow0(w.centre), g.setLightRow2(w.end), g.loadV(2, poly.n);
    g.mvmva(false, Gte::kLight, Gte::kV2, false);
    const int32_t dEnd = wrap(int64_t(g.mac.z) + poly.d) >> 14, dStart = wrap(int64_t(g.mac.x) + poly.d) >> 14;
    if (R <= dEnd || (dEnd < -R && dStart < 0)) return false;
    bool away = false;
    if (dStart <= dEnd) {
        if (world.surfaceFlags(poly) & 0x200) return false;
        away = true;
    }
    const int n = poly.edgeCount;
    int16_t tBest = s16(w.t + 1);
    for (int i = 0; i < n; ++i) {  // both ends beyond one edge plane by more than the radius
        g.loadV(1, poly.edge(i).n), g.mvmva(false, Gte::kLight, Gte::kV1, false);
        const int64_t r14 = int64_t(R) << 14;
        if (r14 < wrap(int64_t(g.mac.x) + edgeD[i]) && r14 < wrap(int64_t(g.mac.z) + edgeD[i])) return false;
    }
    SVec at = w.centre;
    PolyPush in;
    in.p = w.centre, in.radius = w.radius;
    if (polyPush(in, slot, world) == 4) at += in.push, w.push = in.push, w.pushed = 1;
    if (!w.best || away) return false;
    auto edgeMask = [&](SVec q) {  // the edges whose planes q lies outside of by at least 1 (Q14)
        uint16_t m = 0;
        for (int i = 0; i < n; ++i) {
            const SVec en = poly.edge(i).n;
            if (int32_t(uint32_t(wrap(int64_t(en.y) * q.y + int64_t(en.x) * q.x + int64_t(en.z) * q.z + edgeD[i])) & 0xffffc000u) > 0)
                m = uint16_t(m | (1u << i));
        }
        return m;
    };
    g.setLightRow0(at), g.setLightRow2(w.motion);
    g.mvmva(false, Gte::kLight, Gte::kV2, false);
    int32_t gap = wrap(int64_t(poly.d) - int64_t(R) * 0x4000 + g.mac.x);
    uint16_t mask;
    SVec hit, normal;
    if (gap < 0) mask = edgeMask(at);  // already within the radius of the plane
    else {
        int32_t rate = wrap(-int64_t(g.mac.z));
        if (rate < gap) return false;
        for (; gap > 500000; gap >>= 1) rate >>= 1;
        rate += rate == 0;
        const int16_t t = s16((gap << 12) / rate);
        if (tBest < t) return false;
        const SVec tr = scaled(w.motion, gap, rate);
        mask = edgeMask(tr + at);
        if (!mask) normal = poly.n, hit = tr, tBest = t;
    }
    if (mask) {
        // Outside some edges at the contact: those edges as cylinders, the latest entry wins.
        CylinderCast cyl;
        cyl.d = w.motion, cyl.radius = R;
        int16_t tEdge = -1;
        bool inside = false;
        SVec tr, ea, eb;
        for (int i = 0; i < n; ++i) {
            if (!(mask & (1u << i))) continue;
            const SVec a = world.vertex(poly, i);
            cyl.axis = world.vertex(poly, i + 1 < n ? i + 1 : 0) - a, cyl.p = at - a;
            const uint32_t u = cylinderCast(cyl);
            if (u & 0x1a) return false;
            if ((u & 4) && !inside) {
                inside = true;
                continue;
            }
            if (!(u & 1)) continue;
            if (w.t < cyl.t) return false;
            if (tEdge < cyl.t) tEdge = s16(cyl.t), tr = cyl.travel, ea = a, eb = cyl.axis;
        }
        if (tEdge < 0) return false;
        SVec v = tr - ea + at;
        int32_t f = dot(v, eb), ee = lenSq(eb);
        SphereCast corner;
        if (f >= 0 && f <= ee) {  // on the edge's side: the normal from the edge line
            for (; f > 200000; f >>= 1) ee >>= 1;
            v = v - scaled(eb, f, ee);
            normal = {s16((int32_t(v.x) << 14) / R), s16((int32_t(v.y) << 14) / R), s16((int32_t(v.z) << 14) / R)};
            hit = tr, tBest = tEdge;
        } else {  // past an end: that vertex as a sphere
            corner.p = f < 0 ? v : v - eb, corner.d = w.motion, corner.radius = R;
            if (!(sphereCast(corner) & 1)) return false;
            tBest = s16(corner.t), hit = corner.travel, normal = corner.normal;
        }
    }
    if (!mask && gap < 0) return false;  // within the plane's reach and inside every edge: no contact this pass
    if (!(tBest <= w.t && tBest < w.left)) return false;
    w.t = tBest;
    keepNearest(w, hit, normal);
    return true;
}

bool pointPoly(Sweep& w, const PolySlot& slot, const PolyWorld& world) {
    const Poly& poly = *slot.poly;
    const int32_t* edgeD = slot.edgeD;
    Gte& g = gte();
    SVec start = w.centre + w.startOffset;
    g.setLightRow0(start), g.setLightRow2(w.end + w.startOffset), g.loadV(1, poly.n);
    g.mvmva(false, Gte::kLight, Gte::kV1, false);
    const int32_t dStart = wrap(int64_t(poly.d) + g.mac.x) >> 14, dEnd = wrap(int64_t(poly.d) + g.mac.z) >> 14;
    if (!((dStart < 8 || dEnd < 8) && (dStart > -9 || dEnd > -9))) return false;
    bool away = false;
    if (dStart <= dEnd) {
        if (world.surfaceFlags(poly) & 0x200) return false;
        away = true;
    }
    const int n = poly.edgeCount;
    if (s16(dStart) < 8) {  // within the skin: out to 9 in front, if the start lies inside every edge
        for (int i = 0; i < n; ++i) {
            g.loadV(2, poly.edge(i).n), g.mvmva(false, Gte::kLight, Gte::kV2, false);
            if (wrap(int64_t(g.mac.x) + edgeD[i]) > 0x1ffff) return false;
        }
        g.loadIr0(9 - s16(dStart)), g.loadIr(poly.n), g.gpf(0);
        w.push = {s16(g.mac.x >> 14), s16(g.mac.y >> 14), s16(g.mac.z >> 14)};
        start += w.push;
        w.pushed = 1;
        g.setLightRow0(start);
    }
    if (!w.best || away) return false;
    g.setLightRow2(w.motion), g.mvmva(false, Gte::kLight, Gte::kV1, false);
    int32_t e = wrap(int64_t(g.mac.x) - 0x20000 + poly.d), v = wrap(-int64_t(g.mac.z));
    if (e > v) return false;
    for (; e > 500000; e >>= 1) v >>= 1;
    if (v == 0) return false;
    const int16_t t = s16((e << 12) / v);
    if (w.t < t) return false;
    g.loadIr0(t), g.loadIr(w.motion), g.gpf(0);
    auto toward0 = [](int32_t m) { return s16((m < 0 ? m + 0xfff : m) >> 12); };
    const SVec tr{toward0(g.mac.x), toward0(g.mac.y), toward0(g.mac.z)};
    g.setLightRow2(start + tr);
    for (int i = 0; i < n; ++i) {  // the crossing point inside every edge
        g.loadV(1, poly.edge(i).n), g.mvmva(false, Gte::kLight, Gte::kV1, false);
        if (wrap(int64_t(g.mac.z) + edgeD[i]) > 0x1ffff) return false;
    }
    const int32_t d = gteLenSq(tr);
    w.t = t;
    if (d < w.best) w.best = d, w.contact = tr, w.normal = poly.n;
    return true;
}

uint32_t capsulePolyPush(Sweep& w, const PolySlot& slot, const PolyWorld& world) {
    const Poly& poly = *slot.poly;
    const int32_t* edgeD = slot.edgeD;
    const int cnt = poly.edgeCount;
    const int16_t R = w.half.x;
    // The plane seen from the centre's side.
    SVec n = poly.n;
    int32_t dc = wrap(int64_t(n.y) * w.centre.y + int64_t(w.centre.z) * n.z + poly.d + int64_t(n.x) * w.centre.x);
    if (dc < 0) dc = wrap(-int64_t(dc)), n = neg(n);
    // The capsule end nearer the plane.
    int32_t e = wrap(int64_t(w.capA.y) * n.y + int64_t(w.capA.z) * n.z + int64_t(w.capA.x) * n.x);
    SVec end = w.capA;
    if (e > 0) end = w.capB, e = wrap(-int64_t(e));
    const int32_t along = e;
    const SVec axis2{s16(end.x * -2), s16(end.y * -2), s16(end.z * -2)};
    e = wrap(int64_t(e) + dc);
    if (int32_t(R) * 0x4000 <= e) return 0;
    const SVec q = end + w.centre;
    auto outOfFace = [&] {
        const int32_t k = R - (wrap(int64_t(e) + 0x2000) >> 14) + 1;
        w.push = w.push + SVec{s16(wrap(int64_t(n.x) * k + 0x2000) >> 14), s16(wrap(int64_t(n.y) * k + 0x2000) >> 14),
                               s16(wrap(int64_t(n.z) * k + 0x2000) >> 14)};
        return 4u;
    };
    auto outside = [&](SVec p) {  // the edges whose planes p lies outside of
        uint16_t m = 0;
        for (int i = 0; i < cnt; ++i) {
            const PolyEdge& ed = poly.edge(i);
            if (wrap(int64_t(ed.n.x) * p.x + int64_t(ed.n.y) * p.y + int64_t(ed.n.z) * p.z + edgeD[i]) > 0) m = uint16_t(m | (1u << i));
        }
        return m;
    };
    uint16_t mask;
    if (e < 0) {  // that end is through the plane: where the axis crosses it
        int32_t f = along, g = e;
        for (; g > 500000; g >>= 1) f >>= 1;
        const SVec x = q - scaled(end, g, f);
        mask = outside(x);
        if (!mask) mask = 0xffff;  // crossing inside the polygon: every edge
    } else {
        mask = outside(q);
        if (!mask) return outOfFace();
    }
    // The nearest edge to the capsule axis (the farthest when the axis crosses inside).
    int32_t best = -100000;
    int bi = -1;
    LineGap gap, kept;
    gap.b = axis2;
    for (int i = 0; i < cnt; ++i) {
        if (!(mask & (1u << i))) continue;
        const SVec a = world.vertex(poly, i);
        gap.c = q - a;
        gap.a = world.vertex(poly, i + 1 == cnt ? 0 : i + 1) - a;
        int32_t v = lineGap(gap);
        if (mask == 0xffff) v = wrap(-int64_t(v));
        if (best < v) best = v, kept = gap, bi = i;
    }
    const int32_t k0 = wrap(int64_t(e) + 0x2000) >> 14;
    if (best < k0 && mask == 0xffff) return outOfFace();
    if (bi == -1) return 2;
    if (mask == 0xffff) {  // out across the edge line
        SVec nn = narrow(kept.n);
        if (dot(kept.c, nn) > 0) nn = neg(nn);
        const int32_t k = best < 0 ? R - best + 2 : -2 - R - best;
        w.push = w.push + SVec{s16(mulDiv(nn.x, k, kept.len)), s16(mulDiv(nn.y, k, kept.len)), s16(mulDiv(nn.z, k, kept.len))};
        return 4;
    }
    SegmentPush sp;
    sp.c = kept.c, sp.a = kept.a, sp.b = axis2, sp.radius = R;
    SVec nearest;  // its scratch word (no reader in this test)
    if (!segmentPush(sp, nearest)) return 2;
    w.push = w.push + sp.push;
    return 4;
}

bool capsulePoly(Sweep& w, const PolySlot& slot, const PolyWorld& world) {
    const Poly& poly = *slot.poly;
    const int32_t* edgeD = slot.edgeD;
    const int cnt = poly.edgeCount;
    Gte& g = gte();
    g.setLightRow0(w.centre), g.setLightRow2(w.end), g.loadV(2, poly.n);
    g.mvmva(false, Gte::kLight, Gte::kV2, false);
    const int32_t dEnd = wrap(int64_t(poly.d) + g.mac.z) >> 14, dStart = wrap(int64_t(poly.d) + g.mac.x) >> 14;
    if (!(dEnd < w.radius && (-w.radius <= dEnd || -w.radius <= dStart))) return false;
    bool away = false;
    if (dStart <= dEnd) {
        if (world.surfaceFlags(poly) & 0x200) return false;
        away = true;
    }
    int16_t tBest = s16(w.t + 1);
    SVec at = w.centre;
    if (capsulePolyPush(w, slot, world) & 4) at += w.push, w.pushed = 1;
    if (away || !w.best) return false;
    // The capsule end nearer the plane, and its plane gap.
    g.setLightRow0(w.capA), g.mvmva(false, Gte::kLight, Gte::kV2, false);
    int32_t m = g.mac.x;
    SVec end = w.capA;
    if (m >= 1) end = w.capB, m = wrap(-int64_t(m));
    const SVec axis2{s16(end.x * -2), s16(end.y * -2), s16(end.z * -2)};
    g.setLightRow0(at), g.mvmva(false, Gte::kLight, Gte::kV2, false);
    int32_t gap = wrap(int64_t(poly.d) + g.mac.x - int64_t(w.half.x) * 0x4000 + m);
    auto outside = [&](SVec p, bool first) {  // the edges whose planes p lies outside of (Q14 >= 1)
        uint16_t mask = 0;
        g.setLightRow2(p);
        for (int i = 0; i < cnt; ++i) {
            g.loadV(0, poly.edge(i).n), g.mvmva(false, Gte::kLight, Gte::kV0, false);
            if (int32_t(uint32_t(wrap(int64_t(edgeD[i]) + g.mac.z)) & 0xffffc000u) > 0) {
                mask = uint16_t(mask | (1u << i));
                if (first) break;
            }
        }
        return mask;
    };
    uint16_t mask;
    SVec hit, normal, tr;
    int16_t tFace = 0;
    if (gap < 0) mask = outside(end + at, false);  // that end already within the radius
    else {
        g.setLightRow0(w.motion), g.mvmva(false, Gte::kLight, Gte::kV2, false);
        int32_t v = wrap(-int64_t(g.mac.x));
        if (v < 1 || v < gap) return false;
        for (; gap > 500000; gap >>= 1) v >>= 1;
        tFace = s16((gap << 12) / v);
        if (tBest <= tFace) return false;
        tr = scaled(w.motion, gap, v);
        mask = outside(end + tr + at, true);
        if (!mask) tBest = tFace, hit = tr, normal = poly.n;
    }
    if (mask) {
        // The edges as slabs against the capsule axis; the latest entry wins.
        SlabCast slab;
        slab.b = axis2, slab.radius = w.half.x, slab.d = w.motion;
        SVec bestAxis, bestStart = end + at;
        uint32_t last = 0, u = 0;
        int16_t tEdge = -1;
        tr = {};
        for (int i = 0; i < cnt; ++i) {
            if (!(mask & (1u << i))) continue;
            const SVec a = world.vertex(poly, i);
            slab.a = world.vertex(poly, i + 1 < cnt ? i + 1 : 0) - a;
            slab.p = end - a + at;
            int32_t normalY;  // its scratch word (no reader in this test)
            u = slabCast(slab, normalY);
            if (u & 0x1a) return false;
            if (last == 0 || ((u & 1) && tEdge < slab.t)) {
                if (u & 1) {
                    tEdge = s16(slab.t);
                    if (tBest < slab.t) return false;
                    tr = slab.travel, normal = slab.normal;
                }
                bestAxis = slab.a, bestStart = slab.p, last = u;
            }
        }
        if (!last) return false;
        LineNearest line;
        line.a = bestAxis, line.b = axis2, line.c = tr + bestStart;
        const int32_t s = lineNearest(line);
        const SVec x = line.out;
        CapsuleCast cap;
        cap.tmax = w.t, cap.radius = w.half.x;
        if (s < 0 || s > 0x1000) {  // past an end of the capsule axis: the capsule against the edge
            cap.axis = bestAxis, cap.d = w.motion, cap.p = s > 0x1000 ? bestStart + axis2 : bestStart;
            if (capsuleCast(cap)) tBest = s16(cap.t), normal = cap.normal, hit = cap.hit;
        } else {
            if (u & 4) return false;
            int32_t f = dot(x, bestAxis), bb = lenSq(bestAxis);
            if (f < 0 || bb < f) {  // past an end of the edge: the edge's end against the capsule
                cap.axis = axis2, cap.d = neg(w.motion), cap.p = bb < f ? neg(bestStart) + bestAxis : neg(bestStart);
                if (capsuleCast(cap)) tBest = s16(cap.t), normal = neg(cap.normal), hit = neg(cap.hit);
            } else tBest = tEdge, hit = tr;
        }
    }
    if (!(tBest <= w.t)) return false;
    w.t = tBest;
    const int32_t d = gteLenSq(hit);
    if (d < w.best) w.normal = normal, w.best = d, w.contact = hit;
    return true;
}

void applyPush(Sweep& w, const PolySlot* slots, const PolyWorld& world) {
    Gte& g = gte();
    const SVec pp = w.passPush;
    w.centre = w.centre - pp;
    const int32_t r2 = w.radius2, len = root(lenSq(pp));
    g.loadIr0((len + r2) * 0x1000 / len), g.loadIr(pp), g.gpf(12);  // the push grown by radius2 (unused but for the GTE)
    SVec start = w.centre, best = pp;
    int16_t tMin = 0x2000;
    if (w.mode & 8) start += w.startOffset;
    for (int i = 0; i < w.polyCount; ++i) {
        if (!slots[i].poly) continue;
        const Poly& poly = *slots[i].poly;
        const int32_t* edgeD = slots[i].edgeD;
        if (world.surfaceFlags(poly) & 0x30) continue;
        int32_t e = wrap(int64_t(start.x) * poly.n.x + int64_t(start.z) * poly.n.z + poly.d + int64_t(start.y) * poly.n.y);
        if (e < 0) continue;  // behind the plane
        e = std::max(e - 0x20000, 0);
        int32_t v = wrap(-(int64_t(pp.y) * poly.n.y + int64_t(pp.z) * poly.n.z + int64_t(poly.n.x) * pp.x));
        if (!(e <= v && v != 0)) continue;
        for (; e > 500000; e >>= 1) v >>= 1;
        const int32_t t = (e << 12) / v;
        if (t > tMin) continue;
        const SVec tr = scaled(pp, e, v), q = start + tr;
        int k = 0;
        for (; k < poly.edgeCount; ++k) {
            const SVec en = poly.edge(k).n;
            if (int32_t(uint32_t(wrap(int64_t(en.y) * q.y + int64_t(en.x) * q.x + int64_t(en.z) * q.z + edgeD[k])) & 0xffffc000u) > 0x1ffff) break;
        }
        if (k == poly.edgeCount) tMin = s16(t), best = tr;
    }
    if (tMin >= 0x1001) {
        w.centre += pp;
        return;
    }
    const int32_t l = root(lenSq(best));
    if (r2 < l) {
        g.loadIr0((l - r2) * 0x1000 / l), g.loadIr(best), g.gpf(12);
        w.centre += g.irS();
    }
}

namespace {
// The shared loop of 0x44cd40 / 0x442200 over the listed shapes, with `test` the per-type test (none: false).
template <class Test> bool overShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes, bool skipHidden, Test test) {
    bool any = false;
    w.passPush = {};
    for (int i = 0; i < w.shapeCount; ++i) {
        const ShapeRec& rec = *shapes[i];
        const uint32_t t = rec.type;
        Body& other = *rec.owner;
        if ((skipHidden && (t & 0x400)) || ((self.flags0 & 0x2000000) && !(t & 0x200)) || (t & 0x80) ||
            ((self.f24 & 0x1000) && (other.flags0 & 0x10000000)) || ((self.flags10 & 0x400000) && (other.flags0 & 0x4000000)) ||
            ((self.flags0 & 0x4000000) && (other.flags10 & 0x400000)))
            continue;
        const bool hit = test(w, rec, t);
        if (hit) {
            w.hitOwner = rec.owner;
            if (w.normal.y < -11000) w.floorSurf = 0;
            any = true;
        }
        if (!w.pushed) continue;
        w.centre += w.push, w.end += w.push, w.passPush += w.push, w.push = {};
        w.wasPushed = 1, w.pushed = 0;
        if (!(other.marks & 0x40)) {
            other.marks |= 0x40;
            if (req.touchedCount < 15) req.touched[size_t(req.touchedCount++)] = rec.owner;
        }
        if (hit && w.normal.y < -11000) {  // standing on it (flags0 8: a platform) or landing on it
            if (!(other.flags0 & 8)) self.f28 |= 2, other.f28 |= 4;
            else {
                self.f20 |= 8, req.floor = rec.owner;
                if (w.motion.y > 0xb4) self.f28 |= 1, req.flags |= 1;
            }
            if (w.motion.y > 0x14) other.f20 |= 0x10;
        }
    }
    return any;
}
} // namespace

bool sweepShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    return overShapes(self, w, req, shapes, true, [](Sweep& sw, const ShapeRec& rec, uint32_t t) {
        return t & 1 ? sweepSphere(sw, rec) : t & 2 ? sweepCapsule(sw, rec) : t & 8 ? sweepBox(sw, rec) : false;
    });
}

bool moverShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    return overShapes(self, w, req, shapes, false, [](Sweep& sw, const ShapeRec& rec, uint32_t t) {
        return t & 1 ? moverSphere(sw, rec) : t & 2 ? moverCapsule(sw, rec) : (t & 0xc) == 8 ? moverBox(sw, rec) : false;
    });
}

bool boxShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    return overShapes(self, w, req, shapes, true, [](Sweep& sw, const ShapeRec& rec, uint32_t t) {
        return t & 1 ? boxSphere(sw, rec) : t & 2 ? boxCapsule(sw, rec) : t & 8 ? boxBox(sw, rec) : false;
    });
}

namespace {
// One mover pass (0x44cf80 sphere, 0x442430 capsule, 0x440060 box, 0x44ba20 point): the floor slot, the shapes
// (flags0 0x100), the push-back once the pass push exceeds `pushMin`, then every gathered polygon (the floor slot
// again only after a push). `around(phase)` lets the box pass swap its radius between the phases.
template <class PolyTest, class Shapes, class Around>
int16_t runPass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world,
                int32_t pushMin, PolyTest test, Shapes shapesTest, Around around) {
    int16_t code = 0;
    w.hitOwner = 0;
    auto surfRef = [&](const PolySlot& s) { return world.surface(*s.poly); };
    auto takePush = [&] {
        if (!w.pushed) return;
        w.centre += w.push, w.end += w.push, w.push = {}, w.pushed = 0;
    };
    around(0);
    if (slots[0].poly) {  // the floor slot
        w.push = {};
        if (!(world.surfaceFlags(*slots[0].poly) & 0x30) && test(slots[0])) {
            code = 1;
            w.surf = w.floorSurf = surfRef(slots[0]), w.floorPoly = slots[0].poly;
        }
        takePush();
    }
    around(1);
    if ((self.flags0 & 0x100) && shapesTest(self, w, req, shapes)) code = 2;
    around(2);
    const int32_t pushed = lenSq(w.passPush);
    if (pushed > pushMin) applyPush(w, slots, world);
    for (int i = 0; i < w.polyCount; ++i) {
        if (i == 0 && pushed == 0) continue;
        w.push = {};
        const PolySlot& s = slots[i];
        if (!s.poly) continue;
        if (!(world.surfaceFlags(*s.poly) & 0x30) && test(s)) {
            if (w.normal.y < -11000) w.floorSurf = surfRef(s), w.floorPoly = s.poly;
            code = 1;
            w.surf = surfRef(s);
        }
        takePush();
    }
    around(3);
    return code;
}
constexpr auto kNoSwap = [](int) {};
} // namespace

int16_t spherePass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world) {
    return runPass(self, w, req, slots, shapes, world, 0x19, [&](const PolySlot& s) { return polySweep(w, s, world); }, sweepShapes, kNoSwap);
}

int16_t capsulePass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world) {
    return runPass(self, w, req, slots, shapes, world, 0x19, [&](const PolySlot& s) { return capsulePoly(w, s, world); }, moverShapes, kNoSwap);
}

int16_t boxPass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world, BoxPolyScratch& s) {
    const int16_t r = w.radius;
    const bool round = w.mode & 0x200;  // the polygons see the box as a sphere of its half height
    return runPass(self, w, req, slots, shapes, world, 0x19,
                   [&](const PolySlot& sl) { return round ? polySweep(w, sl, world) : boxPoly(w, sl, world, s); }, boxShapes,
                   [&](int phase) { w.radius = round && (phase == 0 || phase == 2) ? w.half.y : r; });
}

int16_t pointPass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world) {
    auto shapesTest = [](Body& b, Sweep& sw, MoveRequest& rq, ShapeRec* const* sh) {
        return sw.mode & 2 ? sweepShapes(b, sw, rq, sh) : sw.mode & 1 ? moverShapes(b, sw, rq, sh) : boxShapes(b, sw, rq, sh);
    };
    return runPass(self, w, req, slots, shapes, world, 3, [&](const PolySlot& s) { return pointPoly(w, s, world); }, shapesTest, kNoSwap);
}

Leaf findLeaf(const Collision& h, SVec p) {
    uint32_t half = h.half;
    auto outside = [&](int16_t a, int16_t c) { return half < uint32_t(absw(a - c)); };
    if (outside(p.x, h.centre.x) || outside(p.y, h.centre.y) || outside(p.z, h.centre.z)) return {};
    // The cube centre is tracked in 32 bits and compared as s16, as the exe does.
    uint32_t x = uint16_t(h.centre.x), y = uint16_t(h.centre.y), z = uint16_t(h.centre.z);
    const uint8_t* node = h.nodes;
    int32_t idx = 0;
    while (node[2] & 1) {  // an inner node: down into p's octant
        half >>= 1;
        int oct = 0;
        if (p.x < int16_t(x)) x -= half;
        else x += half, oct |= 1;
        if (p.z < int16_t(z)) z -= half;
        else z += half, oct |= 2;
        if (p.y < int16_t(y)) y -= half, oct |= 4;
        else y += half;
        idx = reinterpret_cast<const int16_t*>(node + 4)[oct] * 4;
        if (idx == 0) break;
        node = h.nodes + idx;
    }
    return {idx != 0 ? node : nullptr, {s16(int32_t(x)), s16(int32_t(y)), s16(int32_t(z))}, s16(int32_t(half))};
}

int16_t gatherLeaves(const Collision& h, SVec p, int16_t r, Leaf* out) {
    const SVec hi{s16(p.x + r), s16(p.y + r), s16(p.z + r)}, lo{s16(p.x - r), s16(p.y - r), s16(p.z - r)};
    int16_t n = 0;
    Leaf* end = out;
    *end = findLeaf(h, p);
    int first = 0;  // 1 once p's own leaf is listed (it is tested first, then skipped by the scans)
    if (end->node) ++n, first = 1, ++end;
    const Leaf own = out[0];
    auto contains = [](const Leaf& l, SVec q) {
        return absw(q.x - l.centre.x) <= l.half && absw(q.y - l.centre.y) <= l.half && absw(q.z - l.centre.z) <= l.half;
    };
    auto add = [&](SVec q) {
        if (first && contains(own, q)) return;
        int j = first;
        while (j < n && !contains(out[j], q)) ++j;
        if (j < n) return;
        *end = findLeaf(h, q);
        if (end->node) ++n, ++end;
    };
    for (int k = 0; k < 6; ++k) {  // the face centres of the cube
        SVec q = p;
        q[k >> 1] = s16(q[k >> 1] + (k & 1 ? -r : r));
        add(q);
    }
    for (int k = 0; k < 8; ++k) add({k & 1 ? lo.x : hi.x, k & 2 ? lo.y : hi.y, k & 4 ? lo.z : hi.z});
    return n;
}

int16_t gatherPolys(const Collision& h, SVec p, int16_t r, const Body* self, PolyGather& g) {
    const Poly* floor = self ? self->groundPoly : nullptr;
    const int32_t r14 = int32_t(r) << 14;
    std::array<Leaf, 15> leaves;
    const int16_t nLeaves = gatherLeaves(h, p, r, leaves.data());
    int16_t limit = floor ? 1 : 0;  // the exe's slot limit count (the floor slot counted only when there is one)
    int next = 1, pool = 0, nv = 0;
    std::array<Poly*, 0x1f4> visited;
    g.slots[0].poly = nullptr;
    Gte& gt = gte();
    gt.setLightRow0(p);
    gt.setTrans({-(r14 >> 12), 0, 0});  // the edge MVMVAs add it: p · edge normal - r (Q14)
    auto gather = [&] {
        for (int l = 0; l < nLeaves; ++l) {
            const uint8_t* node = leaves[size_t(l)].node;
            const auto* ids = reinterpret_cast<const uint16_t*>(node + 6);
            for (int k = 0, cnt = reinterpret_cast<const int16_t*>(node)[2]; k < cnt; ++k) {
                Poly& poly = *reinterpret_cast<Poly*>(h.polys + ids[k] * 4);
                const uint16_t ec = uint16_t(poly.edgeCount);
                if (ec & 0x8000) continue;
                poly.edgeCount = int16_t(ec | 0x8000);
                visited[size_t(nv++)] = &poly;
                if (nv > 0x1f1) return;
                gt.loadV(0, poly.n), gt.mvmva(false, Gte::kLight, Gte::kV0, false);
                if (absw(gt.mac.x + poly.d) > r14) continue;
                gt.loadIr(poly.centre - p), gt.sqr();
                const int32_t rr = poly.radius + r;
                if (gt.mac.y + gt.mac.x + gt.mac.z > rr * rr) continue;
                const uint32_t sf = h.surfaces[poly.surface].flags;
                if (sf & 0x100) continue;
                if (sf & 0xf006) {
                    if (self ? ((sf & 0xf000) == 0 ? (sf & 4) != 0 : !((self->flags10 >> 16) & sf & 0xf000)) : (sf & 2) != 0) continue;
                }
                int32_t* out = &g.edgeD[size_t(pool)];
                int left = ec;
                for (int e = 0; left > 0; ++e, --left) {  // p within r of every edge plane
                    gt.setLightRow2(h.vertices[poly.edge(e).vertex].v);
                    gt.loadV(0, poly.edge(e).n), gt.mvmva(false, Gte::kLight, Gte::kV0, true);
                    const int32_t a = gt.mac.x, b = gt.mac.z;
                    if (a != b && a - b >= 0) break;
                    *out++ = -b;
                }
                if (left) continue;
                const PolySlot s{&poly, &g.edgeD[size_t(pool)]};
                if (&poly != floor) {
                    g.slots[size_t(next++)] = s;
                    if (++limit > 0xc5) return;
                } else g.slots[0] = s;
                pool += ec;
                if (pool > 0x2a8) return;
            }
        }
    };
    gather();
    for (int i = 0; i < nv; ++i) visited[size_t(i)]->edgeCount &= 0x7fff;
    return next == 1 && !g.slots[0].poly ? 0 : int16_t(next);
}

void cacheShape(Body& e, ShapeCache& c) {
    ShapeRec* r = e.shape;
    if (!r) {  // a new record: from the free list, else the least recently used one
        if (!c.free) {
            r = c.oldest;
            r->owner->shape = nullptr;
            if (r->next) r->next->prev = nullptr;
            c.oldest = r->next;
        } else {
            r = c.free;
            c.free = r->prev;
            if (!c.oldest) c.oldest = r;
        }
        r->next = nullptr, r->prev = c.newest;
        if (c.newest) c.newest->next = r;
        c.newest = r;
        r->type = 0x80000000;
    } else if (ShapeRec* nx = r->next) {  // to the front
        nx->prev = r->prev;
        if (!r->prev) c.oldest = nx;
        else r->prev->next = nx;
        r->prev = c.newest;
        c.newest->next = r;
        c.newest = r;
        r->next = nullptr;
    }
    ShapeRec& s = *r;
    const uint32_t f = e.flags0;
    if (bool(s.type & 0x80) != bool(f & 0x2000000)) s.type |= 0xf0000000;
    if (bool(s.type & 0x200) != bool(f & 0x40000000)) s.type |= 0xf0000000;
    if (!(s.type & 0xf0000000)) return;
    e.shape = r;
    // The entity's frame: its angles (or its built matrix), under the records it hangs on.
    Mat m;
    if (!(e.f24 & 0x8000)) {
        m.m = Mat{}.m, m.t = {};
        mulRotX(m, e.angles.x), mulRotZ(m, e.angles.z), mulRotY(m, e.angles.y);
    } else m = e.coord;
    for (const CoordRec* cr = e.coordRec; cr; cr = cr->super) m.m = mul(cr->m, m).m;
    Gte& g = gte();
    g.setRot(m);
    const SVec mx = e.boxMax, mn = e.boxMin;
    const SVec half{s16((mx.x - mn.x + 1) >> 1), s16(-((mn.y - mx.y + 1) >> 1)), s16((mx.z - mn.z + 1) >> 1)};
    s.owner = &e;
    if (!half.x || !half.y || !half.z) {
        s.type = 0x400;
        return;
    }
    s.type = 0;
    const SVec h{half.x, s16(-half.y), half.z};
    g.loadV0(widenSum(half, mn)), g.rtv0();
    V3 pos = widen(e.pos);
    addTo(pos, g.mac);  // the box centre in the world
    s.centre = narrow(pos);
    s.radius = std::max({h.x, h.y, h.z});
    if (f & 0x2000000) s.type = 0x80;
    if (f & 0x40000000) s.type |= 0x200;
    if (f & 0x400) {
        s.type |= 0x180;
        return;
    }
    if (f & 0x40000) {  // a box: frame, inverse, unit face normals and half-extent axes
        s.type |= 8;
        s.rot = m, s.inv = transposed(m);
        std::array<SVec, 3> unit;
        for (int i = 0; i < 3; ++i) {
            SVec u{};
            u[i] = 0x4000;
            unit[size_t(i)] = rtv(u);
        }
        for (int i = 0; i < 3; ++i) {
            s.half[i] = h[i];
            g.loadIr0(h[i]), g.loadIr(unit[size_t(i)]), g.gpf(0);
            s.axis(i) = shr16(g.mac, 14);
        }
        for (int i = 0; i < 3; ++i) s.faceNormal(i) = unit[size_t(i)];
        g.setRot(rows(unit[0], unit[1], unit[2])), g.loadIr(pos), g.rtir();
        s.invT = {-g.mac.x, -g.mac.y, -g.mac.z};
        return;
    }
    if (f & 1) {  // a capsule along the longest axis, a sphere when that is within 20 of the radius
        s.type |= 2;
        const int ax = std::max(h.x, h.y) < h.z ? 2 : h.x <= h.y ? 1 : 0;
        int32_t other = 0;
        for (int k = 0; k < 3; ++k)
            if (k != ax && other < h[k]) other = h[k];
        if (uint32_t(s.radius - other) < 0x15) s.type = (s.type & ~2u) | 1;
        s.capRadius = other;
        SVec off{};
        off[ax] = s16(s.radius - other);
        off = rtv(off);
        if (off == SVec{}) s.type = (s.type & ~2u) | 1;
        s.capAxis = {s16(off.x * -2), s16(off.y * -2), s16(off.z * -2)};
        s.capStart = off + narrow(pos), s.capEnd = narrow(pos) - off;
        return;
    }
    s.type |= 1;
}

int16_t listShapes(SVec p, const Body* self, int16_t r, Body* const* contacts, Body* const* views, ShapeCache& c) {
    std::array<Body*, 25> found;
    int16_t n = 0;
    auto near = [&](const Body& e) {
        const SVec d = e.pos - p;
        const int32_t R = e.radius + r;
        if (absw(d.x) > R || absw(d.y) > R || absw(d.z) > R) return false;
        gte().loadIr(d), gte().sqr();
        return macSum() <= R * R && e.radius != 0 && !(e.f20 & 0x2000);
    };
    auto onChain = [](const Body& from, const Body* to) {  // `to` among from's +0x90 ancestors
        for (const Body* a = from.parent; a; a = a->parent)
            if (a == to) return true;
        return false;
    };
    // Not the mover, an ancestor of it (unless a platform: flags0 8 / flags1 0x100000) nor a descendant.
    auto related = [&](const Body& e) {
        return (onChain(*self, &e) && !(e.flags0 & 8) && !(e.flags1 & 0x100000)) || onChain(e, self) || self == &e;
    };
    for (Body* const* l = contacts; *l && n < 25; ++l) {
        const Body& e = **l;
        if (!near(e)) continue;
        if (self && (related(e) || ((self->flags10 & 0x1000000) && (e.f20 & 0x40000)))) continue;
        found[size_t(n++)] = *l;
    }
    if (self && n < 25)
        for (Body* const* l = views; *l && n < 25; ++l) {
            const Body& e = **l;
            if ((e.flags0 & 0x2000100) != 0x2000100 || !near(e) || related(e)) continue;
            found[size_t(n++)] = *l;
        }
    for (int i = 0; i < n; ++i) {
        cacheShape(*found[size_t(i)], c);
        c.list[size_t(i)] = found[size_t(i)]->shape;
    }
    return n;
}

namespace {
void listTouched(MoveRequest& req, const ShapeRec& rec) {
    if (req.touchedCount >= 15) return;
    req.touched[size_t(req.touchedCount++)] = rec.owner;
    rec.owner->marks |= 0x40;
}
// The shapes the sweep passed through (those its shape loop skips), not listed yet: `test` = overlapping now.
template <class Test> void overlapPass(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes, Test test) {
    for (int i = 0; i < w.shapeCount; ++i) {
        const ShapeRec& rec = *shapes[i];
        const Body& other = *rec.owner;
        const uint32_t t = rec.type;
        if (other.marks & 0x40) continue;
        if (!(((self.flags0 & 0x2000000) && !(t & 0x200)) || (t & 0x80) || ((self.f24 & 0x1000) && (other.flags0 & 0x10000000)) ||
              ((self.flags10 & 0x400000) && (other.flags0 & 0x4000000)) || ((self.flags0 & 0x4000000) && (other.flags10 & 0x400000))))
            continue;
        if (test(rec, t)) listTouched(req, rec);
    }
}
// The sweep centre within `radius` + the record's radius of its centre (0x4030e0 squares).
bool centreNear(const Sweep& w, const ShapeRec& rec) {
    const V3 d = squared({w.centre.x - rec.centre.x, w.centre.y - rec.centre.y, w.centre.z - rec.centre.z});
    const int32_t R = w.radius + rec.radius;
    return d.y + d.x + d.z <= R * R;
}
// A sphere of radius r at q (box frame) touching the box `ext`.
bool sphereInBox(SVec q, SVec ext, int32_t r) {
    int32_t left = r * r;
    for (int k = 0; k < 3; ++k)
        if (const int32_t gap = absw(q[k]) - ext[k]; gap >= 0 && (left -= gap * gap) < 0) return false;
    return true;
}
bool insideBox(SVec q, SVec ext) { return absw(q.x) <= ext.x && absw(q.y) <= ext.y && absw(q.z) <= ext.z; }
} // namespace

void overlapFlat(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    overlapPass(self, w, req, shapes, [&](const ShapeRec& rec, uint32_t) { return centreNear(w, rec); });
}

void overlapSphere(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    overlapPass(self, w, req, shapes, [&](const ShapeRec& rec, uint32_t t) {
        if (t & 0x101) return centreNear(w, rec);
        if (t & 2) {
            SegmentReach s;
            s.p = w.centre - rec.capStart, s.axis = rec.capAxis, s.radius = w.radius + rec.capRadius;
            return segmentReach(s);
        }
        return (t & 8) && sphereInBox(apply(rec.inv, w.centre - rec.centre), rec.half, w.radius);
    });
}

void overlapCapsule(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    overlapPass(self, w, req, shapes, [&](const ShapeRec& rec, uint32_t t) {
        const SVec axis = w.capB - w.capA;
        if (t & 0x100) return centreNear(w, rec);
        if (t & 1) {
            SegmentReach s;
            s.p = rec.centre - w.centre - w.capA, s.axis = axis, s.radius = w.half.x + rec.radius;
            return segmentReach(s);
        }
        if (t & 2) {
            SegmentGap s;
            s.a = axis, s.b = rec.capAxis, s.c = rec.capStart - w.centre - w.capA;
            segmentGap(s);
            const int32_t R = w.half.x + rec.capRadius;
            return s.dist < R * R;
        }
        if (!(t & 8)) return false;
        BoxCast b;
        b.ext = rec.half, b.p = apply(rec.inv, w.centre - rec.centre + w.capA), b.d = apply(rec.inv, axis), b.radius = w.half.x;
        return boxCast(b, w.scratch.boxCastAxis);
    });
}

void overlapBox(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes) {
    overlapPass(self, w, req, shapes, [&](const ShapeRec& rec, uint32_t t) {
        if (t & 0x100) return centreNear(w, rec);
        if (t & 1) return sphereInBox(apply(w.inv, rec.centre - w.centre), w.half, rec.radius);
        if (t & 2) {
            BoxCast b;
            b.ext = w.half;
            gte().setRot(w.inv);
            b.p = rtv(rec.capStart - w.centre), b.d = rtv(rec.capAxis), b.radius = s16(rec.capRadius);
            return boxCast(b, w.scratch.boxCastAxis);
        }
        if (!(t & 8)) return false;
        // A corner of either box inside the other.
        gte().setRot(rec.inv);
        const SVec off = w.centre - rec.centre;
        for (int k = 0; k < 8; ++k)
            if (insideBox(rtv(w.corner(k) + off), rec.half)) return true;
        gte().setRot(w.inv);
        const SVec off2 = rec.centre - w.centre;
        for (int k = 0; k < 8; ++k) {
            SVec q = k & 1 ? rec.axis(0) : neg(rec.axis(0));
            q = k & 2 ? q + rec.axis(1) : q - rec.axis(1);
            q = k & 4 ? q + rec.axis(2) : q - rec.axis(2);
            if (insideBox(rtv(q + off2), w.half)) return true;
        }
        return false;
    });
}

void hoverProbes(Body& self, const Sweep& w, ShapeRec* const* shapes) {
    gte().setRot(w.rot);
    for (int side = 0; side < 4; ++side) {
        // Three points 300 beyond one side of the box (its two corners there and the middle), from the end.
        SVec v{};
        const int a = side & 2, b = a != 2 ? 2 : 0;
        v[a] = side & 1 ? s16(w.half[a] + 300) : s16(-300 - w.half[a]);
        std::array<SVec, 3> p;
        p[0] = rtv(v);
        v[b] = s16(-2 - w.half[b]), p[1] = rtv(v);
        v[b] = s16(w.half[b] + 2), p[2] = rtv(v);
        for (SVec& q : p) q += w.end;
        const uint16_t bit = std::array<uint16_t, 4>{1, 2, 8, 4}[size_t(side)];
        for (int i = 0; i < w.shapeCount; ++i) {
            const ShapeRec& rec = *shapes[i];
            if ((rec.type & 0x400) || !(rec.owner->flags10 & 0x10000) || !(rec.type & 8)) continue;
            gte().setLight(rec.inv);
            int j = 0;
            while (j < 3 && !insideBox(llv(p[size_t(j)] - rec.centre), rec.half)) ++j;
            if (j < 3) {
                self.hover &= uint16_t(~bit);
                break;
            }
        }
    }
}

void detach(Body& e) {
    // The exe also takes the entity's carried weight (+0x42) off its parent chain and unlinks it from the parent's
    // child list (+0x8c / +0x94); the bodies carry neither (TbActor keeps them), so only the links go.
    e.parent = nullptr, e.coordRec = nullptr;
}

namespace {
// Polygon k's plane distance of q (Q14 >> 14) and whether q lies inside its edges by `slack` (the & 0xffffc000 test).
int32_t planeD(const Poly& p, SVec q) { return (p.n.y * q.y + p.n.z * q.z + p.n.x * q.x + p.d) >> 14; }
bool insideEdges(const Poly& p, const int32_t* edgeD, SVec q, int32_t slack) {
    for (int e = 0; e < p.edgeCount; ++e) {
        const SVec en = p.edge(e).n;
        if (int32_t(uint32_t(en.x * q.x + en.y * q.y + en.z * q.z + edgeD[e]) & 0xffffc000u) > slack) return false;
    }
    return true;
}
} // namespace

void ledgeGrab(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world, PolyGather& g) {
    auto noLedge = [&] { self.f20 = (self.f20 & ~2u) | 0x20, self.f24 &= ~0x300u; };
    Mat m;
    rotMatrixXYZ(m, 0, self.angles.y, 0);
    gte().setRot(m);
    const int hand = self.id == 2 ? 0 : 1;  // Taz / Bugs
    const int16_t lift = self.id == 2 ? 0x172 : 0x159;
    SVec grab = g.grab[size_t(hand)].v;
    if (!(w.mode & 0x10)) grab = {};
    else grab += self.carry;
    g.grab[size_t(hand)].v = grab;
    const SVec handOff = rtv({0, s16(w.radius - self.boxMin.y - lift), s16(w.half.x + 0x3c)});
    const int16_t reach = s16(w.half.x + 10);
    const int16_t up = s16(self.id == 3 ? w.half.y + 0x82 : w.half.y + 0x1e);
    w.end = w.end - grab;
    const SVec handP = handOff + w.end;
    const int16_t mid = s16(((self.boxMax.z - self.boxMin.z) >> 1) + self.boxMin.z);
    const SVec centreOff = rtv({0, 0, mid}), base = w.end - centreOff, end = w.end;
    const SVec upRay{0, s16(-0x32 - w.radius), 0};
    const int16_t n = w.polyCount;
    auto poly = [&](int i) -> const Poly& { return *slots[i].poly; };
    auto edgeD = [&](int i) { return slots[i].edgeD; };
    // Something above the end (a downward-facing polygon within the reach of a ray up from it): no ledge.
    for (int i = 0; i < n; ++i) {
        if (!slots[i].poly || poly(i).n.y <= 11999) continue;
        const Poly& p = poly(i);
        const int32_t a = planeD(p, end), b = -(upRay.z * p.n.z + upRay.y * p.n.y + upRay.x * p.n.x) >> 14;
        if (a < 0 || a > b) continue;
        const SVec q{s16(upRay.x * a / b + end.x), s16(upRay.y * a / b + end.y), s16(upRay.z * a / b + end.z)};
        int e = 0;
        while (e < p.edgeCount && p.edge(e).n.y * q.y + p.edge(e).n.x * q.x + p.edge(e).n.z * q.z + edgeD(i)[e] <= 0x8000) ++e;
        if (e == p.edgeCount) return noLedge();
    }
    // The nearest edge of a grabbable floor (surface 1) at hand height that the end is outside of, not covered by
    // another such floor just beyond it.
    bool found = false;
    int32_t bestD = INT32_MAX;
    SVec ledge;
    for (int i = 0; i < n; ++i) {
        if (!slots[i].poly || poly(i).n.y >= -11999) continue;
        const Poly& p = poly(i);
        if (!(world.surfaceFlags(p) & 1)) continue;
        if (const int16_t h = s16(planeD(p, handP)); h <= -0x29 || h >= 0x29) continue;
        const int32_t* ed = edgeD(i);
        for (int j = 0; j < p.edgeCount; ++j) {
            const SVec en = p.edge(j).n;
            if (reach < (handP.z * en.z + handP.x * en.x + ed[j] + handP.y * en.y) >> 14) break;
            if (end.x * en.x + end.z * en.z + ed[j] + end.y * en.y < 0) continue;
            const SVec a = world.vertex(p, j), b = world.vertex(p, j + 1 == p.edgeCount ? 0 : j + 1), ev = b - a;
            const SVec s{s16(base.x - a.x), s16(base.y - a.y - up), s16(base.z - a.z)};
            int32_t f = s.y * ev.y + s.z * ev.z + s.x * ev.x, ee = ev.x * ev.x + ev.y * ev.y + ev.z * ev.z;
            SVec np;
            if (f < 0) np = a;
            else if (ee < f) np = b;
            else {
                for (; f > 200000; f >>= 1) ee >>= 1;
                np = {s16(ev.x * f / ee + a.x), s16(ev.y * f / ee + a.y), s16(ev.z * f / ee + a.z)};
            }
            const SVec d{s16(base.x - np.x), s16(base.y - np.y - up), s16(base.z - np.z)};
            const int32_t dd = d.z * d.z + d.x * d.x;
            if (dd > bestD) continue;
            const SVec q{s16((en.x * 3 >> 14) + np.x), s16((en.y * 3 >> 14) + np.y), s16((en.z * 3 >> 14) + np.z)};
            int k = 0;
            for (; k < n; ++k) {
                if (k == i || !slots[k].poly || poly(k).n.y >= -11999) continue;
                const Poly& o = poly(k);
                if (!(world.surfaceFlags(o) & 1)) continue;
                if (const int32_t h = planeD(o, q); h <= -0x29 || h >= 0x29) continue;
                if (insideEdges(o, edgeD(k), q, 0)) break;
            }
            if (k == n) ledge = np, bestD = dd, found = true;
        }
    }
    if (const int32_t r = w.half.x + 0x3c + mid; r * r < bestD) found = false;
    if (found) {  // hang from the ledge point, facing it
        const SVec d{s16(base.x - ledge.x), 0, s16(base.z - ledge.z)};
        const int32_t l = root(d.z * d.z + d.x * d.x), R = w.half.x + 10 + mid;
        w.end = {s16(d.x * R / l + ledge.x + centreOff.x), s16(self.boxMin.y + ledge.y - w.radius + lift),
                 s16(centreOff.z + d.z * R / l + ledge.z)};
        self.angles.y = s16(angle(-d.x, -d.z));
    } else
        // A ledge box (flags1 0x100000): a horizontal face at hand height with the end off one side, the hand over it.
        for (int i = 0; i < w.shapeCount; ++i) {  // every one: a later grab overrides an earlier
            const ShapeRec& rec = *shapes[i];
            if ((rec.type & 0x88) != 8 || !(rec.owner->flags1 & 0x100000)) continue;
            SVec P = apply(rec.inv, handP - rec.centre), E = apply(rec.inv, w.end - rec.centre);
            for (int a = 0; a < 3; ++a) {  // with P / E as the grabs before left them
                const int16_t fy = reinterpret_cast<const SVec4*>(&rec.capRadius)[a].v.y;
                if (absw(fy) <= 11999) continue;
                const int32_t top = P[a] + (fy < 1 ? -rec.half[a] : rec.half[a]);
                if (top <= -0x29 || top >= 0x29) continue;
                for (int b = 0; b < 3; ++b) {
                    if (b == a || !(rec.half[b] <= absw(E[b]) && absw(P[b]) <= rec.half[b])) continue;
                    int c = 0;
                    while (c < 3 && (c == b || c == a || absw(E[c]) <= rec.half[c] - 0x1e)) ++c;
                    if (c < 3) continue;
                    const int nb = b + 1 > 2 ? 0 : b + 1;
                    if (absw(P[nb]) <= reach + rec.half[nb] && absw(E[nb]) <= rec.half[nb]) {
                        E[a] = s16(E[a] - top);
                        P = {};
                        if (E[b] < 1) E[b] = s16(-(rec.half[b] + reach)), P[b] = -0x1000;
                        else E[b] = s16(rec.half[b] + reach), P[b] = 0x1000;
                        const int16_t t2 = s16(self.boxMin.y - w.radius + lift);
                        E[a] = fy < 0 ? s16(rec.half[a] - t2) : s16(t2 - rec.half[a]);
                        found = true;
                        P = apply(rec.rot, P);
                        self.angles.y = s16(angle(-P.x, -P.z));
                        w.end = apply(rec.rot, E) + rec.centre;
                        req.floor = rec.owner;
                    }
                    break;
                }
            }
        }
    w.end += grab;
    if (!found) return noLedge();
    if (!(self.f24 & 0x200)) self.f24 |= 0x300, self.f20 &= 0xfffffbdeu;
    w.motion = {};
    w.tmp = w.end - w.start;
    if (lenSq(w.tmp) < 10) w.end = w.start;
}

void ledgeProbes(Body& self, Sweep& w, const PolySlot* slots, const PolyWorld& world, const SVec4* probes) {
    Mat m;
    rotMatrixXYZ(m, 0, self.angles.y, 0);
    gte().setRot(m);
    const SVec fwd = rtv({0, 0, -0x50});
    std::array<SVec, 6> p;
    for (size_t i = 0; i < 6; ++i) p[i] = rtv(probes[i].v) + w.end;
    std::array<SVec, 2> hit;
    std::array<bool, 2> got{};
    uint16_t bits = 0;
    for (int i = 0; i < w.polyCount; ++i) {
        if (!slots[i].poly) continue;
        const Poly& poly = *slots[i].poly;
        const int32_t* ed = slots[i].edgeD;
        const SVec n = poly.n;
        // A climbable wall (surface 8) the end is in front of.
        if (n.y >= 1000 || n.y <= -1000 || !(world.surfaceFlags(poly) & 8)) continue;
        if (int32_t(uint32_t(w.end.y * n.y + w.end.z * n.z + n.x * w.end.x + poly.d) & 0xffffc000u) <= 0) continue;
        for (size_t j = 0; j < 6; ++j) {
            if (j < 2 && got[j]) continue;
            const SVec q = p[j];
            if (s16(planeD(poly, q)) >= 1) continue;  // the probe in front of the wall
            // Along the facing direction onto the wall plane, inside its edges.
            const int32_t a = q.y * n.y + q.x * n.x + q.z * n.z + poly.d, b = fwd.x * n.x + fwd.y * n.y + fwd.z * n.z;
            if (!((b * -3 == a || -a < b * 3) && b != 0)) continue;
            const SVec on = q + SVec{s16(fwd.x * -a / b), s16(fwd.y * -a / b), s16(fwd.z * -a / b)};
            if (!insideEdges(poly, ed, on, 0x14000)) continue;
            if (j < 2) hit[j] = on, got[j] = true;
            else bits = uint16_t(bits | 1u << (j - 2));
        }
    }
    self.climb = bits;
    if (!(got[0] && got[1])) {
        self.f20 = (self.f20 & ~2u) | 0x20, self.f24 &= 0xf3ffffffu;
        return;
    }
    // Both hands on the wall: the end between them (out by the facing, down 0x82), turned to face it.
    SVec c{s16((hit[1].x + hit[0].x) >> 1), s16((hit[1].y + hit[0].y) >> 1), s16((hit[0].z + hit[1].z) >> 1)};
    c += fwd;
    c.y = s16(c.y + 0x82);
    if (lenSq(c - w.end) > 9) w.end = c;
    self.angles.y = s16(angle(s16(hit[1].x - hit[0].x), s16(hit[1].z - hit[0].z)) - 0x400);
    if (!(self.f24 & 0x8000000)) self.f24 |= 0xc000000, self.f20 &= 0xfffffbdeu;
    w.motion = {};
}

void respond(Body& e, Sweep& w, MoveRequest& req, MoverWorld& m) {
    const PolyWorld world{m.h};
    const PolySlot* slots = m.gather.slots.data();
    ShapeRec* const* shapes = m.cache.list.data();
    Gte& g = gte();
    int16_t code = 1, passes = 0, kept = 0;
    std::array<SVec, 8> normals;  // the creases slid along this move and their entities
    std::array<Body*, 8> owners;
    SVec carried{};                // what entities moving into the mover added to its motion
    w.lastNormal = {};
    auto bump = [&](Body* o) {
        if (o) o->f28 |= 4, e.f28 |= 2;
    };
    auto r12 = [](int32_t x) { return s16((x < 0 ? x + 0xfff : x) >> 12); };
    auto dotT = [&](SVec v) { return v.x * req.target.x + v.y * req.target.y + v.z * req.target.z; };
    while (w.left != 0) {
        ++passes;
        w.t = w.left, w.best = 0x7fffffff, w.passPush = {}, w.wasPushed = 0;
        if (w.mode & 8) code = pointPass(e, w, req, slots, shapes, world);
        else if (w.mode & 1) code = capsulePass(e, w, req, slots, shapes, world);
        else if (w.mode & 2) code = spherePass(e, w, req, slots, shapes, world);
        else if (w.mode & 4) code = boxPass(e, w, req, slots, shapes, world, m.boxPoly);
        if (code != 0) {
            w.centre += w.contact;
            g.loadIr(w.normal), g.sqr(), w.tmpInt = g.mac;
            const int32_t l = root(w.tmpInt.z + w.tmpInt.y + w.tmpInt.x);
            w.normal = {s16((w.normal.x << 14) / l), s16((w.normal.y << 14) / l), s16((w.normal.z << 14) / l)};
            const uint32_t f20 = e.f20;
            e.f20 = f20 & ~4u;
            if (code == 1 && w.normal.y > -11000) {  // against a wall
                e.f20 |= 4;
                if (!(f20 & 4) && (e.progMask[0] & 4)) req.flags64 |= 4;
            }
            Body* o = code == 2 ? w.hitOwner : nullptr;
            SVec into{}, v = w.normal;
            w.tmp = w.normal;
            g.setLightRow2(w.normal);
            if (o) {  // an entity moving into the mover adds its velocity (up to the part along the motion)
                v = {s16(o->vel.x >> 4), s16(o->vel.y >> 4), s16(o->vel.z >> 4)};
                g.setLightRow0(v), g.loadV(1, w.motion), g.mvmva(false, Gte::kLight, Gte::kV1, false);
                int32_t a = g.mac.x;
                if (a > 0 && g.mac.z < 0) {
                    g.loadV(2, v), g.mvmva(false, Gte::kLight, Gte::kV2, false);
                    if (int32_t b = g.mac.x; a <= b) {
                        for (; a > 200000; a >>= 1) b >>= 1;
                        g.loadIr0((a << 12) / b), g.loadIr(v), g.gpf(0);
                        v = {r12(g.mac.x), r12(g.mac.y), r12(g.mac.z)};
                    }
                    w.motion = w.motion - v, carried += v;
                }
            }
            g.setLightRow0(w.motion), g.loadV(1, w.motion), g.mvmva(false, Gte::kLight, Gte::kV1, false);
            const int32_t speed2 = g.mac.x, inward = g.mac.z >> 14;  // |motion|², motion · normal
            if (inward < -0x14) {  // a hard impact: more than (5/8)^.5 of the motion into the surface
                const int32_t k = speed2 * 5 >> 3;
                if (k < inward * inward && (code == 1 || (o->flags10 & 0x80000))) e.f28 |= 0x8000, req.flags |= 0x8000;
            }
            auto alongN = [&](int16_t n) {
                int32_t x = n * inward;
                if (x >= 0) x += 0x3fff;
                return s16(x >> 14);
            };
            SVec rest;
            if (w.best < 100) {  // touching: slide, along the crease with an earlier contact when in a corner
                if (kept < 8) normals[size_t(kept)] = w.normal, owners[size_t(kept)] = o ? w.hitOwner : 0, ++kept;
                if (inward < 0) {
                    into = {alongN(w.normal.x), alongN(w.normal.y), alongN(w.normal.z)};
                    rest = w.motion - into;
                    for (int j = 0; j < kept - 1; ++j) {
                        w.tmp = normals[size_t(j)];
                        if (dot(rest, w.tmp) > 0 || dot(w.normal, w.tmp) >= 0) continue;
                        const V3 c = shr(cross(w.tmp, w.normal), 14);
                        int32_t cc = c.y * c.y + c.x * c.x + c.z * c.z;
                        if (cc == 0) continue;
                        if (absw((c.y << 14) / root(cc)) < 0x2f6c && w.motion.y > 0) {  // a steep crease: wedged
                            bump(owners[size_t(j)]);
                            if (o) bump(w.hitOwner);
                            e.f20 = (e.f20 & 0xfffffbdeu) | 2;
                            w.normal = {0, -0x4000, 0};
                        }
                        int32_t pr = rest.x * c.x + rest.y * c.y + rest.z * c.z;
                        cc = c.y * c.y + c.z * c.z + c.x * c.x;
                        if (cc == 0) continue;
                        if (pr < 0) pr = -pr, cc = -cc;
                        for (; pr > 32000; pr >>= 1) cc >>= 1;
                        rest = {s16(c.x * pr / cc), s16(c.y * pr / cc), s16(c.z * pr / cc)};
                        for (int k = 0; k < kept - 1; ++k) {  // a third contact closing the crease: stuck
                            const SVec nk = normals[size_t(k)];
                            if (k == j || dot(w.normal, nk) >= 0 || dot(normals[size_t(j)], nk) >= 0 || dot(rest, nk) >= 0) continue;
                            rest = {};
                            bump(owners[size_t(j)]), bump(owners[size_t(k)]);
                            if (o) bump(w.hitOwner);
                            e.f20 = (e.f20 & 0xfffffbdeu) | 2;
                            w.normal = {0, -0x4000, 0};
                            break;
                        }
                        break;
                    }
                    into = w.motion - rest;
                }
            } else {
                if (inward < 0) into = {alongN(w.normal.x), alongN(w.normal.y), alongN(w.normal.z)};
                kept = 0;
            }
            rest = w.motion;  // the motion before the response
            w.motion = w.motion - into;
            if (o) {
                if (!(o->marks & 0x40)) {
                    o->marks |= 0x40;
                    if (req.touchedCount < 15) req.touched[size_t(req.touchedCount++)] = w.hitOwner;
                }
                if (w.normal.y < -11000 && !(e.f28 & 2)) {
                    if (o->flags0 & 8) {
                        req.floor = w.hitOwner;
                        if (rest.y > 0xb4) e.f28 |= 1, req.flags |= 1;
                    }
                    if (o->flags10 & 1) req.floor = w.hitOwner;
                    if (rest.y > 0x14) o->f20 |= 0x10;
                }
            }
            if (w.normal.y < -11000) {  // on a floor
                if (rest.y > 0xb4) e.f28 |= 1, req.flags |= 1;
                if (w.floorSurf && !(e.f20 & 2)) e.groundSurf = w.floorSurf;
                if (o && (o->flags0 & 8)) e.f20 |= 8;
                else {
                    if (o) e.f28 |= 2, o->f28 |= 4;
                    e.f20 |= 2;
                }
                if (!w.landed) {  // the floor's friction, once a move
                    w.landed = 1;
                    int16_t f;
                    if (w.floorSurf) f = s16(int32_t(uint32_t(uint16_t(w.grip)) * w.floorSurf->friction) >> 12);
                    else f = o ? s16(int32_t(uint32_t(o->f3a) * uint16_t(w.grip)) >> 12) : w.grip;
                    f = s16(f + e.f40);
                    f = f > 0x1000 ? int16_t(0x1000) : f < 0 ? int16_t(0) : f;
                    w.tmp = rest;
                    if (!(e.flags10 & 0x800)) {
                        if (w.tmp.y > 0 && (w.tmp.y = s16(w.tmp.y - s16(((m.dt * 5 & 0x7fffffff) * f + 0xfff) >> 12))) < 0) w.tmp.y = 0;
                        // The motion without its part into the floor, eased by f towards the target's in-plane part.
                        g.setColorRow0(w.tmp);
                        v = narrow(req.target);
                        g.setColorRow2(v), g.loadV(2, w.normal), g.mvmva(false, Gte::kColor, Gte::kV2, false);
                        const int32_t k = g.mac.x >> 14, q = g.mac.z >> 14;
                        auto up14 = [&](int16_t n) {
                            int32_t x = n * k;
                            if (x > 0) x += 0x3fff;
                            return s16(x >> 14);
                        };
                        w.motion = w.tmp = w.tmp - SVec{up14(w.normal.x), up14(w.normal.y), up14(w.normal.z)};
                        w.tmp = {s16(int16_t(req.target.x) - s16(w.normal.x * q >> 14)), s16(int16_t(req.target.y) - s16(w.normal.y * q >> 14)),
                                 s16(int16_t(req.target.z) - s16(w.normal.z * q >> 14))};
                        v = w.tmp - w.motion;
                        auto up12 = [&](int16_t a) {
                            int32_t x = a * f;
                            if (x > 0) x += 0xfff;
                            return s16(x >> 12);
                        };
                        w.motion += SVec{up12(v.x), up12(v.y), up12(v.z)};
                    } else {  // ice (flags10 0x800): damped, nudged towards the target's direction
                        const int32_t k = (w.motion.z * w.normal.z + w.normal.y * w.motion.y + w.motion.x * w.normal.x) >> 14;
                        w.motion = w.motion - SVec{s16(w.normal.x * k >> 14), s16(w.normal.y * k >> 14), s16(w.normal.z * k >> 14)};
                        auto damp = [&](int16_t a) { return s16(a - s16((a * f + 0x800) >> 12)); };
                        w.motion = {damp(w.motion.x), damp(w.motion.y), damp(w.motion.z)};
                        const int32_t dd = dotT(w.motion);
                        int32_t mm = lenSq(w.motion);
                        mm += mm == 0;
                        v = {s16(int16_t(req.target.x) - s16(w.motion.x * dd / mm)), s16(int16_t(req.target.y) - s16(w.motion.y * dd / mm)),
                             s16(int16_t(req.target.z) - s16(w.motion.z * dd / mm))};
                        w.motion += SVec{s16((v.x * 0xdc + 0x800) >> 12), s16((v.y * 0xdc + 0x800) >> 12), s16((v.z * 0xdc + 0x800) >> 12)};
                    }
                    if (w.floorSurf && (w.floorSurf->flags & 0x40) &&
                        ((e.f20 & 0x420) || !(dotT(w.motion0) < 0 || dotT(w.motion) >= 0)))
                        req.flags |= 0x40, e.f28 |= 0x40;
                }
                e.f20 &= 0xfffffbdeu;
            } else {
                if (w.normal.y < -1000 && w.surf && (w.surf->flags & 0x40) &&
                    ((e.f20 & 0x420) || (dotT(w.motion0) >= 0 && dotT(w.motion) < 0)))
                    req.flags |= 0x40, e.f28 |= 0x40;
                if (w.normal.y > 12000 && rest.y < -0x1e) req.flags68 |= 0x800000, e.f24 |= 0x800000;  // a ceiling
            }
            w.tmp = into, w.lastNormal = w.normal;
            if ((w.mode & 0x80) && e.f38) {  // slide damping
                if (w.tmp.y > 0 && (w.tmp.y = s16(w.tmp.y - 10)) < 0) w.tmp.y = 0;
                const uint32_t a = uint32_t(w.tmp.x * int32_t(e.f38) + 0x800), b = uint32_t(w.tmp.y * int32_t(e.f38) + 0x800),
                               c = uint32_t(w.tmp.z * int32_t(e.f38) + 0x800);
                w.motion = w.motion - SVec{s16(int32_t(a) >> 12), s16(int32_t(b) >> 12), s16(int32_t(c) >> 12)};
                if (w.motion.y < 0) {
                    if (e.parent) detach(e);
                    e.f20 &= ~0xau, req.flags64 &= ~0xau, req.floor = 0;
                }
                if (int32_t(a | b | c) >> 12) e.f20 |= 0x40000000, req.flags64 |= 0x40000000;
            }
            if ((e.flags10 & 0x80) && w.normal.y > -11000 && rest.z * w.normal.z + rest.x * w.normal.x < 0)
                e.f28 |= 0x400, req.flags |= 0x400, w.motion = {};
            if ((w.mode & 0x40) && w.normal.y > -11000 && w.normal.y < 0 && req.target.y >= 0 && rest.z * w.normal.z + rest.x * w.normal.x < 0)
                w.motion = {0, s16(s16(m.dt) * 5 + w.motion0.y), 0};
            if ((e.flags10 & 0x800000) && dot(rest, w.normal) < 0) e.f28 |= 0x400, req.flags |= 0x400, w.motion = {};
            w.left = s16(w.left - w.t);
            auto part = [&](int16_t a) {
                int32_t x = a * w.left;
                if (x > 0) x += 0xfff;
                return s16(x >> 12);
            };
            w.step = {part(w.motion.x), part(w.motion.y), part(w.motion.z)};
            w.end = w.centre + w.step;
        }
        if ((w.step.z == 0 && w.step.y == 0 && w.step.x == 0) || passes > 10 || code == 0) break;
    }
    w.end = w.centre + w.step;
    w.motion += carried;
}

void move(Body& e, MoveRequest& req, const MoveState* st, Sweep& w, MoverWorld& m) {
    // The exe keeps the work block's first 0xd8 bytes across the call (0x4d83c0).
    std::array<uint8_t, 0xd8> kept;
    std::memcpy(kept.data(), &w, kept.size());
    const PolyWorld world{m.h};
    const PolySlot* slots = m.gather.slots.data();
    ShapeRec* const* shapes = m.cache.list.data();
    uint32_t fA = 0, fB = 0, fC = 0;
    if (!st) w.grip = 0x1000;
    else {
        fA = st->flagsA, fC = st->flagsC, fB = st->flagsB;
        uint16_t gr = uint16_t(st->grip);
        if (int16_t(gr) > 0 && (gr & 0x4000)) gr = uint16_t(m.S[uint32_t(int32_t(int16_t(gr))) & 0xffff9fffu] << 5);  // an S variable
        w.grip = int16_t(gr);
    }
    req.touchedCount = 0, req.floor = 0, req.flags = 0, req.flags68 = 0, req.flags64 = 0;
    w.landed = 0, w.floorPoly = 0;
    w.mode = e.flags0 & 0x10 ? 2 : e.flags0 & 1 ? 1 : e.flags0 & 0x40000 ? 4 : 10;
    if (e.flags0 & 0x8000000) w.mode |= 8;
    if ((e.flags10 & 0x2000) || (fB & 0x40000000)) w.mode |= 0x20;
    if (e.flags1 & 0x2000) w.mode |= 0x200;
    if (fA & 0x8000) w.mode |= 0x40;
    if (fA & 0x10000000) w.mode |= 0x80;
    addTo(req.target, e.carry), addTo(req.target, req.follow);
    w.self = &e, w.left = 0x1000;
    w.motion = {s16(e.vel.x >> 4), s16(e.vel.y >> 4), s16(e.vel.z >> 4)};
    if (req.follow.y > 0 && w.motion.y < req.follow.y) w.motion.y = req.follow.y;
    w.motion0 = w.motion;
    w.surf = 0, w.floorSurf = 0;
    if (!(e.f24 & 0x200) && !(e.f20 & 0x200) && !(fA & 0xa2008601u) && !(fC & 1)) w.motion.y = s16(w.motion.y + s16(m.dt) * 5);  // gravity
    w.rot.m = Mat{}.m, w.rot.t = {};
    mulRotX(w.rot, e.angles.x), mulRotZ(w.rot, e.angles.z), mulRotY(w.rot, e.angles.y);
    w.inv = transposed(w.rot);
    // The shape: the half extents of the entity's box; capsule (flags0 1) along its longest axis, sphere, box.
    w.half = {s16((e.boxMax.x - e.boxMin.x + 1) >> 1), s16((e.boxMin.y - e.boxMax.y + 1) >> 1), s16((e.boxMax.z - e.boxMin.z + 1) >> 1)};
    const SVec h = w.half;
    int ax = h.x <= h.y ? 1 : 0;
    w.radius = std::max(h.x, h.y), w.radius2 = std::min(h.x, h.y);
    if (w.radius < h.z) w.radius = h.z, ax = 2;
    if (h.z < w.radius2) w.radius2 = h.z;
    if (!h.x || !h.y || !h.z) w.mode |= 0x100;
    for (int k = 0; k < 3; ++k) w.half[k] = std::max<int16_t>(w.half[k], 2);
    if (w.radius < 10 && (!(w.mode & 8) || (e.flags0 & 0x100))) w.mode = (w.mode & ~5) | 2, w.radius = 10;
    int16_t other = 0;
    if (!(w.mode & 1)) {
        if (w.mode & 2) w.radius2 = w.radius;
    } else {
        w.tmp = {};
        for (int k = 0; k < 3; ++k)
            if (k != ax && other < w.half[k]) other = w.half[k];
        w.tmp[ax] = s16(w.radius - other);
        if (w.radius - other < 0x15) w.mode = (w.mode & ~1) | 2;
        w.radius2 = other;
        w.tmp = apply(w.rot, w.tmp);
        w.capA = w.tmp, w.capB = neg(w.tmp);
    }
    if (w.radius2 < 8) w.radius2 = 8;
    w.centre = {s16(e.boxMin.x + w.half.x), s16(e.boxMin.y - w.half.y), s16(e.boxMin.z + w.half.z)};
    if (w.mode & 1) w.half = {other, s16(w.radius - other), other};
    w.centre = apply(w.rot, w.centre);
    if (w.mode & 8) w.startOffset = {s16(-w.centre.x), s16(-8 - w.centre.y), s16(-w.centre.z)};
    w.centre += e.pos;
    if (w.mode & 4) {  // the box's bounding radius, scaled axes and corners
        Gte& g = gte();
        g.loadIr(w.half), g.sqr(), w.tmpInt = g.mac;
        w.radius = s16(root(w.tmpInt.z + w.tmpInt.y + w.tmpInt.x) + 1);
        g.setRot(w.rot);
        for (int i = 0; i < 3; ++i) w.tmp = {}, w.tmp[i] = w.half[i], w.axes[size_t(i)].v = rtv(w.tmp);
        for (int k = 0; k < 8; ++k) {
            const SVec a0 = w.axes[0].v, a1 = w.axes[1].v, a2 = w.axes[2].v;
            SVec c = k & 1 ? a0 : neg(a0);
            c = k & 2 ? c + a1 : c - a1;
            w.corner(k) = k & 4 ? c + a2 : c - a2;
        }
    }
    if (const uint32_t f20 = e.f20; !(f20 & 1)) {
        if (w.motion.y > 0x32 && !(f20 & 0x200)) e.f20 = f20 | 0x20;
    } else e.f20 = w.motion.y < 0 ? f20 & ~0x400u : f20 | 0x400;
    // The pull towards the target velocity: each axis by 2^shift / 0x1000 of the difference.
    SVec sh{};
    if (((fB & 0x80000000) && (e.f20 & 1)) || (fA & 0x80000000)) sh.z = 10, sh.x = 10;
    if (fA & 0x80000000) sh.y = 0xb;
    if ((fB & 0x20000) || (fA & 0x2000400)) {
        sh = {0xc, 0xc, 0xc};
        if ((fB & 0x400000) && e.attached) w.mode |= 0x10;
    }
    if (e.f20 & 0x80000) sh = {10, 9, 10};
    else if (e.f20 & 0x4000) sh = {10, 10, 10};
    else if (fC & 1) sh = {9, 9, 9}, w.landed = 1;
    else if (fC & 8) sh = {8, 8, 8}, w.landed = 1;
    else if (fA & 0xa00) sh = {0xb, 0xb, 0xb};
    else if (fA & 0x20000000) sh = {8, 8, 8};
    auto pull = [](int32_t target, int16_t vel, int16_t s) {
        int32_t v = (target - (vel >> 4)) << (s & 0x1f);
        if (v > 0) v += 0xfff;
        return s16(v >> 12);
    };
    if (sh.x) w.motion.x = s16(w.motion.x + pull(req.target.x, e.vel.x, sh.x));
    if (sh.y) w.motion.y = s16(w.motion.y + pull(req.target.y, e.vel.y, sh.y));
    if (sh.z) w.motion.z = s16(w.motion.z + pull(req.target.z, e.vel.z, sh.z));
    e.f20 &= 0xfffffff9u, e.f28 &= ~2u;
    w.start = w.centre, w.step = w.motion, w.end = w.centre + w.step;
    int32_t reach = root(w.motion.x * w.motion.x + w.motion.y * w.motion.y + w.motion.z * w.motion.z) + 0x32 + e.radius;
    w.shapeCount = e.flags0 & 0x100 ? listShapes(w.centre, &e, s16((e.flags10 & 0x10000 ? 300 : 0) + reach), m.contacts, m.views, m.cache) : 0;
    auto disp = [&] { req.disp = {w.end.x - w.start.x, w.end.y - w.start.y, w.end.z - w.start.z}; };
    if (!(w.mode & 0x20)) {
        w.tmp = w.centre;
        if (w.mode & 8) w.tmp += w.startOffset, reach -= e.radius;
        w.polyCount = gatherPolys(m.h, w.tmp, s16(reach), &e, m.gather);
        w.push = {}, w.pushed = 0;
        if (w.polyCount || w.shapeCount) respond(e, w, req, m);
        w.centre = w.end;
        disp();
        if (((fB & 0x2000000) && (e.f20 & 0x420)) || (fA & 0x2000000)) ledgeGrab(e, w, req, slots, shapes, world, m.gather);
        if ((!(fB & 0x1000000) || !(e.f20 & 0x420)) && !(fA & 0x400)) e.f24 &= 0xf3ffffffu;
        else ledgeProbes(e, w, slots, world, m.probes);
        e.vel = {s16(w.motion.x << 4), s16(w.motion.y << 4), s16(w.motion.z << 4)};
        disp();
    } else {  // no sweep: straight to the target
        req.disp = req.target;
        addTo(w.centre, req.disp);
    }
    if ((e.flags0 & 0x100) && !(w.mode & 0x100)) {
        if (e.flags0 & 0x400) overlapFlat(e, w, req, shapes);
        else if (w.mode & 2) overlapSphere(e, w, req, shapes);
        else if (w.mode & 1) overlapCapsule(e, w, req, shapes);
        else if (w.mode & 4) overlapBox(e, w, req, shapes);
    }
    if ((e.flags10 & 0x4000) && !(w.mode & 0x100)) m.fxTouch(e, w);  // 0x447ee0
    if (e.flags10 & 0x10000) hoverProbes(e, w, shapes);
    if (e.vel == SVec{} && req.disp == V3{} && ((e.f20 & 10) || (fA & 0x80000001u))) {  // at rest
        if (!(e.f20 & 0x100)) e.f20 |= 0x100, req.flags64 |= 0x100;
    } else e.f20 &= ~0x100u;
    if (fA & 1) e.vel = {};
    for (int i = 0; i < req.touchedCount; ++i) req.touched[size_t(i)]->marks &= 0xffbf;
    e.carry = {};
    if (fC & 0x20) {  // the horizontal speed cap, approached by 0xa0 a move
        const int32_t h2 = e.vel.z * e.vel.z + e.vel.x * e.vel.x, cap = st->cap * 0x10;
        if (cap * cap < h2) {
            const int32_t l = root(h2), v = std::max(l - 0xa0, cap);
            e.vel.x = s16(e.vel.x * v / l), e.vel.z = s16(e.vel.z * v / l);
        }
    }
    req.normal = w.lastNormal;
    e.groundPoly = w.floorPoly;
    std::memcpy(static_cast<void*>(&w), kept.data(), kept.size());
}

} // namespace bb::game::tbm
