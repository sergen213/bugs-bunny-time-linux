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

std::optional<V3> partOriginQ12(const AnimState& st, size_t i, const V3& pos, const std::array<int16_t, 3>& rot,
                                const std::array<int32_t, 3>& scale, V3 local) {
    if (i >= st.parts.size()) return std::nullopt;
    auto level = [&](const std::array<int32_t, 3>& r, const std::array<int32_t, 3>& s, const V3& t) {
        Mat m;
        rotMatrixXYZ(m, r[0], r[1], r[2]);
        for (size_t k = 0; k < 9; ++k) m.m[k] = int16_t((m.m[k] * s[k % 3]) >> 12);  // ScaleMatrix: column j by s[j]
        const V3 o = apply(m, local);
        local = {o.x + t.x, o.y + t.y, o.z + t.z};
    };
    for (size_t k = i, n = 0; n <= st.parts.size(); ++n) {  // n: cycle guard
        const TodPart& p = st.parts[k];
        level(p.rot, p.scale, {p.trans[0], p.trans[1], p.trans[2]});
        if (p.parent == -1) {
            level({rot[0], rot[1], rot[2]}, scale, pos);
            break;
        }
        if (p.parent < 0 || size_t(p.parent) >= st.parts.size()) break;  // unattached: its own space
        k = size_t(p.parent);
    }
    return local;
}

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
        while (k < actors.size() && !(actors[k].dead && int(k) != p.platform && int(k) != p.rope)) ++k;
        if (k == actors.size()) {
            if (actors.size() == actors.capacity()) return -1;
            actors.push_back(std::move(a));
        } else {
            actors[k] = std::move(a);
        }
        order_.push_back(uint32_t(k));
        collide_.push_back(k);
        spawned_.push_back(k);
        return int(k);
    }
    // 0x448d40 finds no template with that id in the level and returns 0; every spawn site checks it, so the original
    // spawns nothing either (CC4D 214, L02A2 / L02A5 Bugs' anim 17 / 77 / 97 / 99 events naming the absent 508).
    unsupported.insert(std::format("spawn template {} absent (none, as the exe)", entityId));
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
// flag3 0x08, an attach point in this frame (+0x68) unless flag3 0x40; cond -> action) run. Runtime: L01A Bugs' idle
// fidget anim 72 sets G[176] = 3 on its first frame (A19), which starts the type-2 prop actors' state 361 (counter 89).
// Event flags (f2 / f3): f2 0x40 frees the carried actor (+0x6c); f2 1 spawns `id` attached at the hand (carried
// unless f3 0x80); f2 2 / f3 4 / f3 2 spawn `id` at the hand (ponytail: the hand part's own rotation added by f3 4 / 2
// is not tracked); f2 0x20 puts the carried actor down at the hand; f2 4 grabs Bugs (hand inside his box or f3 0x10):
// attached at the hand (offset kept unless f2 0x80), state `id`, flags14 4; f2 8 lets him go facing away (state
// `id`, health − step +0x16); f3 1 forces Bugs into state `id` facing away (flags14 0x2000000).
uint32_t LitWorld::todTick(const ActorModel& m, AnimState& st, uint16_t anim, uint32_t mode, std::vector<AnimFx>& fx,
                           LitActor* self) {
    const bool boundary = (mode & 0xb) || (st.countdown == 0 && !(st.flags & (AnimPause | 0x100)));
    // 0x4393e0 at the last frame (loop wrap or once end) clears flags14 0x80000 / 0x1000 (runtime: L04B3 ball 100
    // thrown back loses 0x1000 at its anim loop wrap, counter 611; local/wine/compare/w9lit6/o_L04B3_attach.csv).
    const bool atEnd = self && !(mode & 0xb) && boundary && st.tod && st.frame == st.last;
    const uint32_t r = animTick(m, st, anim, mode);
    if (atEnd) self->flags14 &= 0xfff7efff;
    if (self && st.attachNew) {  // packet 10 -> +0x58 (part origin through its parent chain, world); none: +0x68 = 0
        st.attachNew = false;
        self->handSet = st.attachPart >= 0;
        if (self->handSet) self->handPart = st.attachPart;
    }
    if (self && self->handPart >= 0)
        if (const auto o = partOriginQ12(st, size_t(self->handPart), self->pos, {self->rot[0], coordYaw(*self), self->rot[2]}, self->scale))
            self->hand = *o;
    if (!boundary || r != AnimPlaying || st.frame == 0 || fx.empty()) return r;
    const uint16_t frame = uint16_t(st.frame - 1);
    const auto it = std::ranges::find(fx, st.anim, &AnimFx::anim);
    if (it == fx.end()) return r;
    for (AnimFx::Sound& s : it->sounds) {
        if (s.frame != frame) continue;
        if (s.count) { --s.count; continue; }
        s.count = s.reload;
        playSound(self, s.id);  // 0x4393e0 / 0x438970 (Bugs)
    }
    Mover& me = self ? static_cast<Mover&>(*self) : p;
    const bool handSet = self ? self->handSet : p.handSet;
    const V3 hand = handSet ? (self ? self->hand : p.hand) : me.pos;
    const int32_t meIdx = self ? int32_t(self - actors.data()) : kParentBugs;
    int32_t* rider = self ? &self->rider : nullptr;
    // 0x4376f0: an actor put down / spawned at the hand keeps its horizontal offset from me as far as walls allow.
    auto traceOut = [&](Mover& c) {
        V3 q = me.pos;
        int32_t dx = c.pos.x - me.pos.x, dz = c.pos.z - me.pos.z;
        Ground g;
        g.zone = me.zone;
        hf.trace(q, dx, dz, g);
        c.pos.x = me.pos.x + dx;
        c.pos.z = me.pos.z + dz;
    };
    for (const AnimFx::Event& e : it->events) {
        if ((e.frame != frame && !(e.f3 & 8)) || (!handSet && !(e.f3 & 0x40)) || (e.item.cond && !cond(e.item, self))) continue;
        if (e.item.act) act(e.item, self);
        if ((e.f2 & 0x40) && rider && *rider >= 0) actors[size_t(*rider)].flags14 |= 0x100;
        if (e.f2 & 1) {
            if (const int k = spawn(e.id, me.pos, me.yaw); k >= 0) {
                if (self) self->flags18 |= 0x800;
                attach(actors[size_t(k)], meIdx, true);
                if (!(e.f3 & 0x80) && rider) {
                    if (*rider >= 0) actors[size_t(*rider)].flags14 |= 0x100;
                    *rider = k;
                }
            }
        }
        for (const uint32_t kind : {uint32_t(e.f2 & 2), uint32_t(e.f3 & 4) << 8, uint32_t(e.f3 & 2) << 8}) {
            if (!kind) continue;
            if (const int k = spawn(e.id, hand, me.yaw); k >= 0) {
                LitActor& c = actors[size_t(k)];
                c.zone = me.zone;
                if (kind & 0xff00) traceOut(c);  // f3 4 / 2 (0x4376f0)
                // f2 2 without f3 0x20: the spawn takes the hand part's scale (0x4393e0: +0xcc = *(+0x68)->+0x44;
                // runtime: L04B3 ball 100's explosion 8 at scale (10240, 8192), w9lit8/o_back.csv counter 1013).
                // ponytail: f3 0x20's hand-matrix orientation (flagsC 0x20) is not modelled.
                else if (const int hp = self ? self->handPart : st.attachPart; handSet && !(e.f3 & 0x20) && hp >= 0 && size_t(hp) < st.parts.size())
                    c.scale = st.parts[size_t(hp)].scale;
                if (!self) c.flagsC |= 0x20000;
            }
        }
        if ((e.f2 & 0x20) && rider && *rider >= 0) {
            LitActor& c = actors[size_t(*rider)];
            c.parent = -1;
            c.flags14 = (c.flags14 & ~0x800u) | 0x1000;
            c.pos = hand;
            c.zone = me.zone;
            c.rot = self->rot;
            c.yaw = me.yaw;
            *rider = -1;
            traceOut(c);
        }
        if (self && (e.f2 & 4) && !(p.flags14 & 0x3000000) && ((e.f3 & 0x10) || handHitsPlayer(hand))) {
            if (p.carrier >= 0 && actors[size_t(p.carrier)].rider == -2) actors[size_t(p.carrier)].rider = -1;
            if (p.rope >= 0) dropRope();
            p.platform = -1;
            p.local = (e.f2 & 0x80) ? V3{} : V3{p.pos.x - hand.x, p.pos.y - hand.y, p.pos.z - hand.z};
            p.pos = hand;
            p.carrier = meIdx;
            p.carrierPart = attachPart(meIdx, true);
            p.state = e.id;
            p.seq = 0;
            p.anim = 0;
            p.flags14 = 4;
            turnErr_ = -1;
            if (self->rider >= 0) actors[size_t(self->rider)].flags14 |= 0x100;
            self->rider = -2;
        }
        if (self && (e.f2 & 8) && p.carrier == meIdx && self->rider == -2) {
            if (e.f2 & 0x80) p.pos = hand;
            p.yaw = int16_t(self->yaw + 0x800);
            p.carrier = -1;
            p.carrierPart = -1;
            p.local = {};
            self->rider = -1;
            p.state = e.id;
            p.seq = 0;
            p.anim = 0;
            traceOut(p);
            if (self->step >= 0) G[1] = uint8_t(G[1] - defs[size_t(self->def)].steps[size_t(self->step)].w[1]);
            p.flags14 &= 0xfdffffdb;
        }
        if (self && (e.f3 & 1) && !(p.flags14 & 0x3000020) && p.carrier < 0 && p.rope < 0 && p.platform < 0 && handHitsPlayer(hand)) {
            p.yaw = int16_t(self->yaw + 0x800);
            p.state = e.id;
            p.seq = 0;
            p.anim = 0;
            p.flags14 = 0x2000000;
        }
    }
    return r;
}

uint32_t LitWorld::animate(LitActor& a, uint16_t anim, uint32_t mode) {
    ActorDef& d = defs[size_t(a.def)];
    if ((a.flags8 & 0x200000) || a.type == 2) {
        const uint16_t want = (mode & 0xb) ? anim : a.animState.anim;
        for (const auto& [role, list] : d.flipbooks)
            if (role == want && list.size() >= 2) {
                // 0x438970 start of a flipbook actor's clip: box ±(w/2)·scale x, top −h·scale y (sprite record 0),
                // flagsC 0x4000000 lifts it by h/2, inflate grows by 2w·scale x and the radius is that + the half
                // width (runtime: L04B3 explosion 8 spawned by ball 100, scale (10240, 8192): box ±80, top −128,
                // inflate 50 → 370, radius 400; local/wine/compare/w9lit8/o_back.csv counter 1013).
                if ((a.flags8 & 0x200000) && (mode & 0xb) && d.spriteFrames) {
                    const int32_t sx = a.scale[0], hw = uint16_t(d.spriteW) >> 1;
                    const int16_t lo = int16_t(-(hw * sx) >> 12), hi = int16_t((hw * sx) >> 12);
                    a.box = {lo, 0, lo, hi, int16_t(-((a.scale[1] * uint16_t(d.spriteH)) >> 12)), hi};
                    if (a.flagsC & 0x4000000) a.pos.y += uint16_t(d.spriteH) >> 1;
                    const int16_t g = int16_t((uint16_t(d.spriteW) * 2 * sx) >> 12);
                    a.inflate = int16_t(a.inflate + g);
                    a.radius = uint16_t(g + hi);
                    a.radius2 = uint32_t(a.radius) * a.radius;
                }
                return flipbookTick(a.animState, list, anim, mode);
            }
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

// ---- attached actors: the 0x440120 coordinate chain (+0x7c coordinate, +0xc4 super, +0x70 parent, +0x11c local) ----

// A point given in the parent coordinate, in the world: its root (+0x7c: position, yaw; ponytail: its x/z rotation
// is not applied) or, at the hand part `part` (the parent's +0x68 when attached), that part's matrix through the
// animated part chain (Bugs: his hand point, turned by his yaw). `yaw` gets the coordinate's heading (0x440120: atan2
// of its z axis).
V3 LitWorld::frameWorld(int32_t parent, int16_t part, const V3& l, int16_t* yaw) const {
    if (parent >= 0) {
        const LitActor& c = actors[size_t(parent)];
        if (part >= 0) {
            auto at = [&](const V3& q) {
                return partOriginQ12(c.animState, size_t(part), c.pos, {c.rot[0], coordYaw(c), c.rot[2]}, c.scale, q);
            };
            if (const auto o = at(l)) {
                if (yaw) {
                    const auto o0 = at({}), oz = at({0, 0, 0x1000});
                    *yaw = int16_t(ratan2(oz->x - o0->x, oz->z - o0->z));
                }
                return *o;
            }
        }
    }
    const Mover& m = parent == kParentBugs ? static_cast<const Mover&>(p) : actors[size_t(parent)];
    // An attached parent's coordinate matrix: its own parent's frame turned by its matrix yaw (current once its tail
    // ran, while its world yaw +0xde lags; runtime: L04B3 child 269 turns with actor 98 at counter 305).
    int16_t my = m.yaw;
    if (parent >= 0 && actors[size_t(parent)].parent != -1) {
        const LitActor& c = actors[size_t(parent)];
        frameWorld(c.parent, c.parentPart, {}, &my);
        my = int16_t(my + c.matYaw);
    }
    const V3 o = part >= 0 && parent == kParentBugs && p.handSet ? p.hand : m.pos, w = rotY(my, l.x, l.y, l.z);
    if (yaw) *yaw = my;
    return {o.x + w.x, o.y + w.y, o.z + w.z};
}

// 0x44e400 walks the coordinate chain with RotMatrix of each level's current local rot: an attached actor's hand turns
// with its local yaw at once, while its world yaw +0xde lags a tick (runtime: L04B3 actor 98 on pillar 57 turning at
// counter 605, hand (2848, −1381, 5756); local/wine/compare/w9lit7/o_L04B3_attach.csv).
int16_t LitWorld::coordYaw(const LitActor& c) const {
    if (c.parent == -1) return c.yaw;
    int16_t py = 0;
    frameWorld(c.parent, c.parentPart, {}, &py);
    return int16_t(py + c.localYaw);
}

int16_t LitWorld::attachPart(int32_t parent, bool hand) const {
    return !hand ? -1 : parent == kParentBugs ? 0 : actors[size_t(parent)].handPart;
}

// Item 0x20000000 / spawn 0x80 / item 0x1000: c's coordinate becomes a child of the parent's (local identity; at its
// hand: the part +0x68 points to now, kept), its zone the parent's, world position recomputed at once.
void LitWorld::attach(LitActor& c, int32_t parent, bool hand) {
    c.parent = parent;
    c.parentPart = attachPart(parent, hand);
    c.local = {};
    c.localYaw = c.matYaw = 0;
    c.rot[0] = c.rot[2] = 0;
    c.zone = parent == kParentBugs ? p.zone : actors[size_t(parent)].zone;
    followParent(c, {});
}

// The attached actor's update (0x440120 after the movement): its local translation takes this tick's move turned
// into the parent's frame and its world position / heading come from the chain (the heading through the coordinate
// matrix, i.e. the local yaw as of the type-14 tail before: matYaw); sets the parent's flags18 0x800 (carrying). The
// move is turned into the local frame by the parent's world yaw +0xde alone (0x440120: RotMatrix(0, −parent +0xde, 0)),
// not the hand's heading (runtime: L04B3 ball 100 attached to 98's hand at counter 607 while 98 turns). A
// parent's death is handled where it is freed (actorsTick).
void LitWorld::followParent(LitActor& a, const V3& move) {
    if (a.parent == -1) return;
    if (a.parent >= 0) actors[size_t(a.parent)].flags18 |= 0x800;
    int16_t yaw = 0;
    frameWorld(a.parent, a.parentPart, {}, &yaw);
    const V3 l = rotY(int16_t(-(a.parent == kParentBugs ? p.yaw : actors[size_t(a.parent)].yaw)), move.x, move.y, move.z);
    a.local = {a.local.x + l.x, a.local.y + l.y, a.local.z + l.z};
    a.pos = frameWorld(a.parent, a.parentPart, a.local);
    a.yaw = int16_t(yaw + a.matYaw);
    a.zone = a.parent == kParentBugs ? p.zone : actors[size_t(a.parent)].zone;
}

// Item flag 8 / release: back to a root coordinate at its current world position (the parent's +0x6c cleared).
void LitWorld::detach(LitActor& a) {
    if (a.parent >= 0 && actors[size_t(a.parent)].rider == int32_t(&a - actors.data())) actors[size_t(a.parent)].rider = -1;
    a.parent = -1;
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
        a.visible = true, a.far = false;
        a.flags18 &= ~0x100u;
        if (a.entityId != 5 && a.type != 20 && int(i) != p.platform && int(i) != p.rope && a.act3c != 0) {
            if ((a.act3c & 2) && approxLen(ref.x - a.pos.x, ref.y - a.pos.y, ref.z - a.pos.z) > a.act3e) {  // beyond +0x3e
                a.visible = false, a.far = true;  // the exe clears the draw bit on both paths; Enhanced still draws far
                a.flags18 |= 0x100;
                if (!(a.act3c & 0x40)) continue;
            }
            // +0x3c & 1 portal-zone visibility: the actor's zone must be in the visible-zone table 0x4b3460 unflagged.
            // The table is filled by drawing, so it is empty for the load update and the first tick (runtime: L01A's
            // zone-gated start director fires on the 2nd tick, counter 5). Without a renderer (zoneTable unset)
            // every zone counts as visible afterwards.
            if ((a.act3c & 1) && a.visible &&
                (ticks <= 1 || (zoneTable && std::ranges::find(*zoneTable, a.zone) == zoneTable->end()))) {
                a.visible = false;  // the exe clears the draw bit (+4 & 2) on both paths (0x447d40)
                a.flags18 |= 0x100;
                if (!(a.act3c & 0x40)) continue;
            }
            // & 0x20: projected centre within ±viewHalfX (exe 0x100 of the 512-wide 4:3 reference at H = 400; the app
            // widens it to its render FOV) (vertical test and multi-part corners omitted).
            if ((a.act3c & 0x20) && a.visible) {
                const double rx = a.pos.x - cam.eye.x, rz = a.pos.z - cam.eye.z;
                const double cz = (rx * fwd.x + rz * fwd.z) / fl, cx = (rx * fwd.z - rz * fwd.x) / fl;
                const double r = std::max({std::abs(a.box[0]), std::abs(a.box[3]), std::abs(a.box[2]), std::abs(a.box[5])});
                if (cz <= 0 || std::abs(cx * 400 / cz) - r * 400 / cz > viewHalfX) {
                    if (!(a.act3c & 0xc0)) continue;
                    a.visible = false;
                    a.flags18 |= 0x100;
                }
            }
        }
        if (a.type == 16 || a.type == 36) {
            a.visible = false, a.far = false;
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
        // Non-type-14 children are drawn through the chain (no behaviour of theirs moves them).
        if (a.parent != -1 && a.type != 14 && !(a.flags14 & 0x100)) followParent(a, {});
        ++a.timer;
        if (a.flags14 & 0x100) {
            a.dead = true;  // freed by 0x447a50 (0x41f5d0 releases its flag-0x10 voices)
            sfx.releaseOwned(emitter(&a));
            std::erase(order_, uint32_t(i));
            std::erase(collide_, i);
            if (p.platform == int(i)) { p.platform = -1; p.flags14 &= ~0x14u; }
            if (p.rope == int(i)) dropRope();
            detach(a);
            // Its slot is reused by spawns: its children see the parent's flags14 0x100 now (0x440120: dropped,
            // or killed with flags18 0x400).
            for (LitActor& c : actors)
                if (!c.dead && c.parent == int32_t(i)) {
                    if (c.flags18 & 0x400) c.flags14 |= 0x100;
                    else c.parent = -1, c.flags14 = (c.flags14 & ~0x14u) | 0x202;
                }
        }
    };
    auto run = [&](const std::vector<size_t>& v) {
        for (auto it = v.rbegin(); it != v.rend(); ++it) one(*it);
    };
    // The collision list 0x4b39ac is rebuilt in the same order (Bugs between the platforms and the rest; spawns are
    // appended by spawn()); contactPush walks it (runtime: L02A2 counter 227, spawner 327 after the planks).
    collide_.clear();
    for (const auto* v : {&first, &solid, &plats, &rest}) collide_.insert(collide_.end(), v->rbegin(), v->rend());
    run(first);
    run(solid);
    run(plats);
    // Carried (attached coordinate, 0x11c local offset zero): Bugs' world position is the carrier's as of his own
    // update, i.e. before a carrier in the rest list moves -> one tick behind it (runtime: L01a walk-in, Bugs z =
    // carrier z - 25 each tick). His own movement does not apply while attached.
    // Grabbed by an animation event (0x4393e0 f2 4): at the carrier's hand part plus the kept offset (ponytail: the
    // offset is turned by the carrier's yaw, not the part matrix).
    if (p.carrier >= 0 && actors[size_t(p.carrier)].dead) p.carrier = -1;
    const bool held = p.carrier >= 0;
    // World yaw = the coordinate chain's: the carrier's, or its hand part's matrix (runtime: CCMERLIN's walk to the
    // castle hangs Bugs on a type-14 part animated across the level; +0xde reads −660 throughout, the carrier's 0).
    int16_t heldYaw = 0;
    const V3 heldAt = held ? frameWorld(p.carrier, p.carrierPart, p.local, &heldYaw) : V3{};
    if (held) {
        p.pos = heldAt;
        p.yaw = heldYaw;
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
    case 8: rope(i); return;
    case 31: projectile(i); return;
    case 4: spriteEffect(i); return;
    case 0: {  // 0x43eee0 static prop: one rigid part (0x448980) for the model's object, no TOD playback (the title
               // copyright plane's anim 163 root scale 6336/4505 is never applied: RUNTIME_REFERENCE.md §14)
        LitActor& a = actors[i];
        if (a.animState.parts.empty()) {
            a.flags18 |= 0x80;
            if (defs[size_t(a.def)].model >= 0) a.animState.parts.push_back({.flags = 4, .object = 0, .parent = -1});
        }
        return;
    }
    case 3: case 7: case 11: case 15: case 17: case 20: case 21: case 30: case 33: case 35: return;  // no-op / texture link
    default: {
        // 10 tagger, 34 attached:
        LitActor& a = actors[i];
        const ActorDef& d = defs[size_t(a.def)];
        unsupported.insert(std::format("actor type {}", a.type));
        if (d.idleAnim) {
            animate(a, a.anim == d.idleAnim ? 0 : d.idleAnim, a.anim == d.idleAnim ? 0 : AnimLoop);
            a.anim = d.idleAnim;
        }
    }
    }
}

// 0x4addc8: s16 sine table in degrees (≈ -1024·sin, 360 entries), the rope swing's table (bugs.exe bytes).
constexpr int16_t kRopeSin[360] = {
    -1, -17, -35, -53, -71, -89, -107, -124, -142, -160, -177, -195, -212, -230, -247, -265, -282, -299,
    -316, -333, -350, -366, -383, -400, -416, -432, -448, -464, -480, -496, -512, -527, -542, -557, -572, -587,
    -601, -616, -630, -644, -658, -671, -685, -698, -711, -724, -736, -748, -760, -772, -784, -795, -806, -817,
    -828, -838, -848, -858, -868, -877, -886, -895, -904, -912, -920, -928, -935, -942, -949, -955, -962, -968,
    -973, -979, -984, -989, -993, -997, -1001, -1005, -1008, -1011, -1014, -1016, -1018, -1020, -1021, -1022, -1023, -1023,
    -1023, -1023, -1023, -1022, -1021, -1020, -1018, -1016, -1014, -1011, -1008, -1005, -1001, -997, -993, -989, -984, -979,
    -973, -968, -962, -955, -949, -942, -935, -928, -920, -912, -904, -895, -886, -877, -868, -858, -848, -838,
    -828, -817, -806, -795, -784, -772, -760, -748, -736, -724, -711, -698, -685, -671, -658, -644, -630, -616,
    -601, -587, -572, -557, -542, -527, -512, -496, -480, -464, -448, -432, -416, -400, -383, -366, -350, -333,
    -316, -299, -282, -265, -247, -230, -212, -195, -177, -160, -142, -124, -107, -89, -71, -53, -35, -17,
    0, 17, 35, 53, 71, 89, 107, 124, 142, 160, 177, 195, 212, 230, 247, 265, 282, 299,
    316, 333, 350, 366, 383, 400, 416, 432, 448, 464, 480, 496, 512, 527, 542, 557, 572, 587,
    601, 616, 630, 644, 658, 671, 685, 698, 711, 724, 736, 748, 760, 772, 784, 795, 806, 817,
    828, 838, 848, 858, 868, 877, 886, 895, 904, 912, 920, 928, 935, 942, 949, 955, 962, 968,
    973, 979, 984, 989, 993, 997, 1001, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1021, 1022, 1023, 1023,
    1023, 1023, 1023, 1022, 1021, 1020, 1018, 1016, 1014, 1011, 1008, 1005, 1001, 997, 993, 989, 984, 979,
    973, 968, 962, 955, 949, 942, 935, 928, 920, 912, 904, 895, 886, 877, 868, 858, 848, 838,
    828, 817, 806, 795, 784, 772, 760, 748, 736, 724, 711, 698, 685, 671, 658, 644, 630, 616,
    601, 587, 572, 557, 542, 527, 512, 496, 480, 464, 448, 432, 416, 400, 383, 366, 350, 333,
    316, 299, 282, 265, 247, 230, 212, 195, 177, 160, 142, 124, 107, 89, 71, 53, 35, 17,
};

// 0x43efc0 rope / chain. First call: the part list from animation 5 (once), the root node dropped (kept here as
// part 0 with an identity coordinate: it carries the root swing for drawing), segment length = box height (TOD packet
// 9 +0x4a) / node count, amplitude 100, phase 280, +0x1e default 0x80, box x ±inflation. Each tick (chains only,
// flags8 0x400000 = straight pole): the node bend sin(k)·(A/8) with A = sin(phase)·amp, k = 280 - 9 per node from the
// bottom, nodes from the grabbed one up straight; root pitch sin(phase + 90)·amp (sound +0x160 at each turn), root
// matrix Y·X·Z; phase += 4 degrees, amplitude -1 (clamp [+0x1e, 300]); radius / box from the bottom end (0, seg, 0)
// carried up the chain (x never carried, as the exe). Runtime: L02A1 chain at (13039, -5569, 23412): pitch -128..127,
// amp 128, seg 332 (RUNTIME_REFERENCE §13).
void LitWorld::rope(size_t i) {
    LitActor& a = actors[i];
    auto& parts = a.animState.parts;
    if (parts.empty()) {
        a.flags18 |= 0x80;
        animate(a, 5, AnimOnce);
        if (parts.size() < 2) return;
        parts[0].rot = {}, parts[0].trans = {}, parts[0].scale = {4096, 4096, 4096};
        a.ropeSeg = int16_t(a.box[1] / int16_t(parts.size() - 1));
        a.ropeAmp = 100, a.ropePhase = 0x118, a.ropePitch = a.rot[0];
        if (a.reg == 0) a.reg = 0x80;
        a.box[4] = 0, a.box[3] = a.inflate, a.box[0] = int16_t(-a.inflate);
        if (a.flags8 & 0x400000) a.radius = uint16_t(a.inflate), a.radius2 = uint32_t(a.inflate * a.inflate);
        rotMatrixXYZ(a.ropeM, a.ropePitch, a.yaw, a.rot[2]);
        a.ropeM.t = a.pos;
    }
    a.yaw = int16_t(wrapSigned(a.yaw));
    if ((a.flags8 & 0x400000) || parts.size() < 2) return;
    auto setPart = [](TodPart& p, int16_t rx) {
        p.rot = {rx, 0, 0};
        const Mat m = ropePartMatrix(p);
        for (size_t k = 0; k < 9; ++k) p.m[k] = float(m.m[k]) / 4096.0f;
    };
    const int32_t A = (kRopeSin[a.ropePhase] * a.ropeAmp) >> 10;
    int32_t k = 0x118;
    for (size_t q = parts.size() - 1; q >= 1; --q) {  // bottom node up to the top one
        if (int(q) == a.ropeGrab) k = 0xb4;
        int16_t rx = 0;
        if (k >= 0xb5) rx = int16_t((kRopeSin[k] * (A >> 3)) >> 10), k -= 9;
        setPart(parts[q], rx);
    }
    const int32_t ph = a.ropePhase + 0x5a > 0x167 ? a.ropePhase - 0x10e : a.ropePhase + 0x5a;
    const int16_t pitch = int16_t((kRopeSin[ph] * a.ropeAmp) >> 10);
    a.ropeGrab = -1;
    const int16_t side = pitch < a.ropePitch;
    if (side != a.ropeSide && a.param) playSound(&a, uint16_t(a.param));  // 0x43f261 / 0x43f29b
    a.ropeSide = side;
    a.ropePitch = pitch;
    setPart(parts[0], pitch);
    rotMatrixYXZ(a.ropeM, pitch, a.yaw, a.rot[2]);
    a.ropeM.t = a.pos;
    a.ropePhase = a.ropePhase + 4 > 0x167 ? a.ropePhase - 0x164 : a.ropePhase + 4;
    if (a.ropePhase & 2) --a.ropeAmp;
    if (--a.ropeAmp < a.reg) a.ropeAmp = a.reg;
    else if (a.ropeAmp > 300) a.ropeAmp = 300;
    V3 v{0, a.ropeSeg, 0};
    for (size_t q = parts.size() - 1; q >= 1; --q) {
        const V3 o = apply(ropePartMatrix(parts[q]), v);
        v = {0, int16_t(o.y + parts[q].trans[1]), int16_t(o.z + parts[q].trans[2])};
    }
    const V3 e = apply(a.ropeM, v);
    a.radius2 = uint32_t(e.x * e.x + e.z * e.z + a.inflate * a.inflate);
    a.radius = uint16_t(isqrt(int32_t(a.radius2)));
    a.box[1] = int16_t(e.y);
    if (e.z < 0) a.box[2] = int16_t(a.radius), a.box[5] = 0;
    else a.box[5] = int16_t(a.radius), a.box[2] = 0;
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
        if (f & 0x400000) {  // 0x437510: the target actor starts state P1 (title: Taz sets the logo / Bugs states)
            if (ta) startScript(*ta, uint16_t(it.p1)), ta->flags14 &= 0xf9fffdf6;
            else unsupported.insert("item flag 0x400000 on Bugs");
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
    // flagsB 0x2000000 animation rate (0x440120 head): accumulator clamped to [0, 0x999], frame skip (+0x148) =
    // min(acc·3 >> 11 + 2, 5) for the next frame step, then acc -= 8; the tail adds the root part's TOD y change.
    if (fb & 0x2000000) {
        rateAcc_ = std::clamp(rateAcc_, 0, 0x999);
        a.animState.skip = uint8_t(std::min((rateAcc_ * 3 >> 11) + 2, 5));
        rateAcc_ -= 8;
    }

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
    // 0x440120: flagsA 1 (stationary) or flags14 0x800 skips the whole motion block: bob, put-down, hover, slope,
    // speeds, turning, actor collision / body contact, hops, steering and MoveActor (runtime: L04A1 doors 165/177
    // keep flags14 clear of 2 / 0x200).
    const bool moves = !(fa & 1) && !(a.flags14 & 0x800);
    if (moves) {
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
        // flagsA 0x40 (without the unused pick-up 0x20): put the carried actor (+0x6c) down at the hand, its
        // horizontal offset from self traced against the walls (0x4376f0).
        if ((fa & 0x40) && !(fa & 0x20) && a.handSet && a.rider >= 0) {
            LitActor& c = actors[size_t(a.rider)];
            c.flags14 = (c.flags14 & 0xfffff7fd) | 0x1000;
            c.parent = -1;
            c.zone = a.zone;
            c.pos = a.hand;
            c.rot = a.rot;
            c.yaw = a.yaw;
            a.rider = -1;
            V3 q = a.pos;
            int32_t dx = c.pos.x - a.pos.x, dz = c.pos.z - a.pos.z;
            Ground r;
            r.zone = a.zone;
            hf.trace(q, dx, dz, r);
            c.pos.x = a.pos.x + dx;
            c.pos.z = a.pos.z + dz;
        }
        // flagsA 0x20000000 hover: flying (flags14 4), a sine bob (phase +0x108 from 0xc00, step +0x18, amplitude
        // +0x1a) and a random drift of 4 a tick for rand & 0xff ticks, turned around outside the band 200 high whose
        // top is 400 above the floor below (with 0x810000 in Bugs' zone: 200 above the floor while that is above Bugs,
        // else 100 above Bugs; over open floor 200 above Bugs). Outside every zone only the bob runs.
        if (fa & 0x20000000) {
            a.flags14 |= 4;
            if (!(a.flags14 & 1)) {
                a.vy = 0;
                a.phase = 0xc00;
                a.jumpStep = st->w[2];
                a.jumpH = st->w[3];
                a.flags14 |= 1;
            }
            const int32_t s = rsin(a.phase);
            a.phase = int16_t(a.phase + a.jumpStep);
            a.pos.y += (s * a.jumpH) >> 12;
            if (a.phase > 0xfff) a.phase = int16_t(a.phase - 0x1000);
            if (hf.findZone(a.pos, a.zone)) {
                V3 q = a.pos;
                Ground g;
                g.zone = a.zone;
                hf.groundAt(q, g);
                int32_t hi;
                if (g.kind & KindOpen) hi = p.pos.y - 200;
                else if (!(fa & 0x810000) || a.zone != p.zone) hi = g.hoffPrev - 400 + q.y;
                else if (q.y + g.hoffPrev < p.pos.y) hi = q.y + g.hoffPrev - 200;
                else hi = p.pos.y - 100;
                if (hi < a.pos.y) a.hops = 0;
                else if (a.pos.y < hi - 200) a.hops = 1;
                if (a.drift < 1) {
                    a.drift = int16_t(rand_() & 0xff);
                    a.hops = int16_t(rand_() & 1);
                } else {
                    --a.drift;
                    a.pos.y += a.hops == 0 ? -4 : 4;
                }
            }
        }
        // flagsA 0x40000 rolls with the slope: the floor 100 ahead vs here accelerates +0x192 (atan2 of the height
        // step over 100, clamped ±0x1000), forward speed +0x192·(+0x14) >> 12, and the floor 100 to either side
        // (the leading side by the sign of that speed) turns it downhill by atan2(Δ/2, 200).
        if (fa & 0x40000) {
            auto floorAt = [&](int16_t yaw, int32_t dist, int16_t& hoff) {
                const V3 v = rotY(yaw, 0, 0, dist);
                V3 q{a.pos.x + v.x, a.pos.y + v.y, a.pos.z + v.z};
                Ground g;
                g.zone = a.zone;
                hf.groundAt(q, g);
                hoff = g.hoff;
                return !(g.kind & (KindBlocked | KindOpen));
            };
            int16_t ahead = 0, here = 0;
            if (floorAt(a.yaw, 100, ahead) && floorAt(a.yaw, 0, here))
                a.drift = int16_t(std::clamp<int32_t>(a.drift + ratan2(ahead - here, 100), -0x1000, 0x1000));
            lz = int16_t(lz + ((a.drift * st->w[0]) >> 12));
            const int16_t side = int16_t(a.yaw + (int16_t(lz) < 0 ? -0x400 : 0x400));
            int16_t h1 = 0, h2 = 0;
            if (floorAt(side, 100, h1) && floorAt(side, -100, h2) && h1 != h2)
                a.yaw = int16_t(a.yaw + ratan2((h1 - h2) >> 1, 200));
        }
        const int32_t speed = (fb & 0x400000) ? int8_t(S[uint8_t(st->w[0])]) * 2 : st->w[0];
        if (!(fb & 0x80000000)) {
            if (fa & 2) lz += speed;  // 0x440120: forward speed +0x14 with flagsA 2
        } else {
            // flagsB sign: pad-steered (GD+0x12 / +0x14 stick, dt 2). flagsA 0x100000: constant speed, turns at most
            // 0x1e a tick toward the stick heading (camera-relative); else the speed scales with the stick past the
            // dead zone 50 and the heading closes by a quarter. Attached, the local yaw turns instead.
            // flagsA 0x100000 leans the root part (rot y, owned by code: part flag 0x400) 1/32 of the way toward the
            // turn wanted (±0x200; attached: the stick power, as the exe reuses that slot); no stick: it decays by 1/8.
            if (fa & 0x100000) lz += speed * 2;
            TodPart* root = (fa & 0x100000) && !a.animState.parts.empty() ? &a.animState.parts[0] : nullptr;
            auto lean = [&](int32_t want) {
                root->flags |= 0x400;
                root->rot[1] = int16_t(root->rot[1] - ((root->rot[1] - std::clamp(want, -0x200, 0x200)) >> 5));
                rebuildPart(*root);
            };
            if (pad.sx != 0 || pad.sy != 0) {
                const int32_t camYaw = ratan2(cam.target.x - cam.eye.x, cam.target.z - cam.eye.z);
                int32_t pw = (speed * 2 * std::min<int32_t>(0x80, (isqrt(pad.sx * pad.sx + pad.sy * pad.sy) - 50) * 2)) >> 7;
                if (pw == 0) pw = 1;
                auto wrap = [](int32_t v) { return v > 0x800 ? v - 0x1000 : v < -0x800 ? v + 0x1000 : v; };
                const int32_t want = wrap((camYaw + ratan2(pad.sx, pad.sy)) % 0x1000);
                if (a.parent == -1) {
                    a.yaw = int16_t(wrap(a.yaw));
                    int32_t dlt = wrap(want - a.yaw);
                    if (root) lean(dlt);
                    if (!(fa & 0x100000)) lz += pw, dlt >>= 2;
                    else dlt = std::clamp(dlt, -0x1e, 0x1e);
                    a.yaw = int16_t(a.yaw + dlt);
                } else {
                    int16_t py = 0;
                    frameWorld(a.parent, a.parentPart, {}, &py);
                    a.yaw = int16_t(py + a.localYaw);
                    int32_t dlt = wrap(a.yaw - want);
                    if (root) lean(pw);
                    if (!(fa & 0x100000)) lz += pw, dlt >>= 2;
                    else dlt = std::clamp(dlt, -0x1e, 0x1e);
                    a.localYaw = int16_t(a.localYaw - dlt);
                }
            } else if (root) {
                const int32_t r = int16_t(root->rot[1]);
                root->rot[1] = int16_t(r - ((r & ~7) ? r >> 3 : r));
                rebuildPart(*root);
            }
        }
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
    if (moves && (fb & 0x10000)) lz += (fb & 0x400000) ? int8_t(S[uint8_t(st->w[0])]) : st->w[0];
    if (moves && (fb & 0x10)) a.yaw = int16_t(ratan2(cam.eye.x - a.pos.x, cam.eye.z - a.pos.z));
    if (moves && (fa & 0x10209000)) {  // turn toward (0x10000000: away from) Bugs, or actor +0x16 with 0x200000
        // Attached: the turn goes to the local yaw (+0xc0 coordinate rot y under the parent's; runtime: L04B3 actor 98
        // on pillar 57 faces Bugs at counter 305); the move below still turns by the world rot +0xdc, which the tail
        // rebuilds only (runtime: 98 on 57 steps (8, −17) at counter 749 with yaw 1738, not the turned 1708).
        int16_t py = 0;
        const int16_t worldYaw = a.yaw;
        if (a.parent != -1) frameWorld(a.parent, a.parentPart, {}, &py), a.yaw = int16_t(py + a.localYaw);
        const Mover* tgt = &p;
        if (fa & 0x200000) {
            const int k = findActor(uint16_t(st->w[1]));
            tgt = k >= 0 ? &actors[size_t(k)] : nullptr;
        }
        if (tgt) {
            const bool away = fa & 0x10000000;
            // Hovering toward the target (0x20000000): closes the height to 50 under its box top by
            // dy·(0x1000 − len) >> 16 within 0x1000 (len = 0x4078d0's estimate).
            if ((fa & 0x20000000) && !away) {
                const int32_t dx = tgt->pos.x - a.pos.x, dz = tgt->pos.z - a.pos.z;
                const int32_t dy = tgt->pos.y - a.pos.y + 0x32 + (tgt == &p ? boxTop() : static_cast<const LitActor*>(tgt)->box[4]);
                if (const int32_t len = approxLen(dx, dy, dz); len < 0x1000) ly = int16_t(ly + ((dy * (0x1000 - len)) >> 16));
            }
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
        if (a.parent != -1) a.localYaw = int16_t(a.yaw - py), a.yaw = worldYaw;
    }
    V3 w = moves ? rotY(a.yaw, lx, ly, lz) : V3{};
    // Body contact on Bugs through the actor collision (0x433500 mask 0x20108, flagsA 0x800000).
    if (moves && (fa & 0x800000) && !(fa & 0x400000) && !(p.flags14 & 0x2000000) && !linkedToBugs(i, false) &&
        inBox(a, {p.pos.x - w.x, p.pos.y, p.pos.z - w.z}, kPlayerHalf, 0, -boxTop()) && damagePlayer(a, st) && st->w[4])
        forceState(uint16_t(st->w[4]));
    // Contact with another actor (the same 0x433500 call; contactPush): contact ids both ways. flagsB 0x200000 attaches
    // self to the actor +0x16 it touches unless that is a platform (local identity at its hand / root, its +0x6c = self;
    // runtime: L04B3 projectile 100 on actor 98). flagsA 0x20000 hurts it (+0x38 = +0x16, or 1). A platform (flags8 8)
    // is boarded by riders (flags8 0x40000 / flagsB 0x20000000, not rising) less than 100 below its top: child
    // coordinate at the inverse rotation of (x/z offset, top + 4), local yaw, flags14 0x14 (runtime: L04B3 actor 98
    // lands on pillar 57). Other non-sensor contacts push the move (x/z; y onto a platform) when the pushed point's
    // floor allows it; flagsA 0x40000 rollers bounce off instead (both face apart, random ±0x1ff, full speed).
    // The exe hands 0x433500 its int move through a short vector: (x, x >> 16, y) — z is the y move, so only x
    // (and y as z) is pushed against (runtime, 0x433500 hooked: L04B3 ball 100's spawn tick takes (32, 0, 0) ->
    // (−62, −31, −61), local/wine/compare/w9lit7/h_L04B3.txt).
    bool attached = false;  // 0x440120 bVar13: no hop this tick
    if (moves && !(fa & 0x400000)) {
        V3 c{int16_t(w.x), int16_t(w.x >> 16), int16_t(w.y)};
        if (const int h = contactPush(a, int32_t(i), a.inflate, a.radius, a.box, 0x20108, c); h >= 0) {
            LitActor& o = actors[size_t(h)];
            o.contact = a.entityId;
            a.contact = o.entityId;
            if ((fb & 0x200000) && st->w[1] == int16_t(o.entityId) && !(o.flags8 & 8)) {
                detach(a);
                attach(a, h, o.handSet);
                o.rider = int32_t(i);
                a.flags14 = (a.flags14 & 0xfff7e5e4) | 0x40000000;
                o.flags14 |= 0x40000000;
                attached = true;
            }
            if ((fa & 0x20000) && !(o.flags14 & 0x2000000) &&
                (o.type != 14 || o.step < 0 || !(defs[size_t(o.def)].steps[size_t(o.step)].flagsA & 0x40))) {
                o.damage = (((a.flags8 & 0x10000000) && (a.flags14 & 0x201)) || (fa & 0x200000)) ? 1 : st->w[1];
                o.flags14 |= 0x2000000;
                a.flags14 |= 0x8000000;
            }
            const bool rider = (a.flags8 & 0x40000) || (fb & 0x20000000);
            if ((o.flags8 & 8) && rider && a.parent == -1 && !(a.flags14 & 0x10) && (!(a.flags14 & 1) || (a.flags14 & 8)) &&
                -100 < c.y) {
                const bool centred = o.flagsC & 0x1000000;
                Mat inv;
                rotMatrixZYX(inv, 0x1000 - o.rot[0], 0x1000 - o.yaw, 0x1000 - o.rot[2]);
                a.local = apply(inv, {centred ? 0 : int16_t(a.pos.x - o.pos.x), o.box[4] + 4, centred ? 0 : int16_t(a.pos.z - o.pos.z)});
                a.localYaw = a.matYaw = int16_t(a.yaw - o.yaw);
                a.parent = h;
                a.parentPart = -1;
                a.pos.y = o.pos.y + o.box[4];
                w.y = 0;
                a.flags14 = (a.flags14 & 0xfffffdf6) | 0x40000014;
                o.flags14 |= 0x40000000;
                attached = true;
            }
            if (!(o.flags8 & 0x2000000) && a.parent != h) {
                a.flags14 |= 0x40000000;
                o.flags14 |= 0x40000000;
                if (!(fa & 0x40000)) {
                    V3 q{a.pos.x + c.x, a.pos.y, a.pos.z + c.z};
                    Ground g;
                    g.zone = a.zone;
                    hf.groundAt(q, g);
                    if (!(g.kind & ((~(fa >> 16) & 8) | 4)) && !(a.flags8 & 0x2000000) && (o.flags8 & 0x20100)) {
                        if (a.parent == -1 && (o.flags8 & 8) && -100 < c.y && rider) w.y += c.y;
                        else w.x = c.x, w.z = c.z;
                    }
                } else {
                    auto parentYaw = [&](const LitActor& x) { return x.parent == kParentBugs ? p.yaw : actors[size_t(x.parent)].yaw; };
                    int32_t ang = ratan2(int16_t(a.pos.x - o.pos.x), int16_t(a.pos.z - o.pos.z));
                    if (o.parent != -1) o.localYaw = int16_t(ang - parentYaw(o) + 0x800);
                    o.yaw = int16_t(ang + 0x800);
                    o.drift = 0x1000;
                    ang += int32_t(rand_() & 0x1ff);
                    if (a.parent != -1) a.localYaw = int16_t(ang - parentYaw(a));
                    a.yaw = int16_t(ang);
                    a.drift = 0x1000;
                    w = rotY(a.yaw, lx, ly, st->w[0]);
                }
            }
        }
    }
    // Ballistic hop (flagsA 0x10, flags14 clear of 9, | 0x200 unless flagsB 0x800). On the ground with flagsA
    // 0x8000000 it bounces: hop n steps the arc n × +0x18 until that reaches 0x400, then flags14 0x800000 ends it
    // (runtime: L01A carrots 39 from enemy 932, second hop at counters 1159-1175 twice as fast). Else it leaves its
    // parent and either aims at Bugs (flags8 0x8000000 / flagsB 0x100) or actor +0x16 (flags8 0x10000000 / flagsB
    // 0x100000): n = (horizontal distance − (½ its box top − own y + its y) / 70 · speed) / speed ticks of forward
    // speed +0x14, arc step 0x800 / n (none left: free fall 0x200), or hops in place (step +0x18).
    if (moves && !attached && (fa & 0x10) && !(a.flags14 & ((fb & 0x800) ? 9u : 0x209u))) {
        if ((a.flags14 & 0x10) && a.parent >= 0) {  // riding: from the platform's x/z, the offset traced (0x434cd0)
            const LitActor& pl = actors[size_t(a.parent)];
            V3 q{pl.pos.x, a.pos.y, pl.pos.z};
            int32_t dx = a.pos.x - pl.pos.x, dz = a.pos.z - pl.pos.z;
            Ground g;
            g.zone = a.zone;
            hf.trace(q, dx, dz, g);
            a.pos.x = pl.pos.x + dx;
            a.pos.z = pl.pos.z + dz;
        }
        auto startArc = [&](int32_t stepPh) {
            a.phase = 0x400;
            a.jumpH = st->w[3];
            a.jumpStep = int16_t(stepPh);
            a.vy = 0;
            a.flags14 = (a.flags14 & 0xeffff9f1) | 1;
        };
        auto leave = [&] {
            detach(a);
            a.flags14 = (a.flags14 & ~0x14u) | 2;
        };
        if ((fa & 0x8000000) && (a.flags14 & 0x12)) {
            const int32_t hop = st->w[2] * ++a.hops;
            if (hop < 0x400) leave(), startArc(hop);
            else a.flags14 = (a.flags14 & 0xfff7efff) | 0x800000;
        } else if ((a.flags8 & 0x8000000) || (fb & 0x100) || (a.flags8 & 0x10000000) || (fb & 0x100000)) {
            const Mover* tgt = &p;
            int16_t top = boxTop();
            if (!(a.flags8 & 0x8000000) && !(fb & 0x100)) {  // ponytail: live-list order stands in for 0x4b39ac's
                const int k = findActor(uint16_t(st->w[1]));
                tgt = k >= 0 ? &actors[size_t(k)] : nullptr;
                if (tgt) top = actors[size_t(k)].box[4];
            }
            if (tgt && st->w[0]) {
                const int32_t dx = int16_t(tgt->pos.x - a.pos.x), dz = int16_t(tgt->pos.z - a.pos.z);
                const int32_t d = isqrt(dx * dx + dz * dz) - ((top >> 1) - a.pos.y + tgt->pos.y) / 0x46 * st->w[0];
                leave();
                if (const int32_t n = d > 0 ? d / st->w[0] : 0; n != 0) startArc(0x800 / n);
                else a.flags14 |= 0x200;
            }
        } else {
            leave();
            startArc(st->w[2]);
        }
    }
    if (moves && !attached && (fa & 0x10) && (a.flags14 & 1)) arcTick(a);
    if ((w.x | w.y | w.z) && !(fa & 0x80000000) && !(fb & 0x20002) && a.parent == -1) {
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
    // 0x437390 (also on attached actors, +0xc4 set; runtime: L04B3 ball 100 attached to 98's hand at counter 607
    // takes y +29 from the ground under the hand), before the rider update; the resolved move then goes to the local
    // translation (attached) or the world position (root, or released from its platform below).
    if (moves && (!(fa & 0x80000000) || (fb & 4) || a.parent != -1)) {
        const int32_t keepY = w.y, sumXZ = w.x + w.z;
        Ground r;
        moveActor(a, w, r, a.box[4]);
        if (w.x + w.z != sumXZ) a.flags14 |= 0x100000;  // 0x440120: the move was cut (wall) -> blocked
        w.y = (((fb & 4) || (fa & 0x80000000)) ? 0 : w.y) + keepY;  // the step's own y speed back (flagsB 4 flies: no gravity)
        if (r.kind & KindOpen) a.flags18 |= 0x20;
    }
    // Riding a platform (flags14 0x10, 0x440120 tail): local height = its box top + 4, contact flags / ids, then 0x434230
    // at the platform-carried position: off its box -> own coordinate at the platform's x/z + the traced offset
    // (0x434cd0), falling (0x200); a put-down platform (flags14 0x1000) leaves it standing with the platform's
    // rotation; a dead / non-platform parent drops it (0x202).
    if ((a.flags14 & 0x10) && a.parent >= 0) {
        LitActor& pl = actors[size_t(a.parent)];
        a.local.y = pl.box[4] + 4;
        a.flags14 |= 0x40000000;
        pl.flags14 |= 0x40000000;
        if (!pl.contact) pl.contact = a.entityId;
        if (!a.contact) a.contact = pl.entityId;
        followParent(a, {});
        const bool putDown = pl.flags14 & 0x1000;
        if ((pl.flags14 & 0x100) || !(pl.flags8 & 8)) {
            detach(a);
            a.flags14 = (a.flags14 & ~0x14u) | 0x202;
        } else if (putDown || !overPlatform(pl, a, {a.rot[0], a.yaw, a.rot[2]}, a.box)) {
            V3 q{pl.pos.x, a.pos.y, pl.pos.z};
            int32_t dx = a.pos.x - pl.pos.x, dz = a.pos.z - pl.pos.z;
            Ground g;
            g.zone = a.zone;
            hf.trace(q, dx, dz, g);
            a.pos.x = pl.pos.x + dx;
            a.pos.z = pl.pos.z + dz;
            detach(a);
            if (putDown) {
                a.flags14 = (a.flags14 & 0xfffff7e9) | 0x1000;
                a.rot = pl.rot;
                a.yaw = pl.yaw;
            } else {
                a.flags14 = (a.flags14 & ~0x14u) | 0x200;
                a.rot[2] = 0;
            }
        }
    }
    if (a.parent != -1) {
        followParent(a, w);
    } else if (moves) {
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
            uint16_t next = it.next;
            if (f) {
                if ((f & 8) && a.parent != -1) detach(a);  // leave the parent at the current world position
                if (f & 0x200) playSound(&actors[i], uint32_t(it.p1));
                if (f & 0x400) playSound(&actors[i], uint32_t(it.p1), 2);
                if (f & 0x40100) {  // spawn template P2 at the hand (+0x58) if any, else at self
                    const int k = spawn(uint16_t(it.p2), a.handSet ? a.hand : a.pos, a.yaw);
                    if (k >= 0) {
                        LitActor& c = actors[size_t(k)];
                        if (f & 0x1000000) c.reg = a.reg;
                        c.zone = a.zone;
                        if (f & 0x80) attach(c, (f & 0x1000) ? kParentBugs : int32_t(i), !(f & 0x1000) && a.handSet);
                        else if (f & 0x40000) c.yaw = int16_t(rand_() & 0xfff);
                    }
                } else if (f & 0x20000000) {  // attach actor P2 to self (to Bugs with 0x1000); carried (+0x6c) without 0x80
                    if (uint16_t(it.p2) == p.entityId) {
                        if (!(p.flags14 & 0x1000000)) {
                            // 0x440120: Bugs' coordinate becomes a child of the carrier's (+0xc4, +0x70); his flags14
                            // are left alone (runtime: 2 throughout the L01a walk-in) and the turn error 0x4b3c20 reset.
                            if (f & 0x1000000) p.reg = a.reg;
                            p.carrier = int32_t(i);
                            p.carrierPart = attachPart(int32_t(i), a.handSet);
                            p.local = {};
                            p.platform = p.rope = -1;
                            turnErr_ = -1;
                            if (!(f & 0x80)) a.rider = -2;
                        }
                    } else if (const int k = findActor(uint16_t(it.p2)); k >= 0) {
                        LitActor& c = actors[size_t(k)];
                        if (f & 0x1000000) c.reg = a.reg;
                        if (c.parent != -1) {  // taken from another parent (that one's +0x6c cleared, self's flags14 & ~0x14)
                            detach(c);
                            a.flags14 &= ~0x14u;
                        }
                        attach(c, (f & 0x1000) ? kParentBugs : int32_t(i), !(f & 0x1000) && a.handSet);
                        if (!(f & 0x80)) a.rider = k;
                    }
                } else if (f & 0x1000) {  // attach self to Bugs (until item flag 8 detaches)
                    attach(a, kParentBugs, false);
                } else if (f & 0x20) {  // 0x434740: Bugs' local z (0x407b90 of 0x1000 − rot) before box z0 (next P1) / past z1 (P2)
                    Mat inv;
                    rotMatrixZYX(inv, 0x1000 - a.rot[0], 0x1000 - a.yaw, 0x1000 - a.rot[2]);
                    const V3 l = apply16(inv, {int16_t(p.pos.x - a.pos.x), int16_t(p.pos.y - a.pos.y), int16_t(p.pos.z - a.pos.z)});
                    if (l.z < a.box[2]) next = uint16_t(it.p1);
                    else if (l.z > a.box[5]) next = uint16_t(it.p2);
                    else continue;
                } else if (f & 0x4000000) {
                    if (const int k = findActor(uint16_t(it.p2)); k >= 0) actors[size_t(k)].flags14 |= 0x100;
                }
                if (f & 0x80000) nextLevel = it.p1;  // goto level P1
                if (f & 0x80000000) forceState(uint16_t(it.p2));
                if (f & 0x100000) { checkpoint_ = a.pos; checkpointYaw_ = a.yaw; }
                if (f & 0x800) {  // 0x4379b0: Bugs to the hand (+0x58) if any, else to self
                    p.pos = a.handSet ? a.hand : a.pos;
                    p.yaw = a.yaw;
                    p.platform = p.rope = -1;
                    p.flags14 &= ~0x14u;
                    cameraSnap(p.pos);
                }
                if ((f & 0x4000) && a.rider == -2) {  // release Bugs at his attached world position = the carrier's
                    a.rider = -1;                       // current one (coordinate chain), x/z rotation cleared
                    const V3 at = frameWorld(int32_t(i), p.carrierPart, p.local);
                    p.carrier = -1;
                    p.carrierPart = -1;
                    p.pos = at;
                    p.zone = a.zone;
                    p.lean = 0;
                    p.flags14 = (p.flags14 & 0xfffff5f6) | 0x1000;
                } else if ((f & 0x4000) && a.rider >= 0) {  // release the carried actor where it is, x/z rotation cleared
                    LitActor& c = actors[size_t(a.rider)];
                    c.flags14 = (c.flags14 & 0xfffff5f6) | 0x1000;
                    c.parent = -1;
                    c.zone = a.zone;
                    c.rot[0] = c.rot[2] = 0;
                    a.rider = -1;
                }
                if (f & 0x10000) {
                    if (it.p2 != 1) { a.flags14 |= 0x100; return; }
                    kill(int16_t(it.p1));
                }
                if (f & 0x2000000) { a.flags14 |= 0x100; return; }
                if (f & 0x8000) continue;
                if (f & 0x10) break;
            }
            startScript(a, next);
            a.flags14 &= 0xf9fffdf6;
            a.hops = 0;
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
    // flagsB 0x2000000 tail (0x440120): acc += change of the TOD translation y (coord param +0x1c) of the list's
    // second part (`**(+0x78)`: the head part's next; the head is the skeleton root), not counted on frame 1; the
    // last y is kept only by such steps. Runtime: L03C1's intro actor (0, -30000, 0) = RUNTIME_REFERENCE §20.
    if (a.step >= 0 && (step().flagsB & 0x2000000) && a.animState.parts.size() > 1) {
        const int32_t y = a.animState.parts[1].trans[1];
        if (a.animState.frame != 1) rateAcc_ += y - rateY_;
        rateY_ = y;
    }
    // Camera focus 0x4b3950: flagsC 0x10 or step flagsB 0x40 always, flagsC 4 while Bugs is within 2000 (xz, 0x407970).
    if (const bool sf = a.step >= 0 && (step().flagsB & 0x40); (a.flagsC & 0x14) || sf)
        if ((a.flagsC & 0x10) || sf || approxLen(p.pos.x - a.pos.x, 0, p.pos.z - a.pos.z) < 2000) focus_ = int32_t(i);
    if (a.flagsC & 0x20000000) a.pos = {cam.eye.x, cam.eye.y + a.reg, cam.eye.z};  // follows the camera eye
    a.matYaw = a.localYaw;  // the coordinate matrix is rebuilt from the local rotation after the world yaw was read
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
                if (f & 0x200) playSound(&a, uint32_t(it.p1));
                if (f & 0x400) playSound(&a, uint32_t(it.p1), 2);
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
        if (f & 0x400000) {  // 0x437510: the live actor with id P2 starts state P1 (no distance gate); none: skip
            const int t = findActor(uint16_t(it.p2));
            if (t < 0) continue;
            startScript(actors[size_t(t)], uint16_t(it.p1));
            actors[size_t(t)].flags14 &= 0xf9fffdf6;
        } else if (f & 0x18000000) {
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
        if (f & 0x200) playSound(&a, uint32_t(it.p1));
        if (f & 0x400) playSound(&a, uint32_t(it.p1), 2);
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
        if (f & 0x800) { p.yaw = b.yaw; p.pos = b.pos; p.platform = p.rope = -1; p.flags14 = (p.flags14 & ~0x14u) | 0x80000000; cameraSnap(p.pos); }
        if (f & 0x80000000) forceState(uint16_t(it.p2));
        if (f & 0x80000) nextLevel = it.p1;
        if (f & 0x10000) { b.flags14 |= 0x100; return; }
        if (!(f & 0x8000)) return;
    }
}

// ---- type 36 camera director 0x444b50: items set the scripted camera points read by the camera's 0xc00 mode.
// Item layout as type 16 (tag 0x31): point = (P1, P2, u16 `next`). Flags: 0x8000/0x10 distance gate (<= / >= P1) to
// Bugs (0x20) or actor P2 (0x40), y with 0x100; 1 eye point (self 0x80 / actor P2 0x40, with 0x20000 Bugs' offset
// turned into the actor's frame / cam.eye 0x100000 / behind Bugs 0x80000 / literal) + 0x40000 pull toward the eye,
// sets 0x400; 4 clears 0x400|0x4000; 2 target point (self
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
        auto actorAt = [&](bool self) -> const LitActor* {
            if (self) return &actors[i];
            const int k = findActor(uint16_t(it.p2));
            return k < 0 ? nullptr : &actors[size_t(k)];
        };
        auto actorPos = [&](bool self) -> const V3* {
            const LitActor* s = actorAt(self);
            return s ? &s->pos : nullptr;
        };
        if (f & 1) {
            const LitActor* src = (!(f & 0x40) || (f & 2)) ? ((f & 0x80) ? &actors[i] : nullptr) : actorAt(false);
            if (src && (f & 0x20000)) {
                // Bugs' xz offset from the actor (s16) into its yaw frame (RotMatrix(0, 0x1000 - yaw, 0)), then through
                // RotMatrix(actor rot) with the z column scaled to 0 (ScaleMatrix {1, 1, 0}), added to the actor.
                Mat m;
                rotMatrixXYZ(m, 0, 0x1000 - src->yaw, 0);
                V3 v = apply16(m, {int16_t(int16_t(p.pos.x) - int16_t(src->pos.x)), 0, int16_t(int16_t(p.pos.z) - int16_t(src->pos.z))});
                rotMatrixXYZ(m, src->rot[0], src->yaw, src->rot[2]);
                for (size_t r = 0; r < 3; ++r) m.m[r * 3 + 2] = 0;
                v = apply16(m, v);
                cam.point = {src->pos.x + v.x, src->pos.y + v.y, src->pos.z + v.z};
            } else if (src) {
                cam.point = src->pos;
            } else if (!(f & 0x80000)) {
                cam.point = (f & 0x100000) ? cam.eye : lit;
            } else {  // behind Bugs: R(yaw + 0x800)·(0, -P2, P1), both points eased >> 2
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
    } else if (mode & 1) {  // seesaw: tilts toward Bugs' local x (±0x164) and slides him down the slope
        const int32_t sp = a.reg ? a.reg : 0x20, lx = p.local.x;
        const int32_t d = lx < 0 ? a.box[0] - a.inflate : a.inflate + a.box[3];
        if (ridden && d && !(a.param & (lx < 0 ? 1 : 2))) {
            if (lx < 0 ? -0x164 < a.rot[2] : a.rot[2] < 0x164) a.rot[2] = int16_t(a.rot[2] + int16_t((lx < 0 ? -1 : 1) * ((sp * lx) / d)));
            p.local.x += (lx < 0 ? -1 : 1) * ((sp * a.rot[2]) / d);
        } else {
            a.rot[2] = int16_t(a.rot[2] - (a.rot[2] >> 4));
        }
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
    } else if (ridden && ((mode & 0x100) || ((mode & 0x200) && (a.flags14 & 0x200000)))) {  // conveyor: rider local z
        p.local.z += a.param;
    }
    if (mode & 0x20) {  // bob in Y (also 0x22 bob + spin)
        const int32_t c = (rsin(a.p17e) * a.param) >> 12;
        const int16_t ph = a.p17e;
        a.p17e = int16_t(a.reg + ph);
        a.flags14 = ((ph < 0x400 && a.p17e > 0x3ff) || (ph < 0xc00 && a.p17e > 0xbff)) ? a.flags14 | 0x2000 : a.flags14 & ~0x2000u;
        a.p17e &= 0xfff;
        a.pos = {a.base.x, a.base.y + c, a.base.z};
    }
    if (ridden) {  // the rider is a child coordinate of the platform: world = platform matrix · local (0x43f520 mode 8)
        p.pos = platformWorld();
        p.yaw = int16_t(p.yaw + (a.yaw - yaw0));
    }
    const ActorDef& d = defs[size_t(a.def)];
    for (const ActorItem& it : d.items) {  // gated list: first firing item stops unless 0x8000
        if ((it.mask14 && !(a.flags14 & it.mask14)) || (it.mask18 && !(a.flags18 & it.mask18))) continue;
        if (!cond(it.item, &a)) continue;
        act(it.item, &a);
        if (it.flags & 0x200) playSound(&a, uint32_t(it.p1));
        if (it.flags & 0x400) playSound(&a, uint32_t(it.p1), 2);
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

// ---- type 4 sprite effect (0x4452a0) ----

// First call: one sprite part (0x448980) showing record 0 of the first listed class-8 asset, frame 0, 2 ticks per
// frame; box (-w/2, 0, -w/2, w/2, -h, w/2) and radius w/2 from record 0's texel size, flagsC 0x4000000 lowers it
// by h/2. Each tick: countdown 0 -> next record (after the last: record 0), countdown 2; countdown -= 1. Then the
// first item whose condition holds (0x42a7d0, none = true) runs its action; 0x200 plays P1 on frame 0's second tick,
// 0x400 plays P1 (mode 2) every tick, 0x10000 frees it on the last frame's second tick. Drawn as a billboard
// (0x44c170 / 0x438280, apps/bbtime): 2w x 2h world units under the scale, bottom at the position (centred with
// flagsC 0x8000000), whole texture, white, not below depth 150. A def without a class-8 sprite hangs the exe
// (0x4452a0's empty-list loop); here it does nothing.
void LitWorld::spriteEffect(size_t i) {
    LitActor& a = actors[i];
    const ActorDef& d = defs[size_t(a.def)];
    if (!d.spriteFrames) return;
    AnimState& st = a.animState;
    if (!a.boxInit) {
        a.boxInit = true;
        a.flags18 |= 0x80;
        st.frame = 0, st.last = uint16_t(d.spriteFrames - 1), st.ticksPerFrame = 2, st.countdown = 2;
        const int16_t hw = int16_t(uint16_t(d.spriteW) >> 1);
        a.box = {int16_t(-hw), 0, int16_t(-hw), hw, int16_t(-d.spriteH), hw};
        if (a.flagsC & 0x4000000) a.pos.y += uint16_t(d.spriteH) >> 1;
        a.radius = uint16_t(hw);
        a.radius2 = uint32_t(hw * hw + a.inflate * a.inflate);
    }
    if (st.countdown == 0) {
        st.countdown = st.ticksPerFrame;
        if (++st.frame > st.last) st.frame = 0;
    }
    --st.countdown;
    for (const ActorItem& it : d.items) {
        if (it.item.cond && !cond(it.item, &a)) continue;
        if (it.item.act) act(it.item, &a);
        LitActor& b = actors[i];
        if ((it.flags & 0x200) && b.animState.countdown == 0 && b.animState.frame == 0) playSound(&b, uint32_t(it.p1));
        if (it.flags & 0x400) playSound(&b, uint32_t(it.p1), 2);
        if ((it.flags & 0x10000) && b.animState.countdown == 0 && b.animState.frame >= b.animState.last) b.flags14 |= 0x100;
        break;
    }
}

// ---- type 31 projectile (0x446d10) ----

// First call: unless flagsC 0x20020 it pitches toward Bugs' middle (rx = −atan2(dy, horizontal distance), dy to his
// box half height) and spawns the muzzle template 0x14; it does not move that tick. Then each tick: its matrix
// (0x407b90 of rx, yaw, roll) turns (0, 0, +0x20) into the step, traced against the walls (0x434cd0); the end point
// tested against Bugs / actors with flags8 0x180 (0x433380): Bugs takes a hit (0x437d90, then state +0x196 if set),
// an actor gets its contact id and flags14 0x2000000 (+0x38 = 1) when its step takes objects (flagsB 8), else
// 0x40000000; a hit or a wall spawns the impact template 0x13 and removes it, as do 60 ticks.
void LitWorld::projectile(size_t i) {
    LitActor* a = &actors[i];
    int32_t speed = a->inflate;
    if (!a->boxInit) {
        a->boxInit = true;
        a->flags18 |= 0x80;
        if (defs[size_t(a->def)].model >= 0 && a->animState.parts.empty()) a->animState.parts.push_back({.flags = 4, .object = 0, .parent = -1});
        a->act3c |= 0x40;
        a->timer = 0;
        speed = 0;
        if (!(a->flagsC & 0x20020)) {
            const int32_t dx = p.pos.x - a->pos.x, dy = p.pos.y - a->pos.y + (boxTop() >> 1), dz = p.pos.z - a->pos.z;
            a->rot[0] = int16_t(-ratan2(dy, isqrt(dx * dx + dz * dz)));
            a->rot[2] = 0;
        }
        const V3 at = a->pos;
        const int16_t yaw = a->yaw;
        spawn(0x14, at, yaw);
        a = &actors[i];  // spawn may grow the vector
    }
    if (a->timer > 0x3c) { a->flags14 |= 0x100; return; }
    Mat m;
    rotMatrixZYX(m, a->rot[0], a->yaw, a->rot[2]);
    V3 v = apply(m, {0, 0, speed});
    V3 q = a->pos;
    Ground r;
    r.zone = a->zone;
    hf.trace(q, v.x, v.z, r);
    const V3 to{a->pos.x + v.x, a->pos.y + v.y, a->pos.z + v.z};
    LitActor* hit = nullptr;
    const bool hitBugs = handHitsPlayer(to);
    if (!hitBugs)
        for (LitActor& o : actors)
            if (&o != a && o.active && !o.dead && (o.flags8 & 0x180) && pointInBox(o, to)) { hit = &o; break; }
    if (hitBugs || hit) {
        a->reg = 1;
        if (hitBugs) {
            if (!(p.flags14 & 0x3000000) && damagePlayer(*a, nullptr) && a->param) forceState(uint16_t(a->param));
        } else {
            hit->contact = a->entityId;
            const bool takes = hit->type == 14 && hit->step >= 0 && (defs[size_t(hit->def)].steps[size_t(hit->step)].flagsB & 8);
            if (takes) hit->damage = 1, hit->flags14 |= 0x2000000;
            else hit->flags14 |= 0x40000000;
        }
    } else {
        a->pos = to;
        if (!(r.kind & KindBlocked)) return;
    }
    const V3 at = a->pos;
    const int16_t yaw = a->yaw;
    actors[i].flags14 |= 0x100;
    spawn(0x13, at, yaw);
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
bool LitWorld::boxPush(const Mover& m, int32_t mi, int32_t mRadius, const std::array<int16_t, 6>& mb, const LitActor& o, V3& w,
                       int32_t* top) const {
    const auto& ob = o.box;
    const int32_t oi = o.inflate;
    if (!(ob[0] | ob[1] | ob[2] | ob[3] | ob[4] | ob[5])) return false;  // 0x407890: empty box
    const int32_t orad = o.radius;
    const int32_t nx = int16_t(m.pos.x + w.x - o.pos.x), nz = int16_t(m.pos.z + w.z - o.pos.z);
    const int32_t rr = mi + mRadius + orad + oi;
    if (uint32_t(nx * nx + nz * nz) > uint32_t(rr * rr)) return false;
    const int16_t inv = int16_t(0x1000 - o.yaw);
    const V3 rel{int16_t(m.pos.x - o.pos.x), int16_t(int16_t(m.pos.y - o.pos.y) + w.y), int16_t(m.pos.z - o.pos.z)};
    const V3 r = rotY(inv, rel.x, rel.y, rel.z);
    if (top) *top = ob[4] - r.y;
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
        const V3 cw = rotY(m.yaw, k[0], 0, k[1]);
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

// 0x433500's candidate skip for self s vs actor o: o is s's parent (+0x70) or carried (+0x6c), s is o's parent or
// carried, o's parent is Bugs, or s is up o's parent chain. Bugs' +0x70 = his carrier / platform / rope. fromBugs:
// s = Bugs, o = actor i; else s = actor i, o = Bugs (runtime: L03C1 carrier 268 never touches the Bugs it carries).
bool LitWorld::linkedToBugs(size_t i, bool fromBugs) const {
    const LitActor& a = actors[i];
    const int32_t bp = p.carrier >= 0 ? p.carrier : p.platform >= 0 ? p.platform : p.rope;
    if (bp == int32_t(i) || a.rider == -2 || a.parent == kParentBugs) return true;
    for (int32_t k = fromBugs ? a.parent : bp; k >= 0; k = actors[size_t(k)].parent)
        if (fromBugs ? actors[size_t(k)].parent == kParentBugs : k == int32_t(i)) return true;
    return false;
}

// 0x433500 for mover s (actor si, or Bugs: si = kParentBugs) against the collision actors of `mask`. Candidates in
// list order, skipping s's parent / carried, actors whose parent / carried is s, whose parent is Bugs or whose parent
// chain holds s (Bugs: linkedToBugs). Each face push (boxPush) replaces the x/z move (halved with the move so far
// when a contact exists and the candidate is not solid 0x20000). The contact kept: the nearest (s's next position,
// x/z), platforms (flags8 8) first, solids over the rest; when it is not solid the solids get a second pass. Returns
// it with w.y = its box top − s's y in its frame (0x433500's +0x78), -1 none (w unchanged). Bugs also touches a
// platform he overlaps from above without a corner hit (ponytail: stands in for 0x433500's box-corner-in-quad pass).
int LitWorld::contactPush(const Mover& s, int32_t si, int32_t inflate, int32_t radius, const std::array<int16_t, 6>& box,
                          uint32_t mask, V3& w) {
    const bool bugs = si == kParentBugs;
    const V3 at{s.pos.x + w.x, 0, s.pos.z + w.z};
    const V3 q{s.pos.x + w.x, s.pos.y + w.y, s.pos.z + w.z};
    int best = -1;
    uint32_t bestD = ~0u;
    int32_t bestTop = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (const size_t k : collide_) {  // 0x4b39ac order (actorsTick)
            const LitActor& o = actors[k];
            const int32_t ki = int32_t(k);
            if (ki == si || !o.active || o.dead || o.def < 0 || defs[size_t(o.def)].cls != 2 || !(o.flags8 & mask) ||
                (pass && !(o.flags8 & 0x20000)))
                continue;
            if (bugs) {
                if (linkedToBugs(k, true)) continue;
            } else {
                const LitActor& sa = actors[size_t(si)];
                if (sa.parent == ki || sa.rider == ki || o.parent == si || o.rider == si || o.parent == kParentBugs) continue;
                bool up = false;
                for (int32_t c = o.parent; c >= 0 && !up; c = actors[size_t(c)].parent) up = c == si;
                if (up) continue;
            }
            V3 wp = w;
            int32_t top = 0;
            if (!boxPush(s, inflate, radius, box, o, wp, &top)) {
                if (!bugs || !(o.flags8 & 8) || !inBox(o, q, kPlayerHalf, 0, -boxTop())) continue;
                wp = w;
                top = o.pos.y + o.box[4] - q.y;
            }
            const bool solid = o.flags8 & 0x20000;
            if (best < 0 || solid) {
                w.x = wp.x, w.z = wp.z;
            } else {
                w.x = int16_t((w.x + wp.x) >> 1), w.z = int16_t((w.z + wp.z) >> 1);
            }
            const int32_t dx = int16_t(at.x - o.pos.x), dz = int16_t(at.z - o.pos.z);
            const uint32_t d = uint32_t(dx * dx + dz * dz);
            const uint32_t bf = best >= 0 ? actors[size_t(best)].flags8 : 0;
            if (!((d < bestD || (o.flags8 & 8) || (bf & 8)) && (best < 0 || !(bf & 0x20000) || solid))) continue;
            const bool take = !(o.flags8 & 8) ? best < 0 || (!(bf & 8) && (!(bf & 0x20000) || !(o.flags8 & 0x2000000)))
                                              : best < 0 || !(bf & 8) || d < bestD;
            if (take) best = int(k), bestD = d, bestTop = top;
        }
        if (best < 0 || (actors[size_t(best)].flags8 & 0x20000)) break;
    }
    if (best >= 0) w.y = bestTop;
    return best;
}

int LitWorld::playerContacts(V3& w, uint32_t moveFlagsA, int16_t lz) {
    if (p.platform >= 0) {  // riding: the move goes to his platform-local coordinate (0x434230 runs after it)
        const LitActor& pl = actors[size_t(p.platform)];
        if (pl.dead || ((p.flags14 & 1) && !(p.flags14 & 8))) {  // gone / rising
            p.platform = -1;
            p.flags14 = (p.flags14 & ~0x14u) | (p.flags14 & 1 ? 0 : 0x200);
        } else {
            w.y = 0;
            return -1;
        }
    }
    V3 wp = w;
    const int ci = contactPush(p, kParentBugs, playerInflate_, p.animState.radius, p.animState.box, 0x9a690a, wp);
    if (ci < 0) return -1;
    {
        const size_t i = size_t(ci);
        LitActor& a = actors[i];
        // The move stays w0 unless a branch below takes the pushed move wp (the exe's int move vs. its s16 copy).
        const V3 w0 = w;
        // 0x43a880: the pushed x and z are each traced against the walls from his position (0x434cd0; runtime: L02A2
        // Bugs landing beside the plank at counters 227-231 is pushed only once the wall no longer stops it).
        for (int k = 0; k < 2; ++k) {
            V3 at = p.pos;
            int32_t dx = k ? 0 : wp.x, dz = k ? wp.z : 0;
            Ground g;
            g.zone = p.zone;
            hf.trace(at, dx, dz, g);
            (k ? wp.z : wp.x) = int16_t(k ? dz : dx);
        }
        const int32_t below = -wp.y;  // feet below the box top (0x433500's w.y result, negated)
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
            // Boarding: a child coordinate of the platform, local = inverse rotation of (Bugs + move − platform), local
            // y = box top + 4 (0x43a880; the move is then also applied in the platform frame, as the exe).
            Mat inv;
            rotMatrixZYX(inv, 0x1000 - a.rot[0], 0x1000 - a.yaw, 0x1000 - a.rot[2]);
            p.local = apply(inv, {int16_t(p.pos.x - a.pos.x + w.x), int16_t(p.pos.y - a.pos.y), int16_t(p.pos.z - a.pos.z + w.z)});
            p.local.y = a.box[4] + 4;
            p.platform = int(i);
            p.pos.y = a.pos.y + a.box[4] + 4;
            w.y = 0;
            p.flags14 = (p.flags14 & 0xfffffdf4) | 0x14;  // the fall phase +0x108 is kept (runtime: L01A seesaw fall-off)
            return int(i);
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
            if (!stomp()) {
                if (!(p.flags14 & 0x10) && (a.flags8 & 8) && below < 100) w.y += -below;
                else if (!(a.flags8 & 0x2000000) && below > 30) w.x = wp.x, w.z = wp.z;
            }
        }
        return int(i);  // others (e.g. buoyant flags8 0x900000): contact without push
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
