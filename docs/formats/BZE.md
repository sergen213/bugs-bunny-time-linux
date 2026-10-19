# BZE level container (LiT + TB)

Implementation: `engine/core` (`bb/bze.h`, `bb/lzss.h`, `bb/levelscript.h`), tools `bzedump`, `bbimport`,
tests `core.lzss`, `core.bze`, `core.realdata`.

Sources:
- **[BD]** BugsDecomp `doc/bze.md` (LiT PC v1.6), commit `0dd943b8`. The repo has no license file, so only
  facts are restated here (marked [BD]); no text or code copied.
- **[G-LiT]** Ghidra 12.1.2 headless decompile of LiT `bugs.exe` (md5 `121db63d…`, v1.6),
  `/mnt/1st4TB/BugsBunny/ghidra/assets/bugs_all.c`.
- **[GED]** Peer GhidraEngineDiff, TB `bblit2.exe` analysis (to be written up in `docs/re/TIME_BUSTERS.md`).
- **[Data]** Measurements over every BZE in the installs: LiT 167 files (`lit/Datas/bze`), TB 58 files
  (`tb/Datas/BZE`), via `bzedump` / `test_realdata`.

All integers little-endian.

## Status

| Part | LiT | TB |
|---|---|---|
| Container header | Verified (167/167 files) | Verified (58/58) |
| LZSS section compression | Verified (946/946 sections decode) | N/A: sections stored raw (Verified, see below) |
| Section-1 level script | Tested: 167/167 parse to the last byte with [BD] tables | Tested: 58/58 parse to the last byte (strict `bb::parseTbScript`, `core.tb_levels`); grammar in `TB_LEVELS.md` |
| Semantics of section 2..11 payloads | Research Required (other specs in `docs/formats/`) | Research Required |

## Header (0x800 bytes)

| off | LiT | TB |
|---|---|---|
| 0x00 | u32 `1` (version?) [BD] | u32 `0` |
| 0x04 | u32 section count | u32 *budget* (see below) |
| 0x08 | entries… | u32 section count |
| 0x0c | | entries… |
| 0x7fc | u32 checksum | u32 checksum |

Entry = `{u32 id, u32 actualSize, u32 fileSize}` [BD]; TB uses the same entry shape at 0x0c [Data][GED].

- **Checksum** (both games): 32-bit sum of bytes `0x000..0x7fb` read as *signed* 8-bit [BD]. Matches in
  167/167 LiT and 58/58 TB files [Data] (TB coverage includes the garbage bytes below).
- **Bytes between the entry table and 0x7fc**: all zero in LiT (167/167). In TB they are non-zero in 58/58
  files and look like leaked stack memory of the build tool (Win32 pointers such as `0x77e7…`, `0x0032d5..`
  at 0x88+ in `ARBOSS01.bze`) [Data]. Parser ignores them (checksum still covers them).
- **TB budget word** (0x04): values 160000..350000, always a multiple of 1000 (20 distinct values) [Data];
  [GED] identifies it as a memory budget. It is repeated verbatim at section-1 offset 0x33 in 58/58 TB files
  [Data].
- **Layout rules** (enforced by `parseBze`, all hold for every real file [Data]): sections are contiguous in
  table order starting at 0x800 [BD]; `fileSize % 0x800 == 0`; `actualSize <= fileSize`; the sum of fileSizes
  + 0x800 equals the file length exactly. Violations, a count of 0 or one overrunning 0x7fc, an unknown
  first word, or a bad checksum are `FormatError`s carrying the offset.
- LiT `actualSize` is a multiple of 4 in 946/946 sections [Data] ([BD]: "usually rounded up to 4").

Variant detection: first word 1 → LiT, 0 → TB. `bbimport` treats "first word ≤ 1, non-zero count and valid
checksum" as the BZE signature (no magic exists; headerless `.xa` files start with ≥0x800 zero bytes whose
checksum is also 0, hence the count check).

### Section IDs seen [Data]

| Game | Order in file | Files |
|---|---|---|
| LiT | 1,3,4 | 88 |
| LiT | 1,2,3,6,5,7,8,9,10,4 | 61 |
| LiT | 1,2,3,4 | 18 |
| TB | 1,3,6,5,7,8,9,10,11,4 | 46 |
| TB | 1,3,4 | 12 |

[BD] meanings (LiT): 1 load instructions, 2 unknown/ignored, 3 textures/SFX/text, 4 models, 5..10 per-language
assets (FR, EN, DE, ES, IT, NL); the game assumes order 1,2,3,6,(5–10),4 [BD]. TB never has section 2 and adds
id 11 (a 7th language? Research Required).

## LZSS (LiT only)

Every LiT section (946/946) is compressed and uses header byte `0x0b` [Data]. TB sections are stored raw:
[GED] reports the TB tool compresses only with a `/comp` switch and the TB loader reads raw; independently,
no TB section's bytes form an LZSS stream that fills the section (checked by `core.realdata`), TB section 1 is
a plain tag stream ending `2d 2e` [Data].

Stream [BD], confirmed against the game decoder `FUN_00430ff0` [G-LiT]:
- byte 0: bits 0–2 `osize`, bits 3–4 LUT `step`, bits 5–7 unused (we reject non-zero; never seen).
  offset bits = 9+osize, length bits = 7−osize; osize 7 invalid.
- bytes 1–3: 24-bit big-endian *stored count*.
- LUT: `size = 1<<lenBits`; threshold 19 if size ≥ 32 else size/2−1; `lut[i] = 3+i` up to the threshold,
  then `3 + ((i−thr)<<step) + thr` [BD]. The game stores `value−1` and copies with a do-while (+1) [G-LiT];
  equivalent.
- Body: flag byte per 8 items, LSB first; 1 = literal byte, 0 = BE u16 with offset in the high bits, LUT
  index in the low bits; copy `len` bytes from `offset` back, byte by byte (overlap repeats) [BD].
- `0x0b` → 12-bit offset (window 4095), 4-bit length (3..10, then 12,14,…,26).

**Item count / off-by-one (resolved).** The game decodes exactly `stored+1` items, with no input bounds
check (`if (count == 0) stop; count--` after each item, `FUN_00430ff0` @0x00430ff0) [G-LiT]. [BD] notes that
some sections store the real count. [Data] over 946 LiT sections, decoding `stored+1` items within
`actualSize` bytes:

| outcome | sections |
|---|---|
| final item decodes, stream ends at actualSize−0/−1/−2/−3 | 179 / 247 / 95 / 118 (639) |
| final item undecodable (needs bytes past actualSize, or distance-0 pair from zero padding) | 307 |

So 307 sections store the real item count; for them the game decodes one garbage item from padding.
`lzssDecompress` reproduces the game except that such an undecodable **final** item is dropped
(`LzssInfo::tailDropped`); any error before the final item is a `FormatError`. Every real stream then ends
within the 4-byte rounding of actualSize (asserted by `core.realdata`). The decompressed size is not stored
anywhere in the header; it is known only by decoding.

Support: all 167 section-1 payloads end exactly at their `0x2f` end command, i.e. no garbage tail survived in
section 1 (none of the 307 is a section 1).

## Section 1: level script

### LiT [BD]

Top level: `2c` command (inits pools, activates section 4 for models), `4b` command (activates the language
section), `2d <type> {tag operand…}* 2e` chunk, `2f` end. If the section begins `2d 45`, a pseudo-chunk of
`45 xx` tags (no type byte) comes first; each `45` makes the game skip one section [BD]. Tag operand lengths
per chunk type are the [BD] tables (types 00, 05, 07/08/0a, 09, 20, 22, 29, 36, 3b, 47, 49), implemented
verbatim in `levelscript.cpp`. Neither command takes operand bytes (inference: with operands, 167/167 would
not parse to the last byte).

Decoded fields (`ScriptItem`): tag 10 position (3×i32, 20.12), 11 rotation (3×i16, 4.12, 0x1000 = 360°),
12 scale (3×i32, 20.12) in types 07/08/09/0a/20; tag 13 (byte 1 = player) and 27 (u16 entity id) in entity
types 07/08/0a [BD]. Unknown command/type/tag → `FormatError` with the offset.

Result [Data]: 167/167 LiT level scripts parse, ending exactly at the decoded payload size; no table gaps found.

### TB

Structural bytes are the LiT ones minus 1: chunk start `2c`, chunk end `2d`, section end `2e` [GED]. Consistent
[Data]: all 58 TB section-1 streams start `2c 3a` and end `2d 2e`; no pseudo-chunk (TB has no section 2).
TB commands and per-type tag tables come from the TB loader and are specified in `TB_LEVELS.md`; the strict C++
walker is `bb::parseTbScript` (`bb/tb_level.h`). `parseLevelScript(…, TimeBusters)` stays LiT-only (it stops at
offset 1, `unknown chunk type 0x3a`).

## Open questions

1. TB section 11 meaning; TB budget word semantics beyond "memory budget".
2. LZSS: in the 639 sections classified "stored = count−1", a garbage final item lying entirely inside
   actualSize's ≤3 alignment bytes and decoding as a valid pair cannot be told apart from data by bytes alone.
   Worst case is ≤26 spurious bytes at the very end of a section-3/4/5–10 payload; consumers use internal
   offsets, so this is believed harmless. Confirm when section 3/4 parsers exist.
3. Exact LiT header word 0 meaning ("version" in [BD]); only value 1 seen.
4. LiT 1.0 (md5 `59731f03…`) data was not available; the statistics above are for the v1.6 install only.
