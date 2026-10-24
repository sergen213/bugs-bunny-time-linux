// The TB collision sweep and mover (bblit2.exe 0x43d830..0x44d870): the entity, shape cache record and work block
// the mover (0x4450c0) and the camera sweep (0x440360) run on, their per-shape / per-polygon tests, passes and the
// mover itself. The work block and the shape record keep the exe's layout for their numeric fields (the fuzzer,
// tools/re/tb_shapes_fuzz.py, compares them byte for byte with the exe's); their links follow it.
#pragma once
#include "tb_shapes.h"

#include <array>
#include <functional>

namespace bb::game::tbm {

// An SVECTOR in an 8-byte slot (the exe's vector tables).
struct SVec4 {
    SVec v;
    int16_t pad = 0;
};
using Axes = std::array<SVec4, 3>;

struct Poly;
struct ShapeRec;
// A collision surface (12 bytes): +0 friction (Q12), +8 flags (0x30 not solid, 0x40 slide, 1 ledge, 8 climbable,
// 0x100 never gathered, 0x200 one-sided, 0x2 / 0x4 / 0xf000 solid only to some entities).
struct Surface {
    uint16_t friction = 0;
    uint16_t f2 = 0;
    uint32_t f4 = 0;
    uint32_t flags = 0;
};
static_assert(sizeof(Surface) == 12);

// A coordinate record (0x44e5d0): the world matrix of what an entity hangs on, chained to its super record.
struct CoordRec {
    Mat m;
    const CoordRec* super = nullptr;
};

// The entity as the collision code sees it (the exe's entity image; tb_mover.cpp fills it from / back to TbActor).
struct Body {
    uint16_t marks = 0;   // +0x04 0x40: listed as touched this move
    uint32_t flags0 = 0;  // +0x08
    uint32_t flags1 = 0;  // +0x0c
    uint32_t flags10 = 0; // +0x10
    uint32_t progMask[3]{};
    uint32_t f20 = 0, f24 = 0, f28 = 0;  // +0x20 .. +0x28 contact / state flags
    uint16_t id = 0;      // +0x34
    uint16_t f38 = 0;     // +0x38 slide damping (Q12)
    uint16_t f3a = 0;     // +0x3a friction scale of a shape stood on (Q12)
    int16_t f40 = 0;      // +0x40 friction bias
    uint16_t radius = 0;  // +0x4a
    SVec carry;           // +0x58
    SVec boxMin, boxMax;  // +0x60 / +0x68
    SVec vel;             // +0x70 (x16)
    int32_t attached = 0; // +0x84
    ShapeRec* shape = nullptr;  // +0x88 shape cache record
    Body* parent = nullptr;     // +0x90
    Mat coord;            // +0xa4
    const CoordRec* coordRec = nullptr;  // +0xe8 the record it hangs on
    SVec angles;          // +0xf4 pitch, yaw, roll
    SVec pos;             // +0xfc
    const Poly* groundPoly = nullptr;       // +0x108
    const Surface* groundSurf = nullptr;    // +0x144
    uint16_t hover = 0;   // +0x194
    uint16_t climb = 0;   // +0x1a0 ledge probe bits (0x4434c0)
};

// The move request (0x453de0 local_10c).
struct MoveRequest {
    V3 target;
    Body* floor = nullptr;              // +0x10 the entity stood on
    std::array<Body*, 15> touched{};    // +0x14
    V3 disp;                            // +0x50
    int16_t touchedCount = 0;           // +0x60
    uint32_t flags64 = 0, flags68 = 0;  // +0x64 / +0x68 result flags (as the entity's +0x20 / +0x24)
    uint32_t flags = 0;                 // +0x6c (as +0x28)
    SVec normal;                        // +0x70 the last contact normal
    SVec follow;                        // +0x78
};
// The mover's state record (0x4450c0 param_3): grip (0x4000: an S variable index, x32), horizontal speed cap (x16)
// and the state flags +0x20 / +0x24 / +0x28.
struct MoveState {
    int16_t grip = 0;
    int16_t cap = 0;
    uint32_t flagsA = 0, flagsB = 0, flagsC = 0;
};

// 0x4490a0 shape cache record (0xa0 bytes; pool 0x4d7754, LRU 0x4d77c4 newest / 0x4d77c8 oldest).
struct ShapeRec {
    uint32_t type = 0;            // 1 sphere, 2 capsule, 8 box, 0x80 none, 0x200, 0x400; 0xf0000000 stale bits
    uint32_t links[3]{};          // +0x04 the exe's prev / next / owner (below)
    SVec centre;                  // +0x10
    int16_t pad0 = 0;
    int32_t radius = 0;           // +0x18 sphere
    SVec capStart;                // +0x1c capsule
    int16_t pad7 = 0;
    SVec capEnd;                  // +0x24
    int16_t pad8 = 0;
    SVec capAxis;                 // +0x2c
    int16_t pad1 = 0;
    int32_t capRadius = 0;        // +0x34
    uint8_t gap38[0x4c - 0x38]{};
    V3 invT;                      // +0x4c box: minus the frame's translation
    SVec half;                    // +0x58 box half extents
    int16_t pad2 = 0;
    Mat rot;                      // +0x60 box frame -> world
    Mat inv;                      // +0x80 world -> box frame
    // A box's world axes scaled by its half extents (+0x1c, stride 8) and its unit face normals (+0x34, Q14).
    SVec& axis(int i) { return reinterpret_cast<SVec4*>(&capStart)[i].v; }
    const SVec& axis(int i) const { return reinterpret_cast<const SVec4*>(&capStart)[i].v; }
    SVec& faceNormal(int i) { return reinterpret_cast<SVec4*>(&capRadius)[i].v; }
    const SVec& faceNormal(int i) const { return reinterpret_cast<const SVec4*>(&capRadius)[i].v; }
    ShapeRec* prev = nullptr;  // LRU links (the free list through prev)
    ShapeRec* next = nullptr;
    Body* owner = nullptr;
};
static_assert(offsetof(ShapeRec, capRadius) == 0x34 && offsetof(ShapeRec, half) == 0x58 && offsetof(ShapeRec, rot) == 0x60 &&
              offsetof(ShapeRec, inv) == 0x80 && offsetof(ShapeRec, invT) == 0x4c && offsetof(ShapeRec, prev) == 0xa0);

// The words of the exe's scratch block after the work block (mover W+0x1ac, camera W+0x194) that a test reads
// before writing them in degenerate cases, by their place in that test's records: each keeps what its own test
// last wrote there. (The exe's block is one for all the tests of a pass, so another test can write a word in
// between; no trace reaches these cases.)
struct TestScratch {
    BoxSweepScratch camBox;    // 0x44cb50: its boxSweep's words (+0x54 / +0x3e)
    BoxSweepScratch boxSphere; // 0x43d9f0: +0x74 / +0x5e
    int32_t capsuleSlabT = 0;  // 0x440e70 +0x8c: low half its segmentPush's nearest z, high half the last slab t
    int32_t boxSlabT = 0;      // 0x441080 +0xdc (its boxCapsuleCast's +0x8c)
    int32_t boxCapSlabT = 0;   // 0x43dc50 +0xd4
    int16_t boxCastAxis = 0;   // the overlap passes' boxCast (+0x1e)
};

// The sweep work block (camera 0x440360: scratch + 0x15c follow / + 0x10c look; mover 0x4450c0: scratch + 0).
struct Sweep {
    uint32_t pad00 = 0;   // +0x00 the exe's moving entity (`self` below)
    int16_t t = 0;        // +0x04 reach (Q12 of the motion) of the nearest contact this pass
    int16_t left = 0;     // +0x06 motion left (Q12)
    int32_t mode = 0;     // +0x08 1 capsule, 2 sphere, 4 box mover; 8 point (start + startOffset), 0x10 grab
                          // offset, 0x20 no sweep, 0x40 / 0x80 slide modes, 0x100 flat, 0x200 box as a sphere
    uint32_t pad0c[2]{};  // +0x0c / +0x10 the exe's floor surface / last surface (below)
    SVec half;            // +0x14 box mover half extents; the capsule mover's radius is half.x
    int16_t pad1a = 0;
    V3 tmpInt;            // +0x1c
    int32_t f28 = 0;
    SVec tmp;             // +0x2c
    int16_t pad32 = 0;
    SVec centre;          // +0x34
    int16_t pad0 = 0;
    SVec end;             // +0x3c
    int16_t pad7 = 0;
    SVec startOffset;     // +0x44
    int16_t pad8 = 0;
    SVec step;            // +0x4c
    int16_t pad1 = 0;
    SVec motion;          // +0x54
    int16_t pad2 = 0;
    SVec normal;          // +0x5c of the nearest contact (Q14)
    int16_t pad3 = 0;
    SVec contact;         // +0x64 travel to the nearest contact
    int16_t pad4 = 0;
    SVec push;            // +0x6c push-out of the current test
    int16_t pad5 = 0;
    SVec passPush;        // +0x74 summed push of the pass
    int16_t pad7a = 0;
    SVec lastNormal;      // +0x7c the last contact normal of the move
    int16_t pad82 = 0;
    SVec start;           // +0x84 the centre the move started from
    int16_t pad8a = 0;
    SVec capA;            // +0x8c the mover capsule: segment [centre + capA, centre + capB]
    int16_t pad6 = 0;
    SVec capB;            // +0x94
    uint8_t gap9a[0xcc - 0x9a]{};
    std::array<SVec4, 3> axes;  // +0xcc box mover: its world axes scaled by the half extents
    SVec motion0;         // +0xe4 the motion the move started with
    int16_t pade = 0;
    int16_t shapeCount = 0;  // +0xec records listed in shapeList (0x4d7760)
    int16_t padec = 0;
    int16_t radius = 0;   // +0xf0
    int16_t radius2 = 0;  // +0xf2
    int32_t best = 0;     // +0xf4 squared travel to the nearest contact; 0 = no contact search
    Mat rot;              // +0xf8 box mover frame -> world
    Mat inv;              // +0x118 world -> box mover frame
    int32_t wasPushed = 0;  // +0x138 some test of the pass pushed
    int32_t pushed = 0;     // +0x13c the current test pushed the centre out
    uint32_t pad140[2]{}; // +0x140 / +0x144 the exe's last contact's entity / floor polygon (below)
    int16_t polyCount = 0;  // +0x148 gathered polygons (0x4d87c0 slots)
    int16_t pad14a = 0;
    int32_t landed = 0;     // +0x14c the floor friction was applied this move
    int16_t grip = 0;       // +0x150
    int16_t pad152 = 0;
    Body* self = nullptr;   // the moving entity (mover)
    const Surface* floorSurf = nullptr;  // surface of the floor polygon touched (a shape contact with a floor normal clears it)
    const Surface* surf = nullptr;       // surface of the last polygon touched
    Body* hitOwner = nullptr;            // entity of the last contact
    const Poly* floorPoly = nullptr;     // the floor polygon touched
    mutable TestScratch scratch;         // the tests' stale scratch words (also those of the const overlap passes)
    // The box mover's corners (+0x8c, stride 8; the capsule mover's capA / capB are the first two).
    SVec& corner(int i) { return reinterpret_cast<SVec4*>(&capA)[i].v; }
    const SVec& corner(int i) const { return reinterpret_cast<const SVec4*>(&capA)[i].v; }
};
constexpr size_t kSweepExeSize = 0x154;  // the numeric fields at the exe's offsets; the links follow
static_assert(offsetof(Sweep, centre) == 0x34 && offsetof(Sweep, step) == 0x4c && offsetof(Sweep, motion) == 0x54 &&
              offsetof(Sweep, passPush) == 0x74 && offsetof(Sweep, radius) == 0xf0 && offsetof(Sweep, best) == 0xf4 &&
              offsetof(Sweep, pushed) == 0x13c && offsetof(Sweep, capA) == 0x8c && offsetof(Sweep, capB) == 0x94 &&
              offsetof(Sweep, shapeCount) == 0xec && offsetof(Sweep, half) == 0x14 && offsetof(Sweep, startOffset) == 0x44 &&
              offsetof(Sweep, polyCount) == 0x148 && offsetof(Sweep, tmp) == 0x2c && offsetof(Sweep, lastNormal) == 0x7c &&
              offsetof(Sweep, axes) == 0xcc && offsetof(Sweep, motion0) == 0xe4 && offsetof(Sweep, rot) == 0xf8 &&
              offsetof(Sweep, inv) == 0x118 && offsetof(Sweep, landed) == 0x14c && offsetof(Sweep, grip) == 0x150 &&
              offsetof(Sweep, self) >= kSweepExeSize);

// The level collision block (exe header 0x4d9900): 12-byte surfaces, octree nodes (+2 flags: 1 inner, children s16
// x8 at +4 as byte offsets / 4; a leaf: count at +4, u16 polygon offsets / 4 at +6), polygons, vertices (s16 x3 +
// pad), and the root cube.
struct Collision {
    const Surface* surfaces = nullptr;
    const uint8_t* nodes = nullptr;
    uint8_t* polys = nullptr;  // written only by the gather's marks (0x446ea0)
    const SVec4* vertices = nullptr;
    SVec centre;
    uint32_t half = 0;
};
struct PolyEdge {
    SVec n;
    int16_t vertex = 0;
};
struct Poly {
    SVec n;               // +0x00
    int16_t surface = 0;  // +0x06
    int32_t d = 0;        // +0x08
    SVec centre;          // +0x0c bounding sphere
    int16_t radius = 0;   // +0x12
    uint16_t pad14 = 0;
    int16_t edgeCount = 0;  // +0x16 (0x8000 marks it gathered while 0x446ea0 runs), the edges follow at +0x18
    const PolyEdge& edge(int i) const { return reinterpret_cast<const PolyEdge*>(reinterpret_cast<const uint8_t*>(this) + 0x18)[i]; }
};
static_assert(offsetof(Poly, d) == 8 && offsetof(Poly, edgeCount) == 0x16 && sizeof(PolyEdge) == 8);
// A gathered polygon (0x4d87c0 slots): the polygon and its edge planes' distances to the gather origin.
struct PolySlot {
    const Poly* poly = nullptr;
    const int32_t* edgeD = nullptr;
};
struct PolyWorld {
    const Collision& h;
    uint32_t surfaceFlags(const Poly& p) const { return h.surfaces[p.surface].flags; }
    const Surface* surface(const Poly& p) const { return &h.surfaces[p.surface]; }
    SVec vertex(const Poly& p, int edge) const { return h.vertices[p.edge(edge).vertex].v; }
};

// The mover's tables (bblit2.exe .bss): the shape cache (0x4d7754 free list, 0x4d7760 the shapes listed by
// 0x448ce0, 0x4d77c4 / 0x4d77c8 the newest / oldest record in use) and the polygon gather (0x4d87c0 slots, 0x4d8e00
// their edge plane distances, 0x4d98f0 / 0x4d98f8 the hand offsets of 0x4426a0).
struct ShapeCache {
    ShapeRec* free = nullptr;
    std::array<ShapeRec*, 25> list{};
    ShapeRec* newest = nullptr;
    ShapeRec* oldest = nullptr;
};
struct PolyGather {
    std::array<PolySlot, 0xc7> slots{};
    std::array<int32_t, 0x2bc> edgeD{};
    std::array<SVec4, 2> grab{};
};

// 0x445d90: the octree leaf (node, cube centre and half size) holding p; node 0 outside the root or when the root
// is a leaf.
struct Leaf {
    const uint8_t* node = nullptr;
    SVec centre;
    int16_t half = 0;
};
Leaf findLeaf(const Collision& h, SVec p);
// 0x4460c0: the leaves of the cube of half size r around p (p's, then the face centres', then the corners').
int16_t gatherLeaves(const Collision& h, SVec p, int16_t r, Leaf* out);
// 0x446ea0: the polygons of those leaves within r of p (plane, bounding sphere and every edge plane), their surface
// solid to `self` (surface 0x100 never; 0x2 / 0x4 / 0xf000 by the entity's flags10 >> 16): slots 1.. with their
// edge distances, slot 0 = `self`'s ground polygon if gathered. Returns the slot count (0 for none at all).
int16_t gatherPolys(const Collision& h, SVec p, int16_t r, const Body* self, PolyGather& g);
// 0x4490a0: the entity's shape cache record brought to the front of the LRU (a new one from the free list or the
// oldest), rebuilt when stale (0xf0000000) or its solidity flags changed: sphere (1), capsule (2) along the longest
// box axis, box (8, frame / inverse / axes), 0x80 none, 0x180 no collision (flags0 0x400), 0x400 empty box.
void cacheShape(Body& e, ShapeCache& c);
// 0x448ce0: the contact-list entities (then, for a mover, the view-list ones with flags0 0x2000100) whose radius
// reaches within r of p, not `self`, its +0x90 ancestors (unless platforms) or descendants; at most 25, their
// records cached and listed in `c.list`. Returns the count. The lists end with a null.
int16_t listShapes(SVec p, const Body* self, int16_t r, Body* const* contacts, Body* const* views, ShapeCache& c);

// The shapes a mover passes through (those its sweep skips: non-solid to it, flags0 0x2000000 / 0x4000000 pairs,
// 0x80 records), not listed yet, that it overlaps at its end: listed as touched (0x447350 centres within the
// radii; 0x447490 sphere mover, 0x4476e0 capsule mover, 0x447a60 box mover against the shape's form).
void overlapFlat(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
void overlapSphere(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
void overlapCapsule(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
void overlapBox(const Body& self, const Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
// 0x4439d0 (flags10 0x10000): for each side of the box mover, three points 300 beyond it at the end; one inside a
// hover box (a box shape whose entity has flags10 0x10000) clears that side's bit (1 / 2 / 8 / 4) in +0x194.
void hoverProbes(Body& self, const Sweep& w, ShapeRec* const* shapes);
// 0x44d870: the entity leaves its parent (+0x90): the carried weight comes off the parent chain, it leaves the
// child list, loses its coordinate record and ride, and takes its own scale / angles / position as the source.
void detach(Body& e);
// 0x4426a0 (grab state): the hand point ahead at the top of the body; nothing above the end, the nearest edge of a
// grabbable floor (surface 1) at hand height, or a ledge box (flags1 0x100000) face: hang from it (the end moves
// there, the entity faces it; f24 0x300); else f20 0x20. `g.grab` keeps each player's hand offset (mode 0x10).
void ledgeGrab(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world, PolyGather& g);
// 0x4434c0 (climb state): six probes (`probes`, 0x481cf8) around the end against climbable walls (surface 8): the
// four side probes give the climb bits (+0x1a0); both hand probes on a wall: the end between them, facing it.
void ledgeProbes(Body& self, Sweep& w, const PolySlot* slots, const PolyWorld& world, const SVec4* probes);

// The world the mover runs in: the collision block and its tables, the 0-terminated entity lists (contact 0x4d2734,
// view 0x4d2744), the ledge probes (0x481cf8), the S variables, the tick length and the effects touch (0x447ee0).
struct BoxPolyScratch;
struct MoverWorld {
    const Collision& h;
    ShapeCache& cache;
    PolyGather& gather;
    BoxPolyScratch& boxPoly;
    Body* const* contacts;
    Body* const* views;
    const SVec4* probes;
    const uint8_t* S;
    int32_t dt;
    std::function<void(Body&, const Sweep&)> fxTouch;
};
// 0x443f40: up to 11 passes of the mover's sweep (point, capsule, sphere or box by `mode`), each moving to the
// nearest contact and sliding the rest off it (along the crease with an earlier contact, stuck in a corner), with
// the floor's friction once a move and the impact / wall / ceiling / slide flags.
void respond(Body& e, Sweep& w, MoveRequest& req, MoverWorld& m);
// 0x4450c0: the move of `e` by its velocity (+ gravity, pulled towards the request's target velocity by the state
// flags) through the polygons and actor shapes near it, then the ledge / climb passes, the overlaps, the effects and
// hover probes; `w` = the work block (its first 0xd8 bytes kept across the call, as the exe does).
void move(Body& e, MoveRequest& req, const MoveState* st, Sweep& w, MoverWorld& m);

// 0x44bcc0: a sphere of `radius` at p overlapping the gathered polygon (slab behind its plane, within its edges or
// near one): `push` = the way out. 2 = no overlap, 4 = pushed. p becomes relative to the nearest edge's start.
struct PolyPush {
    SVec p;
    int16_t pad0 = 0;
    SVec push;
    int16_t pad1 = 0;
    int16_t radius = 0;
};
uint32_t polyPush(PolyPush& r, const PolySlot& slot, const PolyWorld& world);
// 0x44bfd0: the camera sphere against one gathered polygon: push-out, then the motion's contact with the face,
// an edge (cylinder) or a vertex (sphere), with the shape tests' contract.
bool polySweep(Sweep& w, const PolySlot& slot, const PolyWorld& world);
// 0x44b640: a point mover (start = centre + startOffset) against one gathered polygon, with an 8-unit skin: pushed
// 9 out of the plane when within the skin and inside the edges, then the motion's crossing of the plane (2 units
// in front) inside the edges.
bool pointPoly(Sweep& w, const PolySlot& slot, const PolyWorld& world);
// 0x4412b0: the mover capsule overlapping one gathered polygon: `push` gains the way out (2 / 4 as polyPush; 0 =
// its nearer end is beyond the radius).
uint32_t capsulePolyPush(Sweep& w, const PolySlot& slot, const PolyWorld& world);
// 0x4418a0: the mover capsule against one gathered polygon: push-out (capsulePolyPush), then the motion's contact
// of the nearer capsule end with the face, or of the capsule with an edge (slab, then capsule end caps).
bool capsulePoly(Sweep& w, const PolySlot& slot, const PolyWorld& world);
// 0x443bf0: the pass push (`passPush`) undone and redone against the gathered polygons: the centre moves back by it,
// then forward to the nearest polygon plane it crosses on the way (kept `radius2` off it), or the whole way.
void applyPush(Sweep& w, const PolySlot* slots, const PolyWorld& world);
// One sweep pass of a mover: the floor slot first, the actor shapes (flags0 0x100), the push-back, then every
// gathered polygon (the first again once something pushed). 1 = a polygon, 2 = a shape last touched. 0x44cf80 the
// sphere (polySweep), 0x442430 the capsule (capsulePoly), 0x440060 the box (boxPoly; mode 0x200: polySweep with the
// half height as radius), 0x44ba20 the point (pointPoly; the shapes by the mover's shape mode).
int16_t spherePass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world);
int16_t capsulePass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world);
struct BoxPolyScratch;
int16_t boxPass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world, BoxPolyScratch& s);
int16_t pointPass(Body& self, Sweep& w, MoveRequest& req, const PolySlot* slots, ShapeRec* const* shapes, const PolyWorld& world);

// The camera sphere (`radius`, +0xf0) against one actor shape: push-out (`push`, `pushed`) when it overlaps, then,
// with a contact search (`best` != 0), the motion's contact reached by t <= `t` (nearest kept in `contact`,
// `normal`, `best`; `t` updated). 0x44c840 sphere, 0x44c9b0 capsule, 0x44cb50 box (in the record's frame).
bool sweepSphere(Sweep& w, const ShapeRec& rec);
bool sweepCapsule(Sweep& w, const ShapeRec& rec);
bool sweepBox(Sweep& w, const ShapeRec& rec);
// The mover capsule (`moverRadius` around [capA, capB] from `centre`) against one actor shape, the same contract
// with the contact normal / travel seen from the mover: 0x440c10 sphere, 0x440e70 capsule, 0x441080 box.
bool moverSphere(Sweep& w, const ShapeRec& rec);
bool moverCapsule(Sweep& w, const ShapeRec& rec);
bool moverBox(Sweep& w, const ShapeRec& rec);
// The box mover (`half` in the frame `rot` / `inv`, axes `axes`, corners) against one actor shape, the same
// contract: 0x43d9f0 sphere, 0x43dc50 capsule, 0x43eaf0 box (faces of both, then the edge pairs, 0x43e330 push).
bool boxSphere(Sweep& w, const ShapeRec& rec);
bool boxCapsule(Sweep& w, const ShapeRec& rec);
bool boxBox(Sweep& w, const ShapeRec& rec);
// The words of the exe's scratch block 0x43f060 can read before writing them in a call (a separating axis found
// earlier: the entry as num / den of the motion and its normal); they carry over from the previous call.
struct BoxPolyScratch {
    int32_t num = 0, den = 0;
    SVec normal;
};
// 0x43f060: the box mover against one gathered polygon: the polygon's plane, the box's faces against the
// polygon's bounds and the polygon edges × box axes as separating axes; overlapping: pushed out along the
// shallowest (a contact at once when moving into it), else the latest entry within the motion.
bool boxPoly(Sweep& w, const PolySlot& slot, const PolyWorld& world, BoxPolyScratch& s);
// All listed shapes (`shapes`, `shapeCount` of them) the entity `self` collides with: each test's push moves the
// sweep and is summed in `passPush`; a pushing shape's entity joins the request's touched list once, and a floor
// contact (normal y < -11000) marks stand / bump flags. 0x44cd40 (the camera-sphere tests), 0x442200 (mover
// capsule), 0x43fe20 (box mover).
bool sweepShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
bool moverShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes);
bool boxShapes(Body& self, Sweep& w, MoveRequest& req, ShapeRec* const* shapes);

} // namespace bb::game::tbm
