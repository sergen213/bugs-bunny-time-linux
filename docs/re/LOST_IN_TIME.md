# Bugs Bunny: Lost in Time — `bugs.exe` v1.6 static RE notes

Status: **Researched (static RE only)**; not *Verified Against Original Behavior*. Tags: *observed* (bytes /
decompile at the cited address) vs *inferred*. This file only adds to BugsDecomp
(`research/BugsDecomp/doc/{functions,internals,bze}.md`, v1.6 addresses; no LICENSE in that repo → facts
and addresses only). Engine comparison and function map: [ENGINE_COMPARISON.md](ENGINE_COMPARISON.md).

| file | MD5 | SHA-256 |
|---|---|---|
| `original/lit/Datas/bin/bugs.exe` (772096 bytes, BugsDecomp "1.6") | `121db63d76ed5077d69837b111163cac` | `6e15f92076614e3ee218c34c5dee566bf273e5bfee77d831cfbf78aa2d29e235` |
| `original/lit/Datas/bin/hv3dfx.dll` (961024 bytes) | `fadfd3943a1afa0e9186e9b2efdd26a8` | `4b2444e619467f3a4ee16a7376a4c07cde78dee0e18ce3ee9f171d7fbed55dc3` |

## 1. Init and version

*Observed:* VERSIONINFO `CompanyName "Infogrames"`, `FileVersion/ProductVersion "b1.6"`,
`OriginalFilename "bugs.exe"`; PE timestamp 1999-09-26 22:56:12 UTC; linker 5.00 (MSVC 5), release CRT,
no Rich header. Debug source paths survive: `D:\Projets\Bugs\src\Pcrogl.c`, `D:\Projets\Bugs\src\pcrsoft8.c`.
WinMain 0x405950: `FindWindowA("BBLIT Game")` single instance, bugs.ini 0x409e30, `ReadConfig` 0x409cc0,
language → resource-string block (6 languages, resource strings 1–24), registry
`HKCU\Software\Infogrames\Bugs Bunny Lost In Time`. Startup screens and timer init 0x406190 (three 3 s
splash screens, BZE IDs 0x4e–0x50 = `screen1..3`). Command-line parser 0x408f70 (switches in BugsDecomp
`internals.md`; LiT checks upper- and lower-case spellings, TB lower-cases instead).

`hv3dfx.dll` is the 3dfx OpenGL ICD 1.0.0.0438 (`3dfxogl.dll`, imports `glide3x.dll`), loadable via `/dll:`;
not game code.

## 2. BZE loader (0x42e7a0)

*Observed*, matching BugsDecomp `bze.md`: header read (0x800 bytes) to `workspace + 0x180000`, signed-byte
checksum of 0x7fc bytes vs i32 @0x7fc, `u32 @0 → _DAT_004b3e3c` (=1 in files), `u32 @4` = count,
`{id, actual, fileSize}` from offset 8. Every section is LZSS-decoded (0x430ff0) — there is no raw path.
Section 1 decodes to `workspace + 0x180000`; the asset section (after optional `'-' 'E'` pseudo-chunk
sections) to the **fixed** address `workspace + 0x1ad000` (TB makes this offset per-level, header +4).
Language command `'K'` (0x4b) selects section 6,5,7,8,9,10 for language 0..5 via 0x42e3a0. Level IDs
7, 8, 0x15–0x19, 0x22, 0x36 get the `_8` file-name variant unless a language override is active
(code at the start of 0x42e7a0; matches `internals.md`).

## 3. Renderer

*Observed:* renderer from `config.pc` (0 soft 8-bit, 1 soft 24-bit, 2 OpenGL). Software paths use DirectDraw
(`DirectDrawCreate` in 0x409730/0x40e790/0x41a8f0) and `StretchDIBits` (0x417300, 0x41b110). OpenGL is
loaded through the 360-entry `{PROC*, name}` table at 0x468750 (BugsDecomp `rogl.c`); `SetGLPixelFormat`
0x40df20 asks for a 16-bit RGBA double-buffered format; present via `SwapBuffers` (0x40e270).

GL usage (`LabelGLPointers.java` + raw slot xrefs): `glOrtho(-w/2, w/2, -h/2, h/2, 400.0, 65536.0)` in
0x40daf0 and 0x413990 (`DAT_00467748`/`DAT_0046774c` = width/height); primitives are flushed by 0x4124b0
and 0x413be0 with `glBegin` + `glColor4f`/`glColor3f` + `glTexCoord2f` + `glVertex3fv` (31 call sites) /
`glVertex3f`. No transform/fog/array GL calls exist. **Vertices are CPU-projected screen-space (x, y) with a
linear depth z; texturing is affine.** 0x4124b0 is reached only through a pointer stored by code at 0x40ebbc,
which is why Ghidra's auto-analysis missed it (`CreateFunctionsFromDataPointers.java` recovers it).

## 4. Timing

*Observed:* identical to TB (see TIME_BUSTERS.md §5): `period = QPF × 0.033333335f` (f32 @0x45c26c, 1000.0f
@0x45c268, computed inline in 0x406190), pacer 0x406ee0, skip flag `DAT_004b1d00`, extended-skip flag
`DAT_004b2277`, `/no_skip` flag `DAT_004b1900`, sync flag `DAT_0045f2a4`, frame counter `DAT_004b24a4 += 2`
in the level loop 0x4065e0. **Fixed 30 Hz simulation, render locked to the tick with dropped frames.**

## 5. Input

*Observed* per BugsDecomp (`EnumDInputDevs` 0x402000 … `GetButtonName` 0x403260): DirectInput keyboard +
joystick mapped onto an emulated PSX pad. No `DINPUT.dll` import; *inferred* that DirectInput is created via
`ole32!CoCreateInstance`. One player; `config.pc` holds one keyboard map (16 scancodes) and one joystick map.

## 6. Save / config

*Observed:* `..\bin\Savegame%d.dat` (0x407190, 0x4074f0; slot from `*(u8*)(DAT_0052fd00+0x100df)`),
`..\bin\Savedata.dat` (0x4074f0, 0x4076f0), `..\bin\config.pc` (128 bytes, layout in `internals.md`),
`..\bin\bugs.ini`.

## 7. Aspect ratio / FOV globals (screen setup 0x40d1a0, called as `(w, h, 0x200, 0xF0)` from 0x40e790)

| global | meaning (same formulas as TB, see TIME_BUSTERS.md §9) | TB equivalent |
|---|---|---|
| 0x4efb4c / 0x4efb4e | u16 w / h | 0x639a84 / 0x639a86 |
| 0x4efb50 / 0x4efb52 | u16 w/2, h/2 | 0x639a88 / 0x639a8a |
| 0x4efb54 / 0x4efb56 | s16 Q12 `sx = 4096·w/512`, `sy = 4096·h/384` | 0x639a8c / 0x639a8e |
| 0x4efb58 / 0x4efb5a | u16 reference 512 / 240 | 0x639a90 / 0x639a92 |
| 0x4efb5c | s16 Q12 `4096·240/384` | 0x639a94 |
| 0x4efb7c / 0x4efb80 | f32 w/512, h/240 | 0x639ab0 / 0x639ab4 |
| 0x4efb84 / 0x4efb88 | f32 tier pair (2.0/1.5/1.0/0.85 …) | 0x639ab8 / 0x639abc |
| 0x4efb3c / 0x4efb40 | i32 0 / 10000 | f32 0x639a80 = 0, f32 0x639ac0 = 10000.0 |
| 0x4efb30 / 0x4efb34 / 0x4efb38 | `GsSetProjection` 0x40cd20: H (int, default 400 from 0x40dd40/0x4478b0), (float)H, −(H/2) | 0x639a70 / 0x639a74 / 0x639a7c |
| .rdata 0x45c2c4 / 0x45c2c8 | f32 0.75 / 4096.0 | 0x48130c / 0x481308 |
| .rdata 0x45c298 / 0x45c2a8 / 0x45c2b8 | f64 2.0 / 1.5 / 1.0 tier thresholds | 0x481318 / 0x481310 / 0x4812e0 |

Consumer of sx/sy: 0x408910 (Q12 scale × `RotMatrixX(0x800)` × camera, TB 0x404d40). Derived FOV is the
same as TB: ≈ 65.2° × 51.3° at H = 400, stretched (not widened) on non-4:3 outputs.
