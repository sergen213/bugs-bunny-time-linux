# Time Busters level script (BZE section 1) — `bblit2.exe`

Container, header and section table: `BZE.md`. Geometry formats referenced from here: `MODELS.md` §10.
Textures: `TEXTURES.md`. Status vocabulary as in `docs/RESEARCH.md`; per item below: **Verified (data)** = the
rule is read from the code *and* holds over all 58 shipped TB files; **Inferred** = interpretation; **RR** =
Research Required. Nothing here is *Verified Against Original Behavior* (static analysis only).

Prototypes: `tools/re/tb_sec1.py` (walker: `--all DIR`, `--players DIR`, or dump one file),
`tools/re/tb_world.py` (geometry export / `--check DIR`), `tools/re/tb_switches.py` (dumps the loader's jump
tables from the exe).

Evidence (VAs in `bblit2.exe` 1.0.3.0, md5 `d7445216…`; decompile
`/mnt/1st4TB/BugsBunny/ghidra/diff/out/bblit2.exe.c`, Ghidra project `/mnt/1st4TB/BugsBunny/ghidra/diff/`):

- `[TL]` level loader `FUN_004347c0` (LiT pair `FUN_0042e7a0`, ENGINE_COMPARISON.md). Interpreter loop at
  0x434d1f.., dispatch sites: chunk type 0x434d3f, chunk 0x22 tags 0x434dad, chunk 0x20 tags 0x43504e,
  chunk 0x00 tags 0x435307, chunk 0x3a tags 0x4356e7, entity tags 0x435a76.
- `[JT]` the byte-index + dword jump tables of those six switches, read directly from the exe by
  `tb_switches.py` (index/jump tables 0x436a98..0x436dae). They prove which tag values have a handler; every
  other value goes to the switch default.
- `[LC]` logic-tag encoder `FUN_00433ab0` (tags 0x4d..0x60), emitting into `FUN_00433900` / `FUN_00433a10`.
- `[DATA]` `tb_sec1.py --all` over `original/tb/Datas/BZE` (58 files).

All integers little-endian. "Payload" = bytes after the tag byte.

## 1. Result

**58/58 TB section-1 streams parse to the last byte** with the tables below, in strict mode (any byte the
game would silently skip raises an error): every stream ends with its `0x2e` end byte exactly at
`actualSize − 1`, with no padding [DATA]. Residual gaps: none in the grammar. Semantics still RR are listed
per tag and in §8.

## 2. Interpreter model — Verified (code)

`[TL]` keeps one byte pointer `p` and a *current data section* pointer `D`. Exact control flow:

```
p = section1
if p[0] == 0x2c and p[1] == 0x44:          # pseudo-chunk (never in shipped data)
    p += 1
    while *p != 0x2d:                       # each 0x44 xx reads (and discards) the next section
        if *p == 0x44: p += 2  else: p += 1
    p += 1
D = workspace + W - 0xA000                  # W = header u32 @4; the next section (ID 3 in all files) is read here
while *p != 0x2e:
    c = *p++
    if   c == 0x2b: MODELS()                # see §4
    elif c == 0x2c:                         # chunk
        while *p != 0x2d:
            t = *p++
            handler[t]()                    # each known handler loops over tags until *p == 0x2d (not consumed)
        p += 1                              # consume 0x2d
    elif c == 0x4a: LANGUAGE()              # see §4
    # any other top-level byte: skipped (1 byte)
```

Inside a known chunk type: `tag = *p++`; a known tag reads its fixed payload and advances; **an unknown tag
advances only past the tag byte** (switch default leaves `p` one past the tag). An unknown chunk type is a no-op
and the next byte is read as another type. Consequence for a reimplementation: the grammar is permissive by
design; the strict walker must reject what the game would skip, and the data never contains such bytes except
tag 0x62 (§3.9).

Offsets `{u32 offset, u32 size}` in chunk payloads are relative to `D`, the **most recently loaded section**:
the asset section (ID 3) at start, the language section after `0x4a`, the model section (ID 4) after `0x2b`.
In the data, chunk types 0x00/0x05/0x28/0x48 come before `0x2b` and 0x20/0x22/0x35/0x46 after it (§6).

## 3. Chunk types and tag tables — Verified (code + data)

Chunk type → handler [JT]: `00`, `05`, `07`/`0a`/`63` (one handler), `08`, `09`, `20`, `22`, `28`, `35`, `3a`,
`46`, `48`. Counts over 58 files [DATA]: `22` 17 846, `08` 2 968, `07` 2 853, `0a` 2 203, `09` 745, `20` 123,
`3a` 116, `00` 93, `28` 58, `35` 58, `63` 54, `48` 49, `46` 8, `05` 7. Commands: `2b` 58, `4a` 46.

### 3.1 Chunk 0x00 — audio (before `0x2b`; relative to section 3 or, after `0x4a`, to the language section)

| tag | payload | handler | meaning |
|---|---|---|---|
| `01` | 8 | u32 off → `local_20c`, u32 size: workspace cursor `+= size` (moves where pools/section 4 land); at `0x2b`: `DAT_0063b9e0 = modelArea + off`; at the end of loading `FUN_00429da0(DAT_0063b9e0)` copies `n·0x80+4` bytes (`n` = first u32) to 0x5dabc0 | sample table (Inferred: SFX); ARCA01 `{0, 2820}` |
| `02` | 8 | `{off, size}` → `FUN_00429e50(D+off, size)`: `u32 n`, `u32 off[n]` → AIFF (`FUN_0042a7c0`) → DirectSound buffers | sound bank (Verified code) |
| `04` | 12 | `FUN_00406ff0`: `{u16, u8, u8, u16 ×4}` appended to the table at 0x63afa0 (max 162, `SoundFXBuffer too small`) | SFX entry |
| `43` | 12 | passed to the stubbed logger 0x40ae00 | no effect |
| `4b` | 56 | 7 × `{off, size}`, entry = config language byte (`DAT_00639b30+0x93`, 0..6) → `FUN_00429de0` (table 0x5e16a0); reserve `+= size` | per-language sample table (Inferred: speech) |
| `4c` | 56 | 7 × `{off, size}` per language → `FUN_00429e50` | per-language sound bank |

### 3.2 Chunk 0x05 — music list

On chunk start `FUN_00407480` clears the table. Tag `06`: 3 bytes appended to the 3-byte table at 0x63ba00
(max 40, `MusicBuffer too small`). Semantics of the 3 bytes RR (Inferred: XA track/channel references).

### 3.3 Entity chunks 0x07, 0x0a, 0x63 (instances), 0x08 (templates), 0x09 (aux items)

`[TL]` allocates per chunk: 07/0a/63 → kind 2, a 0x1b0-byte entity from pool `DAT_004d2704`, linked into
`DAT_004d4fd0`; 08 → kind 1, 0x1b0-byte entity linked into `DAT_004d26d0`; 09 → kind 4, a 0x30-byte item from
pool `DAT_004d4fd8`, linked into `DAT_004d2760` (tags marked † write into this item instead of an entity).
In the data, 08 chunks carry no position (templates; their `27` = `(class id, 0)`), 07/0a/63 carry tag `10`.

| tag | payload | effect (entity offsets) |
|---|---|---|
| `0b` | 4 | `FUN_00408410(lastSpriteAsset.data, u16@0)`; `+0x168 = u16@0`, `+0x16a = u16@2` |
| `0c` | 4 | `+0x16c = u16@0`, `+0x16e = u16@2` |
| `0f` | 2 | u16 asset id → `FUN_00460850` (match asset `+6`); appended to the entity's asset list `+0x98` (12-byte links); an asset of kind 0x100 becomes `lastSpriteAsset`. The game hangs if the id is unknown |
| `10` | 12 | position: **s16 from bytes 0–1, 4–5, 8–9** (i32 fields, low halves) → `+0xfc/+0xfe/+0x100` († item `+0x10..`) |
| `11` | 6 | rotation 3 × s16, 4.12 (0x1000 = 360°) → `+0xf4/+0xf6/+0xf8` († item `+0x18..`) |
| `12` | 12 | scale, s16 from bytes 0–1, 4–5, 8–9 → `+0xec/+0xee/+0xf0` |
| `13` | 1 | class byte → `+0x31` (values: 07: 14, 2, 20, 5…; 08: 14, 0, 31…; 0a: 16, 35; 63: 9) |
| `15` | 1 | → `+0x30` |
| `16` | 28 | 7 × u32 → `+0x08` flags0, `+0x0c` flags1, `+0x10`, `+0x2c`, `+0x14`, `+0x18`, `+0x1c` († only `+8`, `+0xc`). flags0 `|= 0x200000` if the first asset's kind has bits 0x180. If flags0 bit 26: flags1 bit 10 → `DAT_004d236c`/`DAT_004d5930`, bit 11 → `DAT_004d2368`/`DAT_004d5934`; both set `+0x6e = 0x40`, `+0x76 = 0x3fff` (§5) |
| `19`, `1a` | 4 | ignored |
| `1b` | 6 | 3 × s16 → `+0x60/+0x62/+0x64` (Inferred: box min) |
| `1c` | 8 | 4 × s16 → `+0x68/+0x6a/+0x6c` (box max), `+0x4a` = r, `+0x4c = r²` († `+0x20/22/24`, `+0x16`) |
| `1d` | 4 | u16@0 → `+0x192` if class ∈ {0x0e, 0x1e, 5, 0}, `+0x16c` if class 8, else `+0x178` |
| `1e` | 2 | u16 → `+0x3c` (players: overwritten from config, §5) |
| `1f` | 1 | → `+0x32` († u16 `+0x1e`) |
| `27` | 4 | u16 instance/class id → `+0x34`, u16 template id → `+0x50`, `+0x82 = 0`; for 07/0a/63: id 2 → `DAT_004d26cc`, id 3 → `DAT_004d2768` (§5) |
| `2f` | 48 | 0x2c-byte record from the logic heap: u16@0 < 10000 → appended to the entity list `+0x110`, else pushed on the global list `DAT_004d5108`. Fields: `+6`=u16@0, `+8`=@2, `+4`=@4, `+0xa`=@8, `+0xc`=@12, `+0x20..+0x28` = u32@16,@20,@24, `+0xe..+0x1e` = u16 @28..@41 (RR) |
| `31` | 12 | 0x10-byte record: u16@0 < 10000 → entity list `+0x10c`, else global `DAT_004d2364`; `+4` = u16@0, `+6..` = 5 × u16 @2..@11 |
| `37` | 10 | r = u16@0 → `+0x3e`; box max x/z `+= r`, min x/z `−= r`; `+0x38` = @4, `+0x3a` = @6, `+0x40` = @8, `+0x42 = +0x44` = @2, `+0x46 = 0x1000`, `+0x4a += r` |
| `38` | 2 | `+0x33 = byte@0 + 0x10` |
| `39` | 12 | `+0x6e` = u16@0, `+0x76` = @2, `+0x118` = u32@4, `+0x121` = byte@8, `+0x122` = byte@10 |
| `40` | 20 | ignored (only aligns the logic heap) |
| `41` | 6 | 3 × s16 → `+0x68/+0x6a/+0x6c` |
| `45` | 2 | → `+0x36` |
| `4d`..`60` | see §3.10 | logic program instructions `[LC]` |
| `61` | 14 | 0x14-byte record appended to `+0x78` (count u16 at its `+2`): 7 × s16 adjusted by `+0x3e`, plus a squared radius at `+0x10` (Inferred: collision sub-volume) |
| `62` | 0 | **no handler** (§3.9) |
| `65` | 2 | → `+0x48` |

### 3.4 Chunk 0x20 — world model placement

One 0x1c-byte object (pool `DAT_004d5988`) linked into `DAT_004d4fbc`. Tag order in the data: `24 21 11 12 10 38
42 62` (123/123).

| tag | payload | effect |
|---|---|---|
| `24` | 8 | `{off, size}` in `D` (section 4): object kind 2, `+0x18 = D+off`, `FUN_004067f0` (octree walk, marks texture use). Format: MODELS.md §10.1 |
| `21` | 4 | u16 world slot (0..49) → `+6`; `+0xa = 0` (data: slot 1 = main level, 2 = second model, …) |
| `11` | 6 | **allocates** the transform node (`FUN_004609e0`) and sets rotation (3 × s16 → `FUN_00404160` RotMatrix); must precede `10/12/38/42` |
| `12` | 12 | scale: **u16 bytes 0–1 used for all three axes** → `FUN_00404710` (Q12, 0x1000 = 1) |
| `10` | 12 | translation, s16 from bytes 0–1, 4–5, 8–9 → `FUN_004047c0`; also registers the slot: model = last `24`, size, transform (`local_190/local_c8/DAT_004d5040[slot]`) for the runtime build `FUN_00406730` at the end of loading |
| `38` | 2 | u16 → transform `+0x76` |
| `42` | 1 | u8 → transform `+0x75`; unlinks the 0x1c object from `DAT_004d4fbc` (`FUN_00460930`) |
| `62` | 0 | no handler |

Data: translation (0,0,0), rotation 0x1000 ×3 (identity) and scale 0x1000 in 123/123 chunks.

### 3.5 Chunk 0x22 — asset (model / animation / sprite sequence / block)

One 0x1c-byte asset (pool `DAT_004d5988`), linked into `DAT_004d4fbc`. Asset `+8` = kind, `+0x18` = data.

| tag | payload | effect |
|---|---|---|
| `24` | 8 | `{off, size}` in `D`. If `(u32 @data+0x18 & 0xffffff) == 0x1ffff` → octree model (MODELS.md §10.1; 59 in data, sky domes): `+0x18 = data`, `FUN_004067f0`, appended to `DAT_004d03d4[]` / sizes `DAT_00639d60[]`. Else actor model (§10.2): relocate `FUN_00406f70`, `FUN_00406eb0`, `+0x18 = data+8`. Kind 2 |
| `25` | 8 | `{off, size}`: animation, kind 4; a 0x10-byte record (pool `DAT_004d2724`): `+0xc = data+8`, `+0xb = data[2] >> 1`, `+8 = u16 data[4]`, `+0xa = data[0xc]` (format RR) |
| `26` | 4 | u16 → `+0xe` |
| `27` | 4 | u16@0 → `+0xa` (13 in the data), u16@2 → `+6` = **asset id** (key for entity tag `0f`) |
| `3e` | 12 | u16@0 → `+0xc`; `{off @4, size @8}` → `+0x18`, kind 1 (RR) |
| `3f` | 8 | `{off, size}`: sprite sequence (MODELS.md §10.3), relocate + `FUN_004084b0`, `+0x18 = data+8`, kind 0x100 |
| `0d` | 12 | ignored |
| `4d`..`60` | §3.10 | logic instructions attached to the asset (`[LC]` called with mode 8: list at asset `+0x14`) |

### 3.6 Chunk 0x28 — textures

Tag `29`, 8 bytes: `{u32 off, u32 packed}` → `FUN_00407f60(D+off, packed)`; texture id = `packed & 0xfff`, byte
size = `packed >> 12` (TEXTURES.md §1). Always before `0x4a`/`0x2b` → offsets in section 3 (58/58).

### 3.7 Chunks 0x35, 0x46

- `35`/`36` (8): `{off, size}` → `DAT_004d26fc = D+off` (section 4; 58/58 levels). RR.
- `46`/`47` (8): `{off, size}` → `DAT_004d2700 = D+off`; if `DAT_004d4fae == 1` it becomes 2 and config byte
  `+0x45 = 1` (8 levels). RR.

### 3.8 Chunk 0x3a — level parameters (twice per level: first chunk, and again after the assets)

| tag | payload | effect |
|---|---|---|
| `3c` | 14 | view parameters used by the camera setup `FUN_00465710` (scaled by H/400, clamped to `@2`): u16@0 → `DAT_004d5954`, @2 → `DAT_004d5980` (max distance), @4 → `DAT_004d2718`, @6 → `DAT_004d272c` (0 < v < 0x800 → 0x1000), @8 → `DAT_004d2730`, @10 → `DAT_004d237a`, @12 → `DAT_004d26c0`. Example (Credits): 12000, 32000, 5120, 4096, 16, 8192, 0. Exact meaning RR (Inferred: draw/fog distances) |
| `66` | 3 | ignored by the loader (data e.g. `84 84 84`; Inferred: an RGB colour read elsewhere) |
| `2a` | 8 | u16@0 → `DAT_004d5124`, s16@2 → `DAT_004d5128`, s16@4 → `DAT_004d512c`, u16@6 → `DAT_004d26bc` (RR) |
| `3d` | 36 | **pool sizes; must be seen before `0x2b`** (otherwise `[TL]` spins forever). u16@0+5 → anim records (0x10 B), @2+5 → 0x78-B pool `DAT_004d2484`, @4+5 → assets/objects (0x1c B), @6+5 → entities (0x1b0 B), @8+5 → 0xc-B asset links, @10 → aux items (0x30 B, no +5), @12 → per-view array ×4 B, @14+5 (min 300) → two 6-B arrays, u32@16+5 → logic heap bytes, u32@20 & ~7 → offset of the model area in the workspace, @24 → 8-B items, @26 → 0x28-B items, @28 → 0xa0-B linked items, @30 → 0x14-B items, @32 → 4-B items, @34 unused |

A native engine with dynamic containers only needs `3d` as an upper bound (the original fails hard on overflow).

### 3.9 Tag 0x62

Present as a zero-payload tag: last tag of every 0x20 chunk (123/123) and inside every 0x63 chunk (54/54).
Neither the 0x20 nor the entity switch has a case for it [JT]: the default skips just the tag byte, so it has
no effect in this build (Inferred: an editor-side end/marker tag). Parsers must accept it with payload 0.

### 3.10 Logic tags 0x4d..0x60 (`[LC]` FUN_00433ab0) — sizes Verified, semantics RR

Used in entity chunks and chunk 0x22: 54 % of all tags, 48 % of all section-1 bytes [DATA]. `[LC]` re-encodes each into a compact
variable-length instruction (`op | flags << 5`, 2-byte operands only when non-zero) in the logic heap
`DAT_004d9b14`; `M` (0x4d) starts a new program block (u16 id; < 10000 → attached to the entity/asset, else the
global list `DAT_004d5984`). Payload sizes and emitted opcode (low 5 bits):

| tag | `M` 4d | `N` 4e | `O`/`P`/`Q` 4f–51 | `R` 52 | `S` 53 | `T` 54 | `U` 55 | `V` 56 | `W` 57 | `X` 58 | `Y` 59 | `Z` 5a | `[` 5b | `\` 5c | `]` 5d | `^` 5e | `_` 5f | `` ` `` 60 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| payload | 2 | 4 | 8 | 8 | 6 | 12 | 12 | 16 | 20 | 8 | 4 | 8 | 8 | 2 | 8 | 12 | 16 | 8 |
| op | block hdr + 13 | 3 | 4/5/6 | 8 | 7 | 10 | 9 | 11 | 12 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 |

These feed the logic VM (TIME_BUSTERS.md appendix); decoding them is the next gameplay-RE step.

## 4. Commands — Verified (code)

- `0x2b` MODELS: requires a prior `3a/3d`; when level index = 16 (AZBOSS02) sets `DAT_004d03fc = 1`; carves the
  pools of §3.8 from the workspace, copies the per-language `48` block (§4.1) behind them, locates section ID 4
  (`FUN_00433560`) and reads it after the pools; `D` = section 4.
- `0x4a` LANGUAGE: reads the section for the configured language (0→6, 1→5, 2→7, 3→8, 4→9, 5→10, 6→11; BZE.md)
  at `workspace + 3d@20`, `D` = that section. Present in the 46 levels that have language sections, absent in the
  12 cut-scene/credits files.

### 4.1 Chunk 0x48 — per-language block

Tags `49` or `68`, 56 bytes: 7 × `{off, size}`, one per language; the configured one (relative to `D`, i.e.
section 3 in the data) is copied into the workspace at the next `0x2b`/`0x4a` → `DAT_004d5b88`. Content RR
(Inferred: per-language text). 49 levels.

## 5. Player characters (Bugs vs Taz) — Verified (code), identity Inferred (strong)

Every one of the 58 files has two instance chunks (type 07) with tag `27` instance id **2** and **3** and a tag
`10` start position + tag `11` rotation (y = heading, 4.12) [DATA] (`tb_sec1.py --players`).

| id | entity global | `16` flags1 | template (`27`@2) | model (via `0f` assets) | identity |
|---|---|---|---|---|---|
| 2 | `DAT_004d26cc` | bit 10 `0x400` (full values `0x8500`/`0x8400`/`0x8480` in levels, `0x400` in cut-scenes) → `DAT_004d236c` | 181 (0 in secret/cut-scene files) | ARCA01 sec4 `0x28d54`, 38 objects, 77 assets | **Bugs**: grey/white vertex colours, carrot geometry |
| 3 | `DAT_004d2768` | `0x800` → `DAT_004d2368` | 182 (or 0) | ARCA01 sec4 `0x2ccf0`, 35 objects, 60 assets | **Taz**: red-brown vertex colours |

End of load [TL]: config byte `+0x51` bit 0 → Bugs gets flags `+0x20 |= 0x2000000` and config `+0x52 |= 4`;
bit 1 → Taz gets it and `+0x52 &= ~4` (Inferred: which character is active). Bugs `+0x3c` = config byte
`+0xa5`, Taz `+0x3c` = config byte `+0x53` (Inferred: per-character health/lives carried between levels).
`FUN_004395d0(DAT_004d236c, 0)` is called last. Peer GameplayRE (`tools/re/tb_players.py`) maps flags1 bit 10
→ pad record 0 (P1, `DAT_004d236c`) and bit 11 → pad record 1 (P2, `DAT_004d2368`), and decodes the players'
state/transition records (tags `2f`/`31`).

Examples (s16 world units, PS1 axes): ARCA01 Bugs (−4107, −27, −7555) / Taz (−3653, −27, −7982), heading ≈ 489
(43°); AZCA01 (−8239, −2020, 10244) / (−8595, −2025, 10605); TRCA01 (−8504, −832, −13432) / (−8658, −795,
−13181); VICA01 (411, 1146, −2863) / (843, 1151, −2809).

## 6. Stream order in the data [DATA]

Typical level (ARCA01): `3a 48 00 28 [4a] 00 [2b] 35 22×474 3a 08… 0a 07… 20 07… 09… 63 … 2e`. Cut-scenes:
`3a [48] 28 [2b] 35 22… 3a 08/20/07/63… 2e`. No file uses the pseudo-chunk. 0x20 chunks may be interleaved
with entity chunks (e.g. ARCA01 has two).

## 7. LiT → TB mapping (for a shared parser)

Structural bytes are LiT − 1 (BZE.md). Chunk types: LiT `29`→TB `28` (textures), `36`→`35`, `3b`→`3a`, `47`→`46`,
`49`→`48`; `00`, `05`, `07`, `08`, `09`, `0a`, `20`, `22` keep their values; TB adds `63`; LiT `09` had its own tag
table, TB `09` shares the entity table. Tag tables differ beyond renumbering, e.g. type 0 `44/4c/4d` (12/48/48) →
`43/4b/4c` (12/56/56: 7 languages); chunk 0x20 `39/43` → `38/42`; chunk 0x22 `3f/40` → `3e/3f` and `28` dropped;
chunk 0x3a sizes `2b 12, 3c 4, 3d 6, 3e 28` → `2a 8, 3c 14, 3d 36, 66 3`; entity tags `16` 8 → 28 bytes, `30/31`
32 → `2f` 48 / `31` 12, and the logic tags `4d..60` are new. Use one table per game; do not derive TB from LiT.

## 8. Open questions

1. Semantics (not sizes) of: `00/01`, `05/06`, `35/36`, `46/47`, `48/49|68` content, `3a/2a`, `3a/3c` fields,
   `3a/66`, entity `16` flag bits other than 26/10/11, `2f`/`31`/`61` records, `22/25` animation format,
   `22/3e` blocks, `0x13` class values.
2. Logic instruction semantics (§3.10) — tie to the VM tables in TIME_BUSTERS.md.
3. Why some files reference template 0 for the players (secret `*SG*`, `*R01`, cut-scenes).
