# Texture gaps: face texture ids with no TIM in the level — LiT + TB

**Status: Verified (code + data, both games). Frame timing: type-20 hold byte and type-2 tag-0x3f flipbook lists
Verified against `bugs.exe` alias-table traces (L04C3 links 9/11, L02A1 links 16/63; GAMEPLAY_STATUS.md "Texture
links"); only script-driven links without a tag-0x3f list (cycled in order, 2 ticks per frame) remain Inferred.**
Tool: `tools/re/texgap.py` (per-level table, `--check`, contact sheets). Evidence PNGs:
`/mnt/1st4TB/BugsBunny/local/re/texgap/`.

## 1. Rule

A face texture id that is absent from the level's TIM list (LiT chunk `0x29`, TB chunk `0x28`) is a **texture
link**: a slot in the id → texture alias table that a level entity fills at run time with the current frame of
its sprite sequence. Every such id is also a gap in the TIM id numbering (ids are allocated per level, the link
slots simply have no TIM).

| | LiT `bugs.exe` | TB `bblit2.exe` |
|---|---|---|
| link declaration | entity chunk 7, tag `0x0b` = `{u16 link_id, u16 0}` → actor `+0x170`; loader case `0x0b` calls `FUN_004247a0(asset+0x54, link_id)` | same tag, actor `+0x168`; `FUN_00408410` |
| frame source | the latest tag-`0x0f` asset of kind `0x100` (chunk `0x22` tag `0x40` sprite sequence): its `0x64` records, tex id u16 `@+10` | chunk `0x22` tag `0x3f` sprite sequence (MODELS.md §10.3), tex id `@+10 & 0xfff` |
| alias table | `DAT_004e5e80[2000]` (u32), zeroed per level by `FUN_004474b0`; upload sets `[id] = id` | `DAT_004d038c[1000]` (u16), zeroed by `FUN_004074a0` / `FUN_00459de0`; upload sets `[id] = id` |
| frame write | `FUN_00438830`: `alias[part+0x14 = actor+0x170] = record+10` | `FUN_00465c80`: `alias[part+0x10 = actor+0x168] = record+10` |
| also re-applied | actor render loop `FUN_0044c4b0`: part flag `0x20` → `alias[part+0x14] = part+0x12` | — |
| emitters | look up `alias[prim tex id]` (`bugs.exe.c` 12089, 12244, 12654, 12826 in `ghidra/diff/out`) | look up `alias[id]`, prim **skipped when 0** (`bblit2.exe.c` 20895, 20986, 21621) |
| unresolved id | alias 0 → bucket 0, which the flush loop never draws (it iterates ids 1..max) | alias 0 → skipped |

So in the original, a gap id is drawn with whatever frame its entity last wrote, and a gap id with no link
entity (or before the entity first runs) is **not drawn at all** — not untextured.

Entities (all chunk 7, tag `0x0b` high u16 and tag `0x0c` always 0 — [DATA]):

| behaviour type (tag `0x13`) | LiT | TB | frame selection |
|---|---|---|---|
| 20 looping flipbook (LiT `FUN_00445500`, TB `FUN_00453d00`) | 129 | 64 | records of the first asset in the actor asset list whose class (chunk `0x22` tag `0x27` u16[0]) is 8 (TB: asset kind `0x17`); start frame = actor `+0x50` (tag `0x42` u16[0]; TB `+0x68`), advance every `+0x54` byte ticks (tag `0x42` u16[2]; TB `+0x6c`), wrap after the last record — Verified (code) |
| 2 script graph (`FUN_004455e0` sets flags8 `0x20`, so `FUN_00438770` routes to `FUN_00438830`) | 391 | 140 | frame from the current script animation (`FUN_00438970`, LOGIC.md); per-entity record order is baked into the asset (e.g. L02A6 lava: 18 entities, 10 frames each, rotated order = flowing phase) — that the type-2 anim struct also carries the link at `+0x2c` and the frame timing are Inferred |

ABR variants (PC port only): `FUN_004247a0` chains every frame texture to its link id (`0x52fd72` link,
`0x52fd84 |= 0x20`); the queue processor `FUN_00422a40` then ORs the link's variant mask (marked by
`FUN_00423eb0` from the faces that use the link id) into each frame, so frames get the semi-transparent variants
the linked faces need. A shader-blending renderer can ignore this.

Rejected hypotheses (checked): VRAM/texture persistence across level loads (`FUN_004474b0` deletes all GL
textures and zeroes the alias table before every level load; the loading-screen file loaded first only uploads
id 1 → slot `0x7cb`); shared/global texture files (the loader opens one BZE per load, `FUN_0042e7a0`; `title`,
`Loading`, `L_*` files carry their own lists); runtime-generated textures (the only other `glTexImage2D` path,
`FUN_00413e40`, is installed in `DAT_004b1a88` and never called); flag bits in the id (ids are plain u16, every
gap id has a link entity); per-level `.bmp` files (256×1280 8-bit palette/blend tables for the software renderer).

## 2. Validation

- `texgap.py lit original/lit/Datas/bze --check` and `texgap.py tb original/tb/Datas/BZE --check`: every
  missing **world** face id resolves to a link whose frames are all loaded TIMs.
- Contact sheets (`<level>_links.png`) show the expected content: L04C3 link 9 = conveyor planks (4 frames),
  11 = rollers (3), 36/38 = Bugs' eyes (blink, 17/18 frames), 280 = electric sparks; L02A1 16/63 = additive water
  shimmer lines (frame 0 of 63 is empty → nothing added); L03A 353 = expanding water ripple rings, 336/204/272 =
  shimmer lines, 202/203 = waves, 3–7 = yellow glow; L02A6 40–57 = lava; L04C1 319–323 = fire/smoke;
  TB TRR01 214 = river shimmer lines (871 faces), 260–262 = coin gauges (collected / empty).
- `L04C3_tex_resolved.png` (textured top view, link → start frame) shows two plank conveyor belts between the
  tiled platforms; `L04C3_top.png` marks the same 176 faces in red (unresolved), `L02A1_tex_resolved.png` shows
  shimmer lines along the shore.
- The faces previously rendered as "grey sails" in L03A are the ripple quads (link 353, ABR additive, full 0–255
  UVs) and the "white additive water" in L02A1 is links 16/63.

## 3. Counts (texgap.py; world = chunk `0x20`, actor = chunk `0x22` tag `0x24` models)

LiT, all 167 files: world **5 271 / 5 271** resolved, actor 2 735 / 3 070 (levels with world gaps shown first).

| level | world missing → resolved | actor missing → resolved | unresolved actor ids |
|---|---|---|---|
| CC2A | 162 → 162 | 7 → 7 | |
| CC4C | 209 → 209 | 4 → 4 | |
| CC4D | 4 → 4 | 9 → 9 | |
| CCMerlin | 103 → 103 | 8 → 8 | |
| L02A1 | 142 → 142 | 98 → 98 | |
| L02A4 | 4 → 4 | 18 → 18 | |
| L02A5 | 48 → 48 | 86 → 86 | |
| L02A6 | 575 → 575 | 8 → 8 | |
| L02B1 | 77 → 77 | 56 → 56 | |
| L02B2 | 81 → 81 | 72 → 72 | |
| L02C4 | 55 → 55 | 84 → 76 | 119 |
| L03A | 242 → 242 | 72 → 72 | |
| L03A2 | 431 → 431 | 76 → 76 | |
| L03ACOM | 56 → 56 | 99 → 99 | |
| L03C | 91 → 91 | 122 → 122 | |
| L03C1 | 43 → 43 | 52 → 4 | 285–287 |
| L03D1 | 504 → 504 | 35 → 35 | |
| L03a2_8 / L03a_8 / L03acom_8 / L03c_8 | 363 / 96 / 48 / 91 → all | 76 / 72 / 99 / 122 → all | |
| L04B2 | 16 → 16 | 26 → 26 | |
| L04C1 | 646 → 646 | 82 → 82 | |
| L04C2 | 234 → 234 | 29 → 29 | |
| L04C3 | 176 → 176 | 5 → 5 | |
| L04E2 | 66 → 66 | 5 → 5 | |
| L05A1 | 88 → 88 | 25 → 25 | |
| L05A2 | 198 → 198 | 68 → 68 | |
| L05A4 | 222 → 222 | 18 → 18 | |
| LS01 | 56 → 56 | 52 → 52 | |
| Merlin | 103 → 103 | 88 → 88 | |
| credits | 19 → 19 | 88 → 36 | 5, 7, 172–174 |
| l04e | 22 → 22 | 75 → 27 | 195–197 |
| 45 other files | 0 | all resolved except CC4B (27 → 23, id 63), CCIntro (39 → 20, 66/72/80), L02C3 (36 → 28, 276), L03B / L03b_8 / L03C2 (52 → 4, 294–296 / 259–261), title (23 → 19, 222) | |

TB, all 58 files: world **1 558 / 1 558** resolved (ARBOSS01 31, ARCA04 44, ARQ01 7, ARQ02 32, ARR01 3, AZBOSS02 6,
AZCASG05 58, AZQ01 96, CCEND 78, CCEraCut 26, CCINTRO 78, Granny 26, TRCA01 31, TRQ01 14, TRQ02 17, TRR01 854,
VICA01 44, VICASG04 28, VIQ01 52, VIQ02 30, VIR01 3), actor 1 064 / 1 113; unresolved actor ids: ARBOSS01 287,
ARCA03 262–263, ARCASG02 203–206, ARCASG04 208, AZCASG03 202–203, AZQ01 195, TRCA02 192–194, TRR01 191,
VICASG01 248–249, azcasg01 254–255.

Unresolved actor ids (residual, all in chunk-`0x22` models): the sampled cases sit in assets that no entity
references (L03B asset 282, l04e 42, L02C4 187, credits 25 — never drawn) or in referenced models whose file
has no link entity for that id (credits asset 1 ids 5/7 = Bugs' eye slots, CC4B 63, CCIntro 66/72/80, title 222).
By the emitter rule above those faces are not drawn in the original. (The ROADMAP's 5 567 / 1 731 face totals
come from the C++ loaders' own world+prop counting; this table counts every file and model.)

## 4. Reimplementation contract

1. Parse chunk 7/8/10 entities: remember the last tag-`0x0f` asset that has a sprite sequence (LiT tag `0x40`,
   TB tag `0x3f`); on tag `0x0b` register `link_id → frames[]` (record tex ids).
2. Keep a per-level alias table `alias[id]`: `alias[id] = id` for loaded TIMs, `0` otherwise.
3. Each tick, link entities write `alias[link_id] = frames[cur]` (type 20: start/hold/wrap from §1; type 2: script
   animation, fallback = cycle the records in order). Resolve face texture ids through `alias` at draw time.
4. Faces whose resolved id is 0 are not drawn (do not fall back to vertex colour).
