# Engine comparison: Lost in Time (LiT 1.6) vs Time Busters (TB)

Status: **Verified Against Original Behavior** = no (static analysis only). Everything below is
**Researched (static RE)**; claims are tagged *observed* (read from bytes / decompiled code at the
cited address) or *inferred* (interpretation of observed facts).

| file | size | MD5 | SHA-256 |
|---|---|---|---|
| `original/lit/Datas/bin/bugs.exe` (LiT v1.6, FileVersion `b1.6`) | 772096 | `121db63d76ed5077d69837b111163cac` | `6e15f92076614e3ee218c34c5dee566bf273e5bfee77d831cfbf78aa2d29e235` |
| `original/tb/Datas/BIN/bblit2.exe` (TB, FileVersion `1, 0, 3, 0`) | 880640 | `d7445216f8cabab1ea33ba1ad029ef7b` | `63483ea5205b8982e384e0b1863624cb8df4fce1601d0cd74393e5b69ecf78c1` |
| `original/lit/Datas/bin/hv3dfx.dll` | 961024 | `fadfd3943a1afa0e9186e9b2efdd26a8` | `4b2444e619467f3a4ee16a7376a4c07cde78dee0e18ce3ee9f171d7fbed55dc3` |
| `original/tb/Datas/BIN/WKeyKill.dll` | 4608 | `450180d360ec4e0d93531e8aa8e92b14` | `fb6d29de131ec3b59e74930e78debeeae34786830a3da1a8870473c121959f8e` |

All addresses are virtual addresses (image base 0x400000 for both exes). LiT addresses use the
BugsDecomp v1.6 column (`research/BugsDecomp/doc/functions.md`; that repo has no LICENSE file, so we
cite addresses/facts only and copy no code).

## Verdict

**Same engine, high confidence (≈95 %).** `bblit2.exe` is built from the LiT code base (a
PSX-port engine with emulated GTE/libgs/libpad), recompiled with a newer compiler and extended.
It is *not* binary/data compatible at every layer: the BZE container is the same, but the section-1
token values and the logic-VM opcode tables were renumbered (see "Incompatibilities").

Evidence (all *observed*):

1. Identical subsystems at the code level (similarity = `difflib` ratio of mnemonic sequences):
   frame pacer LiT 0x406ee0 ↔ TB 0x4156c0 (0.92, same QPC/skip/resync logic and same 1/30 s constant);
   LZSS decompressor 0x430ff0 ↔ 0x436e90 (0.92, same LUT construction and 24-bit BE item count);
   BZE level loader 0x42e7a0 ↔ 0x4347c0 (same 0x800 header read, same signed-byte checksum loop over
   0x7fc bytes compared with the i32 at 0x7fc, same `{id,actual,fileSize}` triplet copy, same
   language → section-ID switch); screen/projection setup 0x40d1a0 ↔ 0x405ef0 (0.87, same 0.75/4096
   math, both called with reference size 512×240); 72 functions with byte-for-byte identical
   mnemonic sequences (PSX GTE helpers, list pools, logic conditions/actions, XA track player).
2. Shared string tables: identical command-line switches (`/no_sync /no_skip /full /win /ogl /opengl
   /soft /soft8 /softpal /softrgb /soft16 /soft24 /skip_intro /cons /fntp /dll:`), `..\bin\config.pc`,
   `..\bin\bugs.ini` with `FirstTime=`/`Language=`, `..\bin\Savegame%d.dat`, `..\bin\Savedata.dat`,
   `..\track\Track%d.xa`, `..\Speeches\Speech00.xa`, `..\speeches\Speech%02d.xa`, AIFF chunk tags
   `SSND INST COMM MARK`, window class `BBLIT Game`, `***********keyboard handler error***********`,
   GPU vendor list (`Direct3D Real3D … 3Dfx`), CPU vendor strings `CyrixInstead/NoVendorName`, and the
   PSX CD-style level table `..\BZE\<NAME>.BZE;1` (LiT file offset 0xaa4e0, TB 0xcdff0).
3. Identical precomputed 8-bit blend/add tables in .data (LiT file offset 0x68df1.., TB 0x8bc15..).
4. TB's internal name is `bblit2` and its window class is still `BBLIT Game`.

## Build / PE facts (*observed*, pefile + Ghidra)

| | LiT `bugs.exe` | TB `bblit2.exe` | `hv3dfx.dll` |
|---|---|---|---|
| PE timestamp (UTC) | 1999-09-26 22:56:12 | 2000-11-17 08:22:00 | 1999-04-29 14:24:18 |
| linker version | 5.00 (MSVC 5) | 6.00 (MSVC 6) | 4.20 |
| Rich header | none | VC6-era: C compiler builds 8168 (139 objs) + 8447 (83 objs), 1 C++ obj, cvtres 5.00.1735 | none |
| CRT | release CRT | **debug CRT** (`dbgheap.c`, `_CrtCheckMemory`, `Microsoft Visual C++ Debug Library`) but optimized game code; log printf stubbed to `ret` (0x40ae00) | MSVCRT.dll |
| relocations | `.reloc` present | none | present |
| sections | .text 0x401000 (0x5a250), .rdata 0x45c000, .data 0x45e000 (0x56db0c virt), .idata, .rsrc, .reloc | .text 0x401000 (0x7f22b), .rdata 0x481000, .data 0x484000 (0x1b949c virt), .rsrc | .text, .data, .rsrc, .reloc |
| entry | 0x451eb0 | 0x46f070 | 0x690093e4 |
| Ghidra functions (after `CreateFunctionsFromDataPointers`) | 948 | 1374 | (not exported) |

Imports (*observed*):

| API family | LiT | TB |
|---|---|---|
| OpenGL | dynamic: `LoadLibraryA(opengl32.dll or /dll:)` + 360-entry `{PROC*, name}` table at 0x468750 | dynamic: inline `GetProcAddress` chain at 0x410430 (413 slots), driver choice `opengl32.dll` / `3dfxvgl.dll`(+`3dfxvglu.dll`) / SGI `opengl.dll`; sets `FX_GLIDE_NO_SPLASH` |
| Direct3D | none | none (string `Direct3D` is only a GL_RENDERER vendor match) |
| DirectDraw | `DDRAW.dll` DirectDrawCreate/EnumerateA (software renderer present, `pcrsoft8.c`) | none (software renderer blits with GDI `StretchDIBits`/`BitBlt`) |
| Glide | none directly (only via hv3dfx.dll ICD) | none directly |
| DirectSound | `DirectSoundCreate` | `DirectSoundCreate` |
| DirectInput | no import; created through `ole32` `CoCreateInstance` (*inferred* from imports) | `DINPUT.dll` `DirectInputCreateA(…, 0x300)` (DirectX 3 interface) at 0x40ee50 |
| WinMM | none | `waveOut*`, `PlaySoundA`, `sndPlaySoundA`, `aux*` (AVI audio, menu sounds) |
| Video | none | `AVIFIL32.dll` (`..\data\Start%d.%s`, `End%d.%s`, `demo%d.avi`) |
| timing | `QueryPerformanceCounter/Frequency`, `GetTickCount` | same |
| other | `ADVAPI32` registry | `ADVAPI32`, `VERSION.dll` (reads own version resource), GDI fonts (`AddFontResourceA`, TTF menu fonts), `ChangeDisplaySettingsA` |

`hv3dfx.dll` = 3dfx "OpenGL 1.1 Client Driver" ICD v1.0.0.0438 (OriginalFilename `3dfxogl.dll`),
378 exports (`Drv*`, `wgl*`, `gl*`), imports `glide3x.dll`. It is a third-party GL driver selectable via
LiT's `/dll:` switch / `bugs.ini DLL=` key — not engine code; irrelevant to the reimplementation.

## Graphics API and vertex space (both games)

*Observed* in both: OpenGL 1.1 immediate mode only. Projection is `glMatrixMode(GL_PROJECTION);
glLoadIdentity(); glOrtho(-w/2, w/2, -h/2, h/2, near, 65536.0)`; modelview stays identity. No
`glLoadMatrix*`, `glMultMatrix*`, `glFrustum`, `gluPerspective`, `glRotate/Translate/Scale`,
`glFog*`, `glDrawArrays` or `glVertexPointer` call site exists in either exe (pointer slots are loaded
but never called — checked by raw xref count on LiT's slot table and by decompile grep on TB).

* LiT: GL prim flush 0x4124b0 (+0x413be0) issues `glBegin` / `glColor4f` (or `glColor3f`) /
  `glTexCoord2f` / `glVertex3fv`. Ortho set at 0x40daf0 / 0x413990: near = 400.0, far = 65536.0
  (`DAT_00467748/4c` = width/height). Texturing is **affine** (2-component texcoords).
* TB: GL prim flush 0x424020 selects per-primitive emitters 0x423010…0x423f10. Ortho at 0x424ae0 /
  0x425290 / 0x422950: near = `f32 @0x639a80` (set to 0.0), far = 65536.0. Emitters use
  `glColor3fv`, `glVertex3fv` and either `glTexCoord2fv` (affine) or, when `DAT_00639dc8 != 0`,
  `glTexCoord4f(s·q, t·q, 0, q)` with `q = |1.0 / v.z|` → **perspective-correct texturing** (new in TB).
  The writer of `DAT_00639dc8` was not found statically (only reads at 0x424041/0x42408c/0x425763/
  0x4257d4) — *inferred* menu/detail option written through a pointer.

**Conclusion:** vertices are fully CPU-transformed and perspective-divided (PSX GTE emulation:
`RotMatrix`, `MulMatrix0`, `ApplyMatrix*` etc.), submitted as screen-space pixels centred on the
viewport (x,y) plus a linear view depth z used only for the depth test (and, in TB, for 1/z texture
perspective). Fog/lighting are baked into vertex colours on the CPU (no `glFog*`, no `glLight*` calls).
Implication for our renderer: the original projection can be reproduced exactly by doing the same
CPU projection; a modern GPU path may instead pass view-space vertices + a projection matrix, but must
match `H`, the Q12 `sx/sy` scales and the 512×240 reference size (see TIME_BUSTERS.md "Aspect/FOV").

## Simulation timing (both games)

*Observed:* timer init computes `period = (float)QPF * 0.033333335f` (LiT inline in 0x406190 using
f32 @0x45c26c; TB 0x415520 using f32 @0x48160c; both also keep `1000.0f / QPF`). Pacer
(LiT 0x406ee0, TB 0x4156c0) advances `deadline += period` each loop and:

* if behind by more than one period and fewer than 2 consecutive skips (4 when `TB DAT_004d24c2` /
  `LiT DAT_004b2277` is set) and `/no_skip` not given → set the *skip-render* flag (TB `DAT_004d03c8`,
  LiT `DAT_004b1d00`);
* if ahead by more than half a period and sync on (`/no_sync` clears it) → busy-wait on QPC until the
  deadline;
* if more than 30 periods behind (`allmul(-period, 30)`) → resynchronise `deadline = now`.

One game-logic step runs per loop iteration regardless of the skip flag (TB 0x4011c0: input/logic,
then per-view render 0x4013b0 which only does minimal work when skipping). **Fixed simulation tick =
30 Hz (1/30 s); rendering is locked to the tick, frames may be dropped, no interpolation.** Both keep a
frame counter incremented by 2 per tick (LiT `DAT_004b24a4`, TB `DAT_004d2708` in 0x401960) —
*inferred* to be PSX 60 Hz field units.

## Incompatibilities between the two games' data (*observed* unless noted)

* BZE header: LiT `{u32 version=1, u32 count}`; TB `{u32 0, u32 workspaceBudget, u32 count}` →
  TB section table starts at 0xC (details in TIME_BUSTERS.md). Checksum rule identical.
* Compression: LiT always LZSS-decodes every section it reads (0x430ff0); TB reads sections raw unless
  the `/comp` command-line switch is present (0x415980 sets `DAT_004d03e8`), and the shipped TB BZEs
  are uncompressed (section 1 begins with the raw token `0x2c`).
* Section-1 structural tokens are LiT−1 in TB: chunk start 0x2d→0x2c, chunk end 0x2e→0x2d, section end
  0x2f→0x2e, command 0x2c→0x2b, language command 'K' 0x4b→0x4a, pseudo-chunk `'-' 'E'`→`',' 'D'`,
  type-0 tags 'D','L','M'→'C','K','L'. Low tag values (0x01…0x31) are unchanged.
* Logic VM: LiT has 99 condition + 129 action handlers (tables 0x4ac550 / 0x4ac6e0, dispatched by
  `TestLogicCond` 0x42a7d0 / `RunLogicAction` 0x42c6d0). TB has 83 conditions (0x4cd560), 142 actions
  (0x4cd6ac) and two further handler tables (0x4cd8e4: 14, 0x4cd91c: 31), all dispatched inline in TB
  0x45d7a0. Many handlers are identical but **sit at different indices**, so logic bytecode is game-
  specific. `align_logic_tables.py` aligns them (94 slot pairs with similarity ≥ 0.8); positional pairs
  of tiny look-alike handlers are hints, not proof.
* Languages: LiT 6 (EN FR DE ES IT NL); TB 7 (`language` byte clamped to 0..6; resource string order
  EN FR DE ES IT PT NL). Both map language index → BZE section ID as 0→6, 1→5, 2→7, 3→8, 4→9, 5→10;
  TB adds 6→11.
* `config.pc`: LiT 128 bytes; TB 0xF8 bytes (read at 0x415dd0, written at 0x415d80).

## Function mapping

### Hand-verified subsystem pairs

Similarity = mnemonic-sequence ratio (`match_functions.sim`). "verified by" lists the shared evidence.

| subsystem | LiT 1.6 | TB | sim | verified by |
|---|---|---|---|---|
| WinMain (single instance via `FindWindowA("BBLIT Game")`, message loop) | 0x405950 | 0x4147f0 (+0x414370 mutex `A2m application mutex`) | 0.42 | strings, call to cmdline parser and frame function |
| command-line parser | 0x408f70 | 0x415980 | 0.43 | same switch strings; TB lower-cases first and adds `/bze:` `/b` `/x` `/y` `/ask` `/?` `/comp` |
| `ReadConfig` (config.pc) | 0x409cc0 | 0x415dd0 | 0.64 | `..\bin\config.pc` |
| `WriteConfig` | 0x409a10 | 0x415d80 | 0.67 | `..\bin\config.pc`, size 0x80 vs 0xF8 |
| `SetConfigDefaults` | 0x409a90 | 0x415fd0 | 0.67 | shape only (*inferred*) |
| bugs.ini reader | 0x409e30 | 0x416290 | 0.98 | `..\bin\bugs.ini`, `FirstTime=` |
| timer init (QPF, 1/30 s) | inline in 0x406190 | 0x415520 | – | constants 0.0333333f / 1000.0f |
| frame pacer | 0x406ee0 | 0x4156c0 | 0.92 | identical control flow |
| per-tick level loop | 0x4065e0 (inline) | 0x401000 init, 0x4011c0 tick, 0x4013b0 per-view render | 0.16 | same skip flag / pacer / message pump; TB refactored |
| BZE level loader | 0x42e7a0 | 0x4347c0 | 0.57 | 0x7fc checksum, triplet copy, language switch |
| find section by ID (language/model sections) | 0x42e3a0 | 0x433560 | – | called from the language command in both loaders |
| LZSS decompressor | 0x430ff0 | 0x436e90 | 0.92 | identical algorithm |
| screen/projection setup (`sx`, `sy`, ref 512×240) | 0x40d1a0 | 0x405ef0 | 0.87 | constants 0.75f, 4096.0f, callers pass 0x200/0xf0 |
| `GsSetProjection` (H = 400) | 0x40cd20 | 0x4061f0 | 0.67 | same globals pattern (H, (float)H, -(H/2)) |
| screen-scale × camera matrix (Q12 diag(sx,sy,1)·RotX(0x800)) | 0x408910 | 0x404d40 | 0.58 | uses sx/sy globals; RotMatrixX/MulMatrix0 callees |
| GL ortho reset | 0x413990 | 0x424ae0 | 0.51 | `glOrtho(-w/2,…,65536)` |
| GL prim flush / rasterizer | 0x4124b0 | 0x424020 | 0.32 | glBegin/glVertex3fv emitters; TB split into per-prim functions |
| `SetGLPixelFormat` | 0x40df20 | 0x422b30 | 0.63 | ChoosePixelFormat path |
| GL proc loading | in `TestGL` 0x40e050 (table 0x468750) | 0x410430 | – | GetProcAddress of all GL names |
| software present (`StretchDIBits`) | 0x417300 | 0x427220 | 1.00 | identical |
| DirectSound init | 0x41da00 | 0x429660 | 0.74 | `DirectSoundCreate` |
| `InitDInput` | 0x402610 | 0x40ee50 | 0.34 | DirectInput creation + keyboard/joystick devices |
| `ReadJoystick` | 0x402180 | 0x40eaa0 | 0.51 | shape + position (*inferred*) |
| `MapJoystickInput` | 0x403010 | 0x40e710 | 0.54 | shape + position (*inferred*) |
| save slot writer | 0x4074f0 | 0x428fb0 | 0.29 | `..\bin\Savegame%d.dat` |
| Savedata.dat | 0x4076f0 | 0x428e50 / 0x428d70 | 0.38 | `..\bin\Savedata.dat` |
| `TestLogicCond` / `RunLogicAction` | 0x42a7d0 / 0x42c6d0 | inlined in 0x45d7a0 | – | table refs 0x45d8e8 / 0x45e2e5 |
| `CondTrue` | 0x42a800 | 0x40aca0 (`mov eax,1; ret`) | 1.00 | logic table slot 0 |
| `ActionNop` (shared empty fn) | 0x417010 | 0x40ae00 (also the stubbed log printf) | 1.00 | single `ret` |

### Automated pairs carrying BugsDecomp names

Produced by `tools/ghidra/match_functions.py` (methods: `hash` identical mnemonic sequence,
`call` call-order propagation, `fuzzy` 4-gram similarity, `str`/`const` unique shared strings/immediates).

| BugsDecomp name | LiT 1.6 | TB | method | score |
|---|---|---|---|---|
| `FiniTrack` | 0x401330 | 0x40d250 | fuzzy | 0.88 |
| `SetTrackSource` | 0x4013c0 | 0x40d2f0 | fuzzy | 0.89 |
| `ResetTrack` | 0x401490 | 0x40d3e0 | hash | 1.0 |
| `RefillTrackBuffer` | 0x4014f0 | 0x40d440 | call | 0.81 |
| `PlayTrack` | 0x4018d0 | 0x40d940 | hash | 1.0 |
| `StopTrack` | 0x401910 | 0x40d980 | hash | 1.0 |
| `UpdateTrack` | 0x4019a0 | 0x40da20 | fuzzy | 0.82 |
| `CvtStereoAdpcm` | 0x401be0 | 0x40dc90 | const | 0.95 |
| `CvtMonoAdpcm` | 0x401e10 | 0x40ded0 | const | 0.96 |
| `FiniDInput` | 0x402790 | 0x40ec20 | fuzzy | 0.92 |
| `FsFOpen` | 0x405760 | 0x40fce0 | call | 0.98 |
| `SquareRoot0` | 0x4079c0 | 0x4040b0 | hash | 1.0 |
| `RotMatrix` | 0x407a50 | 0x404160 | call | 0.99 |
| `RotMatrixZYX` | 0x407b90 | 0x404210 | fuzzy | 0.92 |
| `RotMatrixX` | 0x407c30 | 0x4042b0 | call | 0.67 |
| `RotMatrixY` | 0x407da0 | 0x404430 | call | 0.84 |
| `RotMatrixZ` | 0x407f00 | 0x404590 | call | 0.67 |
| `ScaleMatrix` | 0x408060 | 0x404710 | call | 0.63 |
| `TransMatrix` | 0x408100 | 0x4047c0 | call | 0.67 |
| `MulMatrix0` | 0x4082f0 | 0x4048b0 | hash | 1.0 |
| `MulMatrix2_0` | 0x408640 | 0x404a90 | hash | 1.0 |
| `CompMatrix2LV` | 0x4088b0 | 0x404b80 | call | 1.0 |
| `IdentMatrix` | 0x408a20 | 0x404e70 | hash | 1.0 |
| `CondCseq/CondCsseq` | 0x42a830 | 0x42fe50 | hash | 1.0 |
| `CondCsne` | 0x42a880 | 0x42fe20 | hash | 1.0 |
| `CondCslt` | 0x42a920 | 0x42fef0 | hash | 1.0 |
| `CondCsgt` | 0x42a9e0 | 0x42ffb0 | hash | 1.0 |
| `CondTismnz` | 0x42aa30 | 0x430000 | hash | 1.0 |
| `CondCgeq/CondCggeq` | 0x42aaa0 | 0x430140 | hash | 1.0 |
| `CondCgne` | 0x42ab00 | 0x430110 | hash | 1.0 |
| `CondTigmnz` | 0x42ac70 | 0x430020 | hash | 1.0 |
| `ActionOrgi` | 0x42c8e0 | 0x430dd0 | hash | 1.0 |
| `ActionAndgi` | 0x42c920 | 0x430e10 | hash | 1.0 |
| `ActionMovss` | 0x42c940 | 0x430e30 | hash | 1.0 |
| `ActionIncss` | 0x42c9f0 | 0x430ec0 | hash | 1.0 |
| `ActionRnds` | 0x42cac0 | 0x430f70 | hash | 1.0 |
| `ActionRndg` | 0x42caf0 | 0x430fa0 | hash | 1.0 |
| `BugsRand` | 0x431150 | 0x4395c0 | call | 1.0 |
| `InitListPool` | 0x448890 | 0x4608f0 | hash | 1.0 |
| `RemoveListNode` | 0x4488d0 | 0x460930 | hash | 1.0 |
| `ClearList` | 0x448920 | 0x460980 | hash | 1.0 |
| `AppendListNode` | 0x448950 | 0x4609b0 | hash | 1.0 |
| `fopen` | 0x450740 | 0x46de50 | call | 0.74 |

### Other automated pairs (unnamed LiT functions)

| LiT 1.6 | TB | method | score | instr LiT/TB |
|---|---|---|---|---|
| 0x403b70 | 0x42c050 | fuzzy | 0.83 | 133/138 |
| 0x406ee0 | 0x4156c0 | fuzzy | 0.9 | 189/189 |
| 0x407890 | 0x403f60 | hash | 1.0 | 25/25 |
| 0x4078d0 | 0x404000 | fuzzy | 0.83 | 45/45 |
| 0x407970 | 0x404060 | fuzzy | 0.84 | 40/40 |
| 0x409e30 | 0x416290 | str | 0.98 | 94/92 |
| 0x409f60 | 0x4163b0 | str | 0.96 | 73/71 |
| 0x40a050 | 0x405820 | fuzzy | 0.88 | 96/95 |
| 0x417300 | 0x427220 | hash | 1.0 | 24/24 |
| 0x417b40 | 0x4279b0 | fuzzy | 0.92 | 81/82 |
| 0x4181b0 | 0x40acb0 | fuzzy | 0.96 | 53/57 |
| 0x4182f0 | 0x40aed0 | fuzzy | 0.82 | 74/73 |
| 0x4183c0 | 0x40afa0 | fuzzy | 0.98 | 68/71 |
| 0x4184a0 | 0x40b080 | fuzzy | 0.98 | 76/79 |
| 0x4185a0 | 0x40b190 | const | 0.98 | 95/98 |
| 0x418700 | 0x40b300 | const | 0.98 | 87/90 |
| 0x418830 | 0x40b430 | fuzzy | 0.99 | 121/124 |
| 0x419150 | 0x40bd20 | const | 0.99 | 139/142 |
| 0x4198e0 | 0x40c4c0 | fuzzy | 0.99 | 161/164 |
| 0x419b50 | 0x40c740 | const | 0.99 | 173/176 |
| 0x41c760 | 0x425a90 | hash | 1.0 | 10/10 |
| 0x41cc70 | 0x425d90 | hash | 1.0 | 75/75 |
| 0x41d050 | 0x426170 | const | 0.91 | 194/206 |
| 0x41d4b0 | 0x4290a0 | const | 0.78 | 79/77 |
| 0x41d890 | 0x407480 | hash | 1.0 | 9/9 |
| 0x41dd70 | 0x429d80 | hash | 1.0 | 12/12 |
| 0x41deb0 | 0x407080 | const | 0.93 | 122/128 |
| 0x41e1e0 | 0x429fa0 | str | 0.92 | 45/38 |
| 0x41f450 | 0x42b5c0 | hash | 1.0 | 65/65 |
| 0x41f510 | 0x42b680 | hash | 1.0 | 67/67 |
| 0x4238e0 | 0x42e390 | const | 0.67 | 106/81 |
| 0x42b480 | 0x4309d0 | hash | 1.0 | 8/8 |
| 0x42b880 | 0x430840 | hash | 1.0 | 18/18 |
| 0x42cf40 | 0x4310d0 | hash | 1.0 | 24/24 |
| 0x42cf90 | 0x431120 | hash | 1.0 | 19/19 |
| 0x42cfe0 | 0x431170 | hash | 1.0 | 14/14 |
| 0x42d010 | 0x4311e0 | hash | 1.0 | 14/14 |
| 0x42d240 | 0x403e90 | hash | 1.0 | 8/8 |
| 0x42d510 | 0x4319b0 | hash | 1.0 | 18/18 |
| 0x42d550 | 0x4319f0 | hash | 1.0 | 16/16 |
| 0x42d690 | 0x431b20 | const | 1.0 | 4/4 |
| 0x42d750 | 0x431ba0 | fuzzy | 0.81 | 39/39 |
| 0x42e370 | 0x433530 | hash | 1.0 | 11/11 |
| 0x42e3a0 | 0x433560 | fuzzy | 0.95 | 65/65 |
| 0x42e5e0 | 0x433600 | hash | 1.0 | 13/13 |
| 0x4312c0 | 0x439650 | const | 0.58 | 55/41 |
| 0x4376b0 | 0x44e690 | call | 0.96 | 25/23 |
| 0x447730 | 0x459f00 | fuzzy | 0.86 | 32/37 |
| 0x44be90 | 0x464e90 | call | 1.0 | 7/7 |
| 0x44bec0 | 0x464ec0 | call | 1.0 | 7/7 |
| 0x44fd40 | 0x46b094 | hash | 1.0 | 15/15 |
| 0x4500e0 | 0x46da70 | hash | 1.0 | 70/70 |
| 0x450190 | 0x46db20 | hash | 1.0 | 19/19 |
| 0x450bd0 | 0x46e4b0 | hash | 1.0 | 109/109 |
| 0x450cd0 | 0x46de70 | hash | 1.0 | 66/66 |
| 0x450e20 | 0x46cb10 | hash | 1.0 | 17/17 |
| 0x451310 | 0x46c8f0 | hash | 1.0 | 8/8 |
| 0x451332 | 0x46c912 | hash | 1.0 | 125/125 |
| 0x451505 | 0x46cae5 | hash | 1.0 | 19/19 |
| 0x45154d | 0x46ce0d | hash | 1.0 | 50/50 |
| 0x451dd0 | 0x47f2a0 | hash | 1.0 | 23/23 |
| 0x4520a0 | 0x4705a0 | const | 0.65 | 18/25 |
| 0x452800 | 0x46f230 | const | 0.77 | 5/8 |
| 0x454510 | 0x4722c0 | call | 0.64 | 51/58 |
| 0x4545c6 | 0x46dca0 | const | 0.99 | 87/89 |
| 0x4562be | 0x4724de | hash | 1.0 | 35/35 |
| 0x456476 | 0x472696 | call | 1.0 | 3/3 |
| 0x456540 | 0x472760 | hash | 1.0 | 9/9 |
| 0x456557 | 0x472777 | hash | 1.0 | 23/23 |
| 0x4565a0 | 0x4727c0 | hash | 1.0 | 11/11 |
| 0x4565b5 | 0x4727d5 | call | 1.0 | 6/6 |
| 0x4565cc | 0x4727ec | call | 1.0 | 7/7 |
| 0x4565e5 | 0x472805 | hash | 1.0 | 21/21 |
| 0x45663e | 0x47285e | call | 1.0 | 5/5 |
| 0x45664b | 0x47286b | hash | 1.0 | 15/15 |
| 0x456689 | 0x4728a9 | hash | 1.0 | 54/54 |
| 0x456730 | 0x472950 | call | 0.72 | 187/170 |
| 0x456b50 | 0x47bc00 | hash | 1.0 | 43/43 |
| 0x456bc0 | 0x47bc70 | hash | 1.0 | 44/44 |
| 0x457610 | 0x478f2c | hash | 1.0 | 18/18 |
| 0x457652 | 0x478f6e | hash | 1.0 | 33/33 |
| 0x4576e6 | 0x479002 | hash | 1.0 | 10/10 |
| 0x4577c5 | 0x4790e1 | hash | 1.0 | 11/11 |
| 0x457a00 | 0x479310 | call | 0.81 | 26/33 |
| 0x457a40 | 0x479360 | call | 0.84 | 8/11 |
| 0x457b90 | 0x479660 | call | 0.71 | 82/91 |
| 0x458d10 | 0x47d580 | hash | 1.0 | 15/15 |
| 0x458dc0 | 0x47a930 | hash | 1.0 | 83/83 |
| 0x4595b1 | 0x47b121 | hash | 1.0 | 8/8 |
| 0x459c70 | 0x47a6b0 | str | 0.56 | 57/54 |
| 0x45af80 | 0x47ff90 | hash | 1.0 | 66/66 |

Coverage note (*observed*): of 460 LiT game functions with ≥ 20 instructions (below the CRT at
0x44fc30), 130 have a TB function with 4-gram similarity ≥ 0.5 and 84 with ≥ 0.7; CRT functions match
poorly because TB links the VC6 *debug* CRT. Large game-logic functions were heavily edited.

## Reproduction

Ghidra 12.1.2 project: `/mnt/1st4TB/BugsBunny/ghidra/diff/bbdiff` (outside the repo). Scripts in
`tools/ghidra/`:

```sh
G=/mnt/1st4TB/BugsBunny/ghidra/diff; O=/mnt/1st4TB/BugsBunny/original; S=$PWD/tools/ghidra
/opt/ghidra/support/analyzeHeadless $G bbdiff -import $O/lit/Datas/bin/bugs.exe $O/tb/Datas/BIN/bblit2.exe $O/lit/Datas/bin/hv3dfx.dll
for p in bugs.exe bblit2.exe; do
  /opt/ghidra/support/analyzeHeadless $G bbdiff -process $p -scriptPath $S \
    -preScript CreateFunctionsFromDataPointers.java -postScript CreateFunctionsFromDataPointers.java \
    -postScript LabelGLPointers.java -postScript ExportFunctionFeatures.java $G/out -postScript DecompileAll.java $G/out
done
python3 $S/match_functions.py $G/out/bugs.exe.features.tsv $G/out/bblit2.exe.features.tsv \
  /mnt/1st4TB/BugsBunny/research/BugsDecomp/doc/src/funcs.csv > $G/out/pairs.tsv
(cd $S && python3 align_logic_tables.py $O/lit/Datas/bin/bugs.exe $O/tb/Datas/BIN/bblit2.exe \
  $G/out/bugs.exe.features.tsv $G/out/bblit2.exe.features.tsv) > $G/out/logic_align.tsv
```

| script | purpose |
|---|---|
| `CreateFunctionsFromDataPointers.java` | create functions reached only via data tables / `mov [x], imm` (LiT rasterizer 0x4124b0 is otherwise missed) |
| `LabelGLPointers.java` | name GL/WGL/GLU pointer slots `p_glXxx` (LiT table and TB GetProcAddress chain) |
| `ExportFunctionFeatures.java` | per-function TSV: size, mnemonic hash, callees, imports, strings, immediates, mnemonics |
| `DecompileAll.java` | all (or listed) functions → `<program>.c`, grep-able `//==== <addr> <name>` headers |
| `match_functions.py` | LiT↔TB function pairing |
| `align_logic_tables.py` | logic-VM handler table alignment (needs `pefile`) |

Constants quoted in the docs can be re-read with e.g.
`python3 -c "import pefile,struct;p=pefile.PE('bblit2.exe');print(struct.unpack('<f',p.get_data(0x48160c-0x400000,4)))"`.
