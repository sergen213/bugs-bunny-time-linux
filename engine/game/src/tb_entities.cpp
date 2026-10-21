// TB entity systems around the class-14 interpreter (GAMEPLAY_TB.md §3.3–§3.5): the view list 0x45eee0 that decides
// which entities tick, the class dispatch 0x4cea60, and the combat parts of 0x453de0 — actor contacts, attack probes
// 0x447fd0, pending-hit intake, death and the player-down rule 0x44ea50.
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bb::game {
namespace {

int64_t dist2(const V3& a, const V3& b) {
    const int64_t x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

}  // namespace

// 0x45ee90 + 0x45eee0 (view 0). Every entity starts the tick out of view (+0x24 |= 4) and unlisted; then, in list
// order: the contact list 0x4d2734 takes flags0 & 0x2000100 == 0x100; an activation block +0x118 must return
// 0x40000000; the sleep counter +0x50 drops by the camera's movement (|Δ|/4 · 0x4038 + 0x302a >> 12) and must be
// < 1; +0x6e 0x20 never lists; 2 culls beyond +0x76 from the camera (sleep = the excess) unless 0x40; 4/8 test the
// bounding sphere (+0x4a) against the near plane (z + r < 400) and the side planes (0xd7a/0x8a0 x, 0xf77/0x419 up,
// 0xf6d/0x43e down) and cull unless 0x10/0x40. Listed entities in view clear +0x24 & 4; directors (16/35) stay 4.
void TbWorld::viewList() {
    const TbActor& F = actors[size_t(focus())];
    const V3 eye = cam.eye == V3{} ? F.pos : cam.eye;
    const int64_t mx = eye.x - prevEye_.x, my = eye.y - prevEye_.y, mz = eye.z - prevEye_.z;
    const int32_t speed = isqrt(int32_t(std::min<int64_t>((mx * mx + my * my + mz * mz) >> 4, 0x7fffffff)));
    const int32_t dec = (speed * 0x4038 + 0x302a) >> 12;
    prevEye_ = eye;
    // Camera basis (PS1: x right, y down, z forward); the original uses the GTE view matrix of the same camera.
    double f[3] = {double(cam.look.x - eye.x), double(cam.look.y - eye.y), double(cam.look.z - eye.z)};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 0) f[0] /= fl, f[1] /= fl, f[2] /= fl; else f[2] = 1;
    double r[3] = {f[2], 0, -f[0]};
    const double rl = std::max(1e-9, std::sqrt(r[0] * r[0] + r[2] * r[2]));
    r[0] /= rl, r[2] /= rl;
    const double d[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    TbActor& partner = actors[size_t(1 - focus())];
    const std::vector<TbActor*> all = order();  // activation blocks may spawn
    for (TbActor* ap : all) {
        TbActor& a = *ap;
        a.f24 |= 4;
        a.active = false;
        a.collidable = (a.flags0 & 0x2000100) == 0x100 && !(a.view & 0x20) && !(&a == &partner && (GD[0xa2] & 3));
        if (a.f20 & 0x2000) continue;
        if (a.actBlock && !(runBlock(a, a.actBlock) & 0x40000000)) continue;
        a.sleep -= dec;
        if (a.sleep >= 1) continue;
        a.sleep = 0;
        if (a.view & 0x20) continue;
        bool out = false;
        if (a.view && a.behaviour != 20 && a.parent != int32_t(F.uid) && F.parent != int32_t(a.uid)) {
            if (a.view & 2) {
                const int32_t dist = int32_t(std::lround(std::sqrt(double(dist2(a.pos, eye)))));
                if (dist > a.range) {
                    if (!(a.view & 0x40)) {
                        a.sleep = dist - a.range;
                        continue;
                    }
                    out = true;
                }
            }
            if ((a.view & 0xc) && !out) {
                const double p[3] = {double(a.pos.x - eye.x), double(a.pos.y - eye.y), double(a.pos.z - eye.z)};
                const int32_t z = int32_t(p[0] * f[0] + p[1] * f[1] + p[2] * f[2]);
                const int32_t x = int32_t(p[0] * r[0] + p[1] * r[1] + p[2] * r[2]);
                const int32_t y = int32_t(p[0] * d[0] + p[1] * d[1] + p[2] * d[2]);
                const int32_t rad = a.radius;
                const int32_t ux = (std::abs(x) * 0xd7a - z * 0x8a0) >> 12;
                const int32_t uy = y < 0 ? (-y * 0xf77 - z * 0x419) >> 12 : (y * 0xf6d - z * 0x43e) >> 12;
                // ponytail: bounding sphere only; flag 4's eight-corner box refinement is not ported.
                out = z + rad < 400 || ux > rad || uy > rad;
                if (out && !(a.view & 0x50)) continue;
            }
        }
        a.active = true;
        if (!out) a.f24 &= ~4u;
        if (a.behaviour == 16 || a.behaviour == 35) a.f24 |= 4;
    }
}

// Dispatch table 0x4cea60 by class +0x31. Classes 0/6/7/20 (static model, system, flipbook link) only draw.
void TbWorld::classTick(TbActor& a) {
    auto gdBit = [&](uint16_t b) { return (b & ~7u) < 0x5e1 && (GD[500 + (b >> 3)] >> (b & 7) & 1); };
    switch (a.behaviour) {
    case 14: actorStep(a); return;
    case 16:  // 0x450bc0: first tick removes entities whose GAME_DATA bit is set (flags10 & 4); first block each tick
        if (!(a.f24 & 0x8000)) {
            if ((a.flags10 & 4) && gdBit(a.gdBit)) {
                a.f20 |= 0x2000;
                return;
            }
            a.f24 |= 0x8000;
        }
        if (a.flags1 & 0x14) unsupported.insert("class16 camera target");  // DAT_004d5164 camera hint
        if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
        return;
    case 35:  // 0x450cd0
        if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
        return;
    case 31: textTick(a); return;  // 0x450d00 overlay text
    case 0: case 6: case 7: case 20: return;
    default: unsupported.insert(std::format("class{}", a.behaviour)); return;
    }
}

// 0x453de0 head: a pending hit (+0x20 0x8000000 by a player / 0x4000000 by an enemy on a vulnerable state, a stomp,
// or V 0x80080000's +0x24 0x40000000) costs +0x66 health unless flashing (+0x28 0x200) or dead; players start their
// 60-tick flash (0x4d2058/9). Health < 1 without flags0 0x200 = death (+0x20 0x2000000, event 0x10000000, GD+0xa2
// 0x10/0x20), else event 0x8000000 when the attacker named a transition (+0x5e). The new state's blocks run at once
// and carried children (flags0 & 0x2802) drop. Returns true when the tick ends here.
bool TbWorld::hitIntake(TbActor& a) {
    const TbStateRec& r = *a.rec();
    const uint32_t f = a.f20;
    const bool hit = ((f & 0xc000000) && (r.flagsA & 0x10000) && (f & 0x8000000)) ||
                     ((r.flagsB & 0x8000) && (f & 0x10) && (f & 0x8000000)) || ((r.flagsB & 8) && (f & 0x4000000)) ||
                     (a.f24 & 0x40000000);
    a.f20 &= ~0x10u;  // stood on this tick (set by the mover of the entity on top; I)
    if (hit && !(a.f28 & 0x200) && !(f & 0x2000000)) {
        const bool player = a.flags0 & 0x4000000;
        if (player && a.dmgIn) {
            flash[a.id == 2 ? 0 : 1] = a.id == 2 ? 0x40 : 0x80;
            a.f28 |= 0x200;
        }
        a.hp = int16_t(std::max(0, a.hp - a.dmgIn));
        a.dmgIn = 0;
        if (a.flags1 & 0x200) S[1] = uint8_t((a.hp << 7) / std::max<int16_t>(a.maxHp, 1));
        uint32_t ev = 0x8000000;
        if (!(a.flags0 & 0x200) && a.hp < 1) {
            a.f20 |= 0x2000000;
            ev = 0x10000000;
            if (player) GD[0xa2] |= a.id == 2 ? 0x10 : 0x20;
        }
        if (a.hitTr != -1 || ev == 0x10000000) {
            const bool changed = dispatch(a, ev);
            if (!changed && ev == 0x10000000) {
                a.f20 &= ~0xc000000u;
                a.f28 &= ~0x20000u;
                return true;
            }
            a.attachPart = -1;
            forEachEntity([&](TbActor& c) {
                if (c.parent != int32_t(a.uid) || !(c.flags0 & 0x2802)) return;
                c.f20 &= ~0x8000u;
                c.f24 |= 0x80;
                if (c.progMask[1] & 0x80) runProgram(c, 0, 0x80);
                detach(c);
            });
            for (uint16_t b : {a.rec()->logicA, a.rec()->logicB})
                if (b) {
                    const uint32_t e = runBlock(a, b);
                    if (!(e & 0x90000000)) dispatch(a, e);
                }
            if (a.f20 & 0x2000) return true;
        }
    }
    a.f20 &= ~0xc000000u;
    a.f24 &= ~0x40000000u;
    a.f28 &= ~0x20000u;
    return false;
}

// 0x453de0 contact loop over the mover's actor contacts: both sides remember each other (+0x54) and raise +0x20
// 0x40 (touching a non-player) / 0x80 (a player); a contact attacker (+0x20 0x1000, state flagsA 0x1000000 with
// 0x40000 vs players / 0x80000 vs others) hurts a hittable (flags0 0x80), vulnerable victim; standing on a
// stompable state (flagsB 0x8000) hurts it, on a bouncy one (0x10000000) raises +0x24 8. The bits given to the
// touched entity that its program masks (+0x14/+0x18) run its program at once (0x457a07 -> 0x45ea90): pickups.
void TbWorld::contacts(TbActor& a, const std::vector<TbActor*>& touched) {
    const bool aPl = a.flags0 & 0x4000000;
    for (TbActor* cp : touched) {
        TbActor& c = *cp;
        const bool cPl = c.flags0 & 0x4000000;
        a.contact = int32_t(c.uid), c.contact = int32_t(a.uid);
        const uint32_t ab = cPl ? 0x80 : 0x40;
        a.f20 |= ab, a.trig[0] |= ab;
        const uint32_t c20 = aPl ? 0x80 : 0x40;
        uint32_t c24 = 0;
        c.f20 |= c20;
        if ((c.f20 & 0x1000) && c.rec() && a.rec()) {
            const uint32_t ca = c.rec()->flagsA;
            if ((ca & 0x1000000) && (a.flags0 & 0x80) && (aPl ? (ca & 0x40000) : (ca & 0x80000)) &&
                (cPl ? (a.rec()->flagsA & 0x10000) : (a.rec()->flagsB & 8))) {
                a.dmgIn = c.rec()->dmg;
                a.f20 |= cPl ? 0x8000000 : 0x4000000;
                if (cPl && c.id == 3) a.f28 |= 0x20000;
                c24 = aPl ? 2u : 0x100000u;
                if (aPl && a.hp <= a.dmgIn) c24 |= 1;
                c.f24 |= c24;
                a.hitTr = c.rec()->hitTrans;
            }
        }
        if (!(a.f20 & 0x6000000) && (c.f20 & 0x10) && c.behaviour == 14 && c.rec() && a.rec()) {
            if (c.rec()->flagsB & 0x8000) {
                c.dmgIn = a.rec()->dmg;
                c.f20 |= aPl ? 0x8000000 : 0x4000000;
                if (aPl && a.id == 3) c.f28 |= 0x20000;
                const uint32_t b = cPl ? (c.hp <= c.dmgIn ? 3u : 2u) : 0x100000u;
                a.f24 |= b, a.trig[1] |= b;
                c.hitTr = a.rec()->hitTrans;
            }
            if (c.rec()->flagsB & 0x10000000) a.f24 |= 8, a.trig[1] |= 8;
        }
        if (const uint32_t m20 = c20 & c.progMask[0], m24 = c24 & c.progMask[1]; m20 || m24) runProgram(c, m20, m24);
    }
}

// 0x447fd0: listed entities with flags0 & maskIn whose box (in their own frame, yaw only) lies within 125 of p.
std::vector<TbActor*> TbWorld::probe(TbActor& a, const V3& p, uint32_t maskIn, size_t max) {
    std::vector<TbActor*> out;
    for (TbActor* ep : order()) {
        TbActor& e = *ep;
        if (out.size() >= max) break;
        if (!e.active || !(e.flags0 & maskIn) || (e.f20 & 0x2000) || &e == &a) continue;
        if (e.parent == int32_t(a.uid) || a.parent == int32_t(e.uid)) continue;
        const int32_t R = 125 + e.radius;
        const int32_t dx = p.x - e.pos.x, dy = p.y - e.pos.y, dz = p.z - e.pos.z;
        if (std::abs(dx) > R || std::abs(dy) > R || std::abs(dz) > R) continue;
        if (int64_t(dx) * dx + int64_t(dy) * dy + int64_t(dz) * dz > int64_t(R) * R) continue;
        if (e.box == std::array<int16_t, 6>{}) continue;
        const V3 l = rotY(-e.yaw, dx, dy, dz);
        auto outside = [](int32_t v, int32_t lo, int32_t hi) { return v > hi ? v - hi : lo - v; };
        if (outside(l.x, e.box[0], e.box[3]) < 0x7e && outside(l.y, e.box[4], e.box[1]) < 0x7e &&
            outside(l.z, e.box[2], e.box[5]) < 0x7e)
            out.push_back(&e);
    }
    return out;
}

// 0x453de0 attack hits: states with flagsA 0x40000/0x80000 (vs players / others) hit the contact list (0x1000000
// with the mover) and/or everything a probe at the TOD 0xa00 point finds (0x200000, or without 0x1000000). Each hit
// writes the state's damage and hit transition into the victim (+0x66, +0x5e) and +0x24 0x100000 / 2 (|1 lethal).
void TbWorld::attackHits(TbActor& a, const std::vector<TbActor*>& touched) {
    const TbStateRec& r = *a.rec();
    if (r.flagsA & 0x1040000) a.f20 |= 0x1000; else a.f20 &= ~0x1000u;
    if (!(r.flagsA & 0xc0000) || (a.f20 & 0x410000) || (a.f24 & 0x80)) return;
    std::vector<TbActor*> t;
    if ((r.flagsA & 0x1000000) && (r.flagsA & 0x800000)) t = touched;
    if (((r.flagsA & 0x200000) || !(r.flagsA & 0x1000000)) && a.attachPart >= 0 && t.size() < 10) {
        const std::vector<TbActor*> more = probe(a, a.attachPos, 0x80, 10 - t.size());
        t.insert(t.end(), more.begin(), more.end());
    }
    if (r.flagsA & 0x40000000) unsupported.insert("attack part probe 0x448240");
    const bool aPl = a.flags0 & 0x4000000;
    const TbActor* par = entity(a.parent);
    const bool free = !par || (par->flags0 & 0x1000c) || (par->rec() && (par->rec()->flagsA & 0x20000) && par->attachPart >= 0);
    for (TbActor* cp : t) {
        TbActor& c = *cp;
        const bool cPl = c.flags0 & 0x4000000;
        if (!(c.flags0 & 0x80) || (c.f20 & 0xe000000) || !c.rec() || !free) continue;
        if (aPl ? !(c.rec()->flagsA & 0x10000) : !(c.rec()->flagsB & 8)) continue;
        if (cPl ? !(r.flagsA & 0x40000) : !(r.flagsA & 0x80000)) continue;
        a.contact = int32_t(c.uid), c.contact = int32_t(a.uid);
        c.dmgIn = r.dmg;
        c.f20 |= aPl ? 0x8000000 : 0x4000000;
        if (aPl && a.id == 3) c.f28 |= 0x20000;
        const uint32_t b = cPl ? (c.hp <= c.dmgIn ? 3u : 2u) : 0x100000u;
        a.f24 |= b, a.trig[1] |= b;
        c.hitTr = r.hitTrans;
    }
}

// 0x44ea50 (dispatcher, dead player on a plain event): with the partner present the dead one leaves the game
// (GD+0xa2 1/2) and control moves on; both down: a life (GD+0xa0) brings both back with 6 health at the checkpoint
// 0x4d26e0 (the focus arrives there and calls the partner, forced); no life left: level 5 (RETRY), 3 lives.
void TbWorld::playerDown(TbActor& a) {
    // An absent player (GD+0xa2 1/2: already out after its own death) keeps getting plain events from its idle
    // logic; the runtime capture shows lives untouched then (D: GD+0xa0 3, 0xa2 1), so only the first call counts (I).
    if (GD[0xa2] & (a.id == 2 ? 1 : 2)) return;
    a.vel = {};
    TbActor& f = actors[size_t(focus())];
    if (!(GD[0xa2] & 3)) {
        if (&a == &f) A4 ^= 4;
        GD[0xa2] = a.id == 2 ? uint8_t((GD[0xa2] | 1) & ~0x10) : uint8_t((GD[0xa2] | 2) & ~0x20);
        return;
    }
    f.vel = {};
    GD[0xa2] &= 0xcc;
    for (TbActor& p : actors) p.f20 &= ~0x2000000u, p.hp = 6;
    if (!GD[0xa0]) {
        restart = true, nextLevel = 5;  // GD+0 = 5 (RETRY), 0x4d593a = 1
        GD[0xa5] = GD[0xa6] = 6, GD[0xa0] = 3;
        return;
    }
    --GD[0xa0];
    teleport(f, {checkpoint[0], checkpoint[1], checkpoint[2]});
    f.yaw = lookYaw_;  // checkpoint yaw +0x4d26ec is overwritten by DAT_004d5972
    f.attachPart = -1;
    f.f20 |= 0x10000000;
    f.f24 &= ~0x40u;
    if (f.progMask[0] & 0x10000000) runProgram(f, 0x10000000);
    callFriend(true);
}

// V 8 / V 0x80000010: block p1 (entity list < 10000, else global) on every listed entity with flags0 0x8000
// within radius of the caller; its event is dispatched there.
void TbWorld::runNear(TbActor& a, uint32_t block, uint32_t radius) {
    std::vector<TbActor*> near;
    for (TbActor* e : order())
        if (e->active && (e->flags0 & 0x8000) && dist2(e->pos, a.pos) <= int64_t(radius) * radius) near.push_back(e);
    for (TbActor* e : near)
        if (TbBlock* b = findBlock(*e, uint16_t(block))) {
            const uint32_t r = runItems(*e, *b);
            if (!(r & 0x90000000)) dispatch(*e, r);
        }
}

// V 4 / 0x80000001 / 0x100000 placement: detached, not grounded, sleep counter cleared.
void TbWorld::teleport(TbActor& t, V3 p) {
    if (t.parent >= 0) {
        t.f20 &= ~0x208u;
        detach(t);
    }
    t.groundPoly = -1;
    t.f20 &= ~2u;
    t.pos = p;
    t.sleep = 0;
}

}  // namespace bb::game
