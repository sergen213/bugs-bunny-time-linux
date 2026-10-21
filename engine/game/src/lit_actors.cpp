// LiT actors (GAMEPLAY.md §7): activation pass and update order of 0x447d40, the generic type-14 interpreter
// 0x440120, the type-2 script graph 0x4455e0, moving platforms 0x43f520, logic directors 0x4445e0 / 0x444b50,
// NPC steering 0x437f40 and Bugs' contacts with actor boxes (0x433500 / 0x432fe0 / 0x433380, approximated).
#include "bb/game/lit_world.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bb::game {
namespace {

constexpr int16_t kSteer[10] = {0, 0, 0, 341, -341, 682, -682, 1023, -1023, 0};    // 0x4add98 (+ the word after: k = 9)
constexpr int16_t kSteerFine[10] = {0, 0, 0, 256, -256, 512, -512, 768, -768, 0};  // 0x4addb0 (flagsB 0x80000)
constexpr int32_t kPlayerHalf = 70;  // default player box ±70 / ±40 (§3.7); one half-width for both axes (approximation)

// 0x4079c0(dx² + dy² + dz²): round(sqrt) of the int32 sum (wraps like the exe beyond ±46340 per axis).
int32_t dist(const V3& a, const V3& b, bool y) {
    const uint32_t dx = uint32_t(a.x - b.x), dy = y ? uint32_t(a.y - b.y) : 0, dz = uint32_t(a.z - b.z);
    const int32_t s = int32_t(dx * dx + dy * dy + dz * dz);
    return s < 0 ? INT32_MIN : isqrt(s);  // exe: sqrt(negative) = NaN -> __ftol 0x80000000
}
// 0x4078d0 / 0x407970: activation / director-camera length estimate, max + (mid + min) / 4.
int32_t approxLen(int32_t x, int32_t y, int32_t z) {
    x = std::abs(x), y = std::abs(y), z = std::abs(z);
    const int32_t hi = std::max(x, y), lo = std::min(x, y);
    return hi <= z ? ((hi + lo) >> 2) + z : ((z + lo) >> 2) + hi;
}

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

// 0x433380's point test: q within the +0x30 pre-test radius of o, then in o's yaw frame inside its box grown by inflate + 4
// horizontally (heights not grown).
bool pointInBox(const LitActor& o, const V3& q) {
    const auto& b = o.box;
    if (!(b[0] | b[1] | b[2] | b[3] | b[4] | b[5])) return false;  // 0x407890: empty box
    const int32_t dx = int16_t(q.x - o.pos.x), dy = int16_t(q.y - o.pos.y), dz = int16_t(q.z - o.pos.z);
    const int32_t g = o.inflate + 4;
    if (uint32_t(dz * dz + dx * dx) > o.radius2) return false;
    const V3 l = rotY(int16_t(0x1000 - o.yaw), dx, dy, dz);
    return b[0] - g <= l.x && l.x <= b[3] + g && b[2] - g <= l.z && l.z <= b[5] + g && b[4] <= l.y && l.y <= b[1];
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
    a.radius = d.radius;
    a.radius2 = uint32_t(d.radius) * d.radius;
    a.inflate = d.inflate;
    a.act3c = d.act3c, a.act3e = d.act3e;
    a.param = d.param;
    a.reg = d.reg;
    a.zone = d.zone;
    a.entityId = d.entityId;
    return a;
}

// Walks the live list (0x4b3740) from its head: the first matching entity id in list order.
int LitWorld::findActor(uint16_t entityId) const {
    for (const uint32_t k : order_)
        if (!actors[k].dead && !(actors[k].flags14 & 0x100) && actors[k].entityId == entityId) return int(k);
    return -1;
}

// 0x448d40: copy of template `entityId` (chunk 0x08) at pos / yaw, appended to the live list and to the end of the
// running active list (so it updates later in the same tick, last). Dead slots are reused and the pool never
// reallocates during a tick (behaviours hold references): pool full -> -1, like the exe's fixed actor pool.
int LitWorld::spawn(uint16_t entityId, const V3& pos, int16_t yaw) {
    for (size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].chunk != 0x08 || defs[i].entityId != entityId) continue;
        LitActor a = instantiate(i);
        a.pos = pos;
        a.yaw = yaw;
        a.active = true;
        size_t k = 0;
        while (k < actors.size() && !(actors[k].dead && int(k) != p.platform)) ++k;
        if (k == actors.size()) {
            if (actors.size() == actors.capacity()) return -1;
            actors.push_back(std::move(a));
        } else {
            actors[k] = std::move(a);
        }
        order_.push_back(uint32_t(k));
        spawned_.push_back(k);
        return int(k);
    }
    unsupported.insert(std::format("spawn template {}", entityId));
    return -1;
}

// 0x438970, the sprite path of the actor animation (flipbook actors flags8 0x200000 via 0x4393e0, and every type-2
// actor): the animation is the listed tag-0x3f asset whose role is `anim`, n x {u8 record, u8 ticks}. Start: frame 0,
// countdown = ticks[0]; at the last frame with countdown 0 a once-clip (mode 2) ends (0x100, or keeps returning
// 0x200 with 0x800 = hold), others restart; else countdown 0 advances a frame (countdown = its ticks), then
// countdown -= 1 (u8) and the last frame sets 0x2000. Runtime: L01A's type-2 ambient sprites hold frame 0 (anim 134,
// mode 0x802) and count 2, 1, 0 then hold (anim 1, 3 ticks).
static uint32_t flipbookTick(AnimState& st, const std::vector<uint8_t>& list, uint16_t anim, uint32_t mode) {
    mode &= 0xffff;
    if (mode & AnimStop) { st.flags = 0x100; return AnimStopped; }
    if ((mode & AnimResume) && (st.flags & AnimPause)) st.flags &= ~AnimPause;
    else if (mode & AnimPause) { st.flags |= AnimPause; return AnimPlaying; }
    st.flags &= ~0x3000u;
    const auto ticksOf = [&](uint32_t f) { return f * 2 + 1 < list.size() ? list[f * 2 + 1] : uint8_t(0); };
    if (mode & 0xb) {
        st.anim = anim;
        st.flags = mode | 0x1200;
        st.last = uint16_t(list.size() / 2 - 1);
        st.frame = 0;
        st.countdown = ticksOf(0);
    }
    if (st.frame == st.last && st.countdown == 0) {
        if (st.flags & AnimOnce) {
            const uint32_t f = st.flags;
            st.flags = (st.flags & ~0x200u) | 0x100;
            return (f & AnimHold) ? AnimPlaying : AnimFinished;
        }
        if (st.flags & AnimLoop) st.flags |= 0x80;
        st.frame = 0;
        st.countdown = ticksOf(0);
        st.flags |= 0x1000;
    }
    if (st.countdown == 0) st.countdown = ticksOf(++st.frame);
    --st.countdown;
    if (st.frame == st.last) st.flags |= 0x2000;
    return AnimPlaying;
}

// 0x4393e0 with its frame-boundary side effects: when a frame is shown (countdown 0, not the once-clip end) its
// sound records (tag 0x28: frame match, every `reload + 1`-th time) and logic events (tag 0x0d: frame match or
// flag3 0x08; cond -> action; flag2 0x02 spawns template `id` at the actor) run. Runtime: L01A Bugs' idle fidget
// anim 72 sets G[176] = 3 on its first frame (A19), which starts the type-2 prop actors' state 361 (counter 89).
// Not ported: event flags 0x01/0x40 (attached children), 0x04/0x08 and flag3 0x01/0x04 (grab / release / force Bugs).
uint32_t LitWorld::todTick(const ActorModel& m, AnimState& st, uint16_t anim, uint32_t mode, std::vector<AnimFx>& fx,
                           LitActor* self) {
    const bool boundary = (mode & 0xb) || (st.countdown == 0 && !(st.flags & (AnimPause | 0x100)));
    const uint32_t r = animTick(m, st, anim, mode);
    if (!boundary || r != AnimPlaying || st.frame == 0 || fx.empty()) return r;
    const uint16_t frame = uint16_t(st.frame - 1);
    const auto it = std::ranges::find(fx, st.anim, &AnimFx::anim);
    if (it == fx.end()) return r;
    for (AnimFx::Sound& s : it->sounds) {
        if (s.frame != frame) continue;
        if (s.count) { --s.count; continue; }
        s.count = s.reload;
        playSound(s.id);
    }
    for (const AnimFx::Event& e : it->events) {
        if ((e.frame != frame && !(e.f3 & 8)) || (e.item.cond && !cond(e.item, self))) continue;
        if (e.item.act) act(e.item, self);
        if (e.f2 & 2) {
            const V3 at = self ? self->pos : p.pos;
            if (const int k = spawn(e.id, at, self ? self->yaw : p.yaw); k >= 0) actors[size_t(k)].zone = self ? self->zone : p.zone;
        }
        if ((e.f2 & 0x4d) || (e.f3 & 5)) unsupported.insert(std::format("anim event flags {:x}/{:x}", e.f2, e.f3));
    }
    return r;
}

uint32_t LitWorld::animate(LitActor& a, uint16_t anim, uint32_t mode) {
    ActorDef& d = defs[size_t(a.def)];
    if ((a.flags8 & 0x200000) || a.type == 2) {
        const uint16_t want = (mode & 0xb) ? anim : a.animState.anim;
        for (const auto& [role, list] : d.flipbooks)
            if (role == want && list.size() >= 2) return flipbookTick(a.animState, list, anim, mode);
    }
    const uint32_t r = scene_ && d.model >= 0 ? todTick(scene_->actorModels[size_t(d.model)], a.animState, anim, mode, d.animFx, &a) : AnimMissing;
    if (r != AnimMissing) {
        AnimState& st = a.animState;
        if (st.boxSet) {  // packet 9 (+0x48..+0x54, +0x2e, +0x30)
            a.box = st.box;
            a.radius = st.radius;
            const int32_t r = a.inflate + st.radius;
            a.radius2 = uint32_t(r * r);
        }
        if (st.attachNew) {             // packet 10 -> +0x58 (part origin through its parent chain, world)
            st.attachNew = false;
            if (const auto o = partOrigin(scene_->actorModels[size_t(d.model)], st, size_t(st.attachPart), {a.pos.x, a.pos.y, a.pos.z},
                                          {a.rot[0], a.yaw, a.rot[2]}, a.scale, false))
                a.hand = {(*o)[0], (*o)[1], (*o)[2]}, a.handSet = true;
        }
        return r;
    }
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
    spawned_.clear();
    glyphs.clear();  // 0x4b373c sprite list restarts with each update
    // Activation reference: the camera eye, Bugs while the eye components sum to 0 (load update; 0x447d40).
    const V3 ref = cam.eye.x + cam.eye.y + cam.eye.z != 0 ? cam.eye : p.pos;
    const V3 fwd{cam.target.x - cam.eye.x, 0, cam.target.z - cam.eye.z};
    const double fl = std::max(1.0, std::sqrt(double(fwd.x) * fwd.x + double(fwd.z) * fwd.z));
    std::vector<size_t> first, solid, plats, rest;  // type 0x15, flags8 0x20000, flags8 8, others (each LIFO)
    for (const uint32_t i : order_) {
        LitActor& a = actors[i];
        a.active = false;
        if (a.dead || (a.flagsC & 0x40)) continue;
        a.visible = true;
        a.flags18 &= ~0x100u;
        if (a.entityId != 5 && a.type != 20 && int(i) != p.platform && a.act3c != 0) {
            if ((a.act3c & 2) && approxLen(ref.x - a.pos.x, ref.y - a.pos.y, ref.z - a.pos.z) > a.act3e) {  // beyond +0x3e
                if (!(a.act3c & 0x40)) continue;
                a.visible = false;
                a.flags18 |= 0x100;
            }
            // +0x3c & 1 portal-zone visibility: the visible-zone table 0x4b3460 is filled by drawing, so it is empty
            // for the load update and the first tick (runtime: L01A's zone-gated start director fires on the 2nd
            // tick, counter 5); afterwards every zone counts as visible (portal visibility not simulated).
            if ((a.act3c & 1) && a.visible && ticks <= 1) {
                if (!(a.act3c & 0x40)) continue;
                a.visible = false;
                a.flags18 |= 0x100;
            }
            // & 0x20: projected centre within ±0x100 of the 512-wide 4:3 reference at H = 400 (vertical test and
            // multi-part corners omitted).
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
    // Paused (S[12] != 0: 1 pause menu, 2 resuming, 3 help): only actors with flagsC 0x40000000 run (pause/help
    // menus and the overlays they spawn), the player and camera stand still (0x447d40).
    const bool paused = S[12] != 0;
    auto one = [&](size_t i) {
        if (paused && !(actors[i].flagsC & 0x40000000)) return;
        if (!(actors[i].flags14 & 0x100)) behaviour(i);
        LitActor& a = actors[i];
        ++a.timer;
        if (a.flags14 & 0x100) {
            a.dead = true;  // freed by 0x447a50
            std::erase(order_, uint32_t(i));
            if (p.platform == int(i)) { p.platform = -1; p.flags14 &= ~0x14u; }
        }
    };
    auto run = [&](const std::vector<size_t>& v) {
        for (auto it = v.rbegin(); it != v.rend(); ++it) one(*it);
    };
    run(first);
    run(solid);
    run(plats);
    // Carried (attached coordinate, 0x11c local offset zero): Bugs' world position is the carrier's as of his own
    // update, i.e. before a carrier in the rest list moves -> one tick behind it (runtime: L01a walk-in, Bugs z =
    // carrier z - 25 each tick). His own movement does not apply while attached.
    if (p.carrier >= 0 && actors[size_t(p.carrier)].dead) p.carrier = -1;
    const bool held = p.carrier >= 0;
    const V3 heldAt = held ? actors[size_t(p.carrier)].pos : V3{};
    if (held) {
        p.pos = heldAt;
        p.yaw = actors[size_t(p.carrier)].yaw;
        p.zone = actors[size_t(p.carrier)].zone;
    }
    if (!paused) {
        playerTick();
        ++p.timer;
    }
    if (held) p.pos = heldAt;
    run(rest);
    for (size_t q = 0; q < spawned_.size(); ++q) one(spawned_[q]);  // spawned this tick: appended to the active list
    spawned_.clear();
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
    case 5: pushable(i); return;
    case 16: director(i); return;
    case 36: cameraDirector(i); return;
    case 32: overlayActor(i); return;
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
        // 0 static prop, 4 flipbook effect, 8 rope, 10 tagger, 31 projectile, 34 attached:
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
            if ((f & 0x200000) ? uint32_t(d) < uint32_t(it.p1) : uint32_t(d) > uint32_t(it.p1)) return false;
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
        ap->hops = 0;
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
        // Hand attack: the attach point +0x58 (TOD packet 10 of the last applied frame) inside Bugs' box (0x434840).
        if ((fa & 0x10000) && a.handSet && !(p.flags14 & 0x3000000) && handHitsPlayer(a.hand) && damagePlayer(a, st) && st->w[4])
            forceState(uint16_t(st->w[4]));
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
    // flagsA 0x8000000 on the ground bounces: hop n steps the arc n × +0x18 until that reaches 0x400, then flags14
    // 0x800000 ends it (runtime: L01A carrots 39 from enemy 932, second hop at counters 1159-1175 twice as fast).
    if ((fa & 0x10) && !(a.flags14 & 0x209)) {
        const int32_t hop = (fa & 0x8000000) && (a.flags14 & 0x12) ? st->w[2] * ++a.hops : st->w[2];
        if (hop < 0x400) {
            a.phase = 0x400;
            a.jumpH = st->w[3];
            a.jumpStep = int16_t(hop);
            a.vy = 0;
            a.flags14 = (a.flags14 & 0xeffff9f1) | 1;
        } else {
            a.flags14 = (a.flags14 & 0xfff7efff) | 0x800000;
        }
    }
    if ((fa & 0x10) && (a.flags14 & 1)) arcTick(a);
    if ((w.x | w.y | w.z) && !(fa & 0x80000000) && !(fb & 0x20002)) {
        // 0x437f40: try the heading offsets (table by flagsB 0x80000) until the step neither hits a wall, drops
        // (unless flagsA 0x80000), nor changes zone (unless flagsA 0x40000000, which adopts the new zone); none ->
        // turn around and stand. A trace ending over open floor is re-probed with GroundAt one unit below at the
        // end point, whose result replaces the trace's: no zone or > 200 below = drop.
        const int16_t* tab = (fb & 0x80000) ? kSteerFine : kSteer;
        int k = 0;
        V3 got{};
        int16_t zone = a.zone;
        for (; k < 9; ++k) {
            const V3 v = rotY(tab[k], w.x, w.y, w.z);
            V3 q = a.pos;
            int32_t dx = v.x, dz = v.z;
            Ground r;
            r.zone = a.zone;
            hf.trace(q, dx, dz, r);
            zone = r.zone;
            uint16_t kind = r.kind;
            int16_t hoff = r.hoff;
            if (a.flags14 & 4) {
                kind = uint16_t((kind & ~KindOpen) | KindFloor);
            } else if (!(kind & KindOpen)) {
                if ((kind & KindFloor) && !(a.flags14 & 0x219) && hoff - a.pos.y + q.y > 200) kind = uint16_t((kind & ~6) | KindOpen);
            } else {
                V3 b{q.x + dx, q.y + 1, q.z + dz};
                Ground g;
                g.zone = a.zone;
                hf.groundAt(b, g);
                kind = g.kind;
                hoff = g.hoff;
                if (g.zone == -1) kind |= KindOpen;
                else if ((kind & KindFloor) && g.groundY - a.pos.y > 200) kind = uint16_t((kind & ~6) | KindOpen);
            }
            a.hoff = hoff;
            const bool shortened = (v.z + v.x) - (dz + dx) != 0;
            if ((kind & KindBlocked) || (!(fa & 0x80000) && (kind & KindOpen)) || shortened) a.flags14 |= 0x100000;
            if (kind & KindOpen) a.flags18 |= 0x20;
            const bool zoneChange = !(fa & 0x40000000) && zone != a.zone;
            got = {dx, v.y, dz};
            if (!zoneChange && !(kind & ((~(fa >> 16) & 8) | 4)) && !shortened) break;
        }
        if (fa & 0x40000000) a.zone = zone;
        if ((!(a.flags18 & 0x20) || (fa & 0x80000)) && ((fa & 0x40000000) || (a.flags14 & 0x100000) || k > 8))
            a.yaw = int16_t(a.yaw + (tab[k] >> 3));
        else
            a.yaw = int16_t(a.yaw + tab[std::min(k, 8)]);
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
                        // 0x440120: Bugs' coordinate becomes a child of the carrier's (+0xc4, +0x70); his flags14 are
                        // left alone (runtime: 2 throughout the L01a walk-in) and the turn error 0x4b3c20 reset.
                        if (f & 0x1000000) p.reg = a.reg;
                        p.carrier = int32_t(i);
                        p.platform = -1;
                        turnErr_ = -1;
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
                if ((f & 0x4000) && a.rider == -2) {  // release Bugs at his attached world position = the carrier's
                    a.rider = -1;                       // current one (coordinate chain), x/z rotation cleared
                    p.carrier = -1;
                    p.pos = a.pos;
                    p.zone = a.zone;
                    p.lean = 0;
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
    if (a.animState.flags & 0x100) a.flags14 &= 0xf9f7efff;  // clip finished (+0x15c 0x100)
    const bool landed = res != AnimFinished && same && (st->flagsA & 0x80) && (a.flags14 & 0x12);
    if (res == AnimFinished || landed) {
        if (landed) {
            a.flags14 &= 0xf9ffffff;
        } else {
            a.flags14 &= 0xf9f7efff;
            a.anim = 0;
        }
        if (st->flagsA & 4) { a.flags14 |= 0x100; return; }
        a.flags14 &= ~0x1000u;
        ++a.seq;
        const int16_t sid = a.seq < 5 ? d.states[size_t(a.state)].moves[a.seq] : int16_t(-16);
        if (uint16_t(sid) != 0xfff0 && a.seq < 5) {
            a.step = stepById(d, sid);
            a.anim = 0;
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
            startScript(a, it.next);  // 0x4455e0: +0x158 = 0 (restart), then animates the new step this tick
            a.anim = 0;
            if (a.step < 0) return;
            break;
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
            if ((f & 0x200000) ? uint32_t(dd) < uint32_t(it.p1) : uint32_t(dd) > uint32_t(it.p1)) continue;
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

// ---- type 5 pushable block (0x445a60) ----

// Runs in the platform group (flags8 8), i.e. before Bugs: it applies the push he wrote last tick (+0x40/+0x44,
// playerContacts) and hands back what it could not move by tracing him with (moved − pushed). +0x17c (tag 0x1d):
// 1 / 2 allow X / Z, 0x100 / 0x200 forbid − / +, 0x40 never move (Bugs pushed back), 0x80 keep on the floor
// (cancel when a corner or the centre would drop > 200), 0x20 flags14 4. Each corner of its box (grown by inflate/4,
// in its yaw frame) traces the X and then the Z move; unless flagsC 0x80 the leading face is traced for a wall. No
// animation (runtime: L01A block, csv/litpush_L01A.csv). Not ported: the 0x432fe0 (mask 0x20000) carrier trigger,
// its own 0x433500 push against other solids, the free mode 0x17c & 4 and its trigger list +0xfc (no LiT block
// uses 4; the others are not exercised by the captures).
void LitWorld::pushable(size_t i) {
    LitActor& a = actors[i];
    const uint16_t f = uint16_t(a.param);
    if (!a.boxInit) {  // first call: the part list for drawing (0x448980); the box stays the tag box
        a.boxInit = true;
        a.flags18 |= 0x80;
        const ActorDef& d = defs[size_t(a.def)];
        if (d.idleAnim && a.animState.parts.empty()) animate(a, d.idleAnim, AnimOnce);
        a.box = d.box;
        a.radius = d.radius;
        a.radius2 = uint32_t(d.radius) * d.radius;
        if (f & 0x20) a.flags14 |= 4;
    }
    if (f & 0x40) {
        p.pos.x -= a.pushX;
        p.pos.z -= a.pushZ;
    } else if ((a.flags14 & 0x8000) && (p.flags14 & 0x4000) && !(f & 4)) {
        const int32_t g = int16_t(a.inflate >> 2), half = a.inflate >> 1;
        const V3 lo = rotY(a.yaw, a.box[0] - g, a.box[1], a.box[2] - g), hi = rotY(a.yaw, a.box[3] + g, a.box[4], a.box[5] + g);
        auto allow = [&](int32_t d, uint16_t bit) {
            return (!(f & bit) || ((f & 0x100) && d < 0) || ((f & 0x200) && d > 0)) ? 0 : d;
        };
        int32_t dx = allow(a.pushX, 1), dz = allow(a.pushZ, 2);
        auto trace = [&](int32_t x, int32_t z, int32_t& tx, int32_t& tz, Ground& r) {
            V3 q{x, a.pos.y, z};
            r = Ground{};
            r.zone = a.zone;
            hf.trace(q, tx, tz, r);
            return q.y;
        };
        auto floorCheck = [&](Ground& r, int32_t y) {  // 0x80: a floor more than 200 below cancels the move
            if (!(f & 0x80)) return;
            if ((r.kind & KindFloor) && r.hoff + y - a.pos.y > 200) r.kind |= KindOpen;
            if (r.kind & KindOpen) dx = dz = 0;
        };
        for (const auto& [cx, cz] : {std::pair{lo.x, lo.z}, {hi.x, lo.z}, {lo.x, hi.z}, {hi.x, hi.z}}) {
            Ground r;
            int32_t tx = dx, tz = 0;
            int32_t y = trace(a.pos.x + cx, a.pos.z + cz, tx, tz, r);
            dx = tx;
            floorCheck(r, y);
            tx = 0, tz = dz;
            y = trace(a.pos.x + cx, a.pos.z + cz, tx, tz, r);
            dz = tz;
            floorCheck(r, y);
        }
        if (!(a.flagsC & 0x80)) {  // the leading face must not cross a wall
            const auto& b = a.box;
            Ground r;
            if (dx) {
                int32_t tx = 0, tz = half - b[2] + b[5];
                trace(dx < 0 ? a.pos.x - half + b[0] : a.pos.x + half + b[3], a.pos.z - half + b[2], tx, tz, r);
                if (r.kind & KindBlocked) dx = 0;
            }
            if (dz) {
                int32_t tx = half - b[0] + b[3], tz = 0;
                trace(a.pos.x - half + b[0], dz < 0 ? a.pos.z - half + b[2] : a.pos.z + half + b[5], tx, tz, r);
                if (r.kind & KindBlocked) dz = 0;
            }
        }
        if (f & 0x80) {
            V3 q{a.pos.x + dx, a.pos.y, a.pos.z + dz};
            Ground r;
            r.zone = a.zone;
            hf.groundAt(q, r);
            floorCheck(r, q.y);
        }
        a.pos.x += dx;
        a.pos.z += dz;
        a.flags18 |= 0x8000;
        V3 q = p.pos;
        int32_t bx = dx - a.pushX, bz = dz - a.pushZ;
        Ground r;
        r.zone = a.zone;
        hf.trace(q, bx, bz, r);
        p.pos.x += bx;
        p.pos.z += bz;
    }
    V3 w{};
    Ground r;
    moveActor(a, w, r, a.box[4]);  // gravity (0x437390 with no move)
    a.pos.y += w.y;
    a.pushX = a.pushZ = 0;
    a.flags14 &= 0xffff5fff;
}

// ---- Bugs vs actor boxes ----

// 0x433500 (mask 0x9a690a) as used by 0x43a880, approximated: the first active collision-list actor whose box
// (yaw frame, inflated by +0x20 and Bugs' half-width) contains Bugs' next position is the contact. Platforms
// (flags8 8) are boarded from above (feet less than 100 below the top), pickups/touchables (0x2802, 0x20100) get
// flags14 0x8000 "touched", landing on an actor sets its 0x80 (stomp, + hit 0x4000000 when its step is
// stomp-vulnerable flagsB 0x8000; flagsB 0x10000000 makes Bugs bounce); solids only deflect the move through
// boxPush (0x433500 face push; runtime: Bugs walking into 932 at counters 897-931 is never stopped). Not ported: the
// contact choice between several actors, the vertical result, the re-trace of the pushed move, pushables, swimming.
// 0x433500's face push for Bugs vs actor o (radius pre-test, vertical overlap, then the 4 corners + centre of his
// inflated box moved by w, in o's yaw frame, against o's box grown by its inflation; the face is picked by the
// sector of his position around o's box centre). Each corner past that face counts as a contact and, unless o is a
// sensor (flags8 0x2000000), w is cut so the corner stops on the face — an animated box (TOD packet 9) growing into
// a standing Bugs pushes him out (runtime: L01A enemy 932's attack, counters 757-763). x/z rotation ignored.
bool LitWorld::boxPush(const LitActor& o, V3& w) const {
    const auto& mb = p.animState.box;
    const auto& ob = o.box;
    const int32_t mi = playerInflate_, oi = o.inflate;
    if (!(ob[0] | ob[1] | ob[2] | ob[3] | ob[4] | ob[5])) return false;  // 0x407890: empty box
    const int32_t orad = o.radius;
    const int32_t nx = int16_t(p.pos.x + w.x - o.pos.x), nz = int16_t(p.pos.z + w.z - o.pos.z);
    const int32_t rr = mi + p.animState.radius + orad + oi;
    if (uint32_t(nx * nx + nz * nz) > uint32_t(rr * rr)) return false;
    const int16_t inv = int16_t(0x1000 - o.yaw);
    const V3 rel{int16_t(p.pos.x - o.pos.x), int16_t(int16_t(p.pos.y - o.pos.y) + w.y), int16_t(p.pos.z - o.pos.z)};
    const V3 r = rotY(inv, rel.x, rel.y, rel.z);
    if (!(ob[4] < mb[1] + r.y + w.y && mb[4] + r.y + w.y <= ob[1])) return false;
    const int32_t cx = (ob[3] >> 1) + (ob[0] >> 1), cz = (ob[2] >> 1) + (ob[5] >> 1);
    const int32_t ang = ratan2(r.x - cx, r.z - cz);
    const int32_t a1 = ang < 0 ? ratan2(ob[0] - oi - cx, ob[5] - cz + oi) : ratan2(ob[3] - cx + oi, ob[5] - cz + oi);
    const int32_t a2 = ang < 0 ? ratan2(ob[0] - oi - cx, ob[2] - oi - cz) : ratan2(ob[3] - cx + oi, ob[2] - oi - cz);
    V3 v = rotY(inv, w.x, w.y, w.z);
    const bool sensor = o.flags8 & 0x2000000;
    const int32_t corners[5][2] = {{mb[0] - mi, mb[5] + mi}, {mb[3] + mi, mb[5] + mi}, {mb[0] - mi, mb[2] - mi},
                                   {mb[3] + mi, mb[2] - mi}, {0, 0}};
    int hits = 0;
    for (const auto& k : corners) {
        const V3 cw = rotY(p.yaw, k[0], 0, k[1]);
        const V3 c = rotY(inv, int16_t(cw.x + rel.x), 0, int16_t(cw.z + rel.z));
        const int32_t x = c.x + v.x, z = c.z + v.z;
        if (ang < 0 ? a1 < ang : ang < a1) {
            if (z <= ob[5] + oi && (ang < 0 ? ob[0] - oi <= x : x <= ob[3] + oi)) {
                if (!sensor) v.z = ob[5] - c.z + oi;
                ++hits;
            }
        } else if (ang < 0 ? a2 < ang : ang < a2) {
            if ((ang < 0 ? ob[0] - oi <= x : x <= ob[3] + oi) && z <= ob[5] + oi && ob[2] - oi <= z) {
                if (!sensor) v.x = ang < 0 ? ob[0] - c.x - oi : ob[3] - c.x + oi;
                ++hits;
            }
        } else if (ob[2] - oi <= z && (ang < 0 ? ob[0] - oi <= x : x <= ob[3] + oi)) {
            if (!sensor) v.z = ob[2] - c.z - oi;
            ++hits;
        }
    }
    if (!hits) return false;
    const V3 back = rotY(o.yaw, v.x, v.y, v.z);
    w.x = int16_t(back.x);
    w.z = int16_t(back.z);
    return true;
}

void LitWorld::playerContacts(V3& w, uint32_t moveFlagsA, int16_t lz) {
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
        // The move stays w0 unless a branch below takes boxPush's pushed move wp (the exe's int move vs. its s16 copy).
        const V3 w0 = w;
        V3 wp = w;
        const bool pushed = boxPush(a, wp);  // every solid incl. platforms (the L01A block has flags8 8 | 0x4000)
        // Actors touch only through boxPush's corner hits (runtime: no stomp on 932 at counter 1007, corner x 75 past
        // its box; golden carrot 542 collected at 1185, not while its smaller frames hover next to Bugs). Platforms
        // also keep the overlap test (boarding from above).
        if (!pushed && (!(a.flags8 & 8) || !inBox(a, q, kPlayerHalf, 0, -boxTop()))) continue;
        const int32_t below = q.y - (a.pos.y + a.box[4]);  // feet below the box top (0x433500's w.y result, negated)
        const bool falling = (p.flags14 & 0x208) && !(p.flags14 & 0x10000);
        // Stomp; true when a type-14 actor is stomped (the exe then skips the push: goto LAB_0043d6d3).
        auto stomp = [&] {
            if (falling && below < 100) a.flags14 |= 0x80;
            if (!(a.flags14 & 0x80) || a.type != 14 || a.step < 0 || (p.flags14 & 0x3000000)) return false;
            const ActorStep& st = defs[size_t(a.def)].steps[size_t(a.step)];
            if (st.flagsB & 0x8000) a.flags14 |= 0x4000000, a.damage = 1, a.contact = 1, p.contact = a.entityId;
            if (st.flagsB & 0x10000000) p.flags18 |= 0x40;
            return true;
        };
        // The push applies only from the side (feet more than 30 below the top), not for sensors / move flagsA 0x40.
        auto sidePush = [&](bool stomped) {
            if (!stomped && !(a.flags8 & 0x2000000) && !(moveFlagsA & 0x40) && below > 30) w.x = wp.x, w.z = wp.z;
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
        if ((a.flags8 & 0x4000) && !(p.flags14 & 0x11)) {
            // Pushable, on the ground: the probe 150 ahead / 50 up (0x433380 mask 0x4000, sensors excluded) finds it
            // while he walks (forward speed) -> blocked by a pushable (flags14 0x20000, the push-state rule). A move
            // with flagsA 0x4000000 then pushes: his move goes to the block (+0x40/+0x44, 0x445a60 moves it and hands
            // back what it could not), flags14 0x4000; otherwise the face push stops him.
            const V3 o = rotY(p.yaw, 0, 0, 150);
            const V3 probe{p.pos.x + o.x, p.pos.y - 50, p.pos.z + o.z};
            const LitActor* ahead = nullptr;
            for (const LitActor& b : actors)
                if (b.active && !b.dead && b.def >= 0 && defs[size_t(b.def)].cls == 2 && (b.flags8 & 0x4000) &&
                    !(b.flags8 & 0x2000000) && pointInBox(b, probe)) {
                    ahead = &b;
                    break;
                }
            const bool facing = ahead == &a && lz != 0;
            if (facing) p.flags14 |= 0x20000;
            a.flags14 |= 0x8000;
            if ((moveFlagsA & 0x4000000) && facing) {
                a.pushX = int16_t(w0.x), a.pushZ = int16_t(w0.z);
                p.flags14 |= 0x4000;
            } else {
                w.x = wp.x, w.z = wp.z;
            }
        } else if (a.flags8 & 0x2802) {
            p.flags14 |= 0x40000;
            a.flags14 |= 0x8000;
            sidePush(stomp());
        } else if (a.flags8 & 0x20100) {
            a.flags14 |= 0x8000;
            sidePush(stomp());
        }
        return;
    }
}

// Bugs' attacks (0x43a880): move flagsA 0x800000 body overlap (0x432fe0, approximated: centre inside the target
// box) and 0x10000 hand point (0x433380: his +0x58 attach point, TOD packet 10 of the last applied frame). Each takes
// the first flags8-0x80 collision actor it finds; a type-14 one is hit only when its step is vulnerable (flagsA
// 0x4000). Effect: target flags14 0x4000000, damage 1 (runtime: L01A kick on 932 at counter 1097, health 9999).
void LitWorld::playerAttacks(const PlayerMove& mv) {
    auto hit = [&](LitActor* a) {
        if (!a || (a->type == 14 && (a->step < 0 || !(defs[size_t(a->def)].steps[size_t(a->step)].flagsA & 0x4000)))) return;
        a->flags14 |= 0x4000000;
        a->damage = 1;
        a->contact = 1;
        p.contact = a->entityId;
    };
    auto first = [&](auto&& test) -> LitActor* {
        for (LitActor& a : actors)
            if (a.active && !a.dead && a.def >= 0 && defs[size_t(a.def)].cls == 2 && (a.flags8 & 0x80) && test(a)) return &a;
        return nullptr;
    };
    if (mv.flagsA & 0x800000) hit(first([&](const LitActor& a) { return inBox(a, {p.pos.x, p.pos.y - 100, p.pos.z}, 0, 0, 0); }));
    if ((mv.flagsA & 0x10000) && p.handSet) hit(first([&](const LitActor& a) { return pointInBox(a, p.hand); }));
}

} // namespace bb::game
