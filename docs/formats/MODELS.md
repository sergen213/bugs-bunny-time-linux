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

Object vertex table: `n_vert` × 16 bytes = `float x, float y, float z, u32 pad(0)`. Values are integral
(converted from PS1 s16). The renderer transforms them into a 16-byte screen-space buffer [OV].

Multi-object (actor) models: object vertex tables are **contiguous** and primitive vertex indices address the
**concatenation** of all objects' vertex tables (e.g. L01a model `0xb854`: object 2 uses indices 24–26 with
object 0 having 11 and object 1 having 7 vertices) [DATA]. Whether objects get separate matrices (skinning)
is Research Required (tag `0x25` data likely animates them).

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

**Backface rule [BF]:** with transformed screen vertices `a=v[slot0], b=v[slot1], c=v[slot2]` (slot = index
descriptor order: 0x34 → i0,i1,i2; quads → i0,i1,i2), cull when
`(c.y − b.y)*(a.x − b.x) − (c.x − b.x)*(a.y − b.y) < 0` (`_DAT_0045c308` = 0.0f, read from the exe) in the
projected float buffer `x' = f·x/z, y' = f·y/z` **before** the y flip (`y_screen = centre − y'` is only applied
to the integer copy) [BV], i.e. PS1 camera space with +Y down. Winding convention of the data therefore = PS1 GTE NCLIP
convention; a new renderer should take the sign from this formula, not from OBJ conventions.

**UVs:** `u, v` bytes are **normalised to the whole TIM**: texel = `(u/255)·(w−1)`, `(v/255)·(h−1)`
(tables `0x467f50`/`0x468350` = i/255, multiplied by `texW−1`/`texH−1` in [EM]). Not PS1 texture-page
coordinates. `v = 0` is the first (top) image row. OBJ export uses `vt = (u/255, 1 − v/255)`.

**Implicit UVs (0x4a / 0x4e, and any poly emitted with a null UV table) [EM]:** tri: i0 (0,1), i1 (1,1),
i2 (0,0); quad: i0 (0,1), i1 (1,1), i2 (1,0), i3 (0,0) in normalised (u,v). Used with small single-colour
textures (L01a ids 386–401), so the mapping is visually irrelevant.

**Vertex colours:** `r,g,b` per vertex (Gouraud) or one per polygon (flat). PS1 semantics: texel × colour / 128
(0x80 = neutral, saturating). The PC port divides by 255 instead (world renders half as bright as PS1 —
BBLIT-Remaster `render.c` comment at ~line 1911). Typical world values 0x40–0xC0. A faithful renderer should
pick one convention explicitly (PS1 /128 recommended; document the choice).

## 6. Vertex addressing — Verified

- At object start, the object vertex table (concatenated over all objects, §2) is the current vertex base
  for polygons **and** the base for cell/hull/portal indices (`DAT_007c6240`) [OV].
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
screens: 512 × 512 sprite showing the 512 × 512 TIM; Inferred). Not drawn by [WR]; the sprite renderer was not
located.

## 9. Validation performed

- L01a: 5 world chunks → 3 323 polygons (2 335 `0x40` quads, 954 `0x3c` tris, 33 `0x4e`, 1 `0x4a`), 22 cells,
  4 832 block vertices; bounds above; top-down wireframe shows connected rooms, a rope bridge and tree stumps;
  textured top/front renders confirm UVs, texture ids, quad order and Y orientation.
- All 167 LiT BZE files: every referenced model parses with the stride table, all cell/hull size invariants hold,
  no out-of-range vertex index; 79 levels with world geometry, 168 963 world polygons total.

## 10. TB applicability — Research Required

TB (`bblit2.exe`) uses a **different model format**. Its chunk `0x20`/`0x22` tag `0x24` handlers call
`FUN_004067f0 → FUN_00404ed0`, which walks a node tree: header words `[0]`, `[1]`, `[2]` are offsets, nodes
start at `+0x18`, node byte `+2` = 0 → leaf (drawn via a function pointer), 1 → inner node with eight u16 child
offsets at `+0x10..+0x1f` (×4 bytes) — an **octree**. Actor models are distinguished by
`(u32 @+0x18 & 0xffffff) == 0x1ffff` (`FUN_004347c0` chunk `0x22`) and otherwise go through
`FUN_00406f70`/`FUN_00406eb0`. Example: ARCA01 world model at section-4 `0xb04` (+0x1f7a8). TB textures are
compatible (TEXTURES.md). The LiT primitive/record layouts above must **not** be assumed for TB.

## 11. Unverified / open

- Meaning of flags bit 0; types `0x14`–`0x30`, `0x48`, `0x4c` (no data).
- Chunk `0x22` tags `0x25` (animation/skinned blocks), `0x3f`, `0x40` vs `0x24`, chunk `0x36`.
- Multi-object actor transforms (bones), animation data.
- Exact near/far constants of the cell test (`DAT_004efb38`, `DAT_004efb48`: runtime values).
- Collision data (§7); sprite record (§8).
- GL emitters (`LAB_00414590`, `LAB_00414b40`) not decompiled; layouts verified through the software emitters
  and the shared descriptor tables.
