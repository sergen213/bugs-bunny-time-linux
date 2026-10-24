// TB effects (bblit2.exe): the particle emitters 0x4d6ae0 (fields 0x452060 from logic S 118, emission 0x452ab0 from
// V 0x80200000, update + draw 0x453350 / 0x452e80 once per tick after the entities, slot clear 0x4529d0 from S 138,
// actor touch 0x447ee0) over the shared pool of 120 particles, the weather streaks 0x451370 (S 110 0x432bb0), and
// the model / rig swaps 0x469320 (V 0x80020000, distance LOD of 0x45eee0). Docs: GAMEPLAY_TB.md §3.1 (S 118),
// RUNTIME_REFERENCE.md "TB effects".
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cmath>

namespace bb::game {
namespace {

constexpr int16_t sat16(int32_t v) { return int16_t(std::clamp(v, -0x8000, 0x7fff)); }
std::array<int16_t, 3> v16(const V3& v) { return {sat16(v.x), sat16(v.y), sat16(v.z)}; }
V3 wide(const std::array<int16_t, 3>& v) { return {v[0], v[1], v[2]}; }

// The emitter direction matrix +0x6c: 0x451ca0 of the slot's direction +0x08, which no field writes (0x451e40
// default (0, 0x1000, 0)): rows (0x1000, 0, 0), (0, -0x1000, 0), (0, 0, -0x1000).
const Mat kDirMatrix{{4096, 0, 0, 0, -4096, 0, 0, 0, -4096}, {}};

V3 xform(const Mat& M, const V3& v) {  // world point of a coordinate-local one (GsGetLw matrix + translation)
    const V3 r = applyLV(M, v);
    return {r.x + M.t.x, r.y + M.t.y, r.z + M.t.z};
}

// View basis of the camera eye -> look (as viewList): z forward, x right, y down (PS1 axes).
struct View {
    double e[3], f[3], r[3], d[3];
    explicit View(const TbCamera& c) {
        e[0] = c.eye.x, e[1] = c.eye.y, e[2] = c.eye.z;
        f[0] = c.look.x - c.eye.x, f[1] = c.look.y - c.eye.y, f[2] = c.look.z - c.eye.z;
        const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl > 0) f[0] /= fl, f[1] /= fl, f[2] /= fl; else f[0] = f[1] = 0, f[2] = 1;
        r[0] = f[2], r[1] = 0, r[2] = -f[0];
        const double rl = std::max(1e-9, std::hypot(r[0], r[2]));
        r[0] /= rl, r[2] /= rl;
        d[0] = f[1] * r[2] - f[2] * r[1], d[1] = f[2] * r[0] - f[0] * r[2], d[2] = f[0] * r[1] - f[1] * r[0];
    }
    double z(const V3& p) const { return (p.x - e[0]) * f[0] + (p.y - e[1]) * f[1] + (p.z - e[2]) * f[2]; }
};

}  // namespace

// 0x451e40 rates of one keyed curve: (v1 - v0) / key age, (v2 - v1) / (life - key age); 0 when a span is empty.
void TbWorld::fxRates(FxSlot& s, int which) const {
    for (int k = 0; k < 3; ++k) {
        if (which >= 0 && which != k) continue;
        FxSlot::Curve& c = k == 0 ? s.ang : k == 1 ? s.size : s.bright;
        c.r0 = c.at == 0 ? 0 : int16_t((c.v[1] - c.v[0]) / c.at);
        c.r1 = int32_t(s.life) == c.at ? 0 : int16_t((c.v[2] - c.v[1]) / (int32_t(s.life) - c.at));
    }
}

// The root coordinate of a hung entity (0x405ab0 walks its super chain): the parent's part (+0xe8) or record, then its
// record +0x12c at the local position +0x13c turned by +0x136. A grab hangs it there at once, before its own tick
// moves +0xfc (VICASG04: 131 grabs Bugs, whose program lets his snowball go through that chain; oracle w13 J).
bool TbWorld::hungRoot(const TbActor& a, Mat& M) const {
    const TbActor* par = a.parent >= 0 ? entity(a.parent) : nullptr;
    if (!par) return false;
    const Mat pm = a.parentPart >= 0 ? coordMat(par, a.parentPart) : recordMat(*par);
    for (size_t c = 0; c < 3; ++c) {
        const V3 col = apply(pm, rotY(a.localYaw, c == 0 ? 4096 : 0, c == 1 ? 4096 : 0, c == 2 ? 4096 : 0));
        M.m[c] = int16_t(col.x), M.m[3 + c] = int16_t(col.y), M.m[6 + c] = int16_t(col.z);
    }
    const V3 o = apply(pm, a.local);
    M.t = {pm.t.x + o.x, pm.t.y + o.y, pm.t.z + o.z};
    return true;
}

// 0x405ab0: the entity's root (RotMatrix pitch / yaw / roll, 0x404160; hung: hungRoot) or its part chain under it; no
// entity (an unbound slot) reads the world origin.
Mat TbWorld::coordMat(const TbActor* a, int32_t part) const {
    Mat M;
    if (!a) return M;
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const std::vector<TodPart>& parts = a->anim.parts;
    for (int32_t i = part, guard = 0; i >= 0 && size_t(i) < parts.size() && guard < 64; ++guard) {
        const TodPart& p = parts[size_t(i)];
        double n[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                n[r * 3 + c] = p.m[size_t(r * 3)] * m[c] + p.m[size_t(r * 3 + 1)] * m[3 + c] + p.m[size_t(r * 3 + 2)] * m[6 + c];
        std::copy_n(n, 9, m);
        i = p.parent;
    }
    Mat R;  // the root coordinate: its rotation scaled by +0xec (as 0x44e5d0; VICASG04 131's 1.25, oracle w13 J)
    if (!hungRoot(*a, R)) {
        rotMatrixXYZ(R, a->pitch, a->yaw, a->roll);
        for (size_t i = 0; i < 9; ++i) R.m[i] = int16_t((int32_t(R.m[i]) * a->scale[i % 3]) >> 12);
        R.t = a->pos;
    }
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += R.m[size_t(r * 3 + k)] / 4096.0 * m[k * 3 + c];
            M.m[size_t(r * 3 + c)] = int16_t(std::lround(v * 4096.0));
        }
    }
    const V3 tr = apply(R, partLocal(*a, part));  // the origin as 0x403400 per coordinate (s16 steps)
    M.t = {R.t.x + tr.x, R.t.y + tr.y, R.t.z + tr.z};
    return M;
}

// S 118 (0x432c80): field X & 0xff of the slot this entity owns (0x451e10: the slot bound to it by field 7, else 0)
// = Y; X bits 0x100 / 0x200 / 0x400 (/ 0x800) pick the components of the vector fields.
void TbWorld::fxField(TbActor& a, uint16_t x, uint16_t y) {
    const auto own = std::ranges::find(fx_, int32_t(a.uid), &FxSlot::owner);
    FxSlot& s = own == fx_.end() ? fx_[0] : *own;
    const int16_t v = int16_t(y);
    const uint8_t b = uint8_t(y);
    auto vec = [&](std::array<int16_t, 3>& d) {
        for (int k = 0; k < 3; ++k)
            if (x & (0x100 << k)) d[size_t(k)] = v;
    };
    auto rgb = [&](std::array<uint8_t, 3>& d) {
        for (int k = 0; k < 3; ++k)
            if (x & (0x100 << k)) d[size_t(k)] = b;
    };
    auto curve = [&](FxSlot::Curve& c, int k) {
        vec(c.v);
        if (x & 0x800) c.at = int8_t(b);
        fxRates(s, k);
    };
    switch (x & 0xff) {
    case 1: rgb(s.rgb1); break;
    case 2: rgb(s.rgb2); break;
    case 3: s.life = b, fxRates(s, -1); break;
    case 4: s.count = b; break;
    case 5: s.speed = v; break;
    case 6: s.spread = v; break;
    case 7:  // bind slot Y to this entity (unbinding any other slot it owned)
        for (FxSlot& o : fx_) if (o.owner == int32_t(a.uid)) o.owner = -1;
        if (v >= 0 && size_t(v) < fx_.size()) fx_[size_t(v)].owner = int32_t(a.uid);
        break;
    case 8: s.delay = uint8_t(b + (b == 0)); break;
    case 9:  // coordinate: the attach part (+0x84) for Y = 0, else the entity's own
        s.coordUid = int32_t(a.uid);
        s.coordPart = a.attachPart >= 0 && v == 0 ? a.attachPart : -1;
        break;
    case 10: s.flags = y; break;
    case 0xb: {  // sprite: entity Y's first class-0x17 link (0x451d70); none -> gouraud lines
        const auto it = fxSprites_.find(y);
        if (it == fxSprites_.end() || it->second.asset < 0) s.flags = uint16_t((s.flags & 0xffcf) | 0x40);
        else s.sprite = it->second.asset, s.frames = it->second.frames;
        break;
    }
    case 0xc: s.drag = v; break;
    case 0xd: curve(s.ang, 0); break;
    case 0xe: curve(s.size, 1); break;
    case 0xf: curve(s.bright, 2); break;
    case 0x10: vec(s.accel); break;
    case 0x11: s.sizeSpread = v; break;
    case 0x13: s.f92 = v; break;
    case 0x14: vec(s.offset); break;
    case 0x15: s.f55 = b; break;
    case 0x16: s.frame = b; break;
    case 0x17: s.sub = b; break;
    default: break;  // 0x00, 0x12, > 0x17: no field (0x452060 returns)
    }
}

// 0x4528f0: a particle from the free list, else the oldest of the first slot that has any.
TbWorld::FxPart* TbWorld::fxAlloc(size_t slot) {
    if (fxFree_ > 0) {
        --fxFree_;
    } else {
        const auto from = std::ranges::find_if(fx_, [](const FxSlot& s) { return !s.parts.empty(); });
        if (from == fx_.end()) return nullptr;
        from->parts.erase(from->parts.begin());
    }
    return &fx_[slot].parts.emplace_back();
}

// S 138 (0x4529d0): slot Y (1..19) or every slot (-1) back to the free list; 0 and others do nothing.
void TbWorld::fxClear(int16_t slot) {
    for (size_t i = 0; i < fx_.size(); ++i)
        if (slot == -1 || (slot >= 1 && slot <= 19 && size_t(slot) == i)) fxFree_ += fx_[i].parts.size(), fx_[i].parts.clear();
}

// V 0x80200000 (0x45ad70 -> 0x452ab0): `count` particles of slot `slot` from entity a; skipped for p1 & 1 while a
// is out of view (+0x24 & 4). rand(): 2 per particle (direction), + 1 with flags 0x80 (size spread), + 1 without
// 0x800 (start delay).
void TbWorld::fxEmit(TbActor& a, uint32_t p1, uint16_t slot) {
    if (((p1 & 1) && (a.f24 & 4)) || slot >= fx_.size()) return;
    FxSlot& s = fx_[slot];
    s.emitter = int32_t(a.uid);
    if (s.frames <= s.frame) s.frame = 0;
    if ((s.flags & 0x2000) && (s.flags & 0x100)) s.flags &= ~0x100;
    Mat rot;  // the rotation register while the directions are rotated
    std::array<int16_t, 3> at{};
    if (s.flags & 3) {
        Mat L;
        if (!(s.flags & 0x1000)) L = coordMat(s.coordUid < 0 ? nullptr : entity(s.coordUid), s.coordPart);
        rot = (s.flags & 2) ? L : kDirMatrix;
        if (s.flags & 1) {
            s.off = v16(apply(rot, wide(s.offset)));
            at = {int16_t(s.off[0] + L.t.x), int16_t(s.off[1] + L.t.y), int16_t(s.off[2] + L.t.z)};
        }
    } else {
        at = s.offset;
    }
    if (s.flags & 0x4000) at[1] = int16_t(a.pos.y - a.depth);  // on the water surface
    if (s.sub != 0xff) s.surface = int16_t(a.pos.y - a.depth);
    int32_t even = 0;
    for (int n = 0; n < s.count; ++n) {
        FxPart* p = fxAlloc(slot);
        if (!p) return;
        p->frame = s.frame;
        const auto angle = [&](int16_t spread) {
            return int32_t(int16_t(int16_t(((rng() & 0xfff) * spread) >> 12) - spread / 2));
        };
        const int32_t a1 = angle(s.spread);
        const int16_t up = int16_t(-int16_t(rsin(a1)));
        int16_t fz = int16_t(rcos(a1));
        const int32_t a2 = angle(s.spread);
        const int16_t fx = int16_t((rsin(a2) * fz) >> 12);
        fz = int16_t((rcos(a2) * fz) >> 12);
        const std::array<int16_t, 3> dir = v16(apply(rot, {fx, up, fz}));
        p->pos = at;
        for (int k = 0; k < 3; ++k) p->vel[size_t(k)] = int16_t((dir[size_t(k)] * s.speed) >> 12);
        p->size = (s.flags & 0x80) ? int16_t(int16_t(((rng() & 0xfff) * s.sizeSpread) >> 12) + s.size.v[0] - s.sizeSpread / 2)
                                   : s.size.v[0];
        p->hold = p->cycle = 0;
        p->bright = s.bright.v[0];
        if (!(s.flags & 0x800)) {
            p->age = int8_t(-int8_t(((rng() & 0xfff) * s.delay) >> 12));
        } else {
            p->age = int8_t(-int8_t(even >> 12));
            even += (int32_t(s.delay) << 12) / s.count;
        }
    }
}

void TbWorld::fxTouch(TbActor& a, const V3& p, int32_t r) {
    for (size_t i = 0; i < fx_.size(); ++i) {
        const FxSlot& s = fx_[i];
        if (!(s.flags & 0x400)) continue;
        for (const FxPart& q : s.parts) {
            const int64_t x = p.x - q.pos[0], y = p.y - q.pos[1], z = p.z - q.pos[2], d = q.size + r;
            if (x * x + y * y + z * z <= d * d) {
                a.f28 |= 0x4000, a.trig[2] |= 0x4000;
                a.fxHit = int16_t(i);
                break;
            }
        }
    }
}

// S 110 (0x432bb0): X bit 1 on (count = max), 2 off, 4 on from 0 growing to max, 8 fade out to 0 then off, 0x10 max
// (until set up) = count = target = Y, 0x20 count = target = min(Y, max), 0x40 target = min(Y, max), 0x80 / 0x100 /
// 0x200 wind x / y / z = Y. The first bit set wins.
void TbWorld::fxWeather(uint16_t x, uint16_t y) {
    Weather& w = weather_;
    if (x & 1) w.flags |= 2, w.count = w.max;
    else if (x & 2) w.flags &= 1;
    else if (x & 4) w.count = 0, w.flags |= 2, w.target = w.max;
    else if (x & 8) w.target = 0, w.flags |= 4;
    else if (x & 0x10) {
        if (!(w.flags & 1)) w.max = y;
        w.target = w.count = w.max;
    } else if (x & 0x20) w.target = w.count = std::min(y, w.max);
    else if (x & 0x40) w.target = std::min(y, w.max);
    else if (x & 0x80) w.wind[0] = int16_t(y);
    else if (x & 0x100) w.wind[1] = int16_t(y);
    else if (x & 0x200) w.wind[2] = int16_t(y);
}

// Once per tick after the entities, the camera and the model camera nodes (main loop: 0x465130, 0x451370, 0x453350).
void TbWorld::fxTick() {
    fx.clear();
    const View view(cam);
    constexpr double kH = 400.0;  // projection distance 0x4cf18c (screen y is scaled by 0.625 in the view matrix)
    auto screen = [&](const V3& p, double& sx, double& sy) {
        const double dx = p.x - view.e[0], dy = p.y - view.e[1], dz = p.z - view.e[2];
        const double z = std::max(1.0, dx * view.f[0] + dy * view.f[1] + dz * view.f[2]);
        sx = kH * (dx * view.r[0] + dy * view.r[1] + dz * view.r[2]) / z;
        sy = 0.625 * kH * (dx * view.d[0] + dy * view.d[1] + dz * view.d[2]) / z;
        return z;
    };

    // ---- weather 0x451370: drops in a 1500 x 1024 x 1500 box (world axes) centred 1200 ahead of the eye, moved by
    // the wind minus the look-at point's motion, wrapped; each drawn as a gouraud line (0x808080 -> 0xefefff) from
    // last tick's screen point, kept within `clamp` screen units, when deeper than 499.
    Weather& w = weather_;
    if (w.flags & 2) {
        if (!(w.flags & 1)) {  // 0x451200: set up `max` drops (3 rand() each)
            w.drops.assign(w.max, Drop{});
            for (Drop& d : w.drops) {
                d.p[0] = int16_t(0x2ee - rng() % 0x5dc);
                d.p[1] = int16_t(0x200 - (rng() & 0x3ff));
                d.p[2] = int16_t(0x2ee - rng() % 0x5dc);
            }
            w.flags |= 9;
            w.lastLook = cam.look;
        }
        if (w.target != w.count) {
            if (w.target < w.count) --w.count;
            if (w.count < w.target) ++w.count;
            if ((w.flags & 4) && w.count == 0) w.flags &= 1;
        }
        const bool reset = w.flags & 0x10;
        w.flags &= ~0x10u;
        const std::array<int32_t, 3> off{w.wind[0] - (cam.look.x - w.lastLook.x), w.wind[1] - (cam.look.y - w.lastLook.y),
                                         w.wind[2] - (cam.look.z - w.lastLook.z)};
        w.lastLook = cam.look;
        const V3 centre{int32_t(std::lround(view.e[0] + 1200 * view.f[0])), int32_t(std::lround(view.e[1] + 1200 * view.f[1])),
                        int32_t(std::lround(view.e[2] + 1200 * view.f[2]))};
        for (size_t i = 0; i < std::min<size_t>(w.count, w.drops.size()); ++i) {
            Drop& d = w.drops[i];
            bool wrapped = reset;
            d.prev = d.cur;
            constexpr int16_t half[3] = {0x2ee, 0x200, 0x2ee};
            for (int k = 0; k < 3; ++k) {
                int16_t v = int16_t(d.p[size_t(k)] + off[size_t(k)]);
                if (v > half[k]) v = int16_t(v - 2 * half[k]), wrapped = true;
                else if (v < -half[k]) v = int16_t(v + 2 * half[k]), wrapped = true;
                d.p[size_t(k)] = v;
            }
            const V3 at{centre.x + d.p[0], centre.y + d.p[1], centre.z + d.p[2]};
            double sx, sy;
            const double z = screen(at, sx, sy);
            d.cur = {sat16(int32_t(std::lround(sx))), sat16(int32_t(std::lround(sy)))};
            if (wrapped || d.fresh) {
                d.fresh = false;
                d.prev = d.cur;
            } else {
                for (int k = 0; k < 2; ++k)
                    d.prev[size_t(k)] = int16_t(std::clamp<int32_t>(d.prev[size_t(k)], d.cur[size_t(k)] - w.clamp, d.cur[size_t(k)] + w.clamp));
            }
            if (z - 300 > 199) {
                FxDraw& f = fx.emplace_back();
                f.kind = FxDraw::Line, f.pos = at, f.to = at;
                f.dx = int16_t(d.prev[0] - d.cur[0]), f.dy = int16_t(d.prev[1] - d.cur[1]);
                f.c0 = {0xef, 0xef, 0xff}, f.c1 = {0x80, 0x80, 0x80};
            }
        }
    }

    // ---- particles 0x453350 (0x452e80 for flags 0x4000), slots in order.
    for (size_t si = 0; si < fx_.size(); ++si) {
        FxSlot& s = fx_[si];
        if (s.parts.empty()) continue;
        Mat L;
        if (s.flags & 0x2100) L = coordMat(s.coordUid < 0 ? nullptr : entity(s.coordUid), s.coordPart);
        V3 acc = wide(s.accel);
        if (s.flags & 0x2000) {  // particles in the coordinate's frame: the world acceleration taken into it
            Mat T;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) T.m[size_t(r * 3 + c)] = L.m[size_t(c * 3 + r)];
            acc = apply(T, acc);
        }
        const TbActor* em = s.emitter < 0 ? nullptr : entity(s.emitter);
        const std::array<int16_t, 3> ev = em ? std::array<int16_t, 3>{int16_t(em->vel[0]), int16_t(em->vel[1]), int16_t(em->vel[2])}
                                             : std::array<int16_t, 3>{};
        auto world = [&](const std::array<int16_t, 3>& p) { return (s.flags & 0x2000) ? xform(L, wide(p)) : wide(p); };
        auto curves = [&](FxPart& p) {
            if (++p.hold > s.hold) {
                p.hold = 0;
                if (++p.cycle > s.frames) p.cycle = 0;
            }
            p.size = int16_t(p.size + (p.age > s.size.at ? s.size.r1 : s.size.r0));
            p.bright = int16_t(p.bright + (p.age > s.bright.at ? s.bright.r1 : s.bright.r0));
            if (p.bright < 0) p.bright = 0;
            return int16_t(p.age > s.ang.at ? (p.age - s.ang.at) * s.ang.r1 + s.ang.v[1] : s.ang.r0 * p.age + s.ang.v[0]);
        };
        auto drag = [&](int16_t v, int32_t a) { return int16_t(v - int16_t((v * s.drag) >> 12) + a); };
        bool stop = false;
        for (size_t i = 0; !stop && i < s.parts.size();) {
            FxPart& p = s.parts[i];
            p.age = int8_t(p.age + 1);
            const bool tail = i + 1 == s.parts.size();
            if (p.age >= int32_t(s.life)) {  // expired: back to the free list
                s.parts.erase(s.parts.begin() + ptrdiff_t(i));
                ++fxFree_;
                continue;
            }
            if (s.flags & 0x4000) {  // 0x452e80: flat on the water surface (x / z motion only)
                if (p.age >= 0) {
                    if (p.age == 0 && (s.flags & 0x100))
                        p.pos = {int16_t(s.off[0] + L.t.x), int16_t(s.off[1] + L.t.y), int16_t(s.off[2] + L.t.z)};
                    p.pos[0] = int16_t(p.pos[0] + (p.vel[0] >> 5)), p.pos[2] = int16_t(p.pos[2] + (p.vel[2] >> 5));
                    if (s.flags & 0x200) p.pos[0] = int16_t(p.pos[0] + (ev[0] >> 4)), p.pos[2] = int16_t(p.pos[2] + (ev[2] >> 4));
                    p.vel[0] = drag(p.vel[0], acc.x), p.vel[2] = drag(p.vel[2], acc.z);
                    const int16_t ang = curves(p);
                    const int16_t sn = int16_t(-(rsin(ang) * p.size) >> 12), cs = int16_t((rcos(ang) * p.size) >> 12);
                    const V3 c = world(p.pos);
                    const std::array<V3, 4> q{V3{c.x + sn, c.y, c.z + cs}, V3{c.x + cs, c.y, c.z - sn},
                                              V3{c.x - cs, c.y, c.z + sn}, V3{c.x - sn, c.y, c.z - cs}};
                    double zmax = -1e9;
                    for (const V3& v : q) zmax = std::max(zmax, view.z(v));
                    if (zmax > 0x95 && s.sprite >= 0) {
                        FxDraw& f = fx.emplace_back();
                        f.kind = FxDraw::Ground, f.quad = q, f.asset = uint16_t(s.sprite);
                        f.frame = (s.flags & 0x10) ? 0 : p.frame;
                    }
                }
                ++i;
                continue;
            }
            if (s.sub != 0xff && p.pos[1] >= s.surface && p.age >= 3) {  // reached the surface: re-born in slot `sub`
                const std::array<int16_t, 3> at = p.pos;
                s.parts.erase(s.parts.begin() + ptrdiff_t(i));
                ++fxFree_;
                const size_t sub = size_t(int8_t(s.sub));
                if (sub < fx_.size()) {
                    FxSlot& t = fx_[sub];
                    FxPart* n = fxAlloc(sub);
                    n->frame = t.frame, n->pos = at, n->vel = {}, n->size = t.size.v[0], n->hold = n->cycle = 0, n->age = 0;
                    n->bright = t.bright.v[0];
                    if (sub == si && tail) stop = true;  // appended behind the end of this walk
                }
                continue;
            }
            if (p.age >= 0) {
                if (p.age == 0 && (s.flags & 0x100))
                    p.pos = {int16_t(s.off[0] + L.t.x), int16_t(s.off[1] + L.t.y), int16_t(s.off[2] + L.t.z)};
                const V3 old = world(p.pos);
                for (int k = 0; k < 3; ++k) p.pos[size_t(k)] = int16_t(p.pos[size_t(k)] + (p.vel[size_t(k)] >> 5));
                if (s.flags & 0x200)
                    for (int k = 0; k < 3; ++k) p.pos[size_t(k)] = int16_t(p.pos[size_t(k)] + (ev[size_t(k)] >> 4));
                p.vel = {drag(p.vel[0], acc.x), drag(p.vel[1], acc.y), drag(p.vel[2], acc.z)};
                if (view.z(old) > 0x8b) {
                    if (!(s.flags & 0x40)) {
                        const int16_t ang = curves(p);
                        if (s.sprite >= 0) {
                            FxDraw& f = fx.emplace_back();
                            f.kind = FxDraw::Sprite, f.pos = old, f.size = p.size, f.angle = ang, f.asset = uint16_t(s.sprite);
                            f.frame = (s.flags & 0x10) ? 0 : p.frame;
                        }
                    } else if (const V3 now = world(p.pos); view.z(now) >= 0x8c) {  // LINE_G2 old -> new
                        FxDraw& f = fx.emplace_back();
                        f.kind = FxDraw::Line, f.pos = old, f.to = now, f.c0 = s.rgb2, f.c1 = s.rgb1;
                    }
                }
            }
            ++i;
        }
    }
}

// 0x469320: model list +0x170 := the model source's links, rig list +0x174 := the rig source's (when given); with
// the role 13 + k model and the role 1 + k rig of those lists, the parts keep their pose and the rig's first frame
// rebinds them (0x200 / 0xc00 object packets: object, flags 4 / 8; 0x800 create packets 0x8008 / 0x40 / show; 0x100
// shows), the objects now indexing that model; +0x84 cleared. Nothing without both assets or a built skeleton.
void TbWorld::rigSwap(TbActor& a, const std::vector<uint16_t>* models, const std::vector<uint16_t>* rigs, uint8_t k) {
    if (a.anim.parts.empty()) return;
    if (rigs) a.rigList = *rigs;
    if (models) a.modelList = *models;
    const auto pick = [&](const std::vector<uint16_t>& l, uint16_t role) -> int32_t {
        for (uint16_t x : l)
            if (const auto r = assetRole_.find(x); r != assetRole_.end() && r->second == role) return x;
        return -1;
    };
    const int32_t m = pick(a.modelList.empty() ? a.links : a.modelList, uint16_t(13 + k));
    const int32_t r = pick(a.rigList.empty() ? a.links : a.rigList, uint16_t(1 + k));
    const auto it = m < 0 || r < 0 ? rigMeshes_.end() : rigMeshes_.find({uint16_t(m), uint16_t(r)});
    if (it == rigMeshes_.end()) return;
    a.attachPart = -1;
    a.mesh = it->second.get();
    const Tod& t = it->second->skeleton;
    if (t.frames() == 0) return;
    for (uint32_t i = t.frameStart[0]; i < t.frameStart[1]; ++i) {
        const TodPacket& pk = t.packets[i];
        if (!pk.id || pk.id > a.anim.parts.size()) continue;
        TodPart& p = a.anim.parts[pk.id - 1u];
        const uint32_t ty = uint32_t(pk.type) << 8, f = uint32_t(pk.flag) << 12;
        if (ty == 0x200 || ty == 0xc00) {
            const uint32_t o = ty == 0x200 ? uint32_t(pk.v[0]) & 0xffff : uint32_t(pk.v[0]) & 0xff;
            p.object = int32_t(o) - 1;
            p.flags = o ? 4 : 8;
        } else if (ty == 0x800) {
            if (!(f & 0x1000)) {
                const uint32_t c = (f >> 13) & 7;
                if (c == 0) p.flags = 0x8008;
                else if (c == 1) p.flags = 0x40;
                else if (c == 5) p.flags &= 0x7fff;
            } else if ((f & 0xe000) == 0xa000) {
                p.flags = 0x8008;
            }
        } else if (ty == 0x100) {
            p.flags &= 0x7fff;
        }
    }
}

// 0x45eee0 for a listed entity with view flag 0x400, a built model and +0x121: camera distance `dist` (scaled to the
// 400 projection) against +0x121 / +0x122 × 256 with 100 of hysteresis picks LOD 0 / 1 / 2 (rigSwap with the
// current lists).
void TbWorld::lodTick(TbActor& a, uint32_t dist) {
    const uint32_t d1 = uint32_t(a.lod1) << 8;
    if (a.lod1 == 0 || dist < d1 || dist - d1 < 100) {
        if (a.lod != 0 && dist <= d1 && d1 - dist > 99) rigSwap(a, nullptr, nullptr, 0), a.lod = 0;
    } else if (a.lod2 == 0) {
        if (a.lod != 1) rigSwap(a, nullptr, nullptr, 1), a.lod = 1;
    } else if (const uint32_t d2 = uint32_t(a.lod2) << 8; dist < d2 || dist - d2 < 100) {
        if (a.lod != 1 && dist <= d2 && d2 - dist > 99) rigSwap(a, nullptr, nullptr, 1), a.lod = 1;
    } else if (a.lod != 2) {
        rigSwap(a, nullptr, nullptr, 2), a.lod = 2;
    }
}

} // namespace bb::game
