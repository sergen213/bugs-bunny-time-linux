// TB mover 0x4450c0 (GAMEPLAY_TB.md §1.5) and its world: the collision code (tb_sweep.cpp, tb_shapes.cpp) runs on
// bodies of the entities (the exe's entity images), a copy of the level collision block and the mover's tables. This
// file keeps them per world and moves state between TbActor and the bodies around each call.
#include "bb/game/tb_world.h"
#include "tb_sweep.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace bb::game {
namespace {
using namespace tbm;

// The six ledge probes of 0x4434c0 (.rdata 0x481cf8): hands, sides and the low / high middle, ahead of the body.
constexpr std::array<SVec4, 6> kProbes{{{{-180, -130, 210}, 0}, {{180, -130, 210}, 0}, {{-220, -130, 210}, 0},
                                        {{220, -130, 210}, 0}, {{0, -160, 210}, 0}, {{0, 250, 210}, 0}}};

// An entity's body and the coordinate record it hangs on (its parent part's world matrix, the chain flattened).
struct Slot {
    Body b;
    CoordRec hang;
};

// The mover's state for the world that used it last: the bodies by entity uid (their shape records point back at
// them), the collision block, the shape cache records, the lists 0x45eee0 left this tick (contact 0x4d2734 and view
// 0x4d2744, null-terminated) and the tables / work blocks the exe keeps in its .bss for the whole run.
struct Mover {
    const TbWorld* owner = nullptr;
    uint64_t gen = 0;
    std::unordered_map<uint32_t, std::unique_ptr<Slot>> slots;
    std::vector<uint8_t> block;
    Collision col;
    std::vector<ShapeRec> recs;
    ShapeCache cache;
    std::vector<Body*> contacts, views;
    std::vector<std::pair<TbActor*, Body*>> staged;  // this call's entities and their bodies
    PolyGather gather;
    BoxPolyScratch boxPoly;
    Sweep mover, follow, look;  // 0x4450c0 at scratch + 0, 0x440360 at + 0x15c / + 0x10c
    int16_t rayBoxAxis = 0;     // 0x4463e0's boxCast scratch word (+0x1e)
} g;
std::atomic<uint64_t> gens{0};
TbWorld* g_world = nullptr;  // for the 0x447ee0 callback

Body* bodyOf(const TbActor* a) {
    for (auto& [e, b] : g.staged)
        if (e == a) return b;
    return nullptr;
}
TbActor* actorOf(const Body* b) {
    for (auto& [e, eb] : g.staged)
        if (eb == b) return e;
    return nullptr;
}

// The level block as FUN_00445d00 sets it up (surfaces +4, polygons +0x10, vertices +0xc, octree at +0x20, root
// cube at +0x14 with the size at +0x1c), and the shape pool as the loader links it (free list through prev).
void claim(const TbWorld& w, uint64_t gen, const TbCollision& c, int16_t recs) {
    g.owner = &w, g.gen = gen;
    g.slots.clear();
    const auto raw = c.raw();
    g.block.assign(raw.begin(), raw.end());
    uint8_t* B = g.block.data();
    auto u32 = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, B + o, 4);
        return v;
    };
    auto i16 = [&](size_t o) {
        int16_t v;
        std::memcpy(&v, B + o, 2);
        return v;
    };
    const uint32_t h = u32(0x1c) >> 1;
    g.col = {reinterpret_cast<const Surface*>(B + u32(4)), B + 0x20, B + u32(0x10), reinterpret_cast<const SVec4*>(B + u32(0xc)),
             {s16(i16(0x14) + int16_t(h)), s16(i16(0x16) - int16_t(h)), s16(i16(0x18) + int16_t(h))}, h};
    g.recs.assign(size_t(std::max<int32_t>(recs, 1)), {});
    for (size_t i = 0; i + 1 < g.recs.size(); ++i) g.recs[i].prev = &g.recs[i + 1];
    g.cache = {};
    g.cache.free = g.recs.data();
}

const Poly* polyPtr(const TbCollision& col, int32_t i) {
    return i >= 0 && size_t(i) < col.polys.size() ? reinterpret_cast<const Poly*>(g.block.data() + col.polys[size_t(i)].off) : nullptr;
}

// 0x44e5d0 into the body from the +0xe4 source: free entities +0xec (scale, angles +0xf4, position +0xfc): Rx(pitch)·
// Rz(roll) (flags10 0x100000: Rz·Rx), then Ry(yaw), scaled; attached ones +0x12c (scale 1, yaw +0x136 only,
// position +0x13c in the parent's frame). Entities without a model (+0x9c) or with flags0 0x200000 keep theirs.
void coordMatrix(const TbActor& a, Body& b, bool attached) {
    if (!a.model || (a.flags0 & 0x200000)) return;
    Mat& m = b.coord;
    m.m = Mat{}.m, m.t = {};
    if (attached) {
        mulRotY(m, a.localYaw);
        m.t = {int16_t(a.local.x), int16_t(a.local.y), int16_t(a.local.z)};
        return;
    }
    if (!(a.flags10 & 0x100000)) mulRotX(m, a.pitch), mulRotZ(m, a.roll);
    else mulRotZ(m, a.roll), mulRotX(m, a.pitch);
    mulRotY(m, a.yaw);
    for (size_t i = 0; i < 9; ++i) m.m[i] = int16_t((int32_t(m.m[i]) * a.scale[i % 3]) >> 12);  // 0x404710
    m.t = {int16_t(a.pos.x), int16_t(a.pos.y), int16_t(a.pos.z)};
}

// `anchor(e)`: the world matrix of the record entity e hangs on (TbWorld::coordMat of its parent, +0xe8), none = free.
template <class Anchor> Slot& slotOf(TbActor& a, Anchor&& anchor) {
    auto [it, fresh] = g.slots.try_emplace(a.uid);
    if (fresh) {
        it->second = std::make_unique<Slot>();
        coordMatrix(a, it->second->b, anchor(a).has_value());
    }
    return *it->second;
}

// TbActor -> body (the fields the mover code reads). An attached entity hangs on its slot's coordinate record (the
// parent part's world matrix); its position stays world, as in the exe.
template <class Anchor> void put(TbWorld& w, TbActor& a, Slot& s, Anchor&& anchor) {
    Body& b = s.b;
    b.flags0 = a.flags0, b.flags1 = a.flags1, b.flags10 = a.flags10;
    for (size_t i = 0; i < 3; ++i) b.progMask[i] = a.progMask[i];
    b.f20 = a.f20, b.f24 = a.f24, b.f28 = a.f28;
    b.id = a.id, b.f38 = uint16_t(a.f38), b.f3a = uint16_t(a.f3a), b.f40 = a.f40;
    b.radius = a.radius;
    b.carry = {a.carry[0], a.carry[1], a.carry[2]};
    b.boxMin = {a.box[0], a.box[1], a.box[2]}, b.boxMax = {a.box[3], a.box[4], a.box[5]};
    b.vel = {int16_t(a.vel[0]), int16_t(a.vel[1]), int16_t(a.vel[2])};
    b.attached = a.attachPart >= 0 ? 1 : 0;  // only tested for 0 by the mover
    TbActor* par = w.entity(a.parent);
    b.parent = par ? &slotOf(*par, anchor).b : nullptr;
    const std::optional<Mat> P = par ? anchor(a) : std::nullopt;
    b.coordRec = nullptr;
    if (P) s.hang = {*P, nullptr}, b.coordRec = &s.hang;
    b.angles = {a.pitch, a.yaw, a.roll};
    b.pos = {int16_t(a.pos.x), int16_t(a.pos.y), int16_t(a.pos.z)};
    b.groundPoly = polyPtr(w.col, a.groundPoly);
    b.groundSurf = a.groundSurf >= 0 ? &g.col.surfaces[a.groundSurf] : nullptr;
    b.hover = a.hover;
    b.climb = a.climb;
}

// body -> TbActor (the fields the mover code writes).
void get(TbWorld& w, TbActor& a, const Body& b) {
    a.f20 = b.f20, a.f24 = b.f24, a.f28 = b.f28;
    a.carry = {b.carry.x, b.carry.y, b.carry.z};
    a.vel = {b.vel.x, b.vel.y, b.vel.z};
    a.pitch = b.angles.x, a.yaw = b.angles.y, a.roll = b.angles.z;
    a.pos = {b.pos.x, b.pos.y, b.pos.z};
    a.groundPoly = b.groundPoly ? w.col.polyAtOff(uint32_t(reinterpret_cast<const uint8_t*>(b.groundPoly) - g.block.data())) : -1;
    a.groundSurf = b.groundSurf ? int32_t(b.groundSurf - g.col.surfaces) : -1;
    a.hover = b.hover;
    a.climb = b.climb;  // 0x4434c0's ledge probe bits
}

// 0x447ee0 -> TbWorld::fxTouch with the moved shape (work +0x34 centre, +0xf0 radius); a hit (f28 0x4000) is kept on
// the body and the request, as the exe does, the actor's slot record (+0x128) with it.
void fxTouchHook(Body& b, const Sweep& w, MoveRequest& req) {
    TbActor* a = actorOf(&b);
    if (!a) return;
    const uint32_t before = a->f28;
    const int16_t hit0 = a->fxHit;
    a->f28 = b.f28;
    g_world->fxTouch(*a, {w.centre.x, w.centre.y, w.centre.z}, w.radius);
    if (a->f28 & 0x4000) b.f28 |= 0x4000, req.flags |= 0x4000;
    else a->fxHit = hit0;
    a->f28 = before;
}

} // namespace

// This world's mover state, then the lists as 0x45eee0 left them this tick, in entity-list order: bodies of the
// collidable (contact list) and active (view list) entities and of `a`. Riders on +0x104 have no +0xe8.
void TbWorld::moverStage(TbActor& a) {
    if (!moverGen_) moverGen_ = ++gens;
    if (g.owner != this || g.gen != moverGen_) claim(*this, moverGen_, col, shapeRecs_);
    g_world = this;
    const auto anchor = [this](const TbActor& e) -> std::optional<Mat> {
        const TbActor* par = e.parent >= 0 && !e.rideRec ? entity(e.parent) : nullptr;
        return par ? std::optional<Mat>(coordMat(par, e.parentPart)) : std::nullopt;
    };
    g.contacts.clear(), g.views.clear(), g.staged.clear();
    for (TbActor* e : order()) {
        if (!e->collidable && !e->active && e != &a) continue;
        Slot& s = slotOf(*e, anchor);
        put(*this, *e, s, anchor);
        g.staged.emplace_back(e, &s.b);
        if (e->collidable) g.contacts.push_back(&s.b);
        if (e->active) g.views.push_back(&s.b);
    }
    g.contacts.push_back(nullptr), g.views.push_back(nullptr);
}

// 0x440360, the camera's sphere sweep (0x437800 follow; 0x439700 look view). The sphere (radius r) moves from s along
// m in at most 11 passes; each pass tests the shape records of the contact-list entities within |m| + r (0x448ce0)
// that are solid to the camera (flags10 & 0x4000008, or flags0 0x4000000 unless it is the focus 0x4d236c or the
// look is locked 0x4d2714; record 0x400 = none; spheres 0x44c840, capsules 0x44c9b0, boxes 0x44cb50), then the
// gathered polygons (0x446ea0; surface 0x32 = none; 0x44bfd0); a push-out moves the centre, the end and the pass
// push (applied by 0x443bf0 once it exceeds 5). A contact slides the rest of the motion off the normal (normalised to
// 0x4000): within 100 of it the normal joins up to 8 kept ones and the slide follows the crease with an earlier
// normal, else stops; a rest pointing back against m stops the sweep. m becomes the travelled delta; returns whether
// anything was touched.
bool TbWorld::cameraSweep(TbActor& stage, const V3& s, V3& m, int32_t r, bool look) {
    moverStage(stage);
    const Body* F = bodyOf(&actors[size_t(camFocus)]);
    Sweep& w = look ? g.look : g.follow;
    const PolyWorld world{g.col};
    Gte& gt = gte();
    const SVec start = narrow(s);
    const int32_t reach = root(m.x * m.x + m.y * m.y + m.z * m.z) + int16_t(r);
    w.polyCount = gatherPolys(g.col, start, s16(reach), nullptr, g.gather);
    w.self = nullptr;
    w.shapeCount = listShapes(start, nullptr, s16(reach), g.contacts.data(), g.views.data(), g.cache);
    w.mode = 2;
    bool hit = false;
    int passes = 0;
    w.motion = narrow(m);
    w.radius2 = w.radius = int16_t(r);
    w.left = 0x1000;
    w.centre = start;
    w.step = w.motion, w.end = w.centre + w.step;
    bool touched = w.polyCount || w.shapeCount;
    w.pushed = 0;
    int kept = 0;
    std::array<SVec, 8> normals;
    auto takePush = [&] {
        hit = true;
        w.centre += w.push, w.end += w.push;
    };
    while (touched && w.left) {
        touched = false;
        ++passes;
        w.t = w.left, w.best = 0x7fffffff, w.passPush = {};
        for (int i = 0; i < w.shapeCount; ++i) {
            const ShapeRec& rec = *g.cache.list[size_t(i)];
            const uint32_t t = rec.type;
            const Body& own = *rec.owner;
            if (t & 0x400) continue;
            if (own.flags0 & 0x4000000 ? &own == F || lookLock_ : !(own.flags10 & 0x4000008)) continue;
            if (t & 1 ? sweepSphere(w, rec) : t & 2 ? sweepCapsule(w, rec) : t & 8 ? sweepBox(w, rec) : false) touched = true;
            if (w.pushed) takePush(), w.passPush += w.push, w.push = {}, w.pushed = 0;
        }
        if (lenSq(w.passPush) > 0x19) applyPush(w, g.gather.slots.data(), world);
        for (int i = 0; i < w.polyCount; ++i) {
            w.push = {};
            const PolySlot& sl = g.gather.slots[size_t(i)];
            if (!sl.poly) continue;
            if (!(world.surfaceFlags(*sl.poly) & 0x32) && polySweep(w, sl, world)) touched = true;
            if (w.pushed) {
                takePush();
                if (lenSq(w.push) > 0x32) w.passPush = w.push, applyPush(w, g.gather.slots.data(), world);
                w.push = {}, w.pushed = 0;
            }
        }
        if (touched) {
            hit = true;
            w.centre += w.contact;
            const int32_t l = root(lenSq(w.normal));
            w.normal = {s16((w.normal.x << 14) / l), s16((w.normal.y << 14) / l), s16((w.normal.z << 14) / l)};
            SVec along{};  // the rest's component along the normal
            w.tmp = w.normal;
            gt.setLightRow0(w.motion), gt.setLightRow2(w.normal), gt.loadV(1, w.motion), gt.mvmva(false, Gte::kLight, Gte::kV1, false);
            const int32_t into = gt.mac.z >> 14;  // rest · normal
            auto project = [&] {
                auto r14 = [&](int16_t n) {
                    const int32_t v = n * into;
                    return s16((v >= 0 ? v + 0x3fff : v) >> 14);
                };
                along = {r14(w.normal.x), r14(w.normal.y), r14(w.normal.z)};
            };
            if (w.best < 100) {
                if (kept < 8) normals[size_t(kept++)] = w.normal;
                if (into < 0) {
                    project();
                    SVec slid = w.motion - along;
                    // Slide along the crease with the first earlier normal the slid rest still points into.
                    for (int idx = 0; idx < kept - 1; ++idx) {
                        w.tmp = normals[size_t(idx)];
                        if (w.tmp.x * slid.x + w.tmp.z * slid.z + w.tmp.y * slid.y > 0) continue;
                        const V3 x = cross(w.tmp, w.normal), c{x.x >> 14, x.y >> 14, x.z >> 14};
                        if (!(c.x * c.x + c.y * c.y + c.z * c.z)) continue;
                        int32_t num = slid.y * c.y + slid.z * c.z + slid.x * c.x, den = c.y * c.y + c.x * c.x + c.z * c.z;
                        if (num < 0) num = -num, den = -den;
                        for (; num > 32000; num >>= 1) den >>= 1;
                        slid = {s16(c.x * num / den), s16(c.y * num / den), s16(c.z * num / den)};
                        for (int j = 0; j < kept - 1; ++j)
                            if (j != idx && normals[size_t(j)].z * slid.z + normals[size_t(j)].y * slid.y + normals[size_t(j)].x * slid.x < 0) {
                                slid = {};
                                break;
                            }
                        break;
                    }
                    along = w.motion - slid;
                }
            } else {
                if (into < 0) project();
                kept = 0;
            }
            w.motion = w.motion - along;
            if (w.motion.z * m.z + w.motion.x * m.x + w.motion.y * m.y < 0) {
                w.motion = {}, w.end = w.centre;
                break;
            }
            w.left = s16(w.left - w.t);
            auto part = [&](int16_t a) {
                const int32_t v = w.left * a;
                return s16((v > 0 ? v + 0xfff : v) >> 12);
            };
            w.step = {part(w.motion.x), part(w.motion.y), part(w.motion.z)};
            w.end = w.centre + w.step;
        }
        if (w.step == SVec{} || passes > 10) break;
    }
    m = {w.end.x - start.x, w.end.y - start.y, w.end.z - start.z};
    return hit;
}

// 0x4463e0 (query {start, delta, hit, self +0x1c, flags q +0x20, entity masks +0x24}): after the polygon pass, with
// query flag 1, 0x448920 lists the view-list entities matching the masks (flags0 / flags1 / flags10 & masks[0..2],
// none of masks[3..5]) whose radius +0x4a reaches the ray (in the ray's frame: Ry(-atan2(dx, dz)) · Rx(atan2(dy,
// |dxz|)), -R <= z <= |d| + R, x² + y² <= R²; radius 0 and removed excluded; with a self (0x450650 passes none) also
// self, its +0x90 ancestors without flags0 8 / flags1 0x100000 and its descendants; at most 25), caches their shapes
// (0x4490a0, records +0x88) and tests each: sphere (1) 0x43cb60, capsule (2) 0x449750 start inside else 0x449960,
// oriented box (8) 0x43cdf0 in its inverse frame +0x80; 0x80 = none. A shape hit at a squared distance <= the best
// (the polygon's, +0xa8) takes it (+0x18 = the record's owner +0xc); "inside" hits take it at distance 0 (capsule)
// or without a distance (sphere). A hit writes the shortened delta back into the query (`d`); `uncollide` entities
// lose flags0 0x100 for the query (0x44e7d0 / 0x450650 clear it on the target / parent and child). `n` gets the hit
// normal (query +0x10, Q14): the polygon's, or the shape test's (inside a sphere / capsule: -delta / -the push vector
// normalised to 0x4000; a surface hit: the test's normal; a box: its normal back through the record's rotation +0x60).
std::optional<TbActor*> TbWorld::rayHit(TbActor& self, const V3& s, V3& d, int32_t radius, uint32_t q,
                                        const std::array<uint32_t, 6>& masks, bool excludeSelf,
                                        std::initializer_list<const TbActor*> uncollide, std::array<int16_t, 3>* n) {
    moverStage(self);
    for (auto& [e, b] : g.staged)
        if (std::ranges::find(uncollide, e) != uncollide.end()) b->flags0 &= ~0x100u;
    std::optional<TbActor*> out;
    V3 hitD = d;
    std::array<int16_t, 3> hitN{};
    int32_t best = 0x7fffffff;
    if (const auto h = col.query(s, d, radius, q)) {
        const int32_t x = int16_t(h->p.x - s.x), y = int16_t(h->p.y - s.y), z = int16_t(h->p.z - s.z);
        best = x * x + y * y + z * z, out = nullptr, hitD = {x, y, z}, hitN = h->n;
    }
    auto done = [&] {
        for (auto& [e, b] : g.staged) b->flags0 = e->flags0;
        if (out) d = hitD;
        if (n) *n = hitN;
        return out;
    };
    if (!(q & 1)) return done();
    const Body* selfB = excludeSelf ? bodyOf(&self) : nullptr;
    const SVec start = narrow(s), delta = narrow(d);
    // 0x448920 (ray form): the entities in the ray's frame, loaded as the GTE rotation.
    Gte& gt = gte();
    const int32_t len = root(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    const int16_t yaw = int16_t(-angle(delta.x, delta.z)), pitch = int16_t(angle(delta.y, root(delta.x * delta.x + delta.z * delta.z)));
    Mat frame;
    frame.m = Mat{}.m, frame.t = {};
    mulRotY(frame, yaw), mulRotX(frame, pitch);
    gt.setRot(frame);
    auto chain = [](const Body& from, const Body* to) {  // `to` on the +0x90 parent chain of `from`
        for (const Body* p = from.parent; p; p = p->parent)
            if (p == to) return true;
        return false;
    };
    std::vector<Body*> listed;
    for (Body* const* v = g.views.data(); *v && listed.size() < 0x19; ++v) {
        Body& e = **v;
        if (!((masks[0] & e.flags0) || (masks[1] & e.flags1) || (masks[2] & e.flags10)) || (masks[3] & e.flags0) ||
            (masks[4] & e.flags1) || (masks[5] & e.flags10))
            continue;
        gt.loadV(0, e.pos - start), gt.rtv0();
        const int32_t R = e.radius, z = gt.mac.z;
        if (z < -R || z > len + R) continue;
        gt.sqr();
        if (gt.mac.y + gt.mac.x > R * R) continue;
        if (!R || (e.f20 & 0x2000)) continue;
        if (selfB && ((chain(*selfB, &e) && !(e.flags0 & 8) && !(e.flags1 & 0x100000)) || chain(e, selfB) || &e == selfB)) continue;
        listed.push_back(&e);
    }
    std::vector<const ShapeRec*> shapes;
    for (Body* e : listed) cacheShape(*e, g.cache), shapes.push_back(e->shape);
    auto take = [&](const ShapeRec& rec, SVec v) {  // v: the hit's delta, {} = the start (inside)
        out = actorOf(rec.owner);
        hitD = widen(v);
    };
    auto normal = [&](SVec v) { hitN = {v.x, v.y, v.z}; };
    auto inward = [&](SVec v) {  // -v / |v| at 0x4000
        const int32_t l = std::max(1, root(dot(v, v)));
        hitN = {int16_t(-((v.x << 14) / l)), int16_t(-((v.y << 14) / l)), int16_t(-((v.z << 14) / l))};
    };
    for (const ShapeRec* rp : shapes) {
        const ShapeRec& rec = *rp;
        if (rec.type & 0x80) continue;
        if (rec.type & 8) {  // in the box's frame, the hit back through its rotation
            BoxCast b;
            b.ext = rec.half, b.d = apply(rec.inv, delta), b.p = apply(rec.inv, start - rec.centre), b.radius = int16_t(radius);
            if (boxCast(b, g.rayBoxAxis))
                if (const int32_t r = gteLenSq(b.travel); r <= best) {
                    best = r;
                    take(rec, apply(rec.rot, b.travel));
                    normal(apply(rec.rot, b.normal));
                }
        } else if (rec.type & 1) {
            SphereCast c;
            c.p = start - rec.centre, c.d = delta, c.radius = rec.radius + radius;
            const uint32_t u = sphereCast(c);
            if (u & 0x1a) continue;
            if (!(u & 1)) take(rec, {}), inward(delta);
            else if (c.t < 0x1001)
                if (const int32_t r = gteLenSq(c.travel); r <= best) best = r, take(rec, c.travel), normal(c.normal);
        } else if (rec.type & 2) {
            CapsulePush in;
            in.p = start - rec.capStart, in.axis = rec.capAxis, in.radius = rec.capRadius + radius;
            if (capsulePush(in)) {
                best = 0, take(rec, {}), inward(in.push);
                continue;
            }
            CapsuleCast c;
            c.p = start - rec.capStart, c.axis = rec.capAxis, c.d = delta;
            c.radius = int16_t(int16_t(rec.capRadius) + radius), c.tmax = 0x1000;
            if (capsuleCast(c))
                if (const int32_t r = gteLenSq(c.hit); r <= best) best = r, take(rec, c.hit), normal(c.normal);
        }
    }
    return done();
}

TbWorld::MoveOut TbWorld::move(TbActor& a, const std::array<int32_t, 3>& target, std::vector<TbActor*>& touched,
                               const TbStateRec* r, const V3& follow) {
    moverStage(a);
    Body& b = *bodyOf(&a);
    // Request (0x453de0 local_10c): world target, attached displacement (+0x78, s16); state record (S form grip).
    MoveRequest req;
    req.target = {target[0], target[1], target[2]};
    req.follow = narrow(follow);
    MoveState st;
    if (r) st = {r->grip, r->cap, r->flagsA, r->flagsB, r->flagsC};
    MoverWorld mw{g.col, g.cache, g.gather, g.boxPoly, g.contacts.data(), g.views.data(), kProbes.data(), S.data(), dt,
                  [&req](Body& body, const Sweep& w) { fxTouchHook(body, w, req); }};
    tbm::move(b, req, r ? &st : nullptr, g.mover, mw);
    for (auto& [e, eb] : g.staged) {
        if (e == &a) continue;
        e->f20 = eb->f20, e->f24 = eb->f24, e->f28 = eb->f28;
    }
    get(*this, a, b);
    MoveOut out;
    out.disp = {req.disp.x, req.disp.y, req.disp.z};
    for (int16_t i = 0; i < req.touchedCount && i < 15; ++i)
        if (TbActor* t = actorOf(req.touched[size_t(i)])) touched.push_back(t);
    if (req.floor) out.floor = actorOf(req.floor);
    return out;
}

// 0x453de0 tail: the record rebuilt (0x44e5d0 / 0x44e690); the shape cache record goes stale (+0x88: 0x20000000)
// when the position moved or (0x453de0 only: `hungStale`) the entity hangs on +0xe8, 0x40000000 when the angles changed.
void TbWorld::moverTail(TbActor& a, const V3& pos0, const std::array<int16_t, 3>& ang0, const std::array<int16_t, 6>& box0, bool hungStale) {
    if (g.owner != this || g.gen != moverGen_) return;  // no body yet: the next move builds it fresh
    auto it = g.slots.find(a.uid);
    if (it == g.slots.end()) return;
    Body& b = it->second->b;
    const bool hung = a.parent >= 0 && !a.rideRec && entity(a.parent);
    coordMatrix(a, b, hung);
    if (ShapeRec* rec = b.shape) {
        if ((hung && hungStale) || int16_t(pos0.x) != int16_t(a.pos.x) || int16_t(pos0.y) != int16_t(a.pos.y) || int16_t(pos0.z) != int16_t(a.pos.z))
            rec->type |= 0x20000000;
        if (ang0 != std::array<int16_t, 3>{a.pitch, a.yaw, a.roll}) rec->type |= 0x40000000;
        if (box0 != a.box) rec->type |= 0x10000000;  // 0x450440
    }
}

// 0x45edb5: the removed entity's record leaves the LRU list (newest .. oldest) for the free list.
void TbWorld::moverForget(TbActor& a) {
    if (g.owner != this || g.gen != moverGen_) return;
    auto it = g.slots.find(a.uid);
    if (it == g.slots.end()) return;
    Body& b = it->second->b;
    if (ShapeRec* rec = b.shape) {
        if (!rec->next) g.cache.newest = rec->prev;
        else rec->next->prev = rec->prev;
        if (!rec->prev) g.cache.oldest = rec->next;
        else rec->prev->next = rec->next;
        rec->prev = g.cache.free;
        g.cache.free = rec;
        b.shape = nullptr;
    }
}

} // namespace bb::game
