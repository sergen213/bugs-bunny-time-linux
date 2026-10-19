// LiT actors (GAMEPLAY.md §7): activation pass and update order of 0x447d40, the generic type-14 interpreter
// 0x440120, the type-2 script graph 0x4455e0, moving platforms 0x43f520, logic directors 0x4445e0 / 0x444b50,
// NPC steering 0x437f40 and Bugs' contacts with actor boxes (0x433500 / 0x432fe0 / 0x433380, approximated).
#include "bb/game/lit_world.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bb::game {
namespace {

constexpr int16_t kSteer[9] = {0, 0, 0, 341, -341, 682, -682, 1023, -1023};    // 0x4add98
constexpr int16_t kSteerFine[9] = {0, 0, 0, 256, -256, 512, -512, 768, -768};  // 0x4addb0 (flagsA 0x80000)
constexpr int32_t kPlayerHalf = 70;  // default player box ±70 / ±40 (§3.7); one half-width for both axes (approximation)

int64_t dist2(const V3& a, const V3& b, bool y) {
    const int64_t dx = a.x - b.x, dy = y ? a.y - b.y : 0, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}
int32_t dist(const V3& a, const V3& b, bool y) { return int32_t(std::sqrt(double(dist2(a, b, y)))); }

const PlayerState* stateById(const ActorDef& d, uint16_t id) {
    for (const PlayerState& s : d.states)
        if (s.id == id) return &s;
    return nullptr;
}
int16_t stepById(const ActorDef& d, int32_t id) {
    for (size_t k = 0; k < d.steps.size(); ++k)
        if (d.steps[k].id == id) return int16_t(k);
    return -1;
}

// Point q (PS1 world) inside actor a's box (yaw frame, inflated by +0x20) grown by `grow` horizontally, `up` above
// the top and `down` below the bottom (+Y is down).
bool inBox(const LitActor& a, const V3& q, int32_t grow, int32_t up, int32_t down) {
    const int64_t dx = q.x - a.pos.x, dz = q.z - a.pos.z;
    if (dx * dx + dz * dz > 4000ll * 4000) return false;
    const V3 l = rotY(0x1000 - a.yaw, int32_t(dx), 0, int32_t(dz));
    const int32_t g = grow + a.inflate, ly = q.y - a.pos.y;
    return l.x >= a.box[0] - g && l.x <= a.box[3] + g && l.z >= a.box[2] - g && l.z <= a.box[5] + g &&
           ly >= a.box[4] - a.inflate - up && ly <= a.box[1] + a.inflate + down;
}

} // namespace

LitActor LitWorld::instantiate(size_t i) const {
    const ActorDef& d = defs[i];
    LitActor a;
    a.def = int32_t(i);
    a.type = d.type;
    a.flags8 = d.flags8, a.flagsC = d.flagsC;
    a.pos = d.pos;
    a.rot = d.rot;
    a.yaw = d.rot[1];
    a.scale = d.scale;
    a.box = d.box;
    a.inflate = d.inflate;
    a.act3c = d.act3c, a.act3e = d.act3e;
    a.param = d.param;
    a.reg = d.reg;
    a.zone = d.zone;
    a.entityId = d.entityId;
    return a;
}

int LitWorld::findActor(uint16_t entityId) const {
    for (size_t k = 0; k < actors.size(); ++k)
        if (!actors[k].dead && !(actors[k].flags14 & 0x100) && actors[k].entityId == entityId) return int(k);
    return -1;
}

// 0x448d40: copy of template `entityId` (chunk 0x08) at pos / yaw. Dead slots are reused and the pool never
// reallocates during a tick (behaviours hold references): pool full -> -1, like the exe's fixed actor pool.
int LitWorld::spawn(uint16_t entityId, const V3& pos, int16_t yaw) {
    for (size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].chunk != 0x08 || defs[i].entityId != entityId) continue;
        LitActor a = instantiate(i);
        a.pos = pos;
        a.yaw = yaw;
        for (size_t k = 0; k < actors.size(); ++k)
            if (actors[k].dead && int(k) != p.platform) {
                actors[k] = std::move(a);
                return int(k);
            }
        if (actors.size() == actors.capacity()) return -1;
        actors.push_back(std::move(a));
        return int(actors.size() - 1);
    }
    unsupported.insert(std::format("spawn template {}", entityId));
    return -1;
}

uint32_t LitWorld::animate(LitActor& a, uint16_t anim, uint32_t mode) {
    const ActorDef& d = defs[size_t(a.def)];
    const uint32_t r = scene_ && d.model >= 0 ? animTick(scene_->actorModels[size_t(d.model)], a.animState, anim, mode) : AnimMissing;
    if (r != AnimMissing) return r;
    // ponytail: no TOD for this id (no model / sprite-only actor): frame 0 on the starting tick, then finished, so
    // frame-0 and clip-end item gates each fire once (the exe's 0x4393e0 / 0x438970 timing is not reproduced).
    if (anim) { a.animState.frame = 0, a.animState.countdown = 0, a.animState.flags = 0; return AnimPlaying; }
    a.animState.frame = 0xffff;
    a.animState.flags |= 0x2000;
    return AnimFinished;
}

// Script start / state switch: state `stateId` at step 0; the animation restarts unless the new step has mode
// bit 0x40 and plays the same animation (0x440120 / 0x4455e0).
void LitWorld::startScript(LitActor& a, uint16_t stateId) {
    const ActorDef& d = defs[size_t(a.def)];
    const PlayerState* s = stateById(d, stateId);
    if (!s) return;
    const uint16_t prevAnim = a.step >= 0 ? d.steps[size_t(a.step)].anim : 0;
    a.state = int16_t(s - d.states.data());
    a.seq = 0;
    a.step = stepById(d, s->moves[0]);
    if (a.step < 0 || !(d.steps[size_t(a.step)].mode & 0x40) || d.steps[size_t(a.step)].anim != prevAnim) a.anim = 0;
}

// ---- 0x447d40: activation and update order ----

void LitWorld::actorsTick() {
    if (actors.capacity() < actors.size() + 256) actors.reserve(actors.size() + 512);  // spawn headroom (see spawn())
    const V3 ref = (cam.eye.x | cam.eye.y | cam.eye.z) ? cam.eye : p.pos;  // activation reference: camera eye
    const V3 fwd{cam.target.x - cam.eye.x, 0, cam.target.z - cam.eye.z};
    const double fl = std::max(1.0, std::sqrt(double(fwd.x) * fwd.x + double(fwd.z) * fwd.z));
    std::vector<size_t> first, solid, plats, rest;  // type 0x15, flags8 0x20000, flags8 8, others (each LIFO)
    for (size_t i = 0; i < actors.size(); ++i) {
        LitActor& a = actors[i];
        a.active = false;
        if (a.dead || (a.flagsC & 0x40)) continue;
        a.visible = true;
        a.flags18 &= ~0x100u;
        if (a.entityId != 5 && a.type != 20 && int(i) != p.platform && a.act3c != 0) {
            if ((a.act3c & 2) && dist(ref, a.pos, true) > a.act3e) {  // beyond +0x3e
                if (!(a.act3c & 0x40)) continue;
                a.visible = false;
                a.flags18 |= 0x100;
            }
            // +0x3c & 1 portal-zone visibility: not simulated (always visible). & 0x20: projected centre within
            // ±0x100 of the 512-wide 4:3 reference at H = 400 (vertical test and multi-part corners omitted).
            if ((a.act3c & 0x20) && a.visible) {
                const double rx = a.pos.x - cam.eye.x, rz = a.pos.z - cam.eye.z;
                const double cz = (rx * fwd.x + rz * fwd.z) / fl, cx = (rx * fwd.z - rz * fwd.x) / fl;
                const double r = std::max({std::abs(a.box[0]), std::abs(a.box[3]), std::abs(a.box[2]), std::abs(a.box[5])});
                if (cz <= 0 || std::abs(cx * 400 / cz) - r * 400 / cz > 0x100) {
                    if (!(a.act3c & 0xc0)) continue;
                    a.visible = false;
                    a.flags18 |= 0x100;
                }
            }
        }
        if (a.type == 16 || a.type == 36) {
            a.visible = false;
            a.flags18 |= 0x100;
        }
        a.active = true;
        (a.type == 0x15 ? first : (a.flags8 & 0x20000) ? solid : (a.flags8 & 8) ? plats : rest).push_back(i);
    }
    auto run = [&](const std::vector<size_t>& v) {
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            if (!(actors[*it].flags14 & 0x100)) behaviour(*it);
            LitActor& a = actors[*it];
            ++a.timer;
            if (a.flags14 & 0x100) {
                a.dead = true;  // freed by 0x447a50
                if (p.platform == int(*it)) { p.platform = -1; p.flags14 &= ~0x14u; }
            }
        }
    };
    run(first);
    run(solid);
    run(plats);
    playerTick();
    ++p.timer;
    run(rest);
    if (p.carrier >= 0) {  // attached coordinate: Bugs at the carrier's origin (0x11c local offset zero)
        const LitActor& c = actors[size_t(p.carrier)];
        if (c.dead) {
            p.carrier = -1;
            p.flags14 &= ~4u;
        } else {
            p.pos = c.pos;
            p.yaw = c.yaw;
            p.zone = c.zone;
        }
    }
}

void LitWorld::behaviour(size_t i) {
    switch (actors[i].type) {
    case 2: type2(i); return;
    case 9: {  // 0x4471c0: sky dome; first call builds the parts (0x4393e0 anim 5), then follows the camera eye
        LitActor& a = actors[i];
        if (a.animState.parts.empty()) animate(a, 5, AnimOnce);
        a.pos = {cam.eye.x, cam.eye.y + a.reg, cam.eye.z};
        return;
    }
    case 12: platform(i, 2); return;
    case 13: platform(i, 8); return;
    case 14: type14(i); return;
    case 16: director(i); return;
    case 36: cameraDirector(i); return;
    case 18: platform(i, 1); return;
    case 19: platform(i, 4); return;
    case 22: platform(i, 0x10); return;
    case 23: platform(i, 0x20); return;
    case 24: platform(i, 0x22); return;
    case 25: platform(i, 0x40); return;
    case 26: platform(i, 0x80); return;
    case 27: platform(i, 0x100); return;
    case 28: platform(i, 0x200); return;
    case 29: platform(i, 0x400); return;
    case 3: case 7: case 11: case 15: case 17: case 20: case 21: case 30: case 33: case 35: return;  // no-op / texture link
    default: {
        // 0 static prop, 4 flipbook effect, 5 pushable, 8 rope, 10 tagger, 31 projectile, 32 overlay, 34 attached:
        // not simulated; TOD models loop their initial animation as in the viewer.
        LitActor& a = actors[i];
        const ActorDef& d = defs[size_t(a.def)];
        if (a.type != 0) unsupported.insert(std::format("actor type {}", a.type));
        if (d.idleAnim) {
            animate(a, a.anim == d.idleAnim ? 0 : d.idleAnim, a.anim == d.idleAnim ? 0 : AnimLoop);
            a.anim = d.idleAnim;
        }
    }
    }
}

// ---- items (LOGIC.md §3.1, 0x440120 / 0x4455e0): scan the step's event group; returns true when the actor died ----

bool LitWorld::runItem(size_t i, const ActorItem& it, bool t14) {
    LitActor& a = actors[i];
    const uint32_t f = it.flags;
    if (t14 && ((it.mask14 && !(a.flags14 & it.mask14)) || (it.mask18 && !(a.flags18 & it.mask18)))) return false;
    if (f & 0x40) {
        if (a.animState.frame != it.p1 || a.animState.countdown != 0) return false;
    } else if (f & 4) {
        if (!(a.animState.flags & 0x2000) || a.animState.countdown != 0) return false;
    }
    if ((f & 2) && uint32_t(rand_() & 0xffff) <= uint32_t(it.p1)) return false;
    else if (!(f & 2) && (f & 0x20000) && uint32_t(rand_() & 0xffff) <= uint32_t(it.p2)) return false;
    if (it.item.cond && !cond(it.item, &a)) return false;
    if (t14 && (f & 0x2000) && a.contact != it.p2) return false;
    if (t14 && (f & 0x58400000)) {  // target gates
        const Mover* tgt = &p;
        LitActor* ta = nullptr;
        if (!(f & 0x8000000)) {
            const int k = findActor(uint16_t(it.p2));
            if (k < 0) return false;
            ta = &actors[size_t(k)];
            tgt = ta;
        }
        if (f & 0x400000) {
            unsupported.insert("item flag 0x400000");
        } else if (f & 0x18000000) {
            const int32_t d = dist(tgt->pos, a.pos, f & 1);
            if ((f & 0x200000) ? d < it.p1 : d > it.p1) return false;
        } else if ((f & 0x40000000) && (!ta || ta->animState.frame != it.p1)) {
            return false;
        }
    }
    if (it.item.act) act(it.item, &a);
    return true;
}


// ---- type 14 (0x440120) ----

void LitWorld::type14(size_t i) {
    LitActor* ap = &actors[i];
    const ActorDef& d = defs[size_t(ap->def)];
    if (d.steps.empty() || d.states.empty()) return;
    if (ap->state < 0) {  // first call: start state 2, else 1
        ap->flags18 |= 0x80;
        startScript(*ap, stateById(d, 2) ? 2 : 1);
        ap->phase = 0x800;
        ap->jumpH = 0x80;
        ap->wander = ap->wanderDir = 0;
        if (ap->state < 0) return;
    }
    LitActor& a = *ap;
    auto step = [&]() -> const ActorStep& { return d.steps[size_t(a.step)]; };
    // Hurt (§7.3): hit bits and a vulnerable step -> state 3 (HP -= +0x38), HP < 1 -> state 4 unless immortal.
    const bool hit = (a.flags14 & 0x6000000) && a.step >= 0 &&
                     (((step().flagsA & 0x4000) && (a.flags14 & 0x4000000)) ||
                      ((step().flagsB & 0x8000) && (a.flags14 & 0x80) && (a.flags14 & 0x4000000)) ||
                      ((step().flagsB & 8) && (a.flags14 & 0x2000000)));
    if (!hit) {
        const int16_t sid = d.states[size_t(a.state)].moves[std::min<size_t>(a.seq, 4)];
        if (sid == -16 || a.seq > 4 || a.step < 0) {
            startScript(a, 1);
            a.flags14 &= 0xf9ffffff;
        }
    } else {
        a.reg = int16_t(a.reg - a.damage);
        a.damage = 0;
        const uint16_t to = !(a.flags8 & 0x200) && a.reg < 1 ? 4 : 3;
        if (!stateById(d, to)) {
            if (to == 4) { a.flags14 |= 0x100; return; }
        } else {
            startScript(a, to);
            a.flags14 &= ~9u;
            if (a.step >= 0 && (step().flagsB & 0x4000)) a.yaw = int16_t(p.yaw + 0x800);
        }
    }
    if (a.step < 0) return;
    const ActorStep* st = &step();
    const uint32_t fa = st->flagsA, fb = st->flagsB;

    // Attacks on Bugs (flagsA 0x10000 hand, 0x800000 body contact), damage via 0x437d90 then knock-back state +0x1c.
    if (!(fa & 0x830000) || (a.flags14 & 0x81000)) {
        a.flags14 &= 0xdfffffbf;
    } else {
        a.flags14 |= 0x40;
        if ((fa & 0x10000) && !(p.flags14 & 0x3000000)) {
            // Hand point = TOD attach point +0x58 (packet 0xa, not decoded): 150 in front, 200 up (approximation).
            const V3 hand = [&] { const V3 o = rotY(a.yaw, 0, 0, 150); return V3{a.pos.x + o.x, a.pos.y - 200, a.pos.z + o.z}; }();
            const V3 lp = rotY(0x1000 - p.yaw, hand.x - p.pos.x, 0, hand.z - p.pos.z);
            if (std::abs(lp.x) <= kPlayerHalf && std::abs(lp.z) <= kPlayerHalf && hand.y >= p.pos.y + boxTop() && hand.y <= p.pos.y &&
                damagePlayer(a, st) && st->w[4])
                forceState(uint16_t(st->w[4]));
        }
        if (fa & 0x800000) {
            a.flags14 |= 0x20000000;
            if ((!(p.flags14 & 0x3000000) || (a.flags8 & 0x40000000)) && (a.flags14 & 0x8000) && damagePlayer(a, st) && st->w[4])
                forceState(uint16_t(st->w[4]));
        }
    }

    // Movement (local velocity: x, y, z forward).
    int32_t lx = 0, ly = 0, lz = 0;
    if (!(fa & 1) && !(a.flags14 & 0x800)) {
        if (fb & 1) {  // vertical sine bob around the start height: phase +0x18, amplitude +0x1a
            if (!(a.flags14 & 1)) {
                a.phase = 0;
                a.base.y = a.pos.y;
                a.jumpH = st->w[3];
                a.flags14 |= 1;
            }
            a.pos.y = ((rsin(a.phase) * a.jumpH) >> 12) + a.base.y;
            a.phase = int16_t((st->w[2] + a.phase) & 0xfff);
        }
        if (fa & 0x20040000) unsupported.insert("type14 flagsA 0x20040000 (hover)");
        if (fa & 0x80000000) unsupported.insert("type14 flagsA 0x80000000 (player-steered)");
        else if (fa & 2) lz += (fb & 0x400000) ? int8_t(S[uint8_t(st->w[0])]) * 2 : st->w[0];
        if (fa & 0x400) ly -= st->w[3];
        if (fa & 0x800) ly += st->w[3];
        if (fa & 0x2000000) {  // random wander: ±10 yaw per tick for rand & 0x1ff ticks
            if (a.wander == 0) {
                a.wander = int16_t(rand_() & 0x1ff);
                a.wanderDir = int16_t(rand_() & 1);
            } else {
                --a.wander;
                a.yaw = int16_t(a.yaw + (a.wanderDir ? 10 : -10));
            }
        }
    }
    if (fb & 0x10000) lz += (fb & 0x400000) ? int8_t(S[uint8_t(st->w[0])]) : st->w[0];
    if (fb & 0x10) a.yaw = int16_t(ratan2(cam.eye.x - a.pos.x, cam.eye.z - a.pos.z));
    if (fa & 0x10209000) {  // turn toward (0x10000000: away from) Bugs, or actor +0x16 with 0x200000
        const Mover* tgt = &p;
        if (fa & 0x200000) {
            const int k = findActor(uint16_t(st->w[1]));
            tgt = k >= 0 ? &actors[size_t(k)] : nullptr;
        }
        if (tgt) {
            const bool away = fa & 0x10000000;
            const int32_t ang = ratan2(away ? a.pos.x - tgt->pos.x : tgt->pos.x - a.pos.x, away ? a.pos.z - tgt->pos.z : tgt->pos.z - a.pos.z);
            if (fa & 0x1000) {
                a.yaw = int16_t(ang);
            } else {
                const int32_t dlt = angleDiff(ang, a.yaw);
                if (!(fb & 0x400)) {
                    const int32_t shift = a.param ? a.param : 4;
                    if (fb & 0x20) a.yaw = int16_t(a.yaw + (dlt >> 1));
                    else if (const int32_t r = dlt >> (shift & 31); (shift & 31) && r) a.yaw = int16_t(a.yaw + r);
                    else a.yaw = int16_t(a.yaw + dlt);
                } else if (dlt < -a.param) {
                    a.yaw = int16_t(a.yaw - a.param);
                } else if (a.param < dlt) {
                    a.yaw = int16_t(a.yaw + a.param);
                }
            }
        }
    }
    V3 w = rotY(a.yaw, lx, ly, lz);
    // Body contact on Bugs through the actor collision (0x433500 mask 0x20108, flagsA 0x800000).
    if ((fa & 0x800000) && !(fa & 0x400000) && !(p.flags14 & 0x2000000) &&
        inBox(a, {p.pos.x - w.x, p.pos.y, p.pos.z - w.z}, kPlayerHalf, 0, -boxTop()) && damagePlayer(a, st) && st->w[4])
        forceState(uint16_t(st->w[4]));
    // Ballistic hop (flagsA 0x10): the exe aims the arc at a target; a fixed arc +0x18 step / +0x1a height here.
    if ((fa & 0x10) && !(a.flags14 & 0x209)) {
        a.phase = 0x400;
        a.jumpH = st->w[3];
        a.jumpStep = st->w[2];
        a.vy = 0;
        a.flags14 = (a.flags14 & 0xeffff9f1) | 1;
    }
    if ((fa & 0x10) && (a.flags14 & 1)) arcTick(a);
    if ((w.x | w.y | w.z) && !(fa & 0x80000000) && !(fb & 0x20002)) {
        // 0x437f40: try the heading offsets until the step neither hits a wall, drops > 200 (unless 0x80000), nor
        // changes zone (unless 0x40000000); none -> turn around and stand.
        const int16_t* tab = (fa & 0x80000) ? kSteerFine : kSteer;
        int k = 0;
        V3 got{};
        for (; k < 9; ++k) {
            const V3 v = rotY(tab[k], w.x, w.y, w.z);
            V3 q = a.pos;
            int32_t dx = v.x, dz = v.z;
            Ground r;
            r.zone = a.zone;
            hf.trace(q, dx, dz, r);
            bool drop = false;
            if (!(a.flags14 & 4)) {
                if (r.kind & KindOpen) {
                    V3 b{a.pos.x + dx, a.pos.y + 1, a.pos.z + dz};
                    Ground g;
                    g.zone = a.zone;
                    hf.groundAt(b, g);
                    drop = g.zone == -1 || ((r.kind & KindFloor) && g.groundY - a.pos.y > 200);
                } else if ((r.kind & KindFloor) && !(a.flags14 & 0x219) && r.hoff + q.y - a.pos.y > 200) {
                    drop = true;
                }
            }
            const bool shortened = (v.x + v.z) != (dx + dz), blocked = r.kind & KindBlocked;
            if (blocked || (!(fa & 0x80000) && drop) || shortened) a.flags14 |= 0x100000;
            if (drop) a.flags18 |= 0x20;
            const bool zoneChange = !(fa & 0x40000000) && r.zone != -1 && r.zone != a.zone;
            got = {dx, v.y, dz};
            if (!zoneChange && !blocked && (!drop || (fa & 0x80000)) && !shortened) break;
        }
        if ((!(a.flags18 & 0x20) || (fa & 0x80000)) && ((a.flags14 & 0x100000) || k > 8)) a.yaw = int16_t(a.yaw + (tab[std::min(k, 8)] >> 3));
        else a.yaw = int16_t(a.yaw + tab[std::min(k, 8)]);
        if (k == 9 && (a.flags14 & 0x100000)) {
            a.yaw = int16_t(a.yaw + 0x800);
            w = {};
        } else {
            w = got;
        }
    }
    if (!(fa & 0x80000000) || (fb & 4)) {
        const int32_t keepY = w.y;
        Ground r;
        moveActor(a, w, r, a.box[4]);
        if (fb & 4) w.y = keepY;  // flies: no gravity
        if (r.kind & KindOpen) a.flags18 |= 0x20;
    }
    if (!(fa & 1) || (fb & 4)) {
        a.pos.x += w.x;
        a.pos.y += w.y;
        a.pos.z += w.z;
    }

    // Items of the step's event group.
    if (st->group) {
        auto itb = std::ranges::find_if(d.items, [&](const ActorItem& x) { return x.group == st->group; });
        for (; itb != d.items.end() && itb->group == st->group; ++itb) {
            const ActorItem& it = *itb;
            if (!runItem(i, it, true)) continue;
            const uint32_t f = it.flags;
            if (f) {
                if (f & 0x200) playSound(uint32_t(it.p1));
                if (f & 0x40100) {
                    const int k = spawn(uint16_t(it.p2), a.pos, a.yaw);
                    if (k >= 0) {
                        if (f & 0x1000000) actors[size_t(k)].reg = a.reg;
                        if ((f & 0x40000) && !(f & 0x80)) actors[size_t(k)].yaw = int16_t(rand_() & 0xfff);
                        actors[size_t(k)].zone = a.zone;
                    }
                } else if (f & 0x20000000) {  // attach actor P2 to self (carried, +0x6c); Bugs is P2 = his entity id
                    if (uint16_t(it.p2) == p.entityId && !(p.flags14 & 0x1000000)) {
                        if (f & 0x1000000) p.reg = a.reg;
                        p.carrier = int32_t(i);
                        p.platform = -1;
                        p.flags14 = (p.flags14 & ~0x10u) | 4;
                        a.rider = -2;
                    } else {
                        unsupported.insert("item flag 0x20000000 (attach actor)");
                    }
                } else if (f & 0x1000) {  // attach self to Bugs: snaps to his position (kept until flag 8 detaches)
                    a.pos = p.pos;
                    a.yaw = p.yaw;
                    a.zone = p.zone;
                } else if (f & 0x20) {
                    unsupported.insert("item flag 0x20 (0x434740)");
                    continue;
                } else if (f & 0x4000000) {
                    if (const int k = findActor(uint16_t(it.p2)); k >= 0) actors[size_t(k)].flags14 |= 0x100;
                }
                if (f & 0x80000) nextLevel = it.p1;  // goto level P1
                if (f & 0x80000000) forceState(uint16_t(it.p2));
                if (f & 0x100000) { checkpoint_ = a.pos; checkpointYaw_ = a.yaw; }
                if (f & 0x800) { p.pos = a.pos; p.yaw = a.yaw; p.platform = -1; p.flags14 &= ~0x14u; cameraSnap(p.pos); }
                if ((f & 0x4000) && a.rider == -2) {  // release the carried actor (Bugs) where he is
                    a.rider = -1;
                    p.carrier = -1;
                    p.flags14 = (p.flags14 & 0xfffff5f6) | 0x1000;
                }
                if (f & 0x10000) {
                    if (it.p2 != 1) { a.flags14 |= 0x100; return; }
                    kill(int16_t(it.p1));
                }
                if (f & 0x2000000) { a.flags14 |= 0x100; return; }
                if (f & 0x8000) continue;
                if (f & 0x10) break;
            }
            startScript(a, it.next);
            a.flags14 &= 0xf9fffdf6;
            break;
        }
    }
    if (a.step < 0) return;
    st = &step();
    if ((a.flags8 & 0x1000000) || (st->flagsB & 0x800000)) zoneTriggers(a, &a);

    // Animation: the step's clip; end -> next step (flagsA 4: die), flagsA 0x80 also advances on landing.
    const bool same = a.anim == st->anim;
    const uint32_t res = same ? animate(a, 0, 0) : animate(a, st->anim, uint16_t(st->mode));
    a.anim = st->anim;
    if (res == AnimFinished || (same && (st->flagsA & 0x80) && (a.flags14 & 0x12))) {
        a.flags14 &= 0xf9f7efff;
        a.anim = 0;
        if (st->flagsA & 4) { a.flags14 |= 0x100; return; }
        ++a.seq;
        const int16_t sid = a.seq < 5 ? d.states[size_t(a.state)].moves[a.seq] : int16_t(-16);
        if (uint16_t(sid) != 0xfff0 && a.seq < 5) {
            a.step = stepById(d, sid);
            a.animState.frame = 0;
            a.animState.countdown = 0;
            if (a.step < 0) a.step = 0;
        }
    }
    if (a.flagsC & 0x20000000) a.pos = {cam.eye.x, cam.eye.y + a.reg, cam.eye.z};  // follows the camera eye
    a.flags14 &= 0xb76f777f;
    a.flags18 &= 0xffff9fdf;
    a.contact = 0;
}

// ---- type 2 script graph (0x4455e0) ----

void LitWorld::type2(size_t i) {
    LitActor& a = actors[i];
    const ActorDef& d = defs[size_t(a.def)];
    if (d.steps.empty() || d.states.empty()) return;
    if (a.state < 0) {
        a.flags18 |= 0x80;
        a.flags8 = (a.flags8 & 0xffdfffff) | 0x20;
        startScript(a, stateById(d, 2) ? 2 : 1);
        if (a.state < 0) return;
    }
    const int16_t sid = d.states[size_t(a.state)].moves[std::min<size_t>(a.seq, 4)];
    if (sid == -16 || a.seq > 4 || a.step < 0) startScript(a, 1);
    if (a.step < 0) return;
    const ActorStep& st = d.steps[size_t(a.step)];
    if (st.group) {
        auto itb = std::ranges::find_if(d.items, [&](const ActorItem& x) { return x.group == st.group; });
        for (; itb != d.items.end() && itb->group == st.group; ++itb) {
            const ActorItem& it = *itb;
            if (!runItem(i, it, false)) continue;
            const uint32_t f = it.flags;
            if (f) {
                if (f & 0x200) playSound(uint32_t(it.p1));
                if (f & 0x4000000)
                    if (const int k = findActor(uint16_t(it.p2)); k >= 0) actors[size_t(k)].flags14 |= 0x100;
                if (f & 0x80000) nextLevel = it.p1;
                if (f & 0x80000000) forceState(uint16_t(it.p2));
                if (f & 0x10000) { a.flags14 |= 0x100; return; }
                if (f & 0x8000) continue;
                if (f & 0x10) break;
            }
            startScript(a, it.next);
            return;  // 0x4455e0 returns after a state switch (the new step animates next tick)
        }
    }
    const ActorStep& s2 = d.steps[size_t(a.step)];
    const bool same = a.anim == s2.anim;
    const uint32_t res = same ? animate(a, 0, 0) : animate(a, s2.anim, uint16_t(s2.mode));
    a.anim = s2.anim;
    if (res == AnimFinished) {
        a.anim = 0;
        if (s2.flagsA & 4) { a.flags14 |= 0x100; return; }
        ++a.seq;
        const int16_t n = a.seq < 5 ? d.states[size_t(a.state)].moves[a.seq] : int16_t(-16);
        if (uint16_t(n) != 0xfff0 && a.seq < 5) a.step = std::max<int16_t>(0, stepById(d, n));
    }
}

// ---- logic directors (types 16 0x4445e0 / 36 0x444b50): run the whole list each tick, first firing item stops ----

void LitWorld::director(size_t i) {
    const ActorDef& d = defs[size_t(actors[i].def)];
    for (size_t k = 0; k < d.items.size(); ++k) {
        LitActor& a = actors[i];
        const ActorItem& it = d.items[k];
        const uint32_t f = it.flags;
        if ((f & 2) ? uint32_t(rand_() & 0xffff) <= uint32_t(it.p1)
                    : (f & 0x20000) && uint32_t(rand_() & 0xffff) <= uint32_t(it.p2))
            continue;
        if (it.item.cond && !cond(it.item, &a)) {
            if (it.next)  // failed condition jumps to the item group `next` (0x4445e0)
                for (size_t j = k + 1; j < d.items.size(); ++j)
                    if (d.items[j].group == it.next) { k = j - 1; break; }
            continue;
        }
        if (f & 0x18400000) {
            if (f & 0x400000) { unsupported.insert("director flag 0x400000"); continue; }
            const Mover* tgt = &p;
            if (f & 0x10000000) {
                const int t = findActor(uint16_t(it.p2));
                if (t < 0) continue;
                tgt = &actors[size_t(t)];
            }
            const int32_t dd = dist(tgt->pos, a.pos, f & 1);
            if ((f & 0x200000) ? dd < it.p1 : dd > it.p1) continue;
        }
        if (it.item.act) act(it.item, &a);
        if (f & 0x200) playSound(uint32_t(it.p1));
        if (f & 0x40100) {
            const V3 at = a.pos;
            const int16_t yaw = a.yaw, reg = a.reg, zone = a.zone;
            if (const int s = spawn(uint16_t(it.p2), at, yaw); s >= 0) {
                actors[size_t(s)].zone = zone;
                if (f & 0x40000) actors[size_t(s)].yaw = int16_t(rand_() & 0xfff);
                if (f & 0x1000000) actors[size_t(s)].reg = reg;
                if (f & 0x1000) unsupported.insert("director flag 0x1000 (attach spawn to Bugs)");
            }
        }
        LitActor& b = actors[i];
        if (f & 0x4000000)
            if (const int t = findActor(uint16_t(it.p2)); t >= 0) actors[size_t(t)].flags14 |= 0x100;
        if (f & 0x800) { p.yaw = b.yaw; p.pos = b.pos; p.platform = -1; p.flags14 = (p.flags14 & ~0x14u) | 0x80000000; cameraSnap(p.pos); }
        if (f & 0x80000000) forceState(uint16_t(it.p2));
        if (f & 0x80000) nextLevel = it.p1;
        if (f & 0x10000) { b.flags14 |= 0x100; return; }
        if (!(f & 0x8000)) return;
    }
}

// ---- type 36 camera director 0x444b50: items set the scripted camera points read by the camera's 0xc00 mode.
// Item layout as type 16 (tag 0x31): point = (P1, P2, u16 `next`). Flags: 0x8000/0x10 distance gate (<= / >= P1) to
// Bugs (0x20) or actor P2 (0x40), y with 0x100; 1 eye point (self 0x80 / actor P2 0x40 / cam.eye 0x100000 / behind
// Bugs 0x80000 / literal) + 0x40000 pull toward the eye, sets 0x400; 4 clears 0x400|0x4000; 2 target point (self
// 0x80 / actor P2 0x40 / cam.target 0x100000 / literal), sets 0x800; 8 clears 0x800|0x4000; 0x400 fixed eye distance
// P1 (0x1000), 0x800 clears it; 0x10000 commits eye/target to the follow copies (0x4000) and stops; 0x200 frees self
// and stops; 0x1000 continues with the next item, otherwise stop. ----

void LitWorld::cameraDirector(size_t i) {
    const ActorDef& d = defs[size_t(actors[i].def)];
    for (const ActorItem& it : d.items) {
        LitActor& a = actors[i];
        const uint32_t f = it.flags;
        if (it.item.cond && !cond(it.item, &a)) continue;
        if (f & 0x4000) cam.flags |= 8;
        if (f & 0x8010) {
            // ponytail: neither 0x20 nor 0x40 reuses the exe's stale scratch target; treated as no target (skip)
            const int k = (f & 0x20) ? -1 : (f & 0x40) ? findActor(uint16_t(it.p2)) : -2;
            if (k == -2 || ((f & 0x20) == 0 && k < 0)) continue;
            const uint32_t dd = uint32_t(dist(k < 0 ? p.pos : actors[size_t(k)].pos, a.pos, f & 0x100));
            if ((f & 0x10) ? dd < uint32_t(it.p1) : dd > uint32_t(it.p1)) continue;
        }
        if (it.item.act) act(it.item, &a);
        const V3 lit{it.p1, it.p2, int32_t(it.next)};
        auto actorPos = [&](bool self) -> const V3* {
            if (self) return &actors[i].pos;
            const int k = findActor(uint16_t(it.p2));
            return k < 0 ? nullptr : &actors[size_t(k)].pos;
        };
        if (f & 1) {
            const V3* src = (!(f & 0x40) || (f & 2)) ? ((f & 0x80) ? &actors[i].pos : nullptr) : actorPos(false);
            if (src && (f & 0x20000)) unsupported.insert("camera director flag 0x20000 (point relative to actor)");
            if (src) cam.point = *src;
            else if (!(f & 0x80000)) cam.point = (f & 0x100000) ? cam.eye : lit;
            else {  // behind Bugs: R(yaw + 0x800)·(0, -P2, P1), both points eased >> 2
                const V3 v = rotY(p.yaw + 0x800, 0, int16_t(-it.p2), int16_t(it.p1));
                cam.point.x += (v.x - cam.point.x + p.pos.x) >> 2;
                cam.point.y += (v.y - cam.point.y + p.pos.y) >> 2;
                cam.point.z += (v.z - cam.point.z + p.pos.z) >> 2;
                cam.lookPoint.x += (p.pos.x - cam.lookPoint.x) >> 2;
                cam.lookPoint.y += (p.pos.y - cam.lookPoint.y) >> 2;
                cam.lookPoint.z += (p.pos.z - cam.lookPoint.z) >> 2;
            }
            if (f & 0x40000) {  // pull the point toward the current eye, keeping 500 (or 7/8 when closer than 1000)
                const V3 e{cam.eye.x - cam.point.x, cam.eye.y - cam.point.y, cam.eye.z - cam.point.z};
                const int32_t len = isqrt(e.x * e.x + e.y * e.y + e.z * e.z) | 1;
                const int32_t s = len - 500 < 500 ? len >> 3 : len - 500;
                if (s > 0) {
                    cam.point.x += s * ((e.x << 12) / len) >> 12;
                    cam.point.y += s * ((e.y << 12) / len) >> 12;
                    cam.point.z += s * ((e.z << 12) / len) >> 12;
                }
            }
            cam.flags |= 0x400;
        } else if (f & 4) {
            cam.flags &= ~0x4400u;
        }
        if (f & 2) {
            if (f & 0x40) {
                if (const V3* t = actorPos(false)) cam.lookPoint = *t;
            } else if (f & 0x80) {
                cam.lookPoint = actors[i].pos;
            } else {
                cam.lookPoint = (f & 0x100000) ? cam.target : lit;
            }
            cam.flags |= 0x800;
        } else if (f & 8) {
            cam.flags &= ~0x4800u;
        }
        if (f & 0x400) { cam.fixedDist = it.p1; cam.flags |= 0x1000; }
        else if (f & 0x800) cam.flags &= ~0x1000u;
        if (f & 0x10000) {
            cam.cand = cam.eyeCopy = cam.candCopy = cam.eye;
            cam.targetCopy = cam.target;
            cam.flags |= 0x4000;
            return;
        }
        if (f & 0x200) { actors[i].flags14 |= 0x100; return; }
        if (!(f & 0x1000)) return;
    }
}

// ---- moving platforms 0x43f520 (mode by type: 12 spin, 13 pendulum, 18 seesaw, 19 rise ridden, 22 slide Z,
// 23 bob, 24 bob+spin, 25 lift A30/A31, 26 sink ridden, 27 conveyor, 28 conveyor when on, 29 door) ----

void LitWorld::platform(size_t i, uint32_t mode) {
    LitActor& a = actors[i];
    const V3 before = a.pos;
    const int16_t yaw0 = a.yaw;
    const bool ridden = p.platform == int(i);
    if (a.state < 0) {  // first call
        a.state = 0;
        animate(a, 5, AnimOnce);
        a.flags18 |= 0x80;
        if (mode & 0x20) {
            a.param = int16_t(a.param >> 1);
            a.base = {a.pos.x, a.pos.y + a.param, a.pos.z};
            a.p17e = 0;
            if (!a.reg) a.reg = 0x10;
        } else if (mode & 0x10) {
            a.param = int16_t(a.param >> 1);
            const V3 o = rotY(a.yaw, 0, 0, a.param);
            a.base = {a.pos.x + o.x, a.pos.y + o.y, a.pos.z + o.z};
            a.p17e = 0;
            if (!a.reg) a.reg = 0x10;
        } else if (mode & 0x400) {
            a.base = a.pos;
            a.p17e = 0;
            if (!a.reg) a.reg = 0x10;
            a.flags14 |= 0x2000;
        } else if (mode & 0xc4) {
            if (!a.reg) a.reg = 5;
            if (a.param < 0) a.hi = a.pos.y, a.lo = a.param + a.pos.y;
            else a.lo = a.pos.y, a.hi = a.param + a.pos.y;
            if (mode & 0x40) a.flags14 |= 0x2000;
        } else if (mode & 8) {
            if (a.param < 0) a.p17e = 0x400, a.param = int16_t(-a.param);
            else a.p17e = 0xc00;
            if (!a.reg) a.reg = 0x22;
        } else if (mode & 1) {
            a.rot[2] = 0;
        }
    }
    auto toward = [&](int32_t target, int32_t speed, uint32_t doneFlag) {  // lifts: move y toward target
        if (a.pos.y < target) {
            a.pos.y = std::min(a.pos.y + speed, target);
            if (a.pos.y == target) a.flags14 |= doneFlag;
        } else if (a.pos.y > target) {
            a.pos.y = std::max(a.pos.y - speed, target);
            if (a.pos.y == target) a.flags14 |= doneFlag;
        }
    };
    if (mode & 2) {
        a.yaw = int16_t(a.yaw + ((mode & 0x20) ? 0x44 : a.param));
    } else if (mode & 1) {  // seesaw: rider tilt (+0x1a4 offset) not modelled; relaxes toward level
        a.rot[2] = int16_t(a.rot[2] - (a.rot[2] >> 4));
    } else if (mode & 0x84) {  // 4 rises while ridden / 0x80 sinks while ridden, 5 units per tick
        a.flags14 &= 0xff9fffff;
        if (bool(mode & 4) == ridden) toward(a.hi, 5, 0x400000);
        else toward(a.lo, 5, 0x200000);
    } else if (mode & 0x40) {  // two-position lift toggled by A30 (0x200000 -> lo) / A31 (0x400000 -> hi)
        const uint32_t f = a.flags14;
        if (f & 0x400000) {
            a.flags14 &= ~0x2000u;
            if (a.pos.y < a.hi) a.pos.y = std::min(a.pos.y + a.reg, a.hi);
            else a.flags14 = (f & 0xff9fdfff) | 0x2000;
        } else if (f & 0x200000) {
            a.flags14 &= ~0x2000u;
            if (a.lo < a.pos.y) {
                a.pos.y -= a.reg;
                if (a.pos.y <= a.lo) a.pos.y = a.lo, a.flags14 |= 0x2000;
            } else {
                a.flags14 = (f & 0xff9fdfff) | 0x2000;
            }
        }
    } else if (mode & 8) {  // pendulum (roll)
        a.flags14 &= 0xff9fffff;
        a.rot[2] = int16_t((rsin(a.p17e) * a.param) >> 12);
        a.p17e = int16_t(a.p17e + a.reg);
        if (a.p17e > 0xfff) a.flags14 |= 0x200000, a.p17e = int16_t(a.p17e - 0x1000);
    } else if (mode & 0x10) {  // oscillate along local Z
        const int32_t z = (rsin(a.p17e) * a.param) >> 12;
        const int16_t ph = a.p17e;
        a.p17e = int16_t((a.reg + ph) & 0xfff);
        a.flags14 = ((ph < 0x400 && a.p17e > 0x3ff) || (ph < 0xc00 && a.p17e > 0xbff)) ? a.flags14 | 0x2000 : a.flags14 & ~0x2000u;
        const V3 o = rotY(a.yaw, 0, 0, z);
        a.pos = {a.base.x + o.x, a.base.y + o.y, a.base.z + o.z};
    } else if (mode & 0x400) {  // sliding door: 0x400000 closes to 0, 0x200000 opens to +0x17c
        const uint32_t f = a.flags14;
        if (f & 0x400000) {
            if (a.p17e < 1) a.p17e = 0, a.flags14 = (f & 0xff9fffff) | 0x2000;
            else a.flags14 &= ~0x2000u, a.p17e = int16_t(a.p17e - a.reg);
        } else if (f & 0x200000) {
            if (a.param <= a.p17e) a.p17e = a.param, a.flags14 = (f & 0xff9fffff) | 0x2000;
            else a.flags14 &= ~0x2000u, a.p17e = int16_t(a.p17e + a.reg);
        }
        const V3 o = rotY(a.yaw, 0, 0, a.p17e);
        a.pos = {a.base.x + o.x, a.base.y + o.y, a.base.z + o.z};
    } else if (ridden && ((mode & 0x100) || ((mode & 0x200) && (a.flags14 & 0x200000)))) {  // conveyor: rider +Z local
        const V3 o = rotY(a.yaw, 0, 0, a.param);
        p.pos.x += o.x;
        p.pos.z += o.z;
    }
    if (mode & 0x20) {  // bob in Y (also 0x22 bob + spin)
        const int32_t c = (rsin(a.p17e) * a.param) >> 12;
        const int16_t ph = a.p17e;
        a.p17e = int16_t(a.reg + ph);
        a.flags14 = ((ph < 0x400 && a.p17e > 0x3ff) || (ph < 0xc00 && a.p17e > 0xbff)) ? a.flags14 | 0x2000 : a.flags14 & ~0x2000u;
        a.p17e &= 0xfff;
        a.pos = {a.base.x, a.base.y + c, a.base.z};
    }
    if (ridden) {  // carry the rider with the platform's motion (the exe parents Bugs' coordinate to the platform)
        const V3 rel = rotY(a.yaw - yaw0, p.pos.x - before.x, 0, p.pos.z - before.z);
        p.pos = {a.pos.x + rel.x, p.pos.y + (a.pos.y - before.y), a.pos.z + rel.z};
        p.yaw = int16_t(p.yaw + (a.yaw - yaw0));
    }
    const ActorDef& d = defs[size_t(a.def)];
    for (const ActorItem& it : d.items) {  // gated list: first firing item stops unless 0x8000
        if ((it.mask14 && !(a.flags14 & it.mask14)) || (it.mask18 && !(a.flags18 & it.mask18))) continue;
        if (!cond(it.item, &a)) continue;
        act(it.item, &a);
        if (it.flags & 0x200) playSound(uint32_t(it.p1));
        if (it.flags & 0x10000) { a.flags14 |= 0x100; return; }
        if (!(it.flags & 0x8000)) break;
    }
    if ((a.flags8 & 0x20000000) && !(p.flags14 & 0x10)) {  // contact damage (no knock-back state)
        a.flags14 |= 0x40;
        if ((!(p.flags14 & 0x3000020) || (a.flags8 & 0x40000000)) && ((a.flags14 & 0x8000) || inBox(a, p.pos, kPlayerHalf, 0, -boxTop())))
            damagePlayer(a, nullptr);
    } else {
        a.flags14 &= ~0x40u;
    }
    a.flags14 &= 0xf1676f7f;
}

// ---- Bugs vs actor boxes ----

// 0x433500 (mask 0x9a690a) as used by 0x43a880, approximated: the first active collision-list actor whose box
// (yaw frame, inflated by +0x20 and Bugs' half-width) contains Bugs' next position is the contact. Platforms
// (flags8 8) are boarded from above (feet less than 100 below the top), pickups/touchables (0x2802, 0x20100) get
// flags14 0x8000 "touched", landing on an actor sets its 0x80 (stomp, + hit 0x4000000 when its step is
// stomp-vulnerable flagsB 0x8000; flagsB 0x10000000 makes Bugs bounce), solids (not sensor 0x2000000) stop the
// horizontal move when hit from the side. Not ported: the exe's edge push-out geometry, pushables, swimming.
void LitWorld::playerContacts(V3& w) {
    if (p.platform >= 0) {  // 0x434230: still over the platform?
        const LitActor& pl = actors[size_t(p.platform)];
        const V3 q{p.pos.x + w.x, p.pos.y, p.pos.z + w.z};
        const bool rising = (p.flags14 & 1) && !(p.flags14 & 8);
        if (pl.dead || rising || !inBox(pl, {q.x, pl.pos.y + pl.box[4], q.z}, 0, 0, 0) || q.y + w.y < pl.pos.y + pl.box[4] - 100) {
            p.platform = -1;
            p.flags14 = (p.flags14 & ~0x14u) | (p.flags14 & 1 ? 0 : 0x200);
        } else {
            p.pos.y = pl.pos.y + pl.box[4] + 4;
            w.y = 0;
            return;
        }
    }
    const V3 q{p.pos.x + w.x, p.pos.y + w.y, p.pos.z + w.z};
    for (size_t i = 0; i < actors.size(); ++i) {
        LitActor& a = actors[i];
        if (!a.active || a.dead || a.def < 0 || defs[size_t(a.def)].cls != 2 || !(a.flags8 & 0x9a690a)) continue;
        if (!inBox(a, q, kPlayerHalf, 0, -boxTop())) continue;
        const int32_t below = q.y - (a.pos.y + a.box[4]);  // feet below the box top
        const bool falling = (p.flags14 & 0x208) && !(p.flags14 & 0x10000);
        auto stomp = [&] {
            if (!falling || below >= 100) return;
            a.flags14 |= 0x80;
            const ActorDef& d = defs[size_t(a.def)];
            if (a.type == 14 && a.step >= 0 && !(p.flags14 & 0x3000000)) {
                const ActorStep& st = d.steps[size_t(a.step)];
                if (st.flagsB & 0x8000) a.flags14 |= 0x4000000, a.damage = 1, a.contact = 1, p.contact = a.entityId;
                if (st.flagsB & 0x10000000) p.flags18 |= 0x40;
            }
        };
        if ((a.flags8 & 8) && !(p.flags14 & 0x1000010) && (!(p.flags14 & 1) || (p.flags14 & 8)) && below < 100) {
            stomp();
            p.platform = int(i);
            p.pos.y = a.pos.y + a.box[4] + 4;
            w.y = 0;
            p.flags14 = (p.flags14 & 0xfffffdf4) | 0x14;
            p.phase = 0x800;
            return;
        }
        a.contact = uint16_t(p.entityId);
        p.contact = a.entityId;
        if (a.flags8 & 0x4000) {  // pushable: touched, blocked (pushing not ported)
            a.flags14 |= 0x8000;
            w.x = w.z = 0;
        } else if (a.flags8 & 0x2802) {
            p.flags14 |= 0x40000;
            a.flags14 |= 0x8000;
            stomp();
            if (!(a.flags8 & 0x2000000) && below > 30) w.x = w.z = 0;
        } else if (a.flags8 & 0x20100) {
            a.flags14 |= 0x8000;
            stomp();
            if (!(a.flags8 & 0x2000000) && below > 30) w.x = w.z = 0;
        }
        return;
    }
}

// Bugs' attacks: move flagsA 0x800000 body overlap (0x432fe0, centre inside the target box) and 0x10000 hand
// point (0x433380; attach point not decoded: 150 ahead, 200 up). Targets: flags8 0x80; a type-14 target only
// when its step is vulnerable (flagsA 0x4000). Effect: target flags14 0x4000000, damage 1.
void LitWorld::playerAttacks(const PlayerMove& mv) {
    if (!(mv.flagsA & 0x810000)) return;
    const V3 o = rotY(p.yaw, 0, 0, 150);
    const V3 hand{p.pos.x + o.x, p.pos.y - 200, p.pos.z + o.z};
    for (LitActor& a : actors) {
        if (!a.active || a.dead || !(a.flags8 & 0x80) || a.def < 0) continue;
        const bool body = (mv.flagsA & 0x800000) && inBox(a, {p.pos.x, p.pos.y - 100, p.pos.z}, 0, 0, 0);
        const bool handHit = (mv.flagsA & 0x10000) && inBox(a, hand, 4, 4, 4);
        if (!body && !handHit) continue;
        if (a.type == 14 && (a.step < 0 || !(defs[size_t(a.def)].steps[size_t(a.step)].flagsA & 0x4000))) continue;
        a.flags14 |= 0x4000000;
        a.damage = 1;
        a.contact = 1;
        p.contact = a.entityId;
        return;
    }
}

} // namespace bb::game
