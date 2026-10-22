# Models and level geometry (BZE section 4) — LiT (TB: §10)

Container/LZSS: `BZE.md`. Texture ids/UV normalisation: `TEXTURES.md`.
Status legend as in TEXTURES.md (**Verified** = code + data; **Verified (data)**; **Inferred**; **Research Required**).

Evidence keys
- `[LL]` level loader `FUN_0042e7a0` (LiT `bugs.exe` v1.6), chunk `0x20` handler (`case ' '`) and chunk `0x22`
  handler (`case '"'`); decompile `/mnt/1st4TB/BugsBunny/ghidra/assets/bugs_all.c`.
- `[MAP]` `FUN_0040cd60` = PsyQ `GsMapModelingData` (functions.md: 0x40cd60 v1.6).
- `[WALK]` `FUN_00423eb0` primitive walker (record strides; texture-variant marking).
- `[WR]` `FUN_0040fe90` world/cell renderer; `[AR]` `FUN_00411b10` actor/prop renderer.
- `[OV]` `FUN_0040f150`/`LAB_0040efb0` object-vertex transform; `[BV]` `FUN_0040fa90`/`FUN_0040fc00` 0x44-block transform.
- `[BF]` `FUN_0040fdc0` backface cull; `[EM]` emitters `FUN_004159c0` (tri) / `FUN_004161b0` (quad) (software
  path; GL path `LAB_00414590`/`LAB_00414b40` is not decompiled but receives the same descriptor tables).
- `[DESC]` static descriptor tables in `.data` (dumped from the exe file): vertex-index slots `0x4692b8..`,
  UV byte offsets `0x469510..`, colour byte offsets `0x469698..`, tex/tpage slots `0x469920..`.
- `[DATA]` `tools/re/export_level.py`, `tools/re/tmd.py` over all 167 LiT BZE files.

## 1. Where models are referenced — Verified

Section 1 (load instructions) after command `0x2c` (which loads section 4 and makes it the model section [bze.md])
references models by `{u32 offset, u32 size}` relative to the start of **decompressed section 4**:

| chunk | tag | meaning | evidence |
|-------|-----|---------|----------|
| `0x20` | `0x24` | static world model (level geometry). 1–6 per level (206 chunks in 79 LiT levels) | [LL] `case '$'` → `GsMapModelingData(m+4)`, `FUN_00423eb0(m)`, object kind `+0x0a = 2` |
| `0x22` | `0x24` | actor/prop model (same format), registered under the asset id of tag `0x27` (u16 at +2) | [LL], kind 2 |
| `0x22` | `0x40` | same format as `0x24`; only difference: object kind `+0x0a = 0x100` (meaning unknown) | [LL] |
| `0x22` | `0x25` | **other format** (records start `50 00 …`; animation / skinned data). Research Required | [LL] allocs 0x14-byte struct, kind 4 |
| `0x22` | `0x3f` | `{u16 ?, u16 pad, u32 offset, u32 size}`, kind 1. Research Required | [LL] |
| `0x36` | `0x37` | `{offset,size}` in section 4, stored in `DAT_004b2490`. Research Required | [LL] |

Chunk `0x20` placement tags (all LiT data: 206/206 chunks have rotation 0x1000 ×3 and scale 0x1000) [LL][DATA]:

| tag | size | content |
|-----|------|---------|
| `0x11` | 6 | 3 × s16 angles, 4.12 (0x1000 = 360°) → `RotMatrix` (`FUN_00407a50`). Must come first: it allocates the object |
| `0x12` | 12 | scale; **the X word is used for all three axes** (decompile copies bytes 0–3 thrice) → `ScaleMatrix` |
| `0x10` | 12 | 3 × s32 translation, **same units as vertices** (not 20.12) → `TransMatrix` |
| `0x24` | 8 | model `{offset, size}` |
| `0x21` | 4 | u16 id (stored at +0xc; used as world-model slot `DAT_004b37a0[id]`) |
| `0x39` | 2 | u16 → object +0xb6; `0x43` 1 byte → object +0xb5 (unknown) |

World position of a vertex = `v * scale + translation` (rotation is identity in all shipped data).

## 2. Model header ("PC TMD") — Verified

A PS1 TMD with float vertices. All little-endian; offsets in the object table are relative to the **start of
the object table** (`model + 0x0c`) and are relocated in place by [MAP].

| off | type | field |
|-----|------|-------|
| 0x00 | u32 | magic `0x41` |
| 0x04 | u32 | flags, always 0 [DATA] |
| 0x08 | u32 | object count `nobj` (0 occurs for 688 actor entries: no geometry) |
| 0x0c | obj[nobj] | 28-byte object entries |

Object entry (28 bytes):

| off | type | field |
|-----|------|-------|
| 0x00 | u32 | `vert_top` (offset from object-table start) |
| 0x04 | u32 | `n_vert` |
| 0x08 | u32 | `normal_top` (always points past the data; `n_normal` is always 0) |
| 0x0c | u32 | `n_normal` = 0 [DATA 14579/14579 objects] |
| 0x10 | u32 | `prim_top` |
| 0x14 | u32 | `n_prim` (count of primitive *records*, see §4) |
| 0x18 | s32 | `scale`, always 0 |

Object vertex table: `n_vert` × 16 bytes = `float x, float y, float z, u32 word3`. Values are integral
(converted from PS1 s16). The renderer transforms them into a 16-byte screen-space buffer [OV]. World models
ignore `word3` (0 in world data).

Actor/prop models (correction, GAMEPLAY.md §6, `0x411350`): `word3 & 0x7fff` is the vertex's **slot** in a
per-actor shared vertex buffer and primitive vertex indices address **slots**, not the concatenated tables.
Bit `0x8000` marks a seam vertex: it is written by another part's object too and is **averaged** (×0.5, after
projection in the original) with the value already in the slot. Non-blend slots are `0..N−1` in object order,
blend slots always refer to one of them [DATA: all 4284 role-7 models; Bugs: 445 vertices → 313 slots]. So
"plain concatenation" (e.g. L01a model `0xb854`: object 2 uses indices 24–26 with object 0 having 11 and object
1 having 7 vertices) holds only for models without blend vertices. Each TMD object is posed by its own TOD part
matrix (rigid per-part skeleton, GAMEPLAY.md §6; C++ `bb/actor.h`) — Verified (data + numeric match with
`tools/re/anim.py`).

## 3. Coordinate system — Verified (data), Inferred (handedness)

PS1 convention: +X right, **+Y down**, +Z forward (into the screen). L01a world: x 690…26320, y −4680…2711,
z 5303…33609; ground of the first room at y ≈ −1600 is *above* deeper floors. The prototype OBJ export negates
Y; the front render then shows vegetation on top and canyon strata horizontal (correct orientation). Units:
1 unit ≈ 1 PS1 GTE unit; Bugs-scale objects are a few hundred units tall [DATA].

## 4. Primitive stream — Verified

Starts at `prim_top`. Every record begins with a 4-byte header:

| off | type | field |
|-----|------|-------|
| 0 | u16 | `count` (meaning depends on type, below) |
| 2 | u8 | `flags` (§5) |
| 3 | u8 | `type` |

Two kinds of records [WALK][WR]:
- **Grouped** types: `count` consecutive records of the same type and stride follow, the first one being the
  record holding the header (each record has its own 4-byte header; `count` is meaningful in the first).
  `n_prim` is decremented by `count`.
- **Single** types: one record, `n_prim -= 1`.

| type | kind | stride (bytes) | meaning | evidence |
|------|------|----------------|---------|----------|
| `0x00`, `0x04` | single | 16 | **cell header** (§7); `0x04` = same with a depth bias (`param_3+6 << 2`) | [WR] |
| `0x08`, `0x0c` | single | 56 | **cell bounding hull** (§7); `0x0c` = with depth bias | [WR] |
| `0x10` | single | 20 | **portal** quad (§7) | [WR] |
| `0x44` | single | 4 + 12·count | **vertex block**: `count` × `float x,y,z` (§6) | [WR] `(*DAT_004b1aa4)(count, rec+4)` |
| `0x34` | grouped | 24 | textured triangle, flat colour | [DESC] |
| `0x38` | grouped | 28 | textured quad, flat colour | [DESC] |
| `0x3c` | grouped | 32 | textured triangle, Gouraud (3 colours) | [DESC] |
| `0x40` | grouped | 40 | textured quad, Gouraud (4 colours) | [DESC] |
| `0x48` | grouped | 24 | untextured Gouraud triangle (never seen in LiT data) | [DESC] |
| `0x4a` | grouped | 28 | Gouraud triangle with texture but **no UVs** (implicit UVs, §5) | [DESC][WALK] |
| `0x4c` | grouped | 28 | untextured Gouraud quad (never seen) | [DESC] |
| `0x4e` | grouped | 32 | Gouraud quad with texture, no UVs | [DESC][WALK] |
| `0x64` | grouped | 16 | **sprite / billboard** (§8). Skipped by the world renderer | [WALK][WR] |
| `0x14`,`0x18`,`0x1c`,`0x20`,`0x24`,`0x28`,`0x2c`,`0x30` | grouped | 16,20,24,28,20,24,32,36 | handled by [WALK] only, never seen in LiT data | [WALK] |

Any other type terminates the walker (`default: return`). Types present in LiT data [DATA]:
world `0x00 0x04 0x08 0x0c 0x10 0x3c 0x40 0x44 0x4a 0x4e`; actors `0x34 0x38 0x3c 0x40 0x4a 0x4e 0x64`.

## 5. Polygon record layouts — Verified

Byte offsets inside one record (header at 0..3). `uvN` = 2 bytes `u, v`; `tex` = u16 texture id;
`tpage` = u16 whose **low byte bits 5–6 = PS1 ABR** blend mode; `rgbN` = 4 bytes `r, g, b, 0`; `iN` = u16 vertex index.

| type | 4 | 6 | 8 | 10 | 12 | 14 | 16 … | indices |
|------|---|---|---|----|----|----|------|---------|
| `0x34` tri flat | uv0 | tex | uv1 | tpage | uv2 | **i2** | rgb0 @16 | i0 @20, i1 @22 |
| `0x38` quad flat | uv0 | tex | uv1 | tpage | uv2 | uv3 | rgb0 @16 | i0..i3 @20,22,24,26 |
| `0x3c` tri Gouraud | uv0 | tex | uv1 | tpage | uv2 | **i2** | rgb0..2 @16,20,24 | i0 @28, i1 @30 |
| `0x40` quad Gouraud | uv0 | tex | uv1 | tpage | uv2 | uv3 | rgb0..3 @16..28 | i0..i3 @32,34,36,38 |
| `0x48` tri untex | rgb0..2 @4,8,12 | | | | | | | i0..i2 @16,18,20 (+22 pad) |
| `0x4a` tri | tex @4 | tpage @6 | rgb0..2 @8,12,16 | | | | | i0..i2 @20,22,24 (+26 pad) |
| `0x4c` quad untex | rgb0..3 @4..16 | | | | | | | i0..i3 @20..26 |
| `0x4e` quad | tex @4 | tpage @6 | rgb0..3 @8..20 | | | | | i0..i3 @24..30 |

Source: index slots `0x4692b8` (0x34: 10,11,7 ×u16), `0x4692e8` (0x3c: 14,15,7), `0x469330` (0x38: 10..13),
`0x469360` (0x40: 16..19), `0x4692d0` (0x48: 8,9,10), `0x469300` (0x4a: 10,11,12), `0x469348` (0x4c: 10..13),
`0x469378` (0x4e: 12..15); UV offsets `0x469510/38/60/88`; colour offsets `0x469698/0x469728/0x469800/0x469890/
0x4696e0/0x469770/0x469848/0x4698d8`; tex/tpage u16 slots `(3,5)` for 0x34–0x40, `(2,3)` for 0x4a/0x4e
(`0x469920..0x469948`) [DESC]. The walker reads `tex` from u16[3] and `tpage` low byte from u16[5] (0x1c–0x40 style)
or `tex` from u16[2] (0x4a/0x4e) [WALK]; real data matches (L01a 0x4e: tex 386/397/400/401, tpage 0x80).

**Flags byte (+2)** — values seen: `0x01` (142 163 faces), `0x03` (16 933), `0x09` (7 805), `0x0b` (2 062) [DATA]:
- bit 1 (`0x02`) = **double-sided**: the group's backface cull is skipped when set on the group's first
  record [BF]; the emitter flips winding for back-facing polygons [EM]. Verified.
- bit 3 (`0x08`) = **semi-transparent**: blend mode = `(tpage_lo & 0x60) >> 5` (ABR 0–3); clear → opaque
  variant (4) [WALK][EM]. ABR 2 additionally marks the texture (`DAT_0052fd69[id*48] = 1`). Verified.
- bit 0 (`0x01`) always set in data; meaning unknown (PS1 TMD "light source off" analogue: Inferred).

**Quad vertex order:** PS1 "Z" order — v0, v1, v2, v3 with outline **v0 → v1 → v3 → v2** (triangles
(v0,v1,v2) + (v1,v3,v2)). Evidence: the quad emitter puts i2/uv2 into slot 3 and i3/uv3 into slot 2 [EM];
first L01a quad has indices (0,1,3,2) and UVs (255,0),(255,255),(0,0),(0,255), which only form a non-twisted
square in Z order. Visually verified (textured renders).

**Front face — Verified (data), corrected:** in PS1 world coordinates (+Y down) the **visible side** of a
triangle (v0,v1,v2) — and of both quad triangles (v0,v1,v2), (v1,v3,v2) — is the side the normal
`N = (v0 − v1) × (v2 − v1)` points to. Measured on single-sided near-horizontal world faces: N points up
(−Y) for 708/762 (L01a), 1160/1172 (L03A), 737/900 (L02A1); the remainder are ceilings/undersides.
Equivalently (peer LevelLoaderLiT, engine mapping PS1 → (x, −y, −z)) the visible side is **clockwise on
screen** in a y-up GL-style view. [BF] tests `(c.y − b.y)*(a.x − b.x) − (c.x − b.x)*(a.y − b.y) < 0`
(`_DAT_0045c308` = 0.0f) with `a,b,c` = projected slots 0,1,2 *before* the integer y flip [BV][OV], but the
view matrix `DAT_006ac220..` sign conventions are not decoded, so take the sign from the data rule above,
not from that expression. (An earlier revision of this doc read the [BF] sign the other way — wrong.)

**UVs:** `u, v` bytes are **normalised to the whole TIM**: texel = `(u/255)·(w−1)`, `(v/255)·(h−1)`
(tables `0x467f50`/`0x468350` = i/255, multiplied by `texW−1`/`texH−1` in [EM]). Not PS1 texture-page
coordinates. **`v` is measured from the bottom image row** (`v = 0` → last TIM row, `v = 255` → first row) —
Verified (data): L03A crate model sec4 `0x10fbc`, tex 20 "ACME": top vertices (y = −210) have v = 255, bottom
(y = 0) v = 0; the lettering reads upright only with this convention (also found by LevelLoaderLiT; ship
windows/balusters in L03A agree). Code hint: the PC resample loop in `FUN_00420d10` uploads rows from the
bottom. So sample row `(1 − v/255)·(h−1)` counted from the top of the decoded TIM; in GL with row 0 uploaded
first, use `t = 1 − v/255`. OBJ export: `vt = (u/255, v/255)` (OBJ vt 0 = image bottom).

**Implicit UVs (0x4a / 0x4e, and any poly emitted with a null UV table) [EM]:** tri: i0 (0,1), i1 (1,1),
i2 (0,0); quad: i0 (0,1), i1 (1,1), i2 (1,0), i3 (0,0) in normalised (u,v). Used with small single-colour
textures (L01a ids 386–401), so the mapping is visually irrelevant.

**Vertex colours:** `r,g,b` per vertex (Gouraud) or one per polygon (flat). PS1 semantics: texel × colour / 128
(0x80 = neutral, saturating). The PC port divides by 255 instead (world renders half as bright as PS1 —
BBLIT-Remaster `render.c` comment at ~line 1911). Typical world values 0x40–0xC0. A faithful renderer should
pick one convention explicitly (PS1 /128 recommended; document the choice).

## 6. Vertex addressing — Verified

- At object start, the object vertex table (concatenated over all objects for world models; the slot buffer
  for actor models, §2) is the current vertex base for polygons **and** the base for cell/hull/portal indices
  (`DAT_007c6240`) [OV]. No actor model contains a `0x44` record (the C++ loader rejects one).
- Each `0x44` record replaces the polygon vertex base with its own `count` float3 vertices; polygon indices
  after it are **local to that block** until the next `0x44` (cell headers/hulls/portals keep using the object
  table) [BV][WR]. 0 out-of-range indices in all 79 LiT levels with this rule [DATA].

## 7. World cells, hulls, portals — Verified (layout), Inferred (semantics)

The world model is one object whose stream is a sequence of **cells**:
`0x00` header → [`0x08` hull] → `0x44` vertices → polygon groups → next `0x00` …

`0x00`/`0x04` cell header (16 bytes):

| off | type | field |
|-----|------|-------|
| 0 | u16 | number of primitive records in the cell **including this header** (grouped records count individually) |
| 2 | u8, u8 | flags (0), type |
| 4 | u32 | cell size in 4-byte words from this header (skip distance) |
| 8 | u16[4] | object-table vertex indices of a bounding quad |

Renderer [WR]: if all 4 transformed corners are outside the same frustum plane (near/far/left/right/top/bottom)
the whole cell is skipped (`p += size*4`, `n_prim -= count`), else continue with the next record.
Invariant holds for 1486/1486 cell headers in LiT [DATA].

`0x08`/`0x0c` hull (56 bytes): u16 `count` (records covered, incl. itself), u8 flags, u8 type, u16 `size`
(words), u16 `n_faces` (always 6 [DATA]), then 24 × u16 object-table indices = 6 quads (first two are opposite
faces: corners 0..7). Same reject test over the `n_faces*4` corners; invariant holds for 6782/6782 [DATA].
Purpose beyond culling (collision?) is Research Required.

`0x10` portal (20 bytes): u16 i0..i3 @8..14 (object-table indices), u16 `zone id` @16, u16 pad. The renderer
accumulates the portal's clamped screen rectangle per zone id (`DAT_004b3460` table, ≤ `DAT_004b39dc` zones)
— zone/portal visibility system. Inferred.

**Collision:** no collision-specific data has been identified in section 4. Hulls/portals are only used for
visibility in the renderer. Research Required (look for users of the world model other than [WR], e.g. the
player ground probe; BBLIT-Remaster `YG*` sites `0x410070`, `0x4100ba`, `0x411c7d` read a ground height
`DAT_004efb4e` — start there).

## 8. Sprites (type 0x64) — Inferred

16-byte grouped records: header (count, flags, 0x64), u16 @4 vertex index (centre, Inferred), u16 @6 tpage
(ABR byte, [WALK]), u16 @8 unknown, u16 @10 **texture id** [WALK], s16 @12 width, s16 @14 height (L_* loading
screens: 512 × 512 sprite showing the 512 × 512 TIM; Inferred). Not drawn by [WR]; type-4 actors draw them via
0x44c170 / 0x438280 / GL 0x40a6c0 (whole texture, record 0's size ×2 as a billboard; RUNTIME_REFERENCE.md §20).

## 9. Validation performed

- L01a: 5 world chunks → 3 323 polygons (2 335 `0x40` quads, 954 `0x3c` tris, 33 `0x4e`, 1 `0x4a`), 22 cells,
  4 832 block vertices; bounds above; top-down wireframe shows connected rooms, a rope bridge and tree stumps;
  textured top/front renders confirm UVs, texture ids, quad order and Y orientation.
- All 167 LiT BZE files: every referenced model parses with the stride table, all cell/hull size invariants hold,
  no out-of-range vertex index; 79 levels with world geometry, 168 963 world polygons total.

## 10. TB (`bblit2.exe`) model formats

TB does **not** use the LiT PC-TMD (§2–§8). It has three section-4 model formats, chosen by the section-1 tag
that references them (`docs/formats/TB_LEVELS.md` §3.4/§3.5 for the tags, offsets and placement). Prototype:
`tools/re/tb_world.py` (`--check DIR` asserts every invariant below over all 58 levels).

Evidence keys (TB, decompile `/mnt/1st4TB/BugsBunny/ghidra/diff/out/bblit2.exe.c`, VAs):
`[TL]` loader `FUN_004347c0`; `[OT]` `FUN_00404ed0`/`FUN_004050e0` octree walk; `[RB]` `FUN_00404f40` +
`FUN_004055c0` runtime build of a world model (prims sorted into 4 lists); `[CULL]` `FUN_004052b0` +
`FUN_00409950` node box test; `[DRAW]` `FUN_00405470` leaf draw; `[PV]` `FUN_00405c90` CheckPrimValidity;
`[GLQ]`/`[GLT]` GL quad/tri setup `FUN_0041f420`/`FUN_0041f760` (renderer vtable `FUN_0041ee30`); `[SWQ]`
`FUN_00421140` + `FUN_00420bb0` software quad setup; `[COL]` `FUN_0041f190`/`FUN_0041f300` colour readers;
`[AW]` `FUN_00406f70`/`FUN_00406eb0`/`FUN_00406cd0` actor relocate/mark/draw; `[AQ]`/`[AT]` GL actor quad/tri
`FUN_0041fa40`/`FUN_0041fd80`; `[AV]` `FUN_00406d90`.. `FUN_00406e80` per-object vertex loops; `[SP]` `FUN_004084b0`;
`[DATA]` `tools/re/tb_world.py --check` over all 58 TB files.

Common to all three: vertices are **s16** `x, y, z` + s16 pad (always 0 [DATA], 178 201/178 201 world verts),
8 bytes each, PS1 axes (+Y down, §3). UV bytes are normalised over the whole TIM: float table `i × (1/255)`
(`DAT_004d1e84/88`, constant f32 `0x481660` = 0.0039215689) → same convention as LiT §5. Colour bytes go through
the same table (`DAT_004d1e8c`), i.e. colour = byte/255 in the GL path.

### 10.1 Octree model (world: chunk 0x20 tag 0x24; sky dome: chunk 0x22 tag 0x24) — Verified (data + visual)

Header (24 bytes) [OT][RB]:

| off | type | field |
|---|---|---|
| 0x00 | u32 | `altTree`: offset − 0x18 of tree B root (0 = none). Walked by [OT]; [RB] list `+0x40` |
| 0x04 | u32 | `detailTree`: offset − 0x18 of tree C root (0 = none). [RB] builds it only when `DETAILS` config `DAT_00639b58` ≤ 1 |
| 0x08 | u32 | `uvTable`: offset of the UV table (0 iff every prim has b1 bit 6 [DATA]) |
| 0x0c | u32 | `extra`: offset of an extra block, [RB] stores `model+extra` at runtime `+0x44` (non-zero in 31/182 models). Research Required |
| 0x10 | u32 | unknown (e.g. `0x01580654`); not read by [TL]/[OT]/[RB] |
| 0x14 | u32 | unknown (e.g. `0x1d58`); not read by [TL]/[OT]/[RB] |
| 0x18 | node | tree A root (main) |

Block order in the file [DATA]: tree A, UV table, [extra], [tree B], [tree C]; tree A's last leaf ends exactly at
`uvTable` when present; the UV table is `12 × n` bytes (all `+0x1a` offsets are multiples of 12 and in range,
138 341/138 341).

Node (all trees) [OT]:

| off | type | field |
|---|---|---|
| 0x00 | u16 | unknown (root `0xffff`; others 0/8/16/24/…). Not read by [OT]. Research Required |
| 0x02 | u8 | kind: 0 = leaf, 1 = inner; anything else is ignored by [OT] (never occurs) |
| 0x03 | u8 | 0 |
| 0x04 | s16 ×3 | box corner `x0, y0, z0` |
| 0x0a | u16 ×3 | box size `sx, sy, sz`; box = `[x0, x0+sx] × [y0−sy, y0] × [z0, z0+sz]` ([CULL] builds the 8 corners this way; every leaf vertex lies inside its leaf box, 178 201/178 201 [DATA]) |
| 0x10 | u16 ×8 | inner only: child offsets; child = **tree root** + `u16 × 4` (the base is the root of the tree, not the current node); 0 = empty |
| 0x10 | u16 | leaf only: `nv` vertex count (max 214 [DATA]; prim indices are u8) |
| 0x12 | u16 | leaf only: `np` primitive count |
| 0x14 | vertex[nv] | leaf only: 8-byte vertices (world coordinates; leaf-local index space) |
| 0x14+8·nv | prim[np] | leaf only: 28-byte primitives |

Max tree depth 5 [DATA]. Draw order inside a leaf [RB][DRAW]: opaque quads, opaque tris, semi-transparent quads,
semi-transparent tris (each list in file order). Visibility: a node is visited only if its box passes the 6-plane
test [CULL]; there are no portals/cells in TB.

World primitive, 28 bytes [GLQ][GLT][SWQ][COL][PV]:

| off | type | field |
|---|---|---|
| 0x00 | u8 | `b0`: bit 3 = quad (4 indices) else tri; bit 4 = Gouraud (4 colours) else flat (colour 0 only); bit 1 = semi-transparent, ABR = `(uv[6] >> 5) & 3`, else opaque (mode 4); bits 2 and 5 must be set, bit 6 clear [PV] |
| 0x01 | u8 | `b1`: bit 6 = no UVs (fixed per-vertex UVs below; also invalid together with b0 bit 1) ; bit 5 = flag `|1` at render `+0x67`: a quad whose first triangle faces away is still drawn if its second triangle (i1, i2, i3) faces the camera (`FUN_0041eee0`; tris with it are invalid [PV]); bit 4 = flag `0x80` at render `+0x67`: back-face test skipped, i.e. two-sided (`FUN_0041eee0`/`FUN_0041f080`); bits 0–3 Research Required (values 0,1,4,5,8,9,12,13) |
| 0x02 | u16 | texture id = low 12 bits (`& 0xfff`, the id of chunk 0x28 tag 0x29); high nibble is 7 in 138 341/138 341 [DATA], ignored |
| 0x04 | u8 ×4 | colour 0: r, g, b, pad (pad = PS1 code byte, ignored, not always 0) |
| 0x08, 0x0c, 0x10 | u8 ×4 | colours 1, 2, 3 (Gouraud only; flat prims use colour 0 for all vertices) |
| 0x14 | u8 ×4 | vertex indices i0..i3 (leaf-local; tris use i0..i2, `+0x17` is junk) |
| 0x18 | u16 | copy of `+0x1a` (equal in 138 341/138 341 [DATA]; unused by the renderers) |
| 0x1a | u16 | byte offset into the UV table |

UV table entry (12 bytes, PS1 `POLY_FT4` UV/CLUT/TPAGE layout): `u0 v0` @0, clut @2 (ignored), `u1 v1` @4,
`tpage` @6 (only bits 5–6 = ABR used), `u2 v2` @8, `u3 v3` @10. Vertex i0 ← uv0, i1 ← uv1, i2 ← uv2, i3 ← uv3.

No-UV prims (b1 bit 6) [GLQ][SWQ]: i0 (0,0), i1 (1,0), i2 (1,1), i3 (0,1) — note this is **not** the LiT §5
implicit mapping, and with Z-order quads it gives a crossed mapping; it is used with 4×4 single-colour textures
(e.g. ARCA01 id 347) so only the vertex colour is visible.

**Quad order: PS1 Z order** (outline i0 → i1 → i3 → i2): the software path puts i3 in slot 2 and i2 in slot 3
[SWQ]; in data 2 755/2 759 ARCA01 quads are convex only in Z order (4 degenerate) [DATA]. Visually verified.

Trees: A = main textured world; B (`altTree`) = alpha-tested overlays (ivy, foliage; b1 = `0x18/0x1c/0x2c/…`)
(A/B semantics Inferred from renders); C (`detailTree`) = untextured vertex-coloured shell (b1 bit 6, 4×4
texture), a **far LOD** — Verified (code): the world draw `FUN_00406840` walks A over the near range
`[0, near]`, B over `[0, far]`, and C only over `[near, far]` and only if `near ≠ far`; `FUN_00409840(p1, far)`
sets near = p1, (p1+far)/2 or far for `DETAILS` (`DAT_00639b58`) 0/1/2, and [RB] builds C only for
`DETAILS ≤ 1`. At `DETAILS` 2 C is never drawn (Inferred: the highest-detail setting). `bb::buildTbScene` draws A + B and only validates C.
Drawn together with A, C covers textured walls (visual check, TRCA01).

Sky domes are owned by class-9 entities (chunk 0x63, tag `0f` = the dome's asset id, 54/59; tag `10` position,
e.g. TRCA01 (0, −15500, 5), yaw 180°): `FUN_0044d6f0` builds the entity's octree and forces its scale to 1.
Whether the dome follows the camera at run time is Research Required.

Invalid prims ([PV] fails → logged `CheckPrimValidity failed` and skipped): 48 in all data, dropped by the exporter.

Chunk 0x22 tag 0x24 models in this format (one per level, 59 total; detected by `(u32 @+0x18 & 0xffffff) ==
0x1ffff`, i.e. root node `{u16 0xffff, kind 1}`) [TL] are untextured spheres of radius ≈ 32 000 (ARCA01
`0x3e494`) → sky domes (Inferred). [TL] stores them in `DAT_004d03d4[]` / sizes in `DAT_00639d60[]`.

### 10.2 Actor format (chunk 0x22 tag 0x24, non-octree) — Verified (layout, data), un-posed

[TL] relocates it in place [AW] and stores `model + 8` as the asset's data pointer.

| off | type | field |
|---|---|---|
| 0x00 | u16 | 0 or 1 (2465 × 1, 352 × 0); not read. Research Required |
| 0x02 | u16 | `nobj` objects (0 → 8-byte model `01 00 00 00 00 00 00 00`, no geometry: 259 models) |
| 0x04 | u32 | offset of the primitive block (from model start; relocated to a pointer) |
| 0x08 | `{u32 vtxOff, u32 nv}`[nobj] | per object: vertex table at `model + 8 + vtxOff` (relocated), `nv` 8-byte vertices |

Primitive block [AW] (`FUN_00406cd0` draws, `FUN_00406eb0` marks texture ABR use):

| off | type | field |
|---|---|---|
| 0x00 | u16 | `nC`: tris, 0x28-byte records at block + `offC` |
| 0x02 | u16 | `nD`: quads, 0x28-byte records at block + `offD` |
| 0x04 | u8 | `nA`: tris, 0x24-byte records at block + 0x0c |
| 0x05 | u8 | `nB`: quads, 0x24-byte records at block + `offB` |
| 0x06, 0x08, 0x0a | u16 | `offB`, `offC`, `offD` |

Invariants (2558/2558 non-empty models [DATA]): `offB = 0xc + 0x24·nA`, `offC = offB + 0x24·nB`,
`offD = offC + 0x28·nC`; the object vertex tables are contiguous and start right after the D records; the model
ends exactly after the last vertex. Tri/quad dispatch: `DAT_0063bc28` = tri ([AT]), `DAT_0063bc40` = quad ([AQ]).

Actor primitive (0x24 or 0x28 bytes) [AQ][AT][COL]: `+0` b0, `+1` b1, `+2` tex, `+4..+0x13` colours —
identical meaning to the world prim; then an **inline** UV block `u0 v0` @0x14, clut @0x16, `u1 v1` @0x18,
tpage @0x1a (ABR bits 5–6), `u2 v2` @0x1c, `u3 v3` @0x1e; indices @0x20: u8 ×4 when b1 bit 7 = 0 (0x24-byte
records), u16 ×4 (`+0x20..+0x27`) when b1 bit 7 = 1 (0x28-byte records; bit 7 matches the group 100 %).
b0 seen: `0x34/0x3c` opaque tri/quad, `0x36/0x3e` semi-transparent [DATA].

Indices address the **concatenation of all objects' vertex tables** (max index < total in 2558/2558). At run
time each object's vertices are transformed in order into the 16-byte screen buffer `DAT_004d0418` with that
object's matrix [AV], so multi-object models are skinned per object (bone). The bone matrices come from the
animation assets (chunk 0x22 tag 0x25, §11) — the exporter therefore writes multi-object models **un-posed**
(objects overlap). Single-object models (1525 in all levels) export correctly: ARCA01 carrot `a1_s4_026af8`,
coin `a1_s4_0271e0` (textures and UVs visually correct).

Quirk: [AW] `FUN_00406eb0` loops `nobj` times over the same primitive block (its pointer is never advanced;
checked in the disassembly at 0x406eb0) — harmless (texture-usage marking only).

### 10.3 Sprite-sequence format (chunk 0x22 tag 0x3f, asset kind 0x100) — Layout verified, semantics Inferred

Same 8-byte header + object table as §10.2 (always `nobj = 1`), but `8 + vtxOff == model size` (no vertex data)
and the primitive block holds only `nD` 16-byte LiT-style `0x64` records starting at block + 0xc
(`count` u16 @0 — first record holds the group size, then decreasing —, flags u8 @2, type `0x64` @3) [SP];
record count == `nv` == `nD` in 714/714 [DATA]. [SP] reads flags bit 3 (semi-transparent, ABR from byte @6) and
the texture id u16 @10. Inferred (tex ids are consecutive, e.g. 0x1f,0x20,0x21…): one record = one animation
frame, `s16 x, y, z` @4 (offset), `u16 w, h` @12/@14 sprite size.

### 10.4 Validation performed (TB)

- `tb_world.py --check` over all 58 files: 123 world models (58/58 levels), 59 sky domes, 2817 actor-format
  models, 714 sprite sequences; no out-of-range index, all layout invariants above hold, chunk 0x20 rotation is
  identity everywhere.
- World totals: 6 537 leaves, 178 201 vertices, 115 250 valid prims (92 503 quads, 22 747 tris; tree A 94 557,
  B 1 374, C 19 319); 1 558 prims (1.35 %) reference texture ids that no chunk 0x28 loads (mostly b0 `0x3e`
  semi-transparent) — Research Required (TEXTURES.md §8 has the LiT analogue).
- Visual: textured top + front renders (`render_top.py`, vertex colours not applied) show the Camelot castle
  (ARCA01: towers with conical roofs, banners, ivy walls, portcullis), the Aztec plaza with a stepped pyramid and
  fret borders (AZCA01), a Transylvanian village with spired houses and a castle (TRCA01) and a snowy rock
  valley with grass-topped cliffs and huts (VICA01). PNGs: `/mnt/1st4TB/BugsBunny/local/re/tb/<LEVEL>/{top,front}.png`.

## 11. Unverified / open

- Meaning of flags bit 0; types `0x14`–`0x30`, `0x48`, `0x4c` (no data).
- Chunk `0x22` tags `0x25` (animation/skinned blocks), `0x3f`, `0x40` vs `0x24`, chunk `0x36`.
- Multi-object actor transforms (bones), animation data.
- Exact near/far constants of the cell test (`DAT_004efb38`, `DAT_004efb48`: runtime values).
- Collision data (§7); sprite record (§8).
- GL emitters (`LAB_00414590`, `LAB_00414b40`) not decompiled; layouts verified through the software emitters
  and the shared descriptor tables.
- TB: chunk 0x22 tag `0x25` animation data (13 905 assets; [TL] keeps `data[2] >> 1` → `+0xb`, u16 `data[4]` →
  `+8`, `data[0xc]` → `+0xa`, pointer `data + 8` → `+0xc` of a 0x10-byte struct, asset kind 4) — needed to pose
  §10.2 multi-object actors; tag `0x3e` blocks (kind 1, 351 assets).
- TB octree: node `+0x00` u16, header words `+0x0c` (extra block: starts with a u32 count then records such as
  `08 02 00 06 …` holding s16 coordinates — collision/trigger shapes?), `+0x10`, `+0x14`; prim `b1` bits 0–5;
  missing texture ids (§10.4).
- TB collision: no collision structure identified yet; candidates are the octree boxes, the `extra` block and
  world-prim `b1` low bits.
