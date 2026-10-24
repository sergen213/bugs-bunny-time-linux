// The TB collision code's shape primitives (bblit2.exe): sphere, cylinder, capsule and box tests of a moving or
// resting sphere. Each takes one record, laid out as the exe's callers build it (tools/re/tb_shapes_fuzz.py feeds
// the same bytes to the exe); results go back into the record. Return values are the exe's bit codes.
#pragma once
#include "tb_gte.h"

#include <cstddef>

namespace bb::game::tbm {

// 0x43cb60: a sphere of `radius` around the origin against a point starting at p moving by d. 1 = hit at t
// (Q12 of d; `travel` = the part of d before it, `normal` = the contact normal, Q14), 4 = p starts inside,
// 2 = misses, 8 = moves away, 0x10 = does not reach it.
struct SphereCast {
    SVec p;
    int16_t pad0 = 0;
    SVec d;
    int16_t pad1 = 0;
    int32_t radius = 0;
    SVec travel;
    int16_t pad2 = 0;
    SVec normal;
    int16_t pad3 = 0;
    int32_t t = 0;
};
static_assert(sizeof(SphereCast) == 0x28 && offsetof(SphereCast, normal) == 0x1c);
uint32_t sphereCast(SphereCast& r);

// 0x43c420: the infinite cylinder of `radius` around `axis` through the origin against a point starting at p moving
// by d: codes as sphereCast (no normal).
struct CylinderCast {
    SVec axis;
    int16_t pad0 = 0;
    SVec p;
    int16_t pad1 = 0;
    SVec d;
    int16_t pad2 = 0;
    SVec travel;
    int16_t pad3 = 0;
    int32_t t = 0;
    int32_t radius = 0;
};
static_assert(sizeof(CylinderCast) == 0x28 && offsetof(CylinderCast, radius) == 0x24);
uint32_t cylinderCast(CylinderCast& r);

// 0x449750: a point p inside the capsule of `radius` around the segment [0, axis]: `push` = the way out (to the
// surface + 2); false = outside.
struct CapsulePush {
    SVec p;
    int16_t pad0 = 0;
    SVec axis;
    int16_t pad1 = 0;
    int32_t radius = 0;
    SVec push;
};
static_assert(sizeof(CapsulePush) == 0x1c && offsetof(CapsulePush, push) == 0x14);
bool capsulePush(CapsulePush& r);

// 0x449960: the capsule of `radius` around [0, axis] against a point at p moving by d, reached by t <= `tmax`:
// `hit` = the travel to the contact, `normal` (Q14), t. p moves by -axis when it passes the far cap.
struct CapsuleCast {
    SVec p;
    int16_t pad0 = 0;
    SVec axis;
    int16_t pad1 = 0;
    SVec d;
    int16_t pad4 = 0;
    int16_t radius = 0;
    int16_t tmax = 0;
    SVec hit;
    int16_t pad2 = 0;
    SVec normal;
    int16_t pad3 = 0;
    int32_t t = 0;
};
static_assert(sizeof(CapsuleCast) == 0x30 && offsetof(CapsuleCast, radius) == 0x18 && offsetof(CapsuleCast, t) == 0x2c);
bool capsuleCast(CapsuleCast& r);

// 0x43cdf0: the box of half extents `ext` (grown by `radius`) against a point at p moving by d (both in the box's
// frame): `travel`, `normal` (Q14 along the face axis), t; false = misses.
struct BoxCast {
    SVec ext;
    int16_t pad0 = 0;
    SVec p;
    int16_t pad1 = 0;
    SVec d;
    int16_t pad4 = 0;
    int16_t radius = 0;
    int16_t pad2 = 0;
    int32_t t = 0;
    SVec travel;
    int16_t pad3 = 0;
    SVec normal;
};
static_assert(sizeof(BoxCast) == 0x30 && offsetof(BoxCast, radius) == 0x18 && offsetof(BoxCast, normal) == 0x28);
// `deepAxis` = the exe's scratch word +0x1e: the axis of the deepest slab p is inside of with motion along it; with
// no slab entered the normal takes it, also when no axis moved (then the value an earlier call left there).
bool boxCast(BoxCast& r, int16_t& deepAxis);

// 0x449bb0: a sphere of `radius` at p overlapping the box of half extents `ext`: `push` = the way out.
struct BoxPush {
    SVec p;
    int16_t pad0 = 0;
    SVec ext;
    int16_t pad1 = 0;
    int32_t radius = 0;
    SVec push;
};
static_assert(sizeof(BoxPush) == 0x1c && offsetof(BoxPush, push) == 0x14);
bool boxPush(BoxPush& r);

// 0x43bfb0: the point `out` of the line c + s·b nearest the line through the origin along a (`skew` set when the
// lines are not parallel; parallel: the nearest point of the segment [c, c + b] to the segment [0, a], s clamped to
// 0..1); returns s (Q12).
struct LineNearest {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec c;
    int16_t pad2 = 0;
    SVec out;
    int16_t pad3 = 0;
    int32_t skew = 0;
};
static_assert(sizeof(LineNearest) == 0x24 && offsetof(LineNearest, skew) == 0x20);
int32_t lineNearest(LineNearest& r);

// 0x43c7e0: the slab of `radius` around the plane spanned by a and b through the origin (the cylinder around a
// when a and b are parallel) against a point at p moving by d: codes as sphereCast.
struct SlabCast {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec p;
    int16_t pad2 = 0;
    SVec d;
    int16_t pad3 = 0;
    int32_t radius = 0;
    SVec travel;
    int16_t pad4 = 0;
    SVec normal;
    int16_t pad5 = 0;
    int32_t t = 0;
};
static_assert(sizeof(SlabCast) == 0x38 && offsetof(SlabCast, normal) == 0x2c && offsetof(SlabCast, t) == 0x34);
// `normalY` = the exe's scratch word +4: the plane normal's y >> 12, written only when a and b are not parallel.
uint32_t slabCast(SlabCast& r, int32_t& normalY);

// 0x43cfa0: the segments [0, a] and [c, c + b]: `out` = the offset between their nearest points (its sign
// depends on the case, as in the exe), `dist` = its squared length.
struct SegmentGap {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec c;
    int16_t pad2 = 0;
    SVec out;
    int16_t pad3 = 0;
    int32_t dist = 0;
};
static_assert(sizeof(SegmentGap) == 0x24 && offsetof(SegmentGap, dist) == 0x20);
void segmentGap(SegmentGap& r);

// 0x44a1a0: the capsules [0, a] and [c, c + b] overlapping within `radius`: `push` = the way out (+2).
struct SegmentPush {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec c;
    int16_t pad2 = 0;
    int32_t radius = 0;
    SVec push;
};
static_assert(sizeof(SegmentPush) == 0x24 && offsetof(SegmentPush, push) == 0x1c);
// `nearest` = segmentGap's offset ((0, 0, 1) when they touch), which the exe leaves in its scratch block (+0x54).
bool segmentPush(SegmentPush& r, SVec& nearest);

// 0x44a290: the capsule of `radius` around [0, a] against the segment [c, c + b] moving by d, reached by t <=
// `tmax`: `hit` = the travel to the contact, `normal` (Q14), t.
struct SegmentCast {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec c;
    int16_t pad2 = 0;
    SVec d;
    int16_t pad3 = 0;
    int16_t radius = 0;
    int16_t tmax = 0;
    SVec hit;
    int16_t pad4 = 0;
    SVec normal;
    int16_t pad5 = 0;
    int32_t t = 0;
};
static_assert(sizeof(SegmentCast) == 0x38 && offsetof(SegmentCast, hit) == 0x24 && offsetof(SegmentCast, t) == 0x34);
// `slabT` = the exe's scratch word +0x3c, its slab test's t: a slab test reporting "inside" leaves it as it was.
bool segmentCast(SegmentCast& r, int32_t& slabT);

// 0x447240: the segment [0, axis] passes within `radius` of p (squared lengths through the GTE).
struct SegmentReach {
    SVec p;
    int16_t pad0 = 0;
    SVec axis;
    int16_t pad1 = 0;
    int32_t radius = 0;
};
static_assert(sizeof(SegmentReach) == 0x14);
bool segmentReach(const SegmentReach& r);

// 0x449d20: the box of half extents `ext` rounded by `radius` (faces, edge cylinders, corner spheres) against a point
// at p moving by d (box frame), reached by t <= `tmax`: `hit` = the travel, `normal` (Q14), t.
struct BoxSweep {
    SVec p;
    int16_t pad0 = 0;
    SVec ext;
    int16_t pad1 = 0;
    SVec d;
    int16_t pad2 = 0;
    int16_t radius = 0;
    int16_t tmax = 0;
    SVec hit;
    int16_t pad3 = 0;
    SVec normal;
    int16_t pad4 = 0;
    int32_t t = 0;
};
static_assert(sizeof(BoxSweep) == 0x30 && offsetof(BoxSweep, radius) == 0x18 && offsetof(BoxSweep, t) == 0x2c);
// The exe's scratch words boxSweep reads before writing them in degenerate cases: the entered face's axis (+0x34;
// no face entered) and the edge's axis (+0x1e; p inside every slab).
struct BoxSweepScratch {
    int32_t faceAxis = 0;
    int16_t edgeAxis = 0;
};
bool boxSweep(BoxSweep& r, BoxSweepScratch& s);

// 0x44a9b0: the capsule of `radius` around [p - axis, p + axis] overlapping the box of half extents `ext` (box frame):
// `push` (added to) = the way out (+2); false = apart.
struct BoxCapsulePush {
    SVec ext;
    int16_t pad0 = 0;
    SVec axis;
    int16_t pad1 = 0;
    SVec p;
    int16_t pad2 = 0;
    int32_t radius = 0;
    SVec push;
};
static_assert(sizeof(BoxCapsulePush) == 0x24 && offsetof(BoxCapsulePush, push) == 0x1c);
bool boxCapsulePush(BoxCapsulePush& r);

// 0x44b010: the box of half extents `ext` against the capsule of `radius` around [p - axis, p + axis] moving by d
// (box frame), reached by t <= `tmax`: `hit`, `normal` (Q14), t. Faces first, then the box edges as segments.
struct BoxCapsuleCast {
    SVec ext;
    int16_t pad0 = 0;
    SVec axis;
    int16_t pad1 = 0;
    SVec p;
    int16_t pad2 = 0;
    SVec d;
    int16_t pad3 = 0;
    int16_t radius = 0;
    int16_t tmax = 0;
    SVec hit;
    int16_t pad4 = 0;
    SVec normal;
    int16_t pad5 = 0;
    int32_t t = 0;
};
static_assert(sizeof(BoxCapsuleCast) == 0x38 && offsetof(BoxCapsuleCast, hit) == 0x24 && offsetof(BoxCapsuleCast, t) == 0x34);
// `slabT` = the exe's scratch word +0x8c: each edge's slab test leaves its normal y there (+4 of its scratch) and
// the final segment test takes it as its slab t.
bool boxCapsuleCast(BoxCapsuleCast& r, int32_t& slabT);

// 0x43d830: the distance between the line through the origin along a and the line through c along b; `n` = a × b
// (pre-scaled, 0 when parallel) and `len` = |n|.
struct LineGap {
    SVec a;
    int16_t pad0 = 0;
    SVec b;
    int16_t pad1 = 0;
    SVec c;
    int16_t pad2 = 0;
    V3 n;
    int32_t f24 = 0;
    int32_t len = 0;
};
static_assert(sizeof(LineGap) == 0x2c && offsetof(LineGap, n) == 0x18 && offsetof(LineGap, len) == 0x28);
int32_t lineGap(LineGap& r);

} // namespace bb::game::tbm
