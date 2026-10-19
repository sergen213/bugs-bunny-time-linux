# Textures (BZE section 3) — LiT + TB

Container/LZSS: see `BZE.md`. This file covers the texture payloads inside the (decompressed) sections and
how the PC port turns them into GPU textures.

Status legend: **Verified** = checked against code *and* data; **Verified (data)** = holds on every file
tested, code not fully read; **Inferred** = consistent with evidence, not proven; **Research Required**.

Evidence keys
- `[LL]` LiT `bugs.exe` v1.6 (md5 121db63d…) level loader `FUN_0042e7a0` (Ghidra decompile:
  `/mnt/1st4TB/BugsBunny/ghidra/assets/bugs_all.c`, project `ghidra/assets/bugs`).
- `[Q]` `FUN_004229a0` texture upload queue; `[UP]` `FUN_00422a40` queue processor / ABR variant builder;
  `[CV]` `FUN_00423490` size/mip loop; `[GL]` `FUN_00420d10` 15-bit → RGBA conversion; `[TIM]` `FUN_0040cda0`
  TIM header parser (PsyQ `GetTimInfo`-like).
- `[WALK]` `FUN_00423eb0` model walker that marks which ABR variants each texture needs (see MODELS.md).
- `[TB]` TB `bblit2.exe` loader `FUN_004347c0`, queue `FUN_00407f60`
  (`/mnt/1st4TB/BugsBunny/ghidra/diff/out/bblit2.exe.c`, analysis by peer GhidraEngineDiff).
- `[DATA]` prototype `tools/re/export_level.py` over all 167 LiT BZE files and TB `ARCA01.bze`.

## 1. Where textures are referenced — Verified

Textures are listed by a section-1 chunk; each entry points into **section 3** (the first section loaded after
the pseudo-chunk, i.e. the active asset section when this chunk is parsed [LL]).

| game | chunk type | tag | payload (little-endian) |
|------|-----------|-----|-------------------------|
| LiT  | `0x29` | `0x2a` (8 bytes) | `u32 offset` into section 3, `u32 texture_id` |
| TB   | `0x28` | `0x29` (8 bytes) | `u32 offset` into section 3, `u32 packed` = `(byte_size << 12) \| texture_id` |

- LiT: `case ')'` in `[LL]` → `FUN_004229a0(sec3 + offset, texture_id)` then `FUN_0040cda0` [Q].
  Typical level: one chunk with 300–440 entries; ids start at 1, ascending, gaps allowed (L01a: 421 entries,
  ids 1..425).
- TB: `case 0x28` / tag `0x29` in `[TB]` → `FUN_00407f60(sec3 + offset, packed)`: stores `packed` in the queue
  and copies `packed >> 12` bytes. `byte_size` equals the TIM size for 367/371 entries in ARCA01 [DATA];
  the 4 mismatches are unexplained. Id = low 12 bits: **Inferred** (consistent: ids 1..N ascending).
  TB section-1 delimiters differ from LiT (chunk start `0x2c`, end `0x2d`, see BZE.md / GhidraEngineDiff), so the
  byte pattern is `2c 28 (29 <8 bytes>)* 2d`.
- Every LiT offset (all 79 levels with geometry, plus 88 loading-screen files) lands on a TIM header [DATA].

## 2. Payload = standard PlayStation TIM — Verified

All values little-endian. `[TIM]`, `[GL]`, `[DATA]`.

| off | type | field |
|-----|------|-------|
| 0x00 | u32 | id, must be `0x10` (`[GL]` returns 0 otherwise) |
| 0x04 | u32 | flags: bits 0–2 pixel mode (0 = 4-bit CLUT, 1 = 8-bit CLUT, 2 = 15-bit direct, 3 = 24-bit), bit 3 = CLUT block present |
| 0x08 | —   | CLUT block if flags bit 3 (below), then image block |

CLUT block / image block share a header:

| off | type | field |
|-----|------|-------|
| +0x00 | u32 | block byte size including this 12-byte header |
| +0x04 | u16 | VRAM x (PS1 leftover; **ignored** by the PC port) |
| +0x06 | u16 | VRAM y (ignored) |
| +0x08 | u16 | width in 16-bit units (CLUT: colour count; image: 4-bit → ×4 px, 8-bit → ×2 px, 15-bit → ×1 px) |
| +0x0a | u16 | height (rows) |
| +0x0c | u16[] | data: CLUT colours, or packed pixels (4-bit: low nibble = left pixel) |

Observed in LiT (all levels) [DATA]: 4-bit with 16×1 CLUT (the bulk, e.g. L01a 388), 8-bit with 256×1 CLUT
(L01a 33), 15-bit direct only for the single 512×512 loading screen in each `L_*_N.bze`, and exactly one 24-bit
TIM (texture 217 in `L03B`/`L03b_8`) which `[GL]` rejects (`(flags & 7) > 2 → return 0`), so it is never drawn.
TB ARCA01: 348 × 4-bit/16, 23 × 8-bit/256 [DATA].

**CLUT height:** the PC code only uses CLUT row 0 and locates the image block at `0x14 + clut_w*2` [GL]
(i.e. it assumes CLUT height 1). Every CLUT seen has height 1 [DATA]. A C++ loader should still skip
`clut_block_size` and only read row 0.

Texture image dimensions are arbitrary (e.g. 32×32, 64×32, 256×256). VRAM x/y are meaningless on PC.

## 3. Colour conversion and transparency — Verified (code), visually checked

Colour word `c` (u16, PS1 BGR555 + STP bit 15) → RGBA8 [GL]:

```
r = (c & 0x1f) << 3;  g = ((c >> 5) & 0x1f) << 3;  b = ((c >> 10) & 0x1f) << 3;   // no bit replication
if (c == 0x0000)  a = 0, rgb = 0       // fully transparent ("black" = transparent, PS1 rule)
else              a = per-variant rule below
```

`0x8000` (STP set, black) is **opaque black**; the STP bit is otherwise ignored. 15-bit direct TIMs go through
the same rule per pixel.

The port builds up to five GPU variants per texture id, one per PS1 blend mode, because GL fixed function
cannot express PS1 ABR blending directly [UP]. Variant table: base `0x52fd74`, stride 48 bytes per id,
`+4*abr` holds the variant's GL texture id (matches BBLIT-Remaster `render.c` note "`[0x52fd74 + 48·id + 4·abr]`").
`0x52fd84 + 48*id` is a bit mask of variants needed, filled by `[WALK]` from the model primitives
(bit0..3 = ABR 0..3, bit4 = opaque). Per-variant alpha for a non-zero texel with max channel `m = max(r,g,b)`:

| variant | PS1 meaning | alpha (grey texel r=g=b=v) | alpha (coloured) | rgb rewrite |
|---------|-------------|----------------------------|------------------|-------------|
| 4 opaque | no blend | 255 | 255 | unchanged |
| 0 | B/2 + F/2 | v/2 | m/2 | grey → 255; coloured → `rgb + (255 − m)` (mod 256) |
| 1 | B + F | v − v/4 | m − m/4 | same as 0 |
| 2 | B − F | `ftol(float b)` (exact formula not decoded) | same | rgb later zeroed when `DAT_004ac094 ∉ {0,1}` |
| 3 | B + F/4 | v/4 | m/4 | same as 0 |

A renderer reimplementing PS1 blending in a shader does not need these baked variants: keep one RGBA texture
with `a = (c == 0) ? 0 : 255` plus the raw STP bit, and do PS1 ABR blending per primitive (BBLIT-Remaster does
exactly this, `render.c` ~1700–1960). The table above is only needed for bit-exact PC-port output.

Optional gamma/LUT (`DAT_004ac034`, tables `0x4b5b40`/`0x4d5d40`) is applied to rgb after conversion: a
display setting, not data.

## 4. GPU upload — Verified (code)

[CV]: width/height are rounded to a power of two (`pow(2, ceil(log2 n))`, via `FUN_00451310`/`__ftol`),
optionally doubled when `DAT_009ca874` byte 3 is set, clamped to `DAT_004ac024`, and a mip chain is generated
(loop halving until 1). The source TIM is resampled into the POT texture (`FUN_00421c70` when shrinking,
`FUN_00421f50` otherwise). Because UVs are normalised to the *source* image (MODELS.md §6), POT resampling
does not change the mapping. A new engine can upload the TIM at native size (NPOT is fine on GL 4.6/Vulkan).

GL parameters per texture [CV] (`glTexParameterf` through the `DAT_007c62c0` slot): `GL_TEXTURE_WRAP_S`
(0x2802) and `WRAP_T` (0x2803) = `0x46240400` = 10497.0 = `GL_REPEAT`; `MIN_FILTER` (0x2801) and `MAG_FILTER`
(0x2800) = `0x46180400` = 9729.0 = `GL_LINEAR` — Verified. Internal format: `GL_RGB`/3 components when the
texture has no transparent texel, `GL_RGBA`/4 otherwise (`DAT_0052fd69[id*48]`), `GL_UNSIGNED_BYTE`.

## 5. Texture ↔ mesh association — Verified

Textured primitives carry a **u16 texture id at byte +6** of the primitive record (MODELS.md §5). The id is the
`texture_id` from §1 (`DAT_004e5e80[id] = id` after upload [UP]; the emitters read `DAT_004e5e80[tex]`).
Semi-transparency selection comes from the primitive (flag byte bit 3 + tpage byte bits 5–6, MODELS.md §5).

**Missing ids — Verified, see `TEXTURE_GAP.md`:** ids absent from the `0x29` list (33 of 79 LiT levels, e.g.
L04C3 9/11 = conveyor planks/rollers) are texture links filled at run time by entity tag `0x0b` sprite-sequence
frames through the alias table `DAT_004e5e80`; unlinked ids are not drawn.

## 6. Other section-3 / language-section content (for completeness)

- LiT section 3 starts with `u32 n` + `u32 offsets[n]` (L01a n = 53) pointing at **AIFF** sound effects
  (`FORM…AIFF`, COMM: mono, 16-bit, 44100 Hz, MARK/INST loop points) [DATA]. The TIMs follow after the sounds.
  Section-1 chunk `0x00` tag `0x02` = `{u32 0, u32 end_of_sfx_bank}` (L01a `0x172634`), tag `0x04` entries
  `{u16 sfx_id, u8 bank, u8 slot, u32 flags}` look like SFX mapping — **Inferred**, owner: audio work.
- Chunk `0x49` tag `0x4a` (48 bytes) = six `{u32 offset, u32 size}` into section 3 (L01a `0x172634…`):
  per-language **text/glyph tables** (begin with ascending u16 offsets) — **Inferred**.
- Language sections (ids 5–10): `[0, 0x0b84..0x0c84)` = 8-byte records `{u16 id, u16 0x0400, u8, u8 0x40, u8, u8 0}`
  (sound parameters), then a count+offset table of AIFF **speech** clips (chunk `0x00` tags `0x4c`/`0x4d`:
  six `{offset,size}` pairs, one per language section) [DATA]. **No TIMs** in language sections of tested levels.

## 7. Validation performed

- `tools/re/export_level.py original/lit/Datas/bze/L01a.bze …` → 421 PNGs; contact sheet shows Bugs/Daffy faces,
  clock, ACME crate, fonts, smoke puffs, jungle/rock/wood textures — recognisable and correctly coloured.
- Textured top-down + front renders of the L01a world (`render_top.py`) show spiral tree-stump tops, horizontal
  rock strata on canyon walls, rope-bridge planks: texture association + UV convention correct.
- All 167 LiT files export without error. TB: all 58 files, 17 103 TIM references decode (4-bit/16 and
  8-bit/256 only); ARCA01 contact sheet shows coins, ACME crates, Taz/Bugs icons, stone/wood textures.

## 8. Unverified / open

- Variant-2 (subtractive) alpha formula (`ftol` argument not decoded).
- Whether the uploaded mip chain is ever sampled (MIN_FILTER is plain `GL_LINEAR`).
- TB id packing (low 12 bits) and the 4 size mismatches.
- (Missing texture ids: resolved, `TEXTURE_GAP.md`.)
