// Time Busters character steering: the block of 0x453de0 that turns the entity and builds its local move vector
// (state record flags A +0x20 / B +0x24 / C +0x28), transcribed in the exe's order. Runs only when the state lacks
// flagsA 1 and +0x20 lacks 0x80000000; otherwise roll / pitch reset (unless flagsB 0x200004 / flags10 0x1000).
// Attached entities (+0xe8 coordinate parent) keep their pose from the coordinate chain (TbWorld::partWorld), so the
// parent-relative variants are not taken here.
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cstdlib>

namespace bb::game {

namespace {
int32_t wrap12(int32_t v) {  // & 0xfff, then > 0x800 -> negative (the exe's "u & 0xfff; if (0x800 < u) u -= 0x1000")
    v &= 0xfff;
    return v > 0x800 ? v - 0x1000 : v;
}
int32_t towardZero(int32_t v, int32_t step) { return v > 0 ? std::max(v - step, 0) : std::min(v + step, 0); }
} // namespace

bool TbWorld::pathAlloc(TbActor& a) {
    if (!a.pathSlot && pathFree_ > 0) --pathFree_, a.pathSlot = true;
    return a.pathSlot;
}

bool TbWorld::steer(TbActor& a, const TbStateRec*& r, bool par, std::array<int32_t, 3>& s) {
    const TbPad& pd = padOf(a);
    const int32_t sx = pd.sx, sy = pd.sy;
    const uint32_t dirs = pd.pressed >> 8;  // GD+9: Up 0x10, Right 0x20, Down 0x40, Left 0x80
    // State word values > 0 with 0x6000 read the signed byte S[v & 0x9fff] (0x453de0's 0x4d24a0 reads).
    auto val = [&](int16_t v) -> int32_t { return v > 0 && (v & 0x6000) ? int32_t(int8_t(S[size_t(v & 0x9fff)])) : v; };
    auto add = [](int32_t& c, int32_t v) { c = int16_t(c + v); };  // psVar9[4..6] are s16
    const bool live = !(r->flagsA & 1) && !(a.f20 & 0x80000000u);
    if (!live) {
        if (!(r->flagsB & 0x200004) && !(a.flags10 & 0x1000)) a.roll = a.pitch = 0;
        return true;
    }
    if (r->flagsA & 0x4000000) {  // wander: a random heading every 0..0x1ff ticks
        if (a.wander == 0) {
            a.turn = int16_t(rng() & 0xfff);
            a.wander = uint16_t(rng() & 0x1ff);
        } else {
            --a.wander;
        }
    }
    if ((r->flagsA & 0x3000) && !par)  // face (0x1000) / flee (0x2000) the state's target; 0x4000 snaps
        if (TbActor* t = lookup(uint16_t(r->target), a)) {
            const bool flee = r->flagsA & 0x2000;
            const int32_t dx = flee ? a.pos.x - t->pos.x : t->pos.x - a.pos.x;
            const int32_t dy = t->pos.y - a.pos.y + 0x32 + t->box[4];
            const int32_t dz = flee ? a.pos.z - t->pos.z : t->pos.z - a.pos.z;
            auto stepTo = [&](int32_t d, int16_t& field, bool toTurn) {  // shared shift / cap rule
                if (!(r->flagsB & 0x400)) {
                    const int32_t sh = r->flagsB & 0x20 ? (a.turnRate ? a.turnRate : 2) : 4;
                    const int32_t q = d >> (sh & 0x1f);
                    field = int16_t((q ? q : d) & 0xfff);
                } else if (d < 0 && d < -a.turnRate) {
                    if (toTurn) a.yaw = int16_t(a.yaw - a.turnRate); else field = int16_t(field - a.turnRate);
                } else if (a.turnRate < d) {
                    if (toTurn) a.yaw = int16_t(a.yaw + a.turnRate); else field = int16_t(field + a.turnRate);
                }
            };
            int32_t ang = ratan2(dx, dz);
            if (!(r->flagsA & 0x4000)) {
                if (ang < 0) ang += 0x1000;
                int32_t d = ang - (((a.yaw % 0x1000) + 0x1000) % 0x1000);
                if (d < 0) d += 0x1000;
                if (d > 0x800) d -= 0x1000;
                stepTo(d, a.turn, true);
            } else {
                a.yaw = int16_t(ang);
            }
            if ((r->flagsB & 0x200000) && !flee) {  // pitch toward the target (0x4000: snap)
                const int32_t p = -ratan2(dy, isqrt(dx * dx + dz * dz));
                if (!(r->flagsA & 0x4000)) stepTo(wrap12(p - a.pitch), a.pitch, false);
                else a.pitch = int16_t(p);
            }
        }
    if (r->flagsB & 0x10000) add(s[0], val(r->side) * dt);  // sidestep
    if (r->flagsA & 0x20000000) {  // hover +0x194 above the ground below (0x4463e0), bob phase +0x198
        const int32_t h = a.hover ? a.hover : 500;
        if (!(r->flagsB & 0x200000)) {
            int32_t u = uint16_t(r->vert);
            if (const auto hit = col.query(a.pos, {0, h + 100, 0}, 5, 0)) {
                u = (hit->p.y - a.pos.y - h) >> 6;
                u = std::max(std::min(u, int32_t(r->vert)), -int32_t(r->vert));
            }
            add(s[1], u);
        }
        a.hoverPhase = int16_t((a.hoverPhase + 0x40) & 0xfff);
        add(s[1], (r->vert * (a.hoverPhase - 0x800)) >> 12);
    }
    if (r->flagsA & 0x400000) add(s[2], val(r->fwd) * dt);
    if (!(r->flagsC & 1)) a.f20 &= ~0x1800000u;  // plunge / rise end outside the swim states
    if (r->flagsC & 0x80) {  // lean steering: roll from the stick, yaw from the roll, pitch to the ground slope
        int32_t ro = wrap12(a.roll), want = 0;
        if (dirs & 0xa0) {
            a.yaw = int16_t(a.yaw + ((sx * 3) >> 12));
            want = (sx * 0x199) >> 7;
        }
        int32_t d = want - ro;
        if (d > 0) d += 3;
        d = std::clamp((d >> 4) + (d >> 2), -0x32, 0x32);
        ro += d;
        if (std::abs(ro) > 0x15e) a.f28 |= 0x10000;
        a.roll = int16_t(ro);
        a.yaw = int16_t(a.yaw + ((ro * 6) >> 7));
        if (r->flagsB & 0x40) camCap(a, *r);
        int32_t tp = 0;
        if ((a.f20 & 2) && a.groundPoly >= 0 && !(a.f28 & 2)) {
            const auto& n = col.polys[size_t(a.groundPoly)].n;
            Mat m;
            mulRotY(m, -a.yaw);
            const V3 l = apply16(m, {n[0], n[1], n[2]});
            tp = -ratan2(l.y, l.z) - 0x400;
        }
        a.pitch = int16_t(a.pitch + ((wrap12(tp - a.pitch) + 8) >> 4));
    }
    if (r->flagsC & 8) {  // roll / pitch from the stick, yaw from the roll
        int32_t ro = wrap12(a.roll);
        a.roll = int16_t(a.roll + ((std::max(std::abs(sx * 2), 300) * (sx * 4 - ro)) >> 12));
        a.yaw = int16_t(a.yaw + ((rsin(wrap12(a.roll)) * 0x18) >> 12));
        const int32_t pi = wrap12(a.pitch);
        a.pitch = int16_t(a.pitch + ((std::max(std::abs(sy * 2), 300) * (sy * -4 - pi)) >> 12));
    }
    if (r->flagsC & 0x1000) {  // flight: roll toward the stick x, yaw from the roll, pitch toward the stick y
        int32_t ro = wrap12(a.roll), want = 0;
        if (dirs & 0xa0) {
            a.yaw = int16_t(a.yaw + ((sx * 0xe) >> 12));
            want = (sx << 9) >> 7;
        }
        int32_t d = want - ro;
        if (d > 0) d += 7;
        d = std::clamp((d >> 4) + (d >> 3), -0x3c, 0x3c);
        a.roll = int16_t(ro + d);
        a.yaw = int16_t(a.yaw + (((ro + d) * 0xc) >> 7));
        const int32_t pi = wrap12(a.pitch);
        int32_t e = ((dirs & 0x50) ? (sy * -0x200) >> 7 : 0) - pi;
        if (e > 0) e += 0xf;
        a.pitch = int16_t(pi + std::clamp(e >> 4, -0x16, 0x16));
    }
    if ((r->flagsC & 0x800) && (dirs & 0xa0)) a.yaw = int16_t(a.yaw + ((sx * 0x24) >> 7));
    if ((r->flagsC & 0x10) && a.hoverSpan) {  // pitch along the vertical speed between +0x194 and -0x400
        const int32_t k = std::clamp(((a.hoverSpan * 16 - int16_t(a.vel[1])) * 0x1000) / (a.hoverSpan * 0x20), 0, 0x1000);
        a.pitch = int16_t((((-0x400 - a.hover) * k) >> 12) + a.hover);
    }
    if (!(r->flagsC & 1)) {
        if (!(r->flagsB & 0x80000000u)) {
            if (r->flagsA & 2) add(s[2], val(r->fwd) * dt);
            if (!(a.f20 & 0x80000) && !(r->flagsB & 0x200004) && !(a.flags10 & 0x1000) && !(r->flagsC & 0x1088)) {
                const int32_t ro = wrap12(a.roll), q = ro >> 2;
                a.roll = int16_t(q ? ro - q : 0);
            }
        } else {  // §2.1 stick branch: heading = stick angle + camera yaw
            if (!sx && !sy) {
                stickMag -= stickMag >> 3;
            } else {
                const int32_t mag = std::min(isqrt(sx * sx + sy * sy), 0x80);
                stickMag -= (stickMag - mag) >> 3;
                if ((r->flagsB & 0x4000000) && a.seq != uint16_t(mag > 0x77)) {
                    dispatch(a, uint32_t(mag > 0x77) | 0x4000000);
                    a.attachPart = -1;
                    r = a.rec();
                }
                if ((r->flagsB & 0x8000000) && a.model) {  // anim pace from the stick (§5.3)
                    const int32_t p = (r->flagsB & 0x4000000) && mag < 0x78 ? (mag * 0x1111) >> 12 : mag;
                    a.paceAcc = int16_t(a.paceAcc + p);
                    const int32_t k = a.paceAcc >> 5;
                    if (a.anim.tod) a.anim.ticksPerFrame = uint8_t((5 - k) * (a.anim.tod->resolution >> 1));
                    a.paceAcc = int16_t(a.paceAcc - (k << 5));
                }
                const int32_t v = std::max(int32_t(uint32_t(val(r->fwd) * dt * mag) >> 7), 1);
                if (!par) {
                    int32_t d = ratan2(sx, sy) + camYaw();
                    if (d > 0x800) d -= 0x1000;
                    if (d < -0x800) d += 0x1000;
                    if (a.yaw > 0x800) a.yaw = int16_t(a.yaw - 0x1000);
                    if (a.yaw < -0x800) a.yaw = int16_t(a.yaw + 0x1000);
                    d -= a.yaw;
                    if (d > 0x800) d -= 0x1000;
                    if (d < -0x800) d += 0x1000;
                    if (!(r->flagsB & 4)) {  // lean
                        const int32_t ro = wrap12(a.roll);
                        const int32_t l = std::clamp(wrap12((d >> 3) - ro), -0x78, 0x78);
                        a.roll = int16_t((ro + l) & 0xfff);
                    }
                    a.turn = int16_t(d);
                    int32_t f = v * (0x800 - std::abs(d));
                    f += f < 0 ? -0x7ff : 0x7ff;
                    if (r->flagsA & 2) add(s[2], f >> 11);
                }
            }
        }
    } else {  // swim states (state +0x28 & 1): no gravity, vertical speed +0x72 by depth +0x1a2
        const uint32_t old = a.f20;
        a.f20 |= 0x200;
        if (old & 0x421) {  // arrived jumping / falling: plunge (0x1000000) unless diving (flagsC 2)
            if (!(r->flagsC & 2)) a.f20 = old | 0x1000200;
            a.f20 &= 0xfffffbdeu;
        }
        int32_t vy = int16_t(a.vel[1]);
        if (a.f20 & 0x1000000) {  // plunge: brake by 3/8 (at least 0xf0) until rising
            vy -= std::max((vy * 3) >> 3, 0xf0);
            if (int16_t(vy) < 1) a.f20 = (a.f20 & ~0x1000000u) | 0x800000;
        } else if (a.f20 & 0x800000) {  // rise back to the surface
            if (a.depth < 0x51) {
                vy = std::min(vy + 0x50, -0x50);
                if (a.depth < 0x14) vy = 0, a.f20 &= ~0x800000u;
            } else {
                vy = std::max(vy - 0xa0, -0x1e0);
            }
        } else if (r->flagsC & 4) {  // dive: pitch down 0x28 a tick to -800
            a.pitch = int16_t(std::max(wrap12(a.pitch - 0x28), -800));
            s[2] = int16_t(val(r->fwd) * dt);
        } else if (r->flagsC & 2) {  // under water: stick x turns, <- -> roll, up / down pitch
            a.yaw = int16_t(a.yaw + ((sx * 0x28) >> 7));
            if (r->flagsA & 2) {
                a.yaw = int16_t(a.yaw + ((sx * 0xc) >> 7));
                int32_t ro = wrap12(a.roll);
                ro = dirs & 0xa0 ? ro + ((sx << 5) >> 7) : towardZero(ro, 0xc);
                a.roll = int16_t(std::clamp(ro, -0x200, 0x200));
                int32_t pi = wrap12(a.pitch);
                pi = dirs & 0x50 ? pi - ((sy * 0x24) >> 7) : towardZero(pi, 10);
                a.pitch = int16_t(std::clamp(pi, -800, 800));
                s[2] = int16_t(val(r->fwd) * dt);
            }
        } else {  // at the surface: sink back to depth 30, steer like the stick branch
            if (a.depth < 0x1f) {
                if (vy < 0 && (vy += 0x50) > 0) vy = 0;
            } else {
                vy = std::max(vy - 0xa0, -0xa0);
            }
            if (!(r->flagsB & 0x80000000u) || (!sx && !sy)) {
                a.yaw = int16_t(a.yaw + ((sx << 5) >> 7));
                if (r->flagsA & 2) a.yaw = int16_t(a.yaw + ((sx * 0xc) >> 7));
                s[2] = int16_t(val(r->fwd) * dt);
            } else {
                const int32_t mag = std::min(isqrt(sx * sx + sy * sy), 0x80);
                stickMag -= (stickMag - mag) >> 3;
                const int32_t v = std::max(int32_t(uint32_t(val(r->fwd) * dt * mag) >> 7), 1);
                const int32_t d = std::clamp(wrap12(camYaw() - a.yaw + ratan2(sx, sy)), -0x100, 0x100);
                a.turn = int16_t(d);
                if (r->flagsA & 2) add(s[2], (v * (0x800 - std::abs(d))) >> 11);
            }
        }
        a.vel[1] = int16_t(vy);
        if (!(r->flagsC & 4) && (!(r->flagsA & 2) || !(r->flagsC & 2))) {  // level out: pitch 0x18, roll 0x10
            a.pitch = int16_t(towardZero(wrap12(a.pitch), 0x18));
            a.roll = int16_t(towardZero(wrap12(a.roll), 0x10));
        }
    }
    // LAB_00455b16: a quarter of the pending turn +0x18c per tick; once that rounds to 0 the remainder is applied
    // whole and +0x18c becomes -1 (idle). flagsB 0x40 keeps the heading within the camera cap, 0x400 caps the step.
    if (a.turn != -1) {
        const int32_t t = a.turn;
        int32_t q = (t > 0x800 ? t - 0x1000 : t) >> 2, step = t;
        if (q == 0) a.turn = -1;
        else a.turn = int16_t(t - q), step = q;
        if (r->flagsB & 0x40) {
            int32_t cam = camYaw() & 0xfff, cap = val(r->cap);
            if (r->cap > 0 && (r->cap & 0x6000)) cap <<= 5;
            if (cap < 0) cam = (cam - 0x800) & 0xfff, cap = -cap & 0xfff;
            const uint32_t u = uint32_t(a.yaw - cam + step) & 0xfff;
            int32_t c;
            if (u < 0x801) c = cap < int32_t(u) ? cap : int32_t(u);
            else c = cap < int32_t(0x1000 - u) ? -cap : -int32_t(0x1000 - u);
            step = ((cam + c) & 0xfff) - a.yaw;
        }
        step = ((step - 0x800) & 0xfff) - 0x800;
        if ((r->flagsB & 0x400) && ((step > 0 && a.turnRate < step) || (step < 0 && step < -a.turnRate)))
            step = step > 0 ? a.turnRate : -a.turnRate;
        a.yaw = int16_t(a.yaw + step);
    }
    // Pitch levels out 0x14 a tick unless flagsB 0x200000 / flagsC 0x1088 / flags10 0x1000 (with the dive's -0x28
    // this is -20 a tick: Granny 220, oracle local/wine/compare/w13/tb/swim_sfx).
    if (!(r->flagsB & 0x200000) && !(r->flagsC & 0x1088) && !(a.flags10 & 0x1000) && (a.pitch & 0xfff))
        a.pitch = int16_t(towardZero(wrap12(a.pitch), 0x14));
    if (r->flagsA & 0x800) {  // vertical speed; with flagsB 0x80000000 scaled by the stick magnitude (<= 0x7f)
        if (!(r->flagsB & 0x80000000u)) add(s[1], -val(r->vert) * dt);
        else if (sx || sy) add(s[1], -((val(r->vert) * dt * std::min(isqrt(sx * sx + sy * sy), 0x7f)) >> 7));
    }
    if ((r->flagsB & 0x10) && !par)  // face the camera eye
        a.yaw = int16_t(ratan2(cam.eye.x - a.pos.x, cam.eye.z - a.pos.z));
    // flagsB 0x100: follow camera track path +0x16 through a CameraTrackPath slot (+0x11c, 0x43bc00); none left ends
    // the entity's tick here. The path list 0x4d4fb4 is filled only by flags1 0x40 recorders, which no PC level has
    // (TbWorld::load), so with a slot the follow finds no path and does nothing.
    if ((r->flagsB & 0x100) && !pathAlloc(a)) return false;
    // Roll resets off the ground (+0x20 2) unless flagsC 0x1089 / flagsB 0x200004 / flags10 0x1000.
    if (!(a.f20 & 2) && !(r->flagsC & 0x1089) && !(r->flagsB & 0x200004) && !(a.flags10 & 0x1000)) a.roll = 0;
    // Climbing (flagsA 0x200, hung on +0x90): +0x1a0 = 0xc (R 0x42: 8 down, 4 up); the side speed (+0x1c) circles
    // the pole at radius +0x68 + 10, facing it (+0x136 = heading + 0x800). 0x44f690 limits a pole (flags0 0x10000):
    // the top (own +0x6a + the local height above the pole's +0x6a) blocks climbing (4 cleared; a flagsA 0x800 state
    // rising by +0x14 > 0 stops), below the bottom (pole +0x62) it lets go (+0x20 0x200 cleared, 0x44d870). R1 adds 4
    // to the parent's +0x166 and 0x44f690 walks a rope's segments for other parents; neither applies in TB: no
    // entity reads +0x166, and no PC level has a climbable parent other than a flags0 0x10000 one of a class other
    // than 8 (game.tb allLevels).
    if (TbActor* p = (r->flagsA & 0x200) ? entity(a.parent) : nullptr) {
        a.climb = 0xc;
        const int32_t y = (val(r->side) * dt * 0x80) / p->box[3] + ratan2(a.local.x, a.local.z);
        Mat m;
        mulRotY(m, y);
        const V3 o = apply16(m, {0, 0, int16_t((p->box[3] + 10) * 0x40)});
        a.localYaw = int16_t(y + 0x800);
        a.local.x = int16_t((o.x + 0x20) >> 6), a.local.z = int16_t((o.z + 0x20) >> 6);
        if (a.box[4] + a.local.y < p->box[4]) {
            if ((r->flagsA & 0x800) && r->vert > 0) s[1] = 0;
            a.climb &= ~4u;
        } else if (p->box[1] < a.box[1] + a.local.y) {
            a.f20 &= ~0x200u;
            detach(a);
        }
    }
    return true;
}

// flagsB 0x40 inside the flagsC 0x80 branch: the yaw against the camera yaw clamped to +0x1e (S-values << 5;
// negative = from behind). As decompiled, the coordinate yaw (+0xe4 + 10, the same field as +0xf6 for a root
// entity) becomes the clamped heading minus the old yaw.
void TbWorld::camCap(TbActor& a, const TbStateRec& r) {
    int32_t cam = camYaw() & 0xfff;
    int32_t cap = r.cap;
    if (cap > 0 && (cap & 0x6000)) cap = int32_t(int8_t(S[size_t(cap & 0x9fff)])) << 5;
    if (cap < 0) cam += 0x800, cap = -cap;
    int32_t u = wrap12(a.yaw - int16_t(cam));
    if (cap < u) u = cap;
    else if (u < -cap) u = -cap;
    a.yaw = int16_t(((u + int16_t(cam)) & 0xfff) - a.yaw);
}

} // namespace bb::game
