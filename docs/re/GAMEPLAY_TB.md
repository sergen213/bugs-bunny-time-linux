# Gameplay RE — Time Busters (`bblit2.exe` 1.0.3.0)

Status: **Researched (static RE)**, partly **Verified Against Original Behavior** by runtime traces of the original
under Wine (§9; corrections there override the sections below).
Tags: **O** = observed (decompile `ghidra/diff/out/bblit2.exe.c`, exe bytes, or counted in the 58 shipped BZEs by a
probe), **I** = inferred. Confidence H/M/L. Addresses are TB VAs (image base 0x400000). Conventions as
[GAMEPLAY.md](GAMEPLAY.md): Q12 (0x1000 = 1.0), **Q14 for collision normals (0x4000 = 1.0)**, angles 4.12, PS1 axes
(+Y **down**), 30 Hz tick, C `/` truncates, `>>` arithmetic. TB emulates the PS1 GTE in software (`0x401db0` IR,
`0x401de0` V0, `0x402090` MVMVA, `0x402060` MAC read, `0x401ba0` SQR, `0x402950` RTV0); formulas below are the
integer results of those calls. LiT counterparts and the TB deltas already known are in GAMEPLAY.md §9.

Probes (python3 stdlib; `tb_anim.py pose` also needs numpy + Pillow; each runs with no argument (or `--selftest`)
as a self-check):

| probe | purpose |
|---|---|
| `tools/re/tb_ground.py` | collision mesh: `--check DIR` layout invariants + octree coverage over 58 levels, `--spawns DIR` floor under Bugs/Taz spawns, `LEVEL` dump |
| `tools/re/tb_moves.py` | Bugs/Taz states, transitions, logic blocks, pad conditions, speeds, jump arcs (`LEVEL [--blocks]`, `--all DIR`) |
| `tools/re/tb_anim.py` | packed-TOD animation: `stats [DIR]` parse/replay all 58 levels, `pose LEVEL ID ANIM FRAME OUT.png` |
| `tools/re/tb_music.py` | per-level music from logic action 126 (`BZE_DIR [TRACK_DIR]`) |

---

## 1. Collision ("Plancher") — polygon mesh + octree

### 1.1 Where it lives — O/H

Section-1 chunk **0x35 tag 0x36** `{off, size}` into section 4 (TB_LEVELS.md §3.7; one per level, 58/58) →
`DAT_004d26fc` → init `0x445d00` (log "Initialysing Plancher", called from `0x401000` right after the BZE load).
It is **separate from the render octree** (MODELS.md §10.1): world models are render-only, exactly as LiT's
heightfield is separate from its render cells. Nothing else (no heightfield, no per-cell tiles) exists in TB.

### 1.2 Block layout — O/H (code) + O (data, `tb_ground.py --check`: 58/58 parse to the last byte)

```
+0x00 u32   hi u16 = polygon count [DATA 58/58]; lo u16 unknown; not read by the code
+0x04 u32   surface table offset (block-relative)        -> state +0x04
+0x08 u32   surface count                                -> state +0x10
+0x0c u32   vertex table offset                          -> state +0x14
+0x10 u32   polygon table offset                         -> state +0x0c
+0x14 s16×3 cube corner x0, y0, z0  (+0x1a pad)
+0x1c u32   cube size S             -> half H = S >> 1; centre = (x0 + H, y0 − H, z0 + H) stored as s16
+0x20       octree root node                             -> state +0x08
...         octree nodes | polygon table | vertex table | surface table   (this order, contiguous)
```

Runtime state block `0x4d9900` (pointer `DAT_004d83b0`): +0 = 0, +4 surfaces, +8 root, +0xc polygons, +0x10
surface count, +0x14 vertices, +0x18/+0x1a/+0x1c s16 centre x/y/z, +0x20 u32 half.

**Octree node** (offsets of children are relative to the **root**, ×4):

| off | type | inner (`kind & 1`) | leaf |
|---|---|---|---|
| 0x00 | u16 | 0xffff at the root, otherwise not read | not read |
| 0x02 | u8 | kind = 1 | kind = 0 |
| 0x04 | | s16 child[8] (0 = empty), index = octant | s16 `n` |
| 0x06 | | | u16 poly[n]: polygon offset / 4 from the polygon table |

Octant bits (O, `0x445d90`): **1 = x ≥ cx, 2 = z ≥ cz, 4 = y < cy** (above, PS1). Child cube: `h >>= 1`, centre
moves ±h per axis accordingly. 31 205 nodes in all levels; max 2 143 leaves (AZR01), max 58 polygons per leaf
(TRQ01) [DATA].

**Polygon record** (variable, `0x18 + 8·n` bytes, 4-aligned; addressed as offset/4):

| off | type | field |
|---|---|---|
| 0x00 | s16×3 | plane normal (Q14 unit vector; 7 authoring-degenerate polygons have 0 → never hit) |
| 0x06 | s16 | surface index |
| 0x08 | s32 | plane `d`: `n·P + d = 0` on the plane (units × 0x4000) |
| 0x0c | s16×3 | bounding-sphere centre |
| 0x12 | s16 | bounding-sphere radius |
| 0x14 | u16 | unknown (not read by the queries) |
| 0x16 | u16 | `n` vertex count (3–12 in data: 19 878 tris, 14 225 quads, 198 larger); **bit 15 = runtime "visited" mark** of the queries |
| 0x18 | `{s16 ex, ey, ez; s16 v}`[n] | edge k: **outward edge normal** (Q14, in the polygon plane) and the index of its start vertex |

Vertex: 8 bytes `s16 x, y, z, 0`. Surface record, 12 bytes: **`u16 friction @0`** (Q12; ground grip =
`clamp(stateGrip·friction >> 12 + actor+0x40, 0, 0x1000)` in `0x443f40`, §2.2; 4096 normal, ARCA01 surface 2 = 400
= ice; writable by logic op 30), `u16 @2` (4096; not seen read), `u16 id @4` (logic key; 255 = none), `u16 @6`,
**`u32 flags @8`**. Data invariants [DATA, `--check`]: every edge
normal ⟂ plane normal (cos < 0.01, 117 656/117 656), every vertex within the bounding sphere, 138 edge vertices
> 2 units off their plane (authoring noise), every polygon referenced by ≥ 1 leaf, and the leaf lists are
**conservative**: an exact polygon/cube SAT finds 1 missing reference in 153 992; 20 636 references are extra
(e.g. VIBOSS02 lists its y = 31000 kill plane in all 1 024 bottom leaves) — harmless.

Totals [DATA]: 34 301 polygons (15 274 floor ny < −11000, 18 171 wall, 856 ceiling ny > 11000), 36 077 vertices,
318 surfaces.

### 1.3 Surface flags — O (filters) / I (names)

Two filters exist (O):

* **Segment query `0x4463e0`** (rays: camera, hover, AI line-of-sight, call-friend placement): a polygon is tested if
  `flags & 0xf136 == 0`, else only if every set bit is enabled by the query flags: 0x100 ← q 0x400, 0x10 ← q 2,
  0x20 ← q 4, 0x4 ← q 8, 0x2 ← q 0x100, 0xf000 ← `q & (flags >> 8) & 0xf0`. Query flag words used: camera 0
  (`0x437800`), AI/LOS `0x101 | (actor+0x10 >> 24 & 0xf0)` (`0x44d1f0`, `0x44e7d0`, `0x450650`), call-friend
  `0x703 | …` / `0x702` (`0x44ecd0`), logic `9`/`0x209` (`0x45a660`), `0x105` (`0x464090`). Query flag 1 also
  tests actors (`0x448920`).
* **Mover gather `0x446ea0`** (what actors physically collide with): 0x100 never; 0xf000 bits solid only for
  actors whose flags `+0x10` bits 28–31 intersect them; otherwise 0x4 is not solid; everything else (incl. 0x2) is.

| bit | meaning |
|---|---|
| 0x2 | solid for actors, **transparent to the camera** (query flag 0 never sees it) — I/H. ARCASG01's whole mesh is flag 2 |
| 0x4 | not solid for movers; seen by queries with q 8 — I/M (trigger/water-type surface) |
| 0x10 / 0x20 | query-only (q 2 / q 4); skipped by box-mode response `0x440060`/`0x443bf0` — I/L |
| 0x100 | never solid for movers; queries with q 0x400 only — I/M |
| 0x200 | no contact when the mover is not approaching the plane (no de-penetration) (`0x43f060`, `0x4418a0`, `0x44bfd0`) — I/M one-way |
| 0x40 | contact sets actor `+0x28 \|= 0x40` (`0x443f40`) — role I/L |
| 0x1 | edge/ledge surfaces scanned by `0x4426a0` (floors ny < −11999 within ±41 units) — I/L |
| 0x1000/0x2000/0x4000/0x8000 | per-class barriers (above); logic toggles them at run time |

Logic writes surfaces through VM opcode kind 9 (`'U'` tag 0x55 `{u32 index, u32 A, u32 B}`, dispatcher table
`0x4cd91c`): index **28** `0x4333b0` flags |= A, **29** `0x433400` flags &= ~A, **30** `0x433460` value(+0) = A, for
every surface whose `id@4 == B` (or all if B = 0x80000000) — O/H. In data (O): ARQ01 toggles 0x1002 on ids 0x64–0x6a,
TRQ01 0x4000/0x2 on 0x32–0x3b, AZQ01/Granny set 0x1000, ARBOSS01 sets 0x8000, AZBOSS01 clears 0x80000, TRBOSS02
clears 0x1 on 0x37–0x3a. Flag histogram over 318 surfaces: 0 ×156, 0x1 ×33, 0x1002 ×30, 0x2 ×21, 0x10 ×10, … and one
garbage word 0x3e4ccccd (= float 0.2).

### 1.4 Queries — O/H

**Locate `0x445d90(p, out)`**: if any `|p_i − c_i| > H` → out zeroed (node 0, centre 0, half 0). Else descend:
`h >>= 1`, octant bits as above, centre ±h, child = root + `child[o]·4`; an empty child returns node 0 *with* that
child's cube (centre, h). Output `{node, s16 cx, cy, cz, s16 h}`.

**Next leaf `0x445ed0(cube, p0, dv, out)`** (segment marching): for x, z, y in that order `t = (face − p0)·0x1000 /
dv` (face = c ± h on the side dv points to; 0x7fffffff when dv = 0); take the smallest (strict `<`); if `t >
0x1000` → end. Else `t += 0x14000 / |dv_axis|` (**20 units past the exit face**), locate `p0 + (dv·t >> 12)`; an
empty octant recurses from that cube, a zero half (outside the root) ends.

**Segment query `0x4463e0(q, work)`** — nearest polygon hit by `P0 → P0 + dv` (q +0 P0, +8 dv, +0x20 query flags,
+0x1c actor for actor tests, +0x24 query mask; radius `r` = q +0x44 if flags & 0x200 else **5**):
1. P0 is shifted by a world-origin vector (entity `DAT_004d5924` → `+0x9c` → `+0x6c..+0x70`; I: zero in practice).
2. Leaf = locate(P0) (or the first leaf along the segment). For each polygon of the leaf, skipping visited ones
   (bit 15 of +0x16; ≤ 248 marks, cleared at the end) and filtered surfaces:
   `s0 = (n·P0 + d) >> 14`, `s1 = (n·P1 + d) >> 14`; require **`s0 ≥ 0`, `s1 ≤ 0`, `s1 < s0`** (front → back;
   front = the side the normal points to, i.e. *above* a floor whose normal has ny < 0).
   `rel = dv·s0 / (s0 − s1)` per axis (truncating); `dist = |rel|²`; if `dist ≤ best`: hit `I = P0 + rel`, inside
   test **for every edge `e·(I − V_start) ≤ r << 14`** (polygon widened by r); accept → best, normal, polygon,
   surface.
3. If nothing was accepted in this leaf, march to the next leaf (`0x445ed0`) and repeat; stop at the first leaf
   that produced a hit. Because a listed polygon may be hit *beyond* that leaf, the result is not always the global
   nearest: the `tb_ground.py` port differs from a brute-force nearest on 2 of 1 200 random segments (ARCA01,
   TRCA01, AZQ01). Keep this for parity (I/M: matters for camera rays).
4. Query flag 1: also test actors (`0x448920` list; per-actor shapes box `0x43cb60`, sphere `0x449750`, capsule
   `0x449960`, OBB `0x43cdf0`); the nearest of all wins.
5. Result: return 1, **q +8 = hit point**, q +0x10 = normal (Q14), q +0x3c = surface record, q +0x40 = polygon
   (cleared if an actor was hit; q +0x18 = actor).

**Sphere gather `0x446ea0(centre, R, actor, out)`**: leaves touched by the sphere's bounding box (`0x4460c0`: the
centre leaf, then the six face points and the eight corners of the box, ≤ 15 leaves, no duplicates), then every
polygon with `|n·c + d| ≤ R<<14` (plane distance), `|c − c_poly|² ≤ (r_poly + R)²`, the centre within R of every
edge line (`e·c − (R<<14) ≤ e·V_start`, MVMVA with TR = (−4R, 0, 0)), surface passing the mover filter, and not
the actor's current ground polygon (`actor+0x108`, kept separately); `−e·V_start` per edge is cached for the
response. Limits: ≤ 0x1f2 marked polygons, ≤ 0xc6 contacts, ≤ 0x2a9 cached edges.

### 1.5 Mover `0x4450c0(actor, motion, stateRec, …)` — O (structure H, details M)

Every actor that moves calls it once per tick (type 14 characters, NPCs, objects).

1. **Shape** from actor flags0 `+0x08`: 0x10 → **sphere** (mode 2), else 0x1 → **capsule** (mode 1), else 0x40000
   → **oriented box** (mode 4), else mode 10 (= 8 | 2); flags0 0x8000000 adds 8 (offset sphere: centre at
   `−8 − hy`). Half extents from the actor box `+0x60..+0x6c`: `h = (max − min + 1) >> 1` per axis; centre =
   position + R(actor rot) · box centre. Sphere radius = largest half extent; capsule: axis = longest extent,
   radius = largest of the other two, half segment = longest − radius (< 21 → becomes a sphere); radius ≥ 8;
   extents < 10 → sphere of 10. **Bugs** flags0 `0x05000181` → capsule; **Taz** `0x05000190` → sphere (O/H). The
   box is written every animation frame by TOD packet 0x900 when the entity has no tag-0x61 records (`+0x78` = 0)
   or `+0x123` = 0xff (§5.2; O/H; runtime: ARCA01 item gdBit 473 `+0x60..` = (−107, 0, −17 | 107, −215, 19), r 240,
   from its anim, not tags 0x1b/0x1c (−107, 100, 0 | 0, 100, 17)). The mover adds the carried displacement `+0x58`
   to its target (`0x403c60`) and clears it (`0x445c40`): a state with flagsB 0x20000 adds (`+0x7c` − pos) to it on
   every tick whose frame hold `+0x153` has expired and turns the yaw by the attach part's yaw (`0x45643b`), so the
   entity follows its TOD 0xa00 point (thrown boomerang 401: next position = last tick's attach point).
2. **Velocity**: actor `+0x70/+0x72/+0x74` = velocity × 16. Work value `w = v >> 4`; airborne gravity first:
   `w.y += 5·DAT_004d26c4` unless actor `+0x24`/`+0x20` bit 0x200, state flagsA (`stateRec+0x20`) & 0xa2008601 or
   flagsC (`+0x28`) & 1. Then per axis with shift `k ≠ 0`: `w += ((target − w) << k) >> 12`, rounded away from
   zero (positive values get `+0xfff`); `k = 0` leaves the axis untouched (pure momentum). Shift selection (later
   rules override): flagsB (`+0x24`) & 0x80000000 with actor `+0x20 & 1`, or flagsA & 0x80000000 → kx = kz = 10;
   flagsA & 0x80000000 → ky = 11; flagsB & 0x20000 or flagsA & 0x2000400 → all 12; then the first of: actor
   `+0x20 & 0x80000` → (10, 9, 10); `+0x20 & 0x4000` → all 10; flagsC & 1 → all 9; flagsC & 8 → all 8; flagsA &
   0xa00 → all 11; flagsA & 0x20000000 → all 8. (k 12 = snap, 11 = ½, 10 = ¼, 9 = ⅛, 8 = 1/16 of the error per
   tick.) `w.y > 50` and not `+0x20 & 0x200` → actor `+0x20 |= 0x20` (falling); with `+0x20 & 1` the sign of
   `w.y` sets/clears 0x400. Stored back as `w << 4`.
3. Actor-vs-actor candidates (`0x448ce0`, radius `|v| + 50 + r(+0x4a)`, +300 when `+0x10 & 0x10000`) and world
   polygons (`0x446ea0`, same radius); if any: **response `0x443f40`**.
4. Response = iterative swept-shape slide: remaining time `0x1000` (Q12); per iteration the mode's sweep
   (`0x44ba20` mode 8, `0x442430` capsule, `0x44cf80` sphere, `0x440060` box; polygon tests `0x44bfd0`/`0x4418a0`/
   `0x43f060`, plane → edges → vertices, de-penetration first) returns the earliest contact; the contact normal is
   renormalised to Q14, the position advances to the contact, and the remaining motion loses its normal component
   (`v −= n·(v·n)`); up to 8 contact normals are kept and a motion pushed into two of them is stopped (crease).
5. **Contact classification** (O): **floor = normal y < −11000** (Q14 → slope ≤ 47.8°): actor `+0x20 |= 2`
   (grounded; `|= 8` instead when the contact is an actor with flags0 & 8 = platform), ground polygon remembered
   (`+0x144`, `+0x108`), landing speed along the normal > 0xb4 → `+0x28 |= 1` (hard landing); the tangential
   velocity is matched to the target by the grip factor (§2.2) and the normal component is removed. Two contact
   normals whose crease line is shallow (`|dir.y| < 0x2f6c`, Q14) while moving down → grounded with normal
   (0, −0x4000, 0) (V-shaped valleys). Non-floor
   world contact → `+0x20 |= 4` (wall). Ceiling (ny > 12000) while rising faster than 30 → `+0x24 |= 0x800000`.
   Actor flag `+0x10 & 0x80` + wall → velocity zeroed, `+0x28 |= 0x400`. Bounce (state flag 0x10000000) reflects
   with actor elasticity `+0x38` (Q12).
6. Optional passes: `0x4426a0` (ledge test with surface flag 1: flagsB 0x2000000 while airborne, or flagsA
   0x2000000 hang), `0x4434c0` (flagsB 0x1000000 while airborne, or flagsA 0x400), push/stand on actors per shape
   (`0x447350`/`0x447490`/`0x4476e0`/`0x447a60`), `0x447ee0` (+0x10 0x4000),
   `0x4439d0` (+0x10 0x10000); flagsC & 0x20: horizontal |v| above `stateRec+0x1e · 16` decays by 0xa0 (10
   units/tick) per tick down to that cap.
7. Writes back velocity, the move delta (motion +0x50..+0x58), `+0x20` flags and `actor+0x108` = ground polygon.

The hover rule in `0x453de0` (state flag 0x20000000, O): segment query from the position straight down by
`(+0x194 ?: 500) + 100` with query flags 0; `vy += clamp((hitY − height) >> 6, ±stateRec+0x14)` plus a bob of
`stateRec+0x14 · (phase − 0x800) >> 12`, phase `+0x198 += 0x40` per tick.

### 1.6 Spawn validation — O (`tb_ground.py --spawns`)

Downward query (mover filter with the players' `+0x10` = 0x0002c001) from 100 above each Bugs/Taz spawn over 2000
units, 116 spawns: **42 on a floor** (|dy| ≤ 16), **66 above a floor** (dy 17–1250; spawns are authored in the air
and drop on the first ticks — gameplay levels ≤ 436 (VIR01), the large values are cut-scenes CCTRBO02 1220–1250,
CCAZBO02 739), **2 embedded** (VICASG05: 22/53 units below a 12° slope; the mover's de-penetration lifts them — I),
**6 without floor**: Credits and RETRY (non-gameplay screens; spawns outside the octree cube) and **TRCA02**
(spawns at x ≈ −2900, outside both the collision cube and the world model's x range [−2220, 2220] → the players
must be repositioned by level logic before control — I/M). All floor hits are floors (ny ≤ −14654).

**Reimplementation notes (collision).** Load the block as is (offsets relative to the block; keep the 4-byte
polygon addressing used by the leaves). Implement locate / next-leaf / segment query bit-exactly (integer s16
centres, `>> 14` plane sides, truncating division, 20-unit step, nearest by squared distance with `<=`, radius 5
widening) — the camera, hover, AI and call-friend all depend on it. For movement, a swept sphere/capsule
slide-and-collide against the gathered polygons with the floor threshold ny < −11000 (Q14), up to 8 crease
normals and the surface filters reproduces the observable rules; bit-exact parity needs the per-mode sweeps
(`0x44bfd0` etc.), which are not transcribed here. Surface flags must stay mutable (logic ops 28/29/30).

---

## 2. Player movement constants (Bugs and Taz = generic character type 14 `0x453de0`)

Actor offsets (0x1b0-B entity): `+0xfc` pos s16, `+0xf4/f6/f8` pitch/yaw/roll, `+0x70/72/74` velocity ×16,
`+0x20/+0x24/+0x28` runtime flags, `+0x17c` state record, `+0x178` transition record, `+0x188` sequence index.
`S[]` = logic byte variables `DAT_004d24a0` (action 10 `0x430ce0`: `S[Y] = X`). **dt = `DAT_004d26c4`** = 2 every
tick (`0x401960`), 0 on the first tick after a load (`0x401000`). Pad record `GD+8+pad·0x28` (GAMEPLAY.md §2.3).
Probe: `tools/re/tb_moves.py`.

### 2.1 Locomotion (stick branch: state flagsB 0x80000000, flagsC bit 0 clear)

| what | value / formula | address | O/I | conf |
|---|---|---|---|---|
| stick magnitude | `mag = min(round(sqrt(sx²+sy²)), 0x80)` of pad `+0xc/+0xd` (dead zone applied in the pad layer) | `0x453de0`, `0x4040b0` | O | H |
| smoothed magnitude | `DAT_004d5ec0 -= (DAT_004d5ec0 − mag) >> 3` — written, **never read** (GAMEPLAY.md §3.8 "smoothed 1/8" is dead code) | `0x453de0` | O | H |
| run threshold | `mag > 0x77` (≥ 120): flagsB 0x4000000 states raise event `0x4000000 \| run` → sequence index 0 walk / 1 run | `0x453de0`, `0x44d990` | O | H |
| target speed | `v = max((spd·dt·mag) >> 7, 1)`, `spd` = record `fwd`; `fwd > 0 && fwd & 0x6000` → `spd = (s8)S[fwd & 0x9fff]` | `0x453de0` | O | H |
| Bugs walk / run | walk state 36 `fwd 13` → **26 u/tick**; run 37 `fwd 0x4066` → `S[0x66]` = 25 in 402/450 setters (also 30/32/70/80) → **50 u/tick** | ARCA01 data | O; run R **Consistent** (S[0x66] = 25, steady 48.84 u/tick with a residual heading error); walk **Not exercised** (runtime: RUNTIME_REFERENCE.md §4) | H (M for S) |
| Taz walk / run | 36 `fwd 13` → 26; run 37 `fwd 0x4067` → `S[0x67]` = 25 (150 setters) → 50 | data | O | H/M |
| keyboard | digital stick ±0x7f → mag ≥ 127 → always run | | I | H |
| heading | `t = atan2(sx, sy) + camYaw`, camYaw = atan2 of the view camera block (`0x4d5120 + view·0x2c0`) look-at − eye, **every tick** (LiT only while grounded) | `0x453de0` | O | H |
| forward | `Δ = wrap(t − yaw)`; `fwd += (v·(0x800 − \|Δ\|) ± 0x7ff) >> 11` (rounded away from 0) with flagsA 0x2 | `0x453de0` | O | H |
| turn | `+0x18c = Δ`; per tick `step = wrap(+0x18c) >> 2`, yaw += step; flagsB 0x400 clamps to ±`+0x192` (tag 0x1d; players have none) | `0x453de0` (0x455b16) | O | H |
| lean | roll `+0xf8 += clamp((Δ >> 3) − roll, ±0x78)` unless flagsB 0x4; decay ±0x10/±0x14 per tick | `0x453de0` | O | H |
| other drives | flagsA 0x2 without stick → `fwd += spd·dt`; flagsA 0x400000 constant `+spd·dt`; flagsB 0x10000 lateral `side·dt` | `0x453de0` | O | H |
| local → world | `w = R(pitch, yaw, roll)·(s << 5)`, `(w + 0x10) >> 5` | `0x453de0` | O | H |

### 2.2 Integration, gravity, ground grip (mover §1.5; only with flagsA 0x800000, else `+0x70 = w`, no collision)

| what | value | address | O/I | conf |
|---|---|---|---|---|
| smoothing | §1.5 step 2 (k = 0 keeps the axis) | `0x4450c0` | O | H |
| **gravity** | `vy += 5·dt` = **+10 u/tick²** unless `+0x24 & 0x200` (flagsA 0x2000000 hold), `+0x20 & 0x200`, flagsA & 0xa2008601, flagsC & 1 | `0x4450c0` | O; R **Verified** (vy +160 ×16 every tick; runtime: RUNTIME_REFERENCE.md §4) | H |
| terminal velocity | **none** (k_y = 0 while falling); s16 wrap caught at the tail of `0x453de0`: `y < −30000 && prevY > 30000 && vy > 0xc80` → respawn at checkpoint `0x4d26e0..ec`, camera reset | | O | M |
| ground grip | floor contact: tangential `v += ((target − v)·g + 0xfff) >> 12`, `g = clamp(grip·friction >> 12 + actor+0x40, 0, 0x1000)` (actor support: `actor+0x3a` instead of friction); grip = record `+0x10`: 3000 idle/jump, **1500 Bugs walk/run, 3500 Taz walk**, 750 skid, 1000 Taz spin, 4096 swim/climb; floor contact also takes `5·dt·g >> 12` off the upward speed | `0x443f40` | O | H (code) / M (values) |
| air control | jump (`+0x20` bit 1): kx = kz = 10 → ¼ of the error per tick; **walking off a ledge**: k = 0 → momentum kept, no steering until landing | `0x4450c0` | O (feel I) | M |
| speed cap | flagsC 0x20: horizontal \|v\| ≤ `cap·16`, excess −10 u/tick | `0x4450c0` | O | H |
| invulnerability | 30 ticks (`DAT_004d271c = 0x1e`, damage action `0x431a40`) | | O | H |

### 2.3 Jump (state entry `0x44d990`, flagsA 0x10; allowed if `(+0x20 & 0x421) == 0 || +0x20 & 0x4000`)

`+0x20 |= 1; vy = −0x50 − ((round(sqrt(H·0x50 + 100)) >> 1) << 4)` (stored ×16; O/H, `0x44d990`), H = record
`vert` (`& 0x4000` → `S[]<<4`, `& 0x2000` → `S[]·DAT_004d26b8`) → **vy0 ≈ −(5 + sqrt(20H + 25)) u/tick ≈ −sqrt(2gH)**:
**H is the jump height in units**. Then pure ballistics (k_y = 0): `v += 10; y += v`. **No variable-height cut**
(no ✕-release test; I/M). flagsA 0x8: impulse `+fwd·16` along the facing and `vy −= vert·16`. flagsB 0x100000:
homing jump to the selector target. Actor `+0x194..+0x19a` are **not** the arc (GAMEPLAY.md §3.8 correction): water
bob (`+0x198` phase +100/tick, `+0x19a` amplitude 7..16, `+0x196` ramp +0x40/tick) and pitch-follow scratch.

Flat-ground simulation (exact integers, `tb_moves.jump_arc`). The vy0 formula (Bugs −1760 = −110 × 16) and the bold
Bugs/Taz arcs are **Verified** at runtime (runtime: RUNTIME_REFERENCE.md §4, `tbplay/A_idle_jump.csv`,
`tbplay/C_switch_taz_jump.csv`):

| state (ARCA01) | H | vy0 | apex @ tick | lands | air speed (k 10) |
|---|---|---|---|---|---|
| Bugs 44 standing / 45 moving | 548 | −110 | **550 @ 10** | **21** | 40 / 50 |
| Bugs 46 / 49 / 362 water-exit / 232 swim-exit | 350 / 625 / 750 / 700 | −89 / −117 / −127 / −123 | 352@8 / 627@11 / 744@12 / 696@12 | 17 / 23 / 25 / 24 | 40 / 40 / 60 / 40 |
| Bugs/Taz 201 (ARCA01 instance) spring | 1500 | −178 | 1496 @ 17 | 35 | 100 |
| Taz 44 / 45 / 46 | 493 | −104 | **490 @ 10** | **20** | 38 |
| Taz carry-jump 183, both 118 air-△ hop | 300 | −82 | 296 @ 8 | 16 | 38 / 0 |

### 2.4 TB vs LiT (GAMEPLAY.md §3.7)

| | LiT | TB |
|---|---|---|
| walk / run | 40 / 50 u/tick, run at mag ≥ 110 (−50 dead zone) | 26 / 50, run at mag ≥ 120 |
| accel | ½ error/tick | ground grip/4096 (Bugs walk 0.37, Taz 0.85) × friction; air ¼ (jump) / none (ledge) |
| turn | Δ>>2, ×(0x800−\|Δ\|)/0x800, lean Δ>>3 | same (+ lean clamp ±0x78) |
| jump | sine arc H39/step70: apex 389 @ 15, lands 26, ✕ cut | ballistic g 10: Bugs 550 @ 10 / 21, Taz 490 @ 10 / 20, no cut |
| fall | `70 + 70·cos`, terminal 70 | +10/tick², no terminal |
| glide | 12 u/tick | Bugs helicopter ears (state 66): target vy +19, ky 9 (⅛ per tick) |
| invulnerability | 30 ticks | 30 ticks |

---

## 3. State machine data and abilities

### 3.1 State record (entity tag 0x2f, 48 B → 0x2c-B runtime node, loader `0x4347c0` case 0x2f) — O/H

Readers `0x453de0`, `0x44d990`, `0x4450c0`.

| file @ | rec + | name | meaning |
|---|---|---|---|
| 0x00 u16 | 6 | id | state id (< 10000 per entity, else global list `0x4d5108`) |
| 0x02 u16 | 8 | anim | animation role id (§5.1) |
| 0x04 u16 | 4 | mode | 1 loop, 2 once, 0x802 once + hold; bit 0x40 keep the clip if the anim is unchanged |
| 0x08 u16 | 0xa | logicA | logic block id ('M'), run every tick → `+0x180` |
| 0x0c u16 | 0xc | logicB | second block → `+0x184` (run if A returned no event) |
| 0x10 u32 | 0x20 | flagsA | physics / attack |
| 0x14 u32 | 0x24 | flagsB | control / AI |
| 0x18 u32 | 0x28 | flagsC | swim / vehicle / camera / root motion |
| 0x1c s16 | 0xe | fwd | forward speed (×dt); impulse ×16 with flagsA 0x8; `& 0x6000` → S[] |
| 0x1e s16 | 0x10 | grip | ground velocity-matching factor Q12 (§2.2) |
| 0x20 s16 | 0x12 | dmg | damage written to the victim `+0x66` |
| 0x22 s16 | 0x14 | vert | jump height (flagsA 0x10) / climb speed (0x800, ×dt) / lift (0x8000) / bob amplitude (0x20000000) / impulse (0x8) |
| 0x24 s16 | 0x16 | target | `0x45a120` selector (2 Bugs, 3 Taz, 8/0xE focus, 9/0xF partner, …); path id with flagsB 0x100 |
| 0x26 s16 | 0x1c | side | lateral speed (flagsB 0x10000) / path speed |
| 0x28 s16 | 0x1e | cap | speed cap (flagsC 0x20), strafe limit, path / avoid radius |
| 0x2a s16 | 0x18 | hitTrans | transition the **victim** takes when hit by this state |
| 0x2c s16 | 0x1a | ownHurt | own hurt transition (event 0x8000000: ownHurt, then victim `+0x5e`, then 3) |

flagsA: 0x1 no physics · 0x2 drive forward · 0x4 hide/kill at clip end · 0x8 entry impulse · 0x10 entry jump ·
0x20 pick up actor in front (`0x44e7d0`) · 0x40 throw/release (flagsB 0x40000 put down) · 0x80 advance sequence on
landing · 0x100 may mount rideables (I/M) · 0x200 hang on parent (climb/rope) · 0x800 vertical speed · 0x1000/0x2000
face/flee target (0x4000 snap) · 0x8000 lift · 0x10000 vulnerable to player hits · 0x40000/0x80000 attack hits
players / non-players (probes `0x447fd0`, 0x200000 front, 0x1000000 contact, 0x40000000 part) · 0x400000 constant
forward · **0x800000 use the collision mover** · 0x2000000 hold (no gravity) · 0x4000000 random wander · 0x8000000
push objects · 0x20000000 hover/bob · 0x80000000 mover k 10/11/10.
flagsB: 0x2 probe for interactables (△) · 0x4 align to ground normal · 0x8 vulnerable to enemies · 0x10 face
camera · 0x20/0x400 turn shift/clamp · 0x40 camera-relative strafe · 0x80 yaw follows velocity · 0x100 path follow ·
0x800 entry clears 0x421 (air jumps) · 0x1000/0x2000 `+0x24 |= 0x800/0x1000` (no look mode; rumble) · 0x4000 face away
from attacker · 0x8000 stompable · 0x20000 carry offset · 0x100000 homing jump · 0x1000000/0x2000000 airborne edge /
ledge tests · **0x4000000 walk/run split** · **0x8000000 anim pace from stick** · 0x10000000 bouncy · 0x40000000 no
sweep · **0x80000000 stick steering**. flagsC: 0x1 swim (k 9) · 0x2/0x4 surface/dive pitch · 0x8, 0x80, 0x800,
0x1000, 0x2000 vehicle/board variants · 0x10 pitch follows vy · 0x20 speed cap · 0x100 homing leads target · 0x4000
heading clamp ±0x50 · **0x200000 look mode allowed** · 0x400000 root motion from TOD probes (§5.2 packet 0xa00).
Bits tested in code O/H; names beyond the tested effect I/M.

**Transitions** (tag 0x31 `{u16 event, 5 × u16 state sequence}`, 0xfff0/0 = end; dispatcher `0x44d990`): low ids =
transitions; 0x20000000 next in sequence, 0x2000000 previous, 0x1000000 restart, `0x4000000 | i` select index
(walk/run); sequence end / index > 4 → event 1 (default); 0x8000000 hit; 0x10000000 death (`DAT_004d2720`/
`DAT_004d5948` override, else event 4); event 2 = level start. VM returns 0x80000000/0x90000000/0x50000000 = no
event (O/H).

**Logic blocks** (VM `0x45d7a0`): condition tags carry statement markers — 0x08 IF (top level; **the first true IF
ends the block**), 0x10 IF (continue), 0x20 nested / else-if, `\` (0x5c) with 0x20 = ELSE; chain bits 0x1 AND, 0x2 OR,
0x4 NOT. This resolves GAMEPLAY.md §10.9 "'X' flag bit 0x08" (statement marker). **'X' pad condition** `{u16
flags, required, any-of}`: source 0x400 new press (`held & ~prev`), 0x200 held, 0x100 raw, default pressed;
0x800/0x1000/0x2000/0x4000 → pads 0..3 (default = the actor's pad); 0x8000 adds camera-relative stick directions
(±50); false for players while `GD+0x9f` or `DAT_004d24d7 & 1` (cut-scene). 'N' = return u32 event. Chain bit 0x40
acts as AND for `]` `^` `` ` `` and forces their result true. A false AND / nested marker without a skip slot ends the
block with 0x10000000 (O/H; ported in `engine/game/src/tb_world.cpp`).

Conditions (payload after the encoder 0x433ab0 compression; O/H):

| tag (op) | payload | test |
|---|---|---|
| 'R' (8) | `{u8 flags, u8, u8 cond, u8, u16 X, u16 Y}` | table `0x4cd560[cond]`: 1 `S[Y] == X`; 2 `GD(Y) == X`; 3 `X < S[Y]`; 4 `X < GD(Y)`; 5 `X ≤ S[Y]`; 6 `X ≤ GD(Y)`; 7 `S[Y] < X`; 9 `S[Y] ≤ X`; 0xd `(s8)S[Y] == (s8)S[X]`; 0xf/0x10/0x11 `+0x82` `<`/`>`/`==` `N = Y<<8 \| X`; 0x13 `N < +0x82/30`; 0x14 `+0x82 % 30 == 0 && +0x82/30 == N`; 0x18 `S[Y] & X`; 0x19 `GD[0x58+Y] & X`; 0x1a `S[Y] != X`; 0x35/0x37 sound X (any / own voice) playing → `Y != 1` when silent; 0x36 focus carries a child; 0x45 bit X of `GD+500` (Y = 0: clear); 0x48 `(s16)+0x128 == X`; 0x49 depth `+0x1a2 ∈ [X, Y]`; 0x4a false; 0x80 `0x431680`. `GD(Y)` = u16 at `GD+0x58+Y` for even Y < 0x31, else u8. X is compared unsigned. (The previous decode read the cond from byte 3.) |
| 'T' (10) | `{flags, u8, u8 sub, u8, u32 mask, u32 target}` | table `0x4cd8e4`: sub < 0xc selects target (below); 0 `+0x20 & mask`, 1 `+0x24`, 2 `+0x28`, 3 `+0x08` |
| 'W' (0xc) `0x45a660` | `{flags, mode, p8, pc, p10}` | mode 4: frame counter `+0x14a == p8+1` and hold `+0x153 == 0`; else 8: `+0x14e & 0x2000` (last frame: hold expired on the last frame, kept on the loop-restart tick); else 0x80: `+0x14e & 0x1000` (restarted); 1/2: `rand() & 0xffff` must exceed p8 / pc; 0x20 contact `+0x54` id p10; 0x80000000: target = selector p10, sub `mode & 0x8f000000` (with `mode & 0x0f000000`): 0x81 target frame, 0x82 line of sight, 0x83 bearing sector `((pc−p8) & 0xfff) ≥ (atan2(t−self) − yaw − p8) & 0xfff`, 0x85 target transition event, 0x86 target state id, 0x87 `+0x42`, 0x88 distance² vs p8² (xz, 3D with 0x20000000; 0x10000000 = farther), 0x89 target-yaw sector, 0x8a/0x8b target y / Δy ∈ [pc, p8], 0x8c `+0x1ac`, 0x8d relative-yaw sector; 0x40 side assignment; result `!(mode & 0x10)` |
| 'Z' (0x10) / '[' (0x11) | `{flags, u32 mask}` | parent `+0x08 & mask` / any attached child's |
| ']' (0x13) | `{flags, u16 block, u32 target}` | runs block (< 10000 entity list, else global list) on self (0xc: a result without 0x10000000 ends the calling block), a target (events dispatched there), or every entity with the id (0x20000000); result = `r != 0x10000000` (or 0x40); target/block absent → unchanged |
| '^' (0x14) `0x45cc40` | `{flags, u16 frame, u32 mode, u16 template}` | frame counter == frame+1 (0x40000/0x80000 clamp, 0x800 any) and hold 0 and an attach part (+0x84, unless 0x4000); 0x20 releases what hangs on it (program 0x10000); 0x10000 ground decal; bits 1 2 4 8 0x200 0x400 0x20000 spawn effect `template`; true |
| '`' (0x16) | `{flags, u8 sound, u8 frame, u8 count}` | on frame+1 with hold 0: play `sound` every `count+1`-th time (per-item counter); true |

Target selector `0x45a120(sel, self)`: 0x40000000 child with id (0 any), 0x80000000 parent, 0x20000000 first entity
with id, 0x8000 nearest living entity with id; > 0x10 first living entity with id; 0xb contact `+0x54`, 0xc self,
0xd `+0x1ac`, 8/0xe focus, 9/0xf partner, 2 Bugs / 3 Taz (unless `GD+0xa2` bars them), 4/6 nearest and 5/7 farthest
player (3D / xz), 10 the player recorded in `+0x24 & 0x3000000`, else focus (O/H).

Actions: 'S' (7) table `0x4cd6ac` — 3/4 `S[Y]`/`GD` `|= X`, 5 `&=`, 7/8 `= rand() % X`, 10/11 `= X`, 12 `S[Y] = S[X]`,
16 `+0x82 = 0`, 20 sound, 21 `S[Y] += (s8)X`, 22 `GD(Y) += X` (`0x431030`, u16 at even Y ≤ 0x30), 34 yaw `+= (s8)S[Y] << 4`
(`0x4311a0`), 76/77 octant bit of the bearing to/from target X, 79, 82, 106 GAME_DATA +500 bit, 126 (above);
'V' (0xb) `0x45ad70` `{u32 op, p1, p2, p3}` (non-0x80000000 branch): 0x810 spawn template p3 (p2 times) at the attach
part or the actor (0x800 random yaw), on the spawned or selected target 0x40000 attach (to `+0x84`), 0x4000 event p1,
0x20000 kill; 0x200 release children matching `+0x08 & p1` / `+0x0c & p2` (`+0x20 |= 0x10000`, program run);
0x20/0x40 sound p1; results 0x80000 next, 0x4000000 previous, 0x10000000 restart at p1; 0x400 removes self. 'Y' (0xf)
event to the first child of behaviour 0x21 (5 = drop). `+0x82` is incremented by the entity loop after every tick
unless `+0x28 & 0x20`; state entry does not reset it (trace G). Entity program: tag 0x16 words `{+0x08, +0x0c, +0x10,
u16 block +0x2c, masks +0x14..}` — `0x45ea90` runs block +0x2c when `+0x20` gets a masked bit (0x10000000 call
friend, 0x10000 released) and clears it.

### 3.2 Abilities → states (ARCA01; templates 181 Bugs: 167 states / 108 transitions / 99 blocks, 182 Taz: 113/84/72)

Every regular level carries the same core set (`tb_moves.py --all`). Pad conditions O/H, names I (M unless noted).
✕/□ require L1 and R1 released (those are call/switch).

| button | Bugs (id 2) | Taz (id 3) |
|---|---|---|
| — | start 3 (event 2), idle 8, fidgets 17–31 (`ticks > 30`, random `S[0x1fe]` 0..4), idle-hold 13 | start 3, idle 10 (8/9), fidgets 17–22, 32 |
| stick | walk 36 / run 37 (event 0x13 split), skid 39→40 (no stick & ticks > 20, const fwd 40, grip 750); walk-only 35 when `S[0x6f] & 1` (L3 toggle; no PC key, I/L) | same ids; skid grip 1200; `S[0x6f] & 2` |
| ✕ | jump 44 (standing) / 45 (moving) → fall 52 → land 50 → 13; water 68/69 + ✕ → 362; swim 216/217 + ✕ → 232 | jump 44/45 (H 493) → 52 → 50; same water exits |
| □ | **kick** 54 (anim 58, dmg 1, hits players + enemies) → 55/58/57; combo 63/64 (fwd 42) when `GD+0xad & 1` and `S[0x6d] == 240`; running kick 56 (fwd 36); □ while holding = throw 160 | **spin** 135 (anim 114) → 136 (anim 79, 60 u/tick steerable, grip 1000, contact damage 1 incl. Bugs); hold □ > 11 ticks → 140/134/133; sustained spin 137 (66 u/tick) needs `GD+0xad & 0x20` |
| ○ | **ears-sneak** 38 (○ + stick, 24 u/tick); descending in air: **helicopter ears** 66 (target vy +19, ky ⅛, fwd 24); 67 (vy −20 up, fwd 50) when `GD+0xad & 2` | **pull-a-face** 70 (anim 148) needs `GD+0xad & 0x40`; ○ near a target → lunge 23 (contact damage, hop H 10, fwd 60) → 25 |
| △ | interactable in front (`+0x20 & 0x200000`, flagsB 0x2 probe) → use/pick 147 → carry 150–171 (throw 160/172/173); △ in air → dive 118 (H 300, dmg 1) → 120 → diggable (`+0x24 & 0x400`) → **underground** 124/126, move 127/129 (24 u/tick), ✕ exits; else bonk 121–123 | pick 147 → carry 177–187 (carry-jump 183, put down 186, throw 187); △ + stick + blocked (`+0x20 & 0x100000`) → **push** 65 (fwd 44); △ in air → spin-dive 118 → 120 → 124 / 140.. |
| ledge | airborne ledge test (flagsB 0x2000000, §1.5) → hang 75/73/74, Up + ticks > 15 → climb 76, ✕ jump, Down drop | 75/73, 76 |
| climb | ladder 78–81 (∓8/6 u/tick), wall-net 85–92 / 98–117 | — |
| hurt / death | victim transitions 3 → 377/210; death event 4 → 382 | 3/8/9 → knock-back 205 (fwd −20); 4 → 382 |

`GD+0xac/+0xad` (= `GD[0x58+0x54/0x55]`) are saved ability-unlock bits (I/M: granted in Granny.bze); `S[0x6d]/S[0x6e]`
per-character combo timers (I/M). Level-specific states live in the instance chunk (ARCA01 Bugs 0x14e/0x151/0x154
face-target idles via events 212–247, 201 spring jump H 1500). The per-character code differences are only data
plus the items in GAMEPLAY.md §9 (hit flashes, health mirrors GD+0xA5/+0xA6, Taz-hit victim flag, interactable
masks) and the collision shape (§1.5: Bugs capsule, Taz sphere).

---

## 4. Switching, call friend, co-op camera

### 4.1 Switching — `0x45eee0` per view per tick (O/H; R **Consistent**: A4 0 → 12 → 4, focus follows one tick later — runtime: RUNTIME_REFERENCE.md §4)

The camera target `DAT_004d5028 = focus` is latched in `0x4011c0` **before** `0x45eee0` → the camera follows the old
focus for one tick. Bits of `GD+0xA4` as GAMEPLAY.md §9.

```
for each entity with flags0 0x4000000 (class != 6):
    id 2 → Bugs, id 3 → Taz
    if (A4&4 ? id==3 : id==2): focus = it;   if !(A4&0x10): flags1 = flags1 & ~0x800 | 0x400   // pad 0
    else:                      partner = it; if !(A4&0x10): flags1 = flags1 & ~0x400 | 0x800   // pad 1 (unread in 1P)
airborneTicks(0x4d5eb6) = focus+0x20 & 0xa ? 0 : min(+1, 3)
if cooldown(0x4ceb08) < 45 && !paused(0x4d24a9): cooldown++
if !(A4 & 0x10):                                   // one player
    r1 = GD[+0xa] & 8 (P1 held)
    if r1 && !(A4&8) && !(partner+0x20 & 0x2000000 /*dead*/) && !(S[3] & 0xc) && !GD[+0x9f]:
        A4 |= 8; if !paused: A4 ^= 4; both: +0x28 |= 0x100 → entity program ("switched"), clears 0x100
    elif !r1: A4 &= ~8
    L1 (GD[+0xa] & 4) rising edge vs latch A4&0x20 → call_friend(0)
else:                                               // two players: A4 = A4 & ~1 | 2
    if focus.flags1 & 0x400 && focus.id == 3: swap pad bits, A4 |= 8, toggle A4&4, "switched"
    else: same R1 rule on the focus player's pad (camera focus only; pads stay);
          L1 on the non-focus player's pad → call_friend(0)
S[3] &= 0xf5                                        // per-tick locks 0x2/0x8 cleared; 0x1/0x4 persist
```

Logic action 79 `0x432320(self, {X})`: if `!(GD+0xa2 & 3)` and (X == 0 or X == partner id): toggle `A4 & 4`, both
`+0x28 |= 0x100` — no latch, no 2P/death checks (O/H). In 1P the partner's pad record is never read and no partner
AI code was found → the partner idles in its current state (I/M).

### 4.2 Call friend — `0x45eb10(force, work)` (O/H loop and constants; "in front of the camera" I/M)

Refused (unless `force`) when paused, `GD+0x9f`, `S[3] & 3`, **focus airborne > 2 ticks**, focus `+0x24 & 0x40`
(in water, I/M), no partner, partner dead (`+0x20 & 0x2000000`) or **cooldown < 45** (R **Verified**: refused at +20,
accepted at +80, cooldown `0x4ceb08` resets to 0 — runtime: RUNTIME_REFERENCE.md §4); refusal plays sound 0x5f
(`0x407080`) unless `S[3] & 0x10`/paused/dialog.

```
ok = place(&p, strict = !force)                   // 0x44ecd0; the initial {0,0,-400} offset is overwritten (dead)
if !ok: sound 0x5f; return 0
detach partner (0x44d870); partner.pos = p; partner.yaw = DAT_004d5972 (last look-mode yaw — stale)
partner+0x20 = (|0x10000000) & ~0x4200; +0x24 &= ~0x40; +0x84 = 0; teleport logic if it listens to 0x10000000
cooldown = 0
place(): p = focus.pos; p.y −= 200; partner collision bit 0x100 cleared during the search
    query flags = ((partner+0x10 & focus+0x10) >> 24 & 0xf0) | 0x703 (bit 0 cleared if !strict), radius 20 (§1.4)
    camYaw = atan2(view look-at − eye); best = 0
    for i in 0..7: a = camYaw ± (2·(i/2)+1)·0x100        // ±22.5°, ±67.5°, ±112.5°, ±157.5°
        d = RotY(a)·(0,0,500); segment query (p, d) → d cut at the hit; q8 = d·0x0ccd/4096 (0.8)
        q = hit ? |q8.xz|² : 200000
        if q > best: d ·= (|d| − 50)/|d|; d ·= 0.8; c = p + d      // cut before scale (trace D)
            floor: query (c, (0,400,0)) hits with normal.y < −11000 → f = the hit point
            clear: none of 4 queries (f − (0,150,0)) → RotY(k·0x400)·(0,0,60) hits
            if clear: best = q; result = f                     // the partner lands on the floor
    return best ≥ 0x57e5 (≈ 150 units free) ? result : fail
```

**Arrival** (R **Verified**, traces D/H; `ctest -R game.tb` asserts every tick): the `+0x20 |= 0x10000000` bit runs
the partner's entity program (tag 0x16: block +0x2c = 4, mask 0x10000000 in Bugs 0x1041c000 / Taz 0x10010010) via
`0x45ea90`. Block 4: `]` block 0xc3 on self, S3 `S[0x63] |= 2`, S10 `S[0x67] = 25`, no child with `+0x08 & 0x94` →
V 0x10 spawns template 169 (carrier, behaviour 0x0e) at the partner's position and yaw, S16, event 0xa4 → partner
states 261 261 261 260 262 (Bugs 261 261 260 262). Carrier state 3 (anim 217, 21 frames, once) block 3: frame 1
(W mode 4, p8 0) V 0x40000 attaches the partner to the carrier's `+0x84` part (TOD packet 0xa00 of frames 0–3, list
position 4); frame 17 U6, V 0x200 releases children with `+0x08 & 0x4000000` (`+0x20 |= 0x10000` → partner block 4 → event
0xa5 → 263 50 142, Bugs 263 264 13), sound 0x4b; frame 19 V 0x400 removes the carrier. The attached partner reads the
carrier's previous-tick pose (players tick before created entities): y − floor = −1432, −1432, −1337, −1242, −1139,
−1037, −934, −830, −727, −623, −519, −415, −311, −207, −103, 0, then lands (states as above). The carrier turns to
the camera (flagsB 0x10, not ported: partner x/z differ by ≤ 5 from H's (+4, −1)).

### 4.3 Camera — dispatcher `0x439700(target)`, follow `0x437800` (O/H unless noted)

Camera block `B = 0x4d5120 + view·0x2c0`: `+4` eye, `+0x10` look-at, `+0x2c` spring body P, `+0x3c` script flags,
`+0x44` override actor, `+0xbc` rail camera (`0x43a2c0`), `+0xc0..c8` body velocity (Q12), `+0xe0` follow flags
(`0x4d5200`), `+0xe4` L2/R2 accumulator, `+0xe8` settle counter, `+0x118..` constants set once by `0x437800`
(R **Verified**: peek `0x4d5238` s32×13 = the values below; spring rest `0x4d9ae0` = 1200, blend `0x4d9acc` = 0x1000;
look-at = T.y − 300; 2P framing `e > 15 → D += e`, e clamped 40 — runtime: RUNTIME_REFERENCE.md §4):

| param | value | use |
|---|---|---|
| +0x118 D0 | 1200 | spring rest length (`DAT_004d9ae0` current; 2P zoom changes it) |
| +0x11c / +0x120 | 500 / 20 | overhead guard radius / sideways impulse |
| +0x124 | 400 | body aims at `T.y − 400` (0 when swimming) |
| +0x128 | 50 | vertical dead band (after 20 settled ticks) |
| +0x134 | 300 | look-at `T.y − 300`; snap height |
| +0x138 | 300 | collision sphere radius |
| +0x13c / +0x140 / +0x144 | 30 / 10 / 15 | blocked impulse / snap impulse / slide gain |
| +0x148 | 400 | body velocity damping `v −= v·400 >> 12` (×0.902/tick) |
| +0x12c / +0x130 | 5000 / 800 | not read in `0x437800` (I) |

Modes (`0x439700`): script flags `B+0x3c & 0x300` → scripted paths (`>> 2` lerps); **look mode** when
`DAT_004d2714 == 1` (logic action `0x4325c0`) or **L2 + R2 both held** (View key) while standing (`+0x20 &
0x20d4421 == 0`, `+0x24 & 0x1000 == 0`, not invulnerable `0x4d271c`, no dialog/pause/cam flags 0x10/0x2000): yaw
`0x4d9a9a += sx·40 >> 7` (±0x7bc), pitch `0x4d9a98 −= sy·40 >> 7` (±0x3de), head = pos − (0,400,0), eye = look-at −
R·(0,0,400), focus hidden/frozen; exit restores yaw `DAT_004d5972`, `DAT_004d2714 = 3`, reset `0x4373c0`. Otherwise
**follow**. `0x4373c0` = **camera reset** (flags `&~0x7b | 4`, blend 0x1000, snap `0x437180`: points = pos, y −300,
eye/body 100 behind the yaw, zero velocity/accumulator) — GAMEPLAY.md §1.3/§5 call it "first person": wrong.

```
follow(actor, B):                                            // 0x437800
  init once; flags |= 0x280 (0x300 if GD+0x8a == 1); 2P (A4&0x10): flags = flags & ~0x80 | 0x100 else & ~0x100 | 0x80
  focus changed (DAT_004d9ac4) or flags & 0x2000: segment query focus↔P blocked → snap (flags |= 4)
  flags & 0x80 && |P − focus|² ≥ 0x5f5e11 (2500²) → snap
  blend(0x4d9acc) += (0x1000 − blend) >> 3
  T = !(flags & 0x80) && nobody dead ? (blend == 0x1000 ? (focus + partner)/2 : focus + (partner − focus)·blend >> 13)
                                     : (blend == 0x1000 ? actor : saved + (actor − saved)·blend >> 12)
  L2 xor R2 on the actor's pad, L1 up, alive, B+0x44 == 0, !(B+0x3c & 0x220000): acc(+0xe4) ±= 0x5000
  2P framing (0x100, !0x80): project feet and head of both players
      e = max(x overshoot beyond ±190, y overshoot beyond ±90) clamped to 40
      e == 0 → D −= 30 while D > 1200;  e > 15 → D += e
      split if D > 3000 || |Δxz| > 2700 || |Δy| > 1000: flags |= 0x80, blend = 0, saved = T
  2P split (0x100 && 0x80): D −= (D − 1200) >> 2; after 30 ticks re-merge if |Δxz| < 2300, |Δy| < 1000, partner in
      front and visible; else an off-screen grounded partner gets +0x20 |= 0x80000000 (held in place)
  occlusion queries P↔actor set flags 0x10 (blocked, impulse 30), 0x20, 4 (snap behind: vel = R(yaw+0x800)·10)
  d = (T − (0,400,0)) − P;  e = (|d| − D) >> 2;  move = R(dir d)·(0,0,e) + vel >> 12 + orbit      // ¼ of the error
  vel −= vel·400 >> 12
  airborne (+0x20 & 0x421): move.y −= f4((L.y − T.y) + 300), counter = 0
  counter > 20 && !(0x1000): ey = L.y − P.y − 100; |ey| > 50 → move.y += soft(ey)
  move = sweep(P, move, r 300) (0x440360); P += move
  L.xz −= f8(L − T); L.y −= (e < 0 ? f5 : f4)(L.y − T.y + 300)        // fN(x) = |x|·N < 4096 ? x·|x|·N >> 12 : x
  eye = P + R(dir L − P)·(0,0,−400)
  eye above L && !(0x400) && |L − P|xz < 500: vel += R(yaw ± 0x400)·20
  acc >> 12 ∉ {0, −1}: orbit = rotate(P − T, acc >> 12) − (P − T); acc −= acc·400 >> 12
```

L2/R2 (simulated): the accumulator ramps 5, 9, 13 … to **51 angle units/tick (≈ 4.5°/tick)** after ~25 ticks and
coasts ≈ 443 units (39°) after release. Midpoint flag `0x4d5200 & 0x100`, 0x80 = split/single focus, lerp
`0x4d9acc`. No LiT-style auto-recentre (the spring keeps the distance only; I/M). Follow distances/heights are
hard-coded, not per level: **chunk 0x3a tag 0x3c** is `@0 → 0x4d5954` near/detail draw distance, `@2 → 0x4d5980` far
draw distance (both scaled by H/400, clamped to `@2`, `0x465710`; `0x406840` → `0x409840`), `@4/@6/@8/@10` per-object
render-context fields (I/L: fog/LOD), **`@12 → 0x4d26c0` = pad edge mask** for raw-pad 'X' conditions (O/H) —
correction to TB_LEVELS.md §3.8 / GAMEPLAY.md §2.3.

**Reimplementation notes (§2–§4).** The player is a data interpreter: per tick logicA then logicB (first true IF
wins, 'N' returns the event), events through the tag-0x31 sequences (walk/run index by mag ≥ 120, advance on clip end
or landing with flagsA 0x80); local motion `fwd·dt·mag >> 7` scaled by `(0x800 − |Δ|) >> 11`, turn Δ>>2; integrate
with the mover (gravity +10/tick², k-shift table, k = 0 keeps, floor grip × surface friction); jumps are entry
impulses `vy = −0x50 − ((round(sqrt(80H + 100)) >> 1) << 4)` (×16) — keep `round(sqrt)` and the ×16 store. Two
actors always exist; switching flips `A4 & 4` + pad bits (1P) and the camera retargets one tick later (glide if within
2500 and visible, else snap). Call friend = 8-ray placement 500 around the camera yaw, 45-tick cooldown, ≤ 2
airborne ticks. Camera = damped spring (¼ distance error, ×0.902 velocity, D 1200, heights 400/300) with the 2P
midpoint/zoom/split rules; L2/R2 feed the ±0x5000 accumulator with 400/4096 decay.

---

## 5. Actor animation — chunk 0x22 tag 0x25 ("packed TOD")

**Answer: same Sony-TOD posing model as LiT, different (compressed) encoding — "partially same"** (O/H).
Same: GsCOORDINATE2 part hierarchy, RotMatrix = Rx·Ry·Rz, column ScaleMatrix, CompMatrixLV chain, rig = one-frame
asset applied once, sparse channels persisting across frames and animation switches, packet type numbers (1 coord,
2 object, 3 parent, 8 object control, 9 bbox, 0xa attach/probe), coord flag bits (2 rot, 4 scale, 8 trans), frame
held `res >> 1` ticks, no interpolation, last frame never shown, slot vertex buffer with ×0.5 seam averaging.
Different: 16-bit packet header with implicit length, 8-bit part ids, s16 translations, packed 12-bit angles, s8
delta frames, new types 4/6/7/0xb/0xc/0xd/0xe, roles renumbered (rig 1 / model 13 vs LiT 4 / 7), distance-LOD rigs.
Key VAs: loader `0x4347c0` case 0x25, rig build `0x469210`, LOD re-skin `0x469320`, player `0x469770`, frame applier
`0x468280`, part draw loop `0x469aa0`, GsGetLw `0x405820`, CompMatrixLV `0x404ae0`, RotMatrix `0x404160` (Ry
`0x404430`, Rx `0x4042b0`), ScaleMatrix `0x404710`, TransMatrix `0x4047c0`, vertex transform `0x409640 → 0x408d00`.
Probe `tools/re/tb_anim.py` (`stats`, `pose`, `--selftest`).

### 5.1 Assets, roles, selection — O/H

| what | value | address |
|---|---|---|
| asset node (0x1c B) | `+6` asset id (tag 0x27 u16@2), `+8` kind (2 model, 4 anim, 1 block, 0x100 sprite seq), `+0xa` **role** (tag 0x27 u16@0), `+0xe` tag 0x26 (absent in all data), `+0x14` logic-program list (the chunk's 'M' blocks), `+0x18` data | `0x4347c0`, `0x433ab0` |
| anim record (0x10 B, pool `0x4d2724`) | `+8` u16 nframes, `+0xa` data[0xc] (unused), `+0xb` ticks/frame = data[2] >> 1, `+0xc` → first frame | case 0x25 |
| actor asset links | tag 0x0f list (file order) → `actor+0x98`; lookups take the **first link with the wanted role** | `0x469770`, `0x469210` |
| role 13 | actor model (MODELS.md §10.2), `actor+0x15c` = model+8 | `0x469210` |
| role 1 | **rig**: frame 0 applied once builds the part list (2 025 assets; 2 024 res 1 / 1 frame) | `0x469210` |
| roles 2/14, 3/15 | distance LOD rig + model: `0x45eee0` camera distance (scaled to 400) vs `actor+0x121`/`+0x122` ×256, hysteresis 100 → `0x469320(actor, 0, 0, k)` swaps object pointers, pose kept (LOD meaning M) | `0x45eee0`, `0x469320` |
| role 12 | 80 models; no selecting code found (I/L) | — |
| other roles | animation ids (449 distinct); **state record u16@2 = anim id**, u16@4 = mode | `0x453de0`, `0x459b10` |
| state → player | `actor+0x14c` (current id) == rec+8 ? `0x469770(actor, actor+0x148, 0, 0)` (continue) : `0x469770(…, rec+8, rec+4)` (start) | `0x453de0` |
| ARCA01 Bugs | model asset 15 (38 objects, 443 vertices → 317 slots), rig 17 (41 parts); idle 31, walk 50 (8 frames @ 2 ticks), run 51 (20 @ 1), carrot-hold 24, ear-copter 68, spin 64 | data |
| ARCA01 Taz | model 134, rig 136 (37 parts); idle 31, walk 50 (24 @ 1), run 51 (8 @ 1) | data |

66 of the 274 Bugs/Taz states reference anim ids absent from the ARCA01 asset list → the player returns 0x400 and
keeps the current animation (O/H).

### 5.2 Format (little-endian) — O/H, all 13 905 assets parse to their last byte

Header (8 B, same bytes as the LiT TOD header): `u8 0x50, u8 0, u8 res (ticks/frame = res >> 1; res 1: 2 115 rigs,
2: 5 302, 4: 6 467, 6: 21), u8 0, u32 nframes`. Frame: `u16 size` (not read), `u16 npackets`, `u32 frame number`
(not used for timing), then packets. Packet = `u16 w` + implicit payload: `id = w & 0xff` (1-based part, 0 = actor),
`type = w & 0xf00`, `flags = w & 0xf000`. Packed rotation from words a, b: `rx = a & 0xfff, ry = b & 0xfff, rz = (w
>> 4 & 0xf00) | (a >> 8 & 0xf0) | (b >> 12)`; packed translation `tx = s8(a), tz = s8(a >> 8), ty = s16(b)`.

| type | flags / payload words | semantics | count |
|---|---|---|---|
| 0x100 coord | 0x2000 rot 3×s16; 0x4000 scale 3×s16; 0x8000 trans 3×s16 or 0x1000 packed trans (2 w) | set present channels; rebuild R·diag(s) if rot/scale; clears hidden | 149 326 |
| 0x200 object | 1 | object n (0 = none) | 0 |
| 0x300 parent | 1 | parent part; 0/0xffff → actor root `actor+0xa0` | 1 512 |
| 0x400 rot+trans | 4 (rz high bits in flags) | packed rot + trans, **scale reset to 1**, clears hidden | 159 579 |
| 0x600 | 2 | → part param +0x16 (meaning L) | 0 |
| 0x700 rot+trans | 4 | as 0x400 but keeps scale, keeps hidden | 115 475 |
| 0x800 object control | 0 | `0x1000` clear: create part (subtype `flags >> 13`: 0 normal, 1 special 0x40, 2–4 0x80/0x100/0x200, 5 unhide); set: `flags & 0xe000 == 0xa000` → **hide** (scale 0) else delete | 60 086 |
| 0x900 actor bbox | 7 (6×s16 + r) / 4 (`0x1000`: 6×s8 deltas + r) | `actor+0x60..+0x6c` (x/z inflated by `+0x3e`), `+0x4a` r, `+0x4c` (r + `+0x3e`)² — **the collision box of §1.5** | 57 145 |
| 0xa00 position probe | 0 | part origin → `actor+0x7c..0x80`; with state flagsC 0x400000 and a previous probe: velocity = (new − old)·16 (**root motion**) | 26 661 |
| 0xb00 delta | ⌈3n/2⌉ (s8 per channel) | adds s8 deltas (63 % of all packets) | 1 771 542 |
| 0xc00 create+object+parent | 1 | object = lo byte, parent = hi byte (0/0xff root) | 12 552 |
| 0xd00 full coord | as 0x100 | **absent channels reset** (rot 0, scale 1, trans 0) | 84 592 |
| 0xe00 rot only | 2 | packed rot | 391 952 |

Part node (0x78 B, pool `0x4d2484`): `+6` flags (0x8000 hidden, 8 no object, 4 object, 0x40 special, 0x400
externally driven: rot/trans packets skipped), `+8` coord, `+0xc` object `{vtxOff, nv}`, `+0x10` GsCOORDINATE2,
`+0x5c` s16 scale/rot/trans params. Quirk (O): a part with flag 0x400 skips 0x100/0xb00 reads without advancing
the cursor (never triggered by data alone).

### 5.3 Posing, drawing, timing — O/H

* Rig build: when `actor+0x9c` (parts) is empty, apply the role-1 frame 0, then **part 1's super = actor root coord
  `actor+0xa0`**. Bugs: part 1 = null root, parts 2..35 = objects 1..34 chained by 0xc00, parts 36..41 extras
  (carrot = object 25, shown only in anim 24).
* Local `R = Rx·Ry·Rz` (`0x404160`), `M = R·diag(scale)`, `t = trans`; trig tables sin `0x484230`, cos `0x486230`
  (4096 s16). World: GsGetLw `workm = super.workm ⊗ coord`, cached per frame stamp `0x63bfa0`; zero rotation (hidden)
  → part not drawn.
* Vertex word 3 = **slot index** (`& 0x7fff`) into the actor's 16-B screen buffer `0x4d0418`; bit 0x8000 = seam
  vertex averaged ×0.5 with the slot (after projection); primitives index slots — **correction to MODELS.md
  §10.2** (true for the 353 multi-object models with seams; 37 role-13 models have word 3 = 0 everywhere, use L).
* Player `0x469770(actor, state = actor+0x148, animId, mode, work)`: state `+0` last = nframes − 1, `+2` counter,
  `+4` anim id, `+6` flags (0x1000 just started, 0x2000 last frame, 0x100 finished, 0x10 paused, 0x200 started),
  `+9` frame-skip, `+0xa` ticks/frame, `+0xb` countdown. Modes 1 loop, 2 once, 0x800 hold, 8 restart, 4 resume, 0x10
  pause, 0x20 stop, 0x80 sync with a streamed track (I: speech). One call per tick: countdown 0 → apply the next frame
  (k frames with frame-skip), else at the end once → return 0x100 (0x200 with hold), loop → rewind and apply frame
  0; on each frame step run the asset's logic list (`0x45d7a0`). **Displayed frames 0..nframes−2**, each `ticks`
  ticks, no interpolation. Flipbook actors (`+8 & 0x200000`) → `0x465dc0`.
* Stick pace (flagsB 0x8000000, `0x453de0`): `acc += p` (×0x1111 >> 12 below run), `k = acc >> 5`, `acc −= k << 5`,
  ticks `+0x152 = (5 − k)·record ticks` (full stick 1×, slow up to 5×).

### 5.4 Visual verification — O

`tb_anim.py pose` (ARCA01, front + side, float math, orthographic, no back-face culling) →
`/mnt/1st4TB/BugsBunny/local/re/tb/anim/`: `bugs_anim31_f0/f1` (idle, hands on hips; f0 = f1 identical frames),
`bugs_anim50_f0/f4` (walk: alternating stride and arm swing), `bugs_anim51_f0/f10` (run), `bugs_anim24_f1` (carrot
in hand), `bugs_anim68_f3` (spinning ears), `taz_anim31_f0/f3`, `taz_anim50_f0/f12`, `taz_anim51_f0/f4`. Inspected:
Bugs grey/white with long ears, whiskers, gloves, feet, all limbs attached; Taz round tan belly, brown body, long
clawed arms, short legs, tail and hair tufts; run poses lean forward.

**Reimplementation notes (animation).** Decode each 0x25 asset once into frames of fixed packet structs (table
§5.2). Per actor: links from tag 0x0f (first role match wins), build parts from role 1 frame 0, parent part 1 to the
actor root; keep per-part s16 scale/rot/trans plus the Q12 matrix updated exactly as the applier does (rot/scale
rebuild R·diag(s), trans-only keep R, 0xd00 resets absent channels, 0xb00 adds s8 deltas, hide = scale 0). Drive it
from the state record (anim id u16@2, mode u16@4, restart only on id change), one call per tick, skip the last frame,
loop to frame 0, fire the asset logic per frame step, 0x100 when a once-anim ends; write the collision box from
0x900 packets; honour 0xa00 root motion under state flagsC 0x400000. Render through the slot buffer (average 0x8000
seams). LOD swaps at `actor+0x121/+0x122`.

---

## 6. Music — logic action 126

### 6.1 Mechanism — O/H

* Action **126** (`0x432db0`, act table `0x4cd6ac`) stores `DAT_0063b9ec = low byte of arg1`; the level loader's
  `0x407480` resets it to 0. Encoded in section 1 as tag **`'S'` 0x53** `{u16 action id, s16 arg1, s16 arg2}`
  (encoder `0x433ab0`, VM kind 7; arg bytes become the action struct +2/+4).
* Every tick (`0x4011c0` → `0x407460`, skipped while paused `DAT_004d24a9`) calls **`PlayMusic(n)` `0x4291e0`**: n =
  0 → stop; n = −1 → resume the current track (`DAT_004cca38`, initial value **99**); else file
  **`..\track\Track(n−1).xa`**, retried as **`Track14.xa`** when missing; n = 1 → index 0 → stop. Same track and
  stream ended → reopen (loop). PlayMusic(−1) is called on unpause (`0x4014c0`) and on window re-activation
  (`0x414ddc`, WinMain state 8).

### 6.2 Per-level tracks — O (`tb_music.py`, all 58 levels)

| level | n → file | level | n → file | level | n → file |
|---|---|---|---|---|---|
| Credits | 2 → Track1 | AZBOSS02 | 14 → Track13 | ARCASG04 | 34 → Track33 |
| Granny (hub) | 3 → Track2 | VICA01 | 15 → Track14 | ARQ01, ARQ02 | 35 → Track34 |
| CCEraCut | 3 → Track2 | VICASG01 | 16 → Track15 | ARR01 | 36 → Track35 |
| azca01 | 4 → Track3 | VICASG02 | 17 → Track16 | ARBOSS01 | 37 → Track36 |
| azcasg01 | 5 → Track4 | VICASG03 | 18 → Track17 | ARBOSS02 | 38 → Track37 |
| AZCASG02 | 6 → Track5 | VICASG04 | 19 → Track18 | TRCA01, TRCA02 | 39 → Track38 |
| AZCASG03 | 7 → Track6 | VICASG05 | 20 → Track19 | TRCASG01 | 40 → Track39 |
| AZCASG04 | 8 → Track7 | VIQ01 | 21 → Track20 | TRQ01 | 41 → Track40 |
| AZCASG05 | 9 → Track8 | VIQ02 | 22 → Track21 (+ 8 → Track7 from entity 317) | TRQ02 | 42 → Track41 (+ 46 → Track45 missing → **Track14**) |
| AZQ01 | 10 → Track9 | VIR01 | 23 → Track22 | TRR01 | 43 → Track42 |
| AZQ02 | 11 → Track10 | VIBOSS01 | 24 → Track23 | TRBOSS01 | 44 → Track43 |
| AZR01 | 12 → Track11 | VIBOSS02 | 25 → Track24 | TRBOSS02 | 45 → Track44 |
| AZBOSS01 | 13 → Track12 | ARCA01–05 | 26–30 → Track25–29 | | |
| ARCASG01 | 31 → Track30 | ARCASG02 | 32 → Track31 | ARCASG03 | 33 → Track32 |

Silent (no action 126): RETRY and the cut-scenes CCARBO02, CCARIN01, CCAZBO02, CCAZIN01, CCEND, CCINTRO, CCTRBO02,
CCTRIN01, CCVIBO02, CCVIIN01. Track files 1–44 exist; every one is used. The setter is level logic: in 34 levels a
chunk-09 aux item program (mostly block id 10), else a class-16 director (chunk 0x0a) or a class-14 entity (I/H:
runs at level start; TRQ02/VIQ02 switch tracks later from template/entity programs).

### 6.3 Menu music — O/H

The TB front end (`menu.men`, GDI, WinMain state 2 → `0x41c0f0`) **never calls PlayMusic**; its only audio is
`PlaySoundA("..\data\bong.wav")` in the sound-volume option (`0x41b060`). The original TB menu is **silent**.
`Track14.xa` is only the missing-file fallback (and what PlayMusic(−1) would pick before any level track, via the
initial 99). For the collection menu the closest original choice is the hub track **Track2.xa** (Granny, n = 3) —
I/M choice, not an original behaviour; `apps/bbtime/music.cpp` currently uses Track14 (UNVERIFIED there).

**Reimplementation notes (music).** Implement action 126 as "set wanted track"; once per tick compare with the
playing one: 0/1 stop, −1 resume, else `Track(n−1).xa` with the Track14 fallback, loop at end of stream; reset the
wanted track to 0 on level load (music stops until the level logic runs).

---

## 7. Corrections to other documents (owners should apply)

| doc | claim | correction (section here) |
|---|---|---|
| GAMEPLAY.md §4.5 / §10.9 | "polygon + octree floor, not decoded"; surface `u16 value @0` | decoded §1 (surface @0 = friction; flags §1.3); the header fields listed there are correct |
| GAMEPLAY.md §3.8 | stick magnitude smoothed 1/8; arc fields actor `+0x194..0x19a` | smoothing is dead code (§2.1); `+0x194..` = water bob / pitch follow (§2.3) |
| GAMEPLAY.md §5 / §1.3 | `0x4373c0` = first person | camera reset; look mode lives in `0x439700` (§4.3) |
| GAMEPLAY.md §9 | call friend teleports to `{0,0,−400}` | that offset is overwritten; 8-ray placement (§4.2) |
| GAMEPLAY.md §10.9 | 'X' flag bit 0x08 unknown | IF statement marker (§3.1) |
| TB_LEVELS.md §3.8 | 3a/3c = camera view parameters | draw distances + pad edge mask (§4.3) |
| MODELS.md §10.2 | actor prims index the concatenated vertex tables | they index slots (vertex word 3), seams averaged (§5.3) |
| AUDIO_XA.md (TB) | action-126 hits not found; TB menu track = Track14 | per-level table §6.2; menu silent (§6.3) |

## 8. Residual unknowns (Research Required)

1. Runtime confirmation: arcs, gravity, camera constants/spring rest/look-at, 2P framing, call-friend cooldown and
   arrival, idle fidgets are now Verified, run speed and switch Consistent (§9; RUNTIME_REFERENCE.md §4). Still open:
   walk speeds (not exercisable from the keyboard); Bugs state 23 (anim 40) lasts 10 ticks in trace G although its
   packed TOD (asset 31, 4 frames) gives 6 under the `0x469770` rule that matches every other clip.
2. Collision: header +0 low u16, polygon +0x14, surface +2/+6; surface flags 0x1/0x4/0x10/0x20/0x40/0x100/0x80000
   gameplay names; per-mode sweep internals (`0x44ba20`, `0x442430`, `0x44cf80`, `0x440060`, polygon tests) not
   transcribed; world-origin shift entity `DAT_004d5924+0x9c`; how TRCA02 relocates its players.
3. Player: flagsA 0x100/0x20000/0x10000000, flagsB 0x1/0x200/0x20000000, flagsC 0x20000; VM pieces W 0x40/0x82/0x87/
   0x8c, R 0x45(X = 0xffff)/0x48/0x80, the V 0x80000000 branch (damage, rig swap, camera), op 0x15 '_' (`0x45c670`,
   camera target), anim-asset logic blocks (chunk 0x22 'M', run by `0x469770` when a hold expires); the carrier's
   face-camera yaw (flagsB 0x10); partner behaviour in 1P (no AI found; idles — I/M); ability-unlock writers for
   `GD+0xac/+0xad`.
4. Camera: `0x440360` sphere-probe output layout (look-mode eye placement I/L); params +0x12c/+0x130.
5. Animation: role-12 models, packet 0x600, part subtypes 2–4, the 37 models with all-zero vertex word 3.
6. Music: whether the PlayMusic(−1) → Track99 → Track14 path is reachable in normal play.

## 9. Runtime verification (original under Wine, ARCA01 / Granny) — O (runtime)

Captures `local/wine/tbplay/*.csv` (WineRef harness `tools/wine/`, one row per tick `0x4d2708`), reproduced by
`engine/game` (`ctest -R game.tb`, numbers asserted). Per-item status: `docs/GAMEPLAY_STATUS.md` (TB section).
Independent verdicts for the same captures: RUNTIME_REFERENCE.md §4.

| claim | runtime | result |
|---|---|---|
| §2.3 jump arcs | Bugs vy0 −1760, apex 550 @ 10, lands @ 21 (state 50 the tick after); Taz apex 490 @ 10 | Verified |
| §2.1 walk/run, grip | S[0x66] = S[0x67] = 25; keyboard run 49 u/tick (mag 127); grip ramp 13/14, 21/23, 26/28 … 33/36 | Verified (run, grip); RUNTIME_REFERENCE.md §4: run **Consistent** (steady velocity (528, 0, 576)/16 = 48.84 u/tick ≈ 50·(0x800 − \|Δ\|)/0x800 with a residual heading error); walk 26 **Not exercised** (keyboard = full stick) |
| §4.1 switch | A4 0 → 12 on the R1 tick, camera target `0x4d5028` one tick later | Verified (tick order/latch); RUNTIME_REFERENCE.md §4: **Consistent** (A4 0 → 12 → 4) |
| §2.2 gravity, §4.3 constants | vy +160 (×16) per tick; `B+0x118..0x148` = 1200 500 20 400 50 5000 800 300 300 30 10 15 400, `0x4d9ae0` 1200, `0x4d9acc` 0x1000, look-at T.y − 300 | Verified (RUNTIME_REFERENCE.md §4) |
| §4.2 call friend | spot (326, 148) from Bugs for camYaw 490 = first candidate; cooldown 50 at level start, refused < 45; arrival (D, H `H_callfriend_carrier.csv`): carrier entity 169 at the floor spot, partner at its part 1432 above, descending 0, 95, 95, 103 … per tick, landed after 15 ticks, states 261 261 260 262 … 263 50 142 | Verified; **corrections**: `d ·= (\|d\| − 50)/\|d\|` comes **before** `d ·= 0.8` (q uses the 0.8-scaled cut vector); the floor query starts at the candidate (not 150 above) and the result is the floor point; the drop is the carrier's animation (§4.2 arrival), not gravity |
| §1.5 collision box | `+0x60..` = tag-0x61 record 0 (Bugs −70 0 −60 \| 70 −400 80, Taz −85 0 −50 \| 85 −240 125); item gdBit 473 (no records) = its TOD 0x900 box (−107 0 −17 \| 107 −215 19, r 240) | **correction**: the box comes from tag 0x61 records (loader case 0x61, action 82 `0x432530` selects `+0x123`, `0x450460` morphs ±20/tick); TOD 0x900 applies when `+0x78` = 0 or `+0x123` = 0xff (all-zero packets for the players); Verified for the item (`local/wine/tbnpc/H_item_pickup.pkl.gz`) |
| §3.4 pickup by touch | Bugs poked 150 beside item 473: next tick the item is in its 2nd state, `+0x82` 1, GD+0x5a/+0x6c/+0x84 0 → 1, GD+0x22f 0 → 2 (bit 473), S[0xa8] 1; 31 ticks later its 3rd state, then up 150/tick, `+0x20` 0x2000 7 ticks later at y −1105 | **Verified** (`game.tb`): the mover's contact gives the touched entity `+0x20` 0x80 (player) / 0x40 and its program (`+0x2c`, masks `+0x14`/`+0x18`) runs at once (`0x457a07` → `0x45ea90`); global block 10026: S106 bit, ']' 373 (S22 GD(Y) += X) and 10032 (S11/S90 totals), event 0xdb |
| §4.3 camera | D (`0x4d9ae0`) = 1200, flags `0x4d5200` = 0x1280 (1P) / 0x300 (2P), body `0x4d514c` = eye + 400·dir(L − eye), rest \|aim − eye\| = 1203; L follows T with f8 (xz) / f4 (y) exactly | **correction**: the spring error `(\|d\| − D) >> 2` is measured from the **eye** (B+4) to T − (0,400,0) and moves the body; no vertical dead band observed after landing; L2/R2 ramp 5, 9, 13 … as §4.3, L2 = +yaw orbit |
| §4.3 2P framing | D +16, +28, +38, then +40/tick as the players separate; split at \|Δxz\| 2752 → snap to the focus, D −= (D − 1200) >> 2 | Verified; projection distance ≈ 248 for the ±190 × ±90 bounds (fit, I/M) |
| §3.2 idle fidgets | trace G `G_idle_rng.csv` (100 s idle, rand seed `0x4cf26c` per tick): 8 → 20 21 22, 29 30 31, 26 27 28, 17 18 19, 23 (37 changes); `+0x82` 7, 8, 9 … not reset on state entry | **Verified**: replaying the traced seed per tick, all 37 changes on the original tick (`game.tb`); needs W mode 1/8, S7, S16 and the TB last-frame flag (§3.1). Open: state 23 hold (§8 item 1) |
| §3.3 view list 0x45eee0 | guard 267 (+0x6e 0x41a, range 7500) not listed (+0x82 frozen at 390) with the camera ~7.3k away; listed again once Bugs was poked 1200 in front of it (`local/wine/tbnpc/B_guard_attack.pkl.gz`, full entity pool per tick) | Consistent |
| §3.4 NPC logic (guard, template 265) | notice → 357 522 497 504 502 501 364 435 502 501 364 (B); 364's block on anim frame 7 spawns 401 (+16 ticks) with event 0x100/0x101 (rand) → 394/395, 401 flies out and back (flagsB 0x20000 carry), hits Bugs (its program, block 397: S[0x7a] = 1), removes itself on its anim's last frame (+73); 364's last frame → event 300 (435, +78) since S[0x7a] = 1; F2 (spin, seeds): 394's last frame rand() % 2 → 435 (F: 500) | **Verified** (`game.tb`: B sequence and relative ticks 16/73/78/126/127/133, flight dy exact and dx ±1 per tick; F2 guard sequence for 240 ticks with the traced rand() seeds). There is no V 0x80100000 in ARCA01; the first-tick event 2 only applies while `+0x17c` is unset |
| §3.5 damage | guard melee 394 (dmg 1, probe at TOD 0xa00 point) → Bugs hurt 208/189, health −1, flash 0x4d2058 0x40 → 0x7c (60 ticks); Bugs death → GD+0xa2 1, A4 4 (Taz), lives 3 kept (C/D captures) | Consistent (bbtime: health 6 → 5, state 189, flash; death → partner, then both down → lives 2, both 6 health) |

### 3.3–3.5 Entities, view list, combat (O/H code; runtime above)

* **Entity list** `0x4d4fd0`: every chunk 0x07/0x0a/0x63 in file order (appended by `0x4609b0`), spawns (`0x460b00`
  copies the 0x08 template entity) appended and added to the current view list. Templates `0x433620`: +0x50 =
  tag-0x27 template id → `0x45a120` (first living entity with the id, ids > 0x10) else the 0x08 template; the
  template's +0x10c/+0x110/+0x114 lists are appended after the own ones (own first wins); class 16 only +0x114.
* **View list** `0x45eee0` (per view, after the camera): `0x45ee90` sets +0x24 |= 4 for all; contact list
  `0x4d2734` = flags0 & 0x2000100 == 0x100 (not +0x6e 0x20, not an absent partner); activation block +0x118 must
  return 0x40000000; sleep +0x50 -= (|Δcamera|/4·0x4038 + 0x302a) >> 12; +0x6e (tag 0x39 @0): 0x20 never, 2 range
  +0x76 (@2) from the camera eye (cull → +0x50 = excess), 4/8 frustum (z + r(+0x4a) < H=400; |x|·0xd7a − z·0x8a0;
  up |y|·0xf77 − z·0x419; down y·0xf6d − z·0x43e; > r = out; 4 adds an 8-corner box test), 0x10/0x40 keep
  out-of-view entities listed, 1 world-slot test, 0x400 LOD, 0x80 shadow list. Loop `0x460630` ticks listed entities
  by class table `0x4cea60` (14 `0x453de0`, 16 `0x450bc0`, 35 `0x450cd0`, 20 `0x453d00`, 2 `0x4599e0`, 9 `0x44d6f0` …).
* **Hits** (`0x453de0`): intake at the head (vulnerable state flagsA 0x10000 vs +0x20 0x8000000 player hits,
  flagsB 8 vs 0x4000000 enemy hits, flagsB 0x8000 stomps, +0x24 0x40000000 forced): health +0x3c −= +0x66, players
  flash 60 ticks (+0x28 0x200), < 1 → +0x20 0x2000000, event 0x10000000 (overrides `0x4d2720`/`0x4d5948`, then
  `0x4d5944`/`0x4d273c`, else 4), else event 0x8000000 when +0x5e set; contact loop sets +0x54 both ways and
  +0x20 0x40/0x80; attacks (flagsA 0xc0000) use the mover contacts (0x1000000) or probe `0x447fd0` (point = +0x7c,
  within 125 of the target box); results +0x24 0x100000 / 2 (|1 lethal) feed the entity program masks.
* **Player down** `0x44ea50` (dispatcher, dead player, plain event): partner present → dead one absent (GD+0xa2
  1/2), control switches; both down → GD+0xa0 lives −1, checkpoint `0x4d26e0..ec`, both 6 health, forced call
  friend; no life → level 5 (RETRY), lives 3. V 0x80080000 = forced damage; V 2/8 (0x8… branch) and V 0x2000 set
  the checkpoint. Ported in `engine/game/src/tb_entities.cpp`.

## 10. HUD, overlay text, fades (tb_hud.cpp) — Verified (runtime) / Implemented

- S30 = 0x4312f0: `textStatus = 0x4680b0(X, priority Y)` (template-0x18 class-31 entity, interpreter 0x466d50 over the
  chunk-0x48 language block); R 0x22 = 0x430760 (spawn text X, true if status & X), R 0x23 = 0x4307a0 (last status & X).
- S51 = 0x4319f0: X 0 → 0x464e90(1, 0x100) fade in, X 1 → 0x464ec0(2, 0x100) fade out (holds black); S52 = 0x431a30:
  0x464e90(0x41, 0x400), a dark–light–dark blink (TB has no flash colour; 0x464ef0 scales the light colours).
- HUD 0x463170 (class 6, appended after the level's entities): see GAMEPLAY_STATUS / RUNTIME_REFERENCE §4 for the
  oracle timings. The counter slot is GD+0x58+GD[0xa9]; ARCA01 sets GD[0xa9] = 20 in trigger zone block 10.
- Trigger zones: chunk 0x09 aux items (0x30 B, list 0x4d2760: +0x10 origin, +0x18 rot, +0x20 x extent, +0x22 height,
  +0x24 z extent, +8/+0xc flags, +0x28 blocks keyed by entity id; 10 any player, 8 Bugs, 9 Taz). 0x44f550 runs them for
  class-14 entities with flags0 0x1000000 whose position lies in the (inverse-yaw) box (0x448780); id 0x15 with
  flags1 & 2 removes the entity; flags +0xc 0x1000 = water (+0x1a2 depth, +0x24 0x40). Not ported (starts the intro).
- VM register: S[0x1fe..0x1ff] is the running entity's +0x126 (0x45d7a0 entry/exit).
