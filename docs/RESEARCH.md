# Research: prior art, executables, and original data

Status legend (per project rules): **Research Required** / **Designed** / **Implemented** / **Tested** / **Verified Against Original Behavior**.
Confidence: **High** = read directly in source/binary/data; **Med** = single secondary source or partial evidence; **Low** = inference.
`[LOCAL]` = verified by us against the files under `/mnt/1st4TB/BugsBunny/original` (read-only). `[INFERENCE]` = our reading, not stated by a source.

No third-party code is copied into this repository. Everything below is facts (addresses, sizes, field layouts, behaviour), restated in our words.

Sub-documents:
- [research/web.md](research/web.md): PCGamingWiki, WSGF, PCGW community fixes, ModDB, other RE projects, developer attribution (all with URLs).

---

## 1. Executables and hashes `[LOCAL]` (High)

| File | Size | MD5 | SHA-256 | PE timestamp (UTC) | Entry | Version resource |
|---|---|---|---|---|---|---|
| LiT `Datas/bin/bugs.exe` | 772 096 | `121db63d76ed5077d69837b111163cac` | `6e15f92076614e3ee218c34c5dee566bf273e5bfee77d831cfbf78aa2d29e235` | 0x37EEA48C = 1999-09-26 22:56:12 | 0x451EB0 | Company "Infogrames", FileVersion "b1.6", OriginalFilename bugs.exe |
| TB `Datas/BIN/bblit2.exe` | 880 640 | `d7445216f8cabab1ea33ba1ad029ef7b` | `63483ea5205b8982e384e0b1863624cb8df4fce1601d0cd74393e5b69ecf78c1` | 0x3A14EAA8 = 2000-11-17 08:22:00 | 0x46F070 | Company "Behaviour Interactive", FileVersion "1, 0, 3, 0", **InternalName "bblit2"**, Description "Bugs & Taz: Time Busters" |
| LiT `Datas/BugsBunny.exe` (launcher) | | `40547596aab302ec241cd1bedfdc72cd` | | | | |
| LiT `Datas/bin/hv3dfx.dll` | 961 024 | `fadfd3943a1afa0e9186e9b2efdd26a8` | | | | "1.1.0 3Dfx ICD (Apr 29 1999)" |
| TB `Datas/BIN/WKeyKill.dll` | | `450180d360ec4e0d93531e8aa8e92b14` | | | | |

PE sections `[LOCAL]`:
- LiT: `.text` 0x401000 (raw 0x5A400), `.rdata` 0x45C000, `.data` 0x45E000–0x9CBB0C (raw 0x53A00; the rest is BSS), `.idata` 0x9CC000, `.rsrc`, `.reloc`. Imports: KERNEL32, USER32, DSOUND, GDI32, ADVAPI32, ole32, **DDRAW**. OpenGL is loaded dynamically (`opengl32.dll`, overridable with `/dll:`).
- TB: `.text` 0x401000 (raw 0x80000), `.rdata` 0x481000, `.data` 0x484000–0x63D49C (raw 0x4D000), `.rsrc`. Imports: KERNEL32, USER32, GDI32, ADVAPI32, VERSION, AVIFIL32, WINMM, DINPUT, DSOUND. **No DDRAW import** (relevant to §2.9).

Version identification of LiT:
- BugsDecomp detects the version from the 8 bytes at VA 0x45CA00 (`GameVersion.cpp`): `0x3FE2D2D2D2D2D2D3` = v1.0, `0x3FE5555555555555` = v1.6. It lists v1.0 MD5 `59731F0354F8A66E63502BD3B521BD82` and v1.6 MD5 `121DB63D76ED5077D69837B111163CAC`. **Our exe is the BugsDecomp v1.6 build** (High).
- BBLIT-Remaster's reference "1.6" is a different file: MD5 `6672ce79f18cf5110e00059d5f031a35`, SHA-256 `7342f724…512139`, `.text` CRC32 `ecffb7c9` (Russian "CITY" edition per its README).
- We ran its read-only checker (`tools/verify_exe.py`, Python, does not modify the input) on our exe. Results: same size, same marker, all 29 hook-site byte patterns and all 7 IAT slots match. **Only the `.text` CRC32 differs: ours is `0xc1d00325`.** So the code layout is the same, but some code bytes differ. Which bytes, and why (disc-check patch? localisation?), is unknown. BBLIT's source comments mention an unexplained "hpRT" build that has the same marker but a different CRC. Because of the CRC mismatch, BBLIT-Remaster would refuse our exe and run DirectDraw pass-through only (High).
- The BBLIT "Spanish executable" report gives marker `3fe2d2d2d2d2d2d3`. That is BugsDecomp's **v1.0** marker, so the Spanish exe is a v1.0 layout `[INFERENCE, High]`. BBLIT's own doc does not draw this link.
- The LiT `Help/Readme0.txt` says "Version 1.0", while the exe resource says "b1.6". The readme was evidently not updated.

Source-path strings in LiT: `D:\Projets\Bugs\src\Pcrogl.c`, `D:\Projets\Bugs\src\pcrsoft8.c`. These are a French-language path and per-renderer C modules (OpenGL and 8-bit software). Both exes keep PS1 ISO-9660 names such as `..\BZE\L01A.BZE;1`, `..\XA\MUSIC.XA;1` (LiT) and `..\BZE\MUSIC.XA;1` (TB).

---

## 2. BBLIT-Remaster (`research/BBLIT-Remaster`)

- URL: https://github.com/InLineR495/BBLIT-Remaster. **License: MIT** (© 2026 InLineR495). v1.0-beta. Comments are in Russian.
- Its own notes directory (`re/notes/*.md`, DESIGN §…) is referenced in comments but **not in the clone**.

### 2.1 What it is (High)
- **It is a proxy `ddraw.dll`, built with i686 MinGW, and runs inside the original 32-bit Windows process.** Under Linux it needs Proton/Wine with `WINEDLLOVERRIDES="ddraw=n,b"`. It is **not** a native port. `bugs.exe` imports DDRAW.dll statically, so the proxy loads before `main` (`src/dllmain.c`).
- `DllMain` checks the host:
  - image base 0x400000;
  - the marker at 0x45CA00;
  - the `.text` CRC32.
- If the checks pass, it detours the PE entry 0x451EB0 to a "LateInit" running on the main thread before the CRT. The stolen bytes are re-executed and execution resumes at 0x451EB5. Otherwise it forwards DirectDraw unchanged. It also forwards 5 legacy exports that `d3dim.dll` needs.
- Every patch site is verified against its expected bytes before writing (`src/hook.c`, `src/addr.h`).
- Required launch: `bugs.exe /opengl /win /b0 /x800 /y600`, working dir `BIN`. Proton also needs `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1 PROTON_NO_ESYNC=1`. The README says the **unmodded original also freezes** with Wine's newer sync primitives.

### 2.2 How it intercepts rendering (High)
- The proxy is only a loader vehicle. The real interception is the engine's **OpenGL** path:
  - It hooks `GetProcAddress`/`LoadLibraryA` (IAT).
  - It hands the engine counting stubs for every opengl32 entry point.
  - It wraps the **36 GL/WGL functions the engine actually uses** (`src/glrec.c`). These are immediate-mode only: `glBegin/glEnd/glVertex3f(v)/glColor3f/4f/glTexCoord2f`, `glOrtho`, `glBindTexture/glTexImage2D/glDeleteTextures`, `glBlendFunc/glAlphaFunc/glDepthFunc`, enable/disable, `wglSwapBuffers`, etc.
- **hv3dfx.dll is not involved.** It is a 3Dfx OpenGL ICD/"stand-alone" MiniGL v1.1.0 (Apr 1999) that imports `glide3x.dll` `[LOCAL]`. The game uses it only when told to via `bugs.ini DLL=` or `/dll:` (Readme0.txt). TB instead names `3dfxvgl.dll` and the SGI software OpenGL `[LOCAL]`.
- The engine picks a "driver profile" from `glGetString(VENDOR/RENDERER)` at 0x422050. The match list is `3Dfx, RENDITION, NEC, "", NVIDIA, ATI, Direct3D, Real3D`. **"Direct3D" here is a GL vendor string, not a D3D renderer.**
- Per logic tick, BBLIT records the engine's primitives and state into a **snapshot**: vertices (x, y, z, u, v, rgba, object key, local index, tag), primitives (tri/quad) and GL state, with max 98 304 verts and 32 768 prims. It suppresses the engine's own draw calls and swap, then draws with its own GL 4.6 context. That context shares a share-group with the engine context, uses an MSAA FBO, and blits in the engine's context because of Wine's per-context surfaces.

### 2.3 Geometry: CPU-transformed, screen-space, reconstructed (High; usable knowledge)
- The PC port keeps PS1 behaviour. The engine transforms vertices on the CPU with a high-level **PS1 GTE emulation** (Q12 fixed point; see BugsDecomp `gte.*`). It writes **screen X, Y plus view-space Z** as floats into a vertex pool: 0x734DE0–0x7BD960, 35 000 × 16 bytes, index at 0x623654. It then submits them through `glOrtho` immediate mode, so the GL path never sees a 3D projection.
- Primitives are sorted, PS1 ordering-table style (sort entries at 0x604130, capacity 2000; arena `malloc(1 000 000)` pointer at 0x4E5E44). Quads use the PS1 "zigzag" vertex order (second half = v1, v3, v2).
- Reconstruction (`src/render.c` header): clip coords are rebuilt as x_c = x_ndc·d, y_c = y_ndc·d, w_c = −Z, where **d = (Z ≥ 0 ? Z + 1 : −Z)**, the same divisor as the engine function 0x40FA90. This gives perspective-correct UVs and real near-plane clipping. Screen-space writers (text, HUD, overlays, letterbox bars) stay in the engine's ortho.
- Camera at build time: `v = C·(w − eye)`. C = MATRIX at 0x4EFAE0 (Q12, = S·R), −eye at 0x4EFB20, eye int[3] 0x4B38C0, target 0x4B38CC, GTE M 0x4E5E60, camera flags 0x4B3940. `probe_cam.c` reads these and checks W = C⁻¹·M for actor parts.
- Vertex identity for interpolation is a key **(object, local index)**. The object is the room block source pointer, the actor part pointer, the shadow owner actor, or the billboard actor plus its ordinal. Writers are identified by render-dispatch slots: C2 rooms 0x40FA90, C4 parts 0x411350, C6 billboards 0x40A6C0, C7 text 0x40B130, overlays 0x40B980/0x40BE10, bars 0x40C5E0, HUD bar 0x40C850 (`src/mark.c`).
- Tools (Python, operate on BBLIT's own runtime dumps, not on game files):
  - `dump_faces.py` matches A/B faces by key sequence and diffs uv, colour, texture and position.
  - `dump_uv.py` gives per-texture UV changes between ticks (UV animation) and jump detection.
  - `dump_objects.py` gives per-object vertex, texture, ABR and range stats.
  - `repair_check.py` checks affine/kNN reconstruction of unpaired vertices.
  - `trace_motion.py` / `trace_raster.py` analyse interpolation smoothness.
  - `cap_flicker.py` finds brightness spikes in captured frames.
  - `sweep_report.py` reports per-level capacity peaks. Its level-name table matches BugsDecomp's level IDs.
  - `build_spanish.py` builds an address-remapped profile.
  - `verify_exe.py` is described in §1.

### 2.4 Frame pacing vs simulation tick (High)
- **The logic runs at a fixed 30 Hz.** The README says: "position, camera and animation advance once per frame, so raising the cap just speeds the game up".
- The pacer function at **0x406EE0** keeps an accumulating QPC deadline: int64 ticks-per-frame at 0x7C95C8, last deadline at 0x7C95E8, and a sync-enabled flag at 0x45F2A4 that is 0 with `/no_sync`. When the engine falls behind it skips builds (build counter 0x4B18FC, skip counter 0x4B18F8). FRAMETICK is at 0x4065E0.
- BBLIT draws frames in between ticks by interpolating the two latest snapshots in view space. The display trails logic by 1 tick (≈33 ms). α is computed from the predicted vblank display time.
- Some objects animate on alternate ticks (15 Hz). BBLIT detects the move/still pattern and stretches the step. Rotating translucent effects (hub vortex) can jump 90–180° per tick; these are drawn stepped or re-spun at a smoothed rate.
- For a native engine: keep a 30 Hz fixed tick plus render interpolation. This is consistent with the PCGW "locked at 30 FPS" note for **both** games (see web.md).

### 2.5 Transparency / texture fixes (High; usable knowledge)
- The PC port emulates the PS1 semi-transparency modes **ABR 0..3** by converting a per-mode copy of each texture (0x420D90; variant table `0x52FD74 + 48·id + 4·abr`, emitter 0x41493A) and drawing everything with `SRC_ALPHA`. This flickers and is wrong.
- BBLIT undoes the conversion (inverse gamma 1/1.2 and a max-offset recovered from the variant alpha) and applies the real PS1 equations:
  - ABR 0: B/2 + F/2
  - ABR 1: B + F
  - ABR 2: B − F (reverse subtract)
  - ABR 3: B + F/4
- **Texels without the STP bit stay opaque** inside semi-transparent polygons.
- Semi-transparent primitives do not write depth and get a small depth bias, because the PS1 had no Z-buffer.
- Vertex colour: the PC port divides by 255. On the PS1 **0x80 is neutral**, so the PC world is about half as bright. BBLIT uses ×1.8 (×2.0 is exact PS1).
- UV clamp is needed on alpha-blended faces, and centroid UVs under MSAA.
- Flat ground "decals" (shallow pits) rely on PS1 draw order. With a Z-buffer they must be drawn after the ground without depth writes.
- Backface culling of room quads (0x40FDC0) tests only one triangle half. That produces cracks (sky visible) on non-planar quads, so cull only when both halves face away.
- For a native renderer, use **PS1 semantics directly from the asset data** (TIM CLUT STP bit, ABR from primitive flags). None of the PC port's variant conversion is needed.

### 2.6 Widescreen / FOV (High)
- Hor+: during the engine's frame build, BBLIT shrinks the camera sx (Q12 word at **0x4EFB54**) by F_cull = (4/3)/aspect/(1 + guard), so the engine does not cull geometry outside 4:3. It then un-squeezes in its own render with the vertical FOV unchanged.
- Engine-side edges that had to be widened (`src/edges.c`, `src/fog.c`):
  - The actor on-screen test in the ActorManager is X ±0x100 (H1, 0x4481C7…0x4484BD; uses the non-squeezed matrix 0x52FCE0). ±256 = half of the PS1 512-wide screen, consistent with BBLIT-Findings #297 in web.md.
  - Portal tests against ±w/2 and h/2 globals 0x621610 / 0x621614 / 0x62161C.
  - Vertical clip against the h word 0x4EFB4E.
  - Quad-split flag at 0x414C0A.
- `[LOCAL]` The projection-setup function is at **LiT ~0x40D1A0** and is structurally identical to **TB 0x405EF0**. It:
  - stores w, h, w/2, h/2;
  - computes the aspect ratio and picks a constant pair by thresholds 2.0 / 1.5 / 1.0;
  - computes **sx = f·4096 and sy = (f′·4096·sx) >> 12 using the hard-coded constant 0.75 (3:4)**.
  - LiT constants are at 0x45C2C4 = 0.75 and 0x45C2C8 = 4096.0; TB's are at 0x48130C and 0x481308.
- Global map LiT → TB:

  | LiT | TB | Meaning |
  |---|---|---|
  | 0x4EFB4C/4E | 0x639A84/86 | w, h |
  | 0x4EFB50/52 | 0x639A88/8A | w/2, h/2 |
  | 0x4EFB54/56 | **0x639A8C/8E** | **sx, sy (Q12)** |
  | 0x4EFB58/5A | 0x639A90/92 | |
  | 0x4EFB7C…88 | 0x639AB0…BC | aspect, constant pair |
  | 0x4EFB40 = 10000 (int) | 0x639AC0 = 10000.0f | |

  The scale-matrix user diag(sx, sy, 0x1000) is at LiT 0x408910 and TB 0x404D40. The two are byte-pattern twins.

### 2.7 Fog / draw distance (High)
- 0x40CC80 is the only writer of fog parameters: D = min(0xFFFF, b + K·u) with K = (9·fogDist) >> 1. fogDist is `config.pc`+0x1C (0 near, 1 normal, 2 far); u is a per-level word at 0x4B38A0. Derived globals: 0x4EFB40 = D, 0x4EFB48 = −D (**float**), 0x4EFB60 = −0.75·D, 0x4EFB64 = 1/(0.25·D). So the fog ramp covers the last 25 % of D.
- Runtime (L01A, fogDist 2, 640×480; RUNTIME_REFERENCE.md §5): u = 1764, D = 39876 ⇒ base **b = 24000**; 0x4EFB48 = −39876.0 (f32), 0x4EFB60 = −29907.0, 0x4EFB64 = 1.00311e-4 — formula **Verified**.
- Separately, **actor activation distance** is word `actor+0x3E` (≈6–7k units), compared at 0x447F3A. Beyond it, flagged actors (`+0x3C & 2`) are neither updated nor drawn, independent of fog. The sleep timer at `+0x24` is (dist − edx) >> 7.
- Room/portal traversal depth is limited by `room+0xB5` (0x410F6C).
- Hard capacity limits:
  - part pool capacity = word at 0x4B2244 = (BZE value @2) + 100; exhaustion **spins forever** (`jmp $`);
  - **40 shadows** (the 41st hangs the stock game; 0x44C981 → 0x44CB77);
  - vertex pool 35 000; sort list 2000.
- No-fog condition (0x44CB90): type 9 (sky backdrop) or flag 0x20000000 together with 0x2000000.

### 2.8 Input (High)
- The engine builds a **PS1 controller report**. The pointer is at 0x4B18D8 (buffer 0x4B31C0). The joystick path is `MapJoystickInput` 0x403010, called from 0x449223; `PadUpdate` is 0x4491C0, called from 0x40660B.
- Analog: the game computes bx = (|v| − 50)·2, with **run at bx ≥ 0x78**.
- Camera yaw step: 0x1000 = 360°. Triggers turn 0x26 per tick (0x4323CE branch, camera flags 0x4B3940).
- `config.pc` joyMap is at 0x9CA85C (= config+0x3C), useJoystick at 0x9CA858. Pause byte is 0x4B226C.
- BBLIT adds XInput, radial dead-zone, jump buffer and Start/Back mapping. These are design ideas only.

### 2.9 Other observed engine facts
- Renderer reinit 0x40E790 is triggered by writing 0x13 to 0x45F2B0. The video-menu apply slot is 0x4AC8B8 → 0x42E050; it calls WriteConfig 0x409A10. Video globals: W 0x9CA830, H 0x9CA834, fullscreen 0x9CA838, renderer 0x9CA824. These match the BugsDecomp `CONFIG_PC` at 0x9CA820.
- Level load is at 0x42E7A0. The current level id is `[[0x52FD00] + 0x10000]`, which is the BugsDecomp `GAME_DATA.currentLevel`.
- ActorSpawn is 0x448D40 and ActorFree 0x447A50. The player pointer is 0x4B324C, and the player world position is `[[0x4B324C] + 0xE4]` (world "up" is −Y).
- The pushable-stone collision box (actor type 5) is offset from its mesh (170 vs 240 units). The README says the PS1 version has the same bug.
- The title logo is a flat in-scene actor (z ≈ −2140), and the time machine pokes through it in the original.
- **TB applicability:** `bblit2.exe` does not import DDRAW, so the proxy approach would not load there without another vehicle. The addresses are LiT-only.

### 2.10 Classification

| Finding | Native renderer | Depends on the original process |
|---|---|---|
| 30 Hz fixed tick, interpolate rendering only | ✅ design rule | pacer hook |
| Screen-space + view-Z vertex format, divisor d(Z) | ✅ explains original output; our engine transforms in 3D itself | snapshot recorder |
| PS1 ABR equations, STP opacity, 0x80-neutral vertex colour, no depth-write for translucent | ✅ | variant un-conversion |
| Quad zigzag order, both-halves backface rule, decal ordering | ✅ | detour of 0x40FDC0 |
| Hor+ with fixed vFOV; **logic visibility tests stay 4:3 / 512-wide** | ✅ design rule | sx squeeze, H1 patches |
| Fog ramp = last 25 % of D, D formula, fogDist 0/1/2 | ✅ | 0x40CC80 detour |
| Actor activation radius `+0x3E`, sleep timer | ✅ (gameplay-relevant; keep identical) | AR patch |
| Capacity limits (40 shadows, part pool, 35k verts) | ⚠ reproduce limits only where gameplay depends on them; never hang | — |
| Analog run threshold, camera step 0x26/tick | ✅ | input hooks |
| Addresses, IAT slots, CRC checks, Wine/Proton sync workaround | ❌ | ✅ |

---

## 3. BugsDecomp (`research/BugsDecomp`)

- URL: https://github.com/quantumdude836/BugsDecomp. Author Drew McGowen. Last commit `0dd943b`, 2024-04-04.
- **No LICENSE file, and no license or copyright notice in the repo** `[LOCAL]`. Treat it as **all rights reserved**: we use facts only and copy no code.
- What it is: a Visual Studio C/C++ DLL that a launcher **injects into the running original `bugs.exe`**. It patches decompiled functions over the originals with jumps (`PATCH_CODE(addr_1_0, addr_1_6, fn)`). The decompiled C calls the game's own CRT and reads and writes absolute game addresses. **Every source file is Windows- and memory-layout-dependent.** The reusable parts are the documented semantics.
- Docs: `doc/bze.md`, `doc/internals.md`, `doc/functions.md` (generated from `doc/src/funcs.csv`, which also has a PSX US address column).

### 3.1 Recovered functions (v1.0 / v1.6 VA), by subsystem

`*` = several equivalent functions were merged into one address in the PC build. "BBLIT" rows come from BBLIT-Remaster `addr.h`/comments, v1.6 layout, and are not decompiled.

**BZE / level loading / file system**

| Purpose | 1.0 | 1.6 | Source |
|---|---|---|---|
| FsOpen (binary open; retry relative to the data root, stripping `..\`) | 0x4056C0 | 0x4056C0 | BugsDecomp |
| FsFOpen | 0x405760 | 0x405760 | BugsDecomp |
| FsSeek / FsRead / FsClose | 0x405800 / 0x405820 / 0x405840 | same | BugsDecomp |
| FsFindRoot (scans CD drives for volume label "BBLIT") | 0x405850 | 0x405850 | BugsDecomp |
| Level load | — | 0x42E7A0 | BBLIT |
| BZE base/size/limit globals | — | 0x4B2364 / 0x4B38A4 / 0x4B372C | BBLIT probe.c |
| LZSS decompressor, section-1 tag interpreter | not located in either project's address tables | | **Research Required** |

**LZSS / section-1 tags / entities:** the algorithm and the tag tables are documented in `bze.md` (summary in §3.3). No function addresses are given.

**Level logic (conditions/actions)**: dispatchers `TestLogicCond` 0x42A850 / 0x42A7D0 and `RunLogicAction` 0x42C760 / 0x42C6D0.

| Cond | 1.0 | 1.6 | | Action | 1.0 | 1.6 |
|---|---|---|---|---|---|---|
| CondTrue | 0x42A880 | 0x42A800 | | ActionNop | 0x42D400* | 0x417010* |
| CondCiseq | 0x42A890 | 0x42A810 | | ActionIncs / Incg | 0x42C790 / 0x42C7B0 | 0x42C700 / 0x42C720 |
| CondCseq, CondCsseq | 0x42A8B0* | 0x42A830* | | ActionMovsi / Movgi | 0x42C800 / 0x42C820 | 0x42C770 / 0x42C790 |
| CondCisne / Csne | 0x42A8E0 / 0x42A900 | 0x42A860 / 0x42A880 | | ActionDecs / Decg | 0x42C840 / 0x42C860 | 0x42C7B0 / 0x42C7D0 |
| CondCisltu / Cislt | 0x42A930 / 0x42A950 | 0x42A8B0 / 0x42A8D0 | | ActionClrs / Clrg | 0x42C8C0 / 0x42C8E0 | 0x42C830 / 0x42C850 |
| CondCsltu / Cslt | 0x42A970 / 0x42A9A0 | 0x42A8F0 / 0x42A920 | | ActionNegs / Negg | 0x42C900 / 0x42C920 | 0x42C870 / 0x42C890 |
| CondCisleu | 0x42A9D0 | 0x42A950 | | ActionOrsi / Orgi | 0x42C950 / 0x42C970 | 0x42C8C0 / 0x42C8E0 |
| CondCisgtu / Cisgt | 0x42A9F0 / 0x42AA10 | 0x42A970 / 0x42A990 | | ActionAndsi / Andgi | 0x42C990 / 0x42C9B0 | 0x42C900 / 0x42C920 |
| CondCsgtu / Csgt | 0x42AA30 / 0x42AA60 | 0x42A9B0 / 0x42A9E0 | | ActionMovss / Movgg | 0x42C9D0 / 0x42C9F0 | 0x42C940 / 0x42C960 |
| CondCisgeu | 0x42AA90 | 0x42AA10 | | ActionMovgs / Movsg | 0x42CA20 / 0x42CA50 | 0x42C990 / 0x42C9C0 |
| CondTismnz | 0x42AAB0 | 0x42AA30 | | ActionIncss / Incgg | 0x42CA80 / 0x42CAB0 | 0x42C9F0 / 0x42CA20 |
| CondTisaz, CondTismz | 0x42AAD0* | 0x42AA50* | | ActionRnds / Rndg | 0x42CB50 / 0x42CB80 | 0x42CAC0 / 0x42CAF0 |
| CondCigeq | 0x42AAF0 | 0x42AA70 | | ActionAddsi / Addgi | 0x42CED0 / 0x42CEF0 | 0x42CE40 / 0x42CE60 |
| CondCgeq, CondCggeq | 0x42AB80* | 0x42AAA0* | | ActionSubsi / Subgi | 0x42CF50 / 0x42CF70 | 0x42CEC0 / 0x42CEE0 |
| CondCigne / Cgne | 0x42AB20 / 0x42AB50 | 0x42AAD0 / 0x42AB00 | | ActionGvcrt (give carrot) | 0x42D1A0 | 0x42D110 |
| CondCigltu / Ciglt / Cgltu | 0x42ABB0 / 0x42ABD0 / 0x42AC00 | 0x42AB30 / 0x42AB50 / 0x42AB80 | | | | |
| CondCigleu / Ciggtu / Ciggt | 0x42AC30 / 0x42AC50 / 0x42AC70 | 0x42ABB0 / 0x42ABD0 / 0x42ABF0 | | | | |
| CondCggtu / Ciggeu / Tigmnz | 0x42ACA0 / 0x42ACD0 / 0x42ACF0 | 0x42AC20 / 0x42AC50 / 0x42AC70 | | | | |
| CondTigaz, CondTigmz | 0x42AD20* | 0x42ACA0* | | | | |
| CondTisxz / Tigxz | 0x42B370 / 0x42B390 | 0x42B2F0 / 0x42B310 | | | | |
| CondTbdact / Tbuact (button down/up "action") | 0x42B3C0 / 0x42B3E0 | 0x42B340 / 0x42B360 | | | | |
| CondTpnmv / Tpmov (player not moving / moving; not decompiled) | 0x42B450 / 0x42B490 | 0x42B3D0 / 0x42B410 | | | | |
| CondTkdesc (Esc key; PC-only) | 0x42C700 | 0x42C670 | | | | |

Semantics are in `bze.md` "Level Logic":
- **scratch** is a 256-byte per-entity temp array; **global** is the 256-byte `logicGlobals` stored in each save slot;
- conditions take two u8 args, actions two u16 args;
- carrot-count updates handle the 16-bit golden-carrot total split across bytes 0x07 and 0xFC;
- `BugsRand` is at 0x4312C0 / 0x431150.

**Rendering / math**

| Purpose | 1.0 | 1.6 | Source |
|---|---|---|---|
| PS1 GTE/libgte emulation: SquareRoot0, ratan2, rcos, rsin | 0x4079E0, 0x407A00, 0x407A30, 0x407A50 | 0x4079C0, 0x4079E0, 0x407A10, 0x407A30 | BugsDecomp |
| RotMatrix, RotMatrixYXZ / ZYX / X / Y / Z | 0x407A70…0x407F20 | 0x407A50…0x407F00 | BugsDecomp |
| ScaleMatrix, TransMatrix, ApplyMatrix(SV/LV), MulMatrix0/2/2_0, CompMatrix(2)LV, IdentMatrix, gte_ApplyMatrix(SV), Square0 | 0x408080…0x408BC0 | 0x408060…0x408BA0 | BugsDecomp |
| libgs: GsSetProjection, GsMapModelingData, GsInitCoordinate2, GsSetRefView2L (not decompiled) | 0x40CD80, 0x40CDC0, 0x40CE90, 0x40CF10 | 0x40CD20, 0x40CD60, 0x40CE30, **0x40CEB0** | BugsDecomp. BBLIT hooks 0x40CEB0 as "begin frame build", which is the same function |
| Fog parameter writer | — | 0x40CC80 | BBLIT |
| Projection/viewport setup (sx, sy, w, h) | — | ~0x40D1A0 | `[LOCAL]` objdump, §2.6 |
| Room block emit (divisor 0x40FA90), part emit 0x411350, billboards 0x40A6C0, text 0x40B130 | — | as listed | BBLIT |
| Draw3DFrame (sort + draw) via slot 0x4B1A70 from WM_PAINT 0x40E323 | — | 0x4124B0 | BBLIT |
| Tri / quad emitters (slots 0x4B1A78 / 0x4B1A74) | — | 0x414590 / 0x414B40 | BBLIT |
| Room backface cull | — | 0x40FDC0 | BBLIT |
| ABR texture-variant conversion; GL vendor profile | — | 0x420D90; 0x422050 | BBLIT |
| SetGLPixelFormat (16-bit RGB(A) PFD) / TestGL / UnloadGL | 0x40DF80 / 0x40E0B0 / 0x40E210 | 0x40DF20 / 0x40E050 / 0x40E1B0 | BugsDecomp |
| Renderer Reinit | — | 0x40E790 | BBLIT |

**Animation:** no function recovered. BBLIT observes that some objects animate at 15 Hz and that UV animation exists (`dump_uv.py`). **Research Required.**

**Collision:** no function recovered. Known behaviour: the stone collision offset (BBLIT), and culling gating solidity (BBLIT-Findings via web.md). **Research Required.**

**Objects / lists**

| Purpose | 1.0 | 1.6 | Source |
|---|---|---|---|
| AllocListNode / InitListPool / RemoveListNode / ClearList / AppendListNode (linked-list node pools; command 0x2C initialises them) | 0x4489C0 / 0x448A00 / 0x448A40 / 0x448A90 / 0x448AC0 | 0x448850 / 0x448890 / 0x4488D0 / 0x448920 / 0x448950 | BugsDecomp |
| ActorSpawn / ActorFree | — | 0x448D40 / 0x447A50 | BBLIT |
| ActorManager activation-distance and screen tests | — | 0x447F3A; 0x4481C7…0x4484BD | BBLIT |

**Input (PS1 pad emulation)**

| Purpose | 1.0 | 1.6 |
|---|---|---|
| EnumDInputDevs, ReadJoystick, GetKeyName, MapAltKeyboardInput, MapKeyboardInput, InitDInput, FiniDInput, ReadKbdDInput, ReadKbdWinMsg | 0x402000…0x4028A0 | same |
| libpad: PadStartCom, PadGetState, PadInitDirect | 0x402FD0, 0x402FE0, 0x402FF0 | same |
| PadInfoMode / SetActAlign / SetMainMode (merged no-ops) | 0x408A70* | 0x408A50* |
| MapJoystickInput, GetPressedButton, GetButtonName | 0x403010, 0x403210, 0x403260 | same |

**Audio (streamed "tracks")**: InitTrack 0x401000, FiniTrack 0x401330, SetTrackSource 0x4013C0, ResetTrack 0x401490, RefillTrackBuffer 0x4014F0, PlayTrack 0x4018D0, StopTrack 0x401910, CheckTrackDone 0x401930, UpdateTrack 0x4019A0, ConvertTrackAudio 0x401AF0, CvtStereoAdpcm 0x401BE0, CvtMonoAdpcm 0x401E10 (all identical in 1.0 and 1.6).
- DirectSound ring buffer; input is PCM or 4-bit ADPCM decoded to 16-bit.
- Decode tables: a 16-entry step adjustment at 0x45C080 and an **89-entry step table** at 0x45C0C0. That shape is the **IMA/DVI ADPCM** table layout `[INFERENCE, Med]`, not PS1 XA ADPCM.
- Mono ADPCM is rejected by InitTrack (the mono converter is unreachable).
- `[LOCAL]` `track/TRACK*.xa` start with no RIFF/XA header (raw nibbles). The web sources disagree on the codec (web.md item 8). AssetFormatsRE owns this. **Research Required.**

**Save / config / misc**

| Purpose | 1.0 | 1.6 |
|---|---|---|
| WriteConfig / SetConfigDefaults / SaveAltConfig / LoadAltConfig / ReadConfig | 0x409A40 / 0x409AC0 / 0x409CE0 / 0x409D00 / 0x409D20 | 0x409A10 / 0x409A90 / 0x409C80 / 0x409CA0 / 0x409CC0 |
| ConPrintf / ShowRetryDialog | 0x4055D0 / 0x405640 | same |
| CRT: malloc, free, fopen, fread, rand, … | 0x44FDD0… | 0x44FC30… (not decompiled) |

Save-game load/write functions: not recovered (**Research Required**).

### 3.2 Known structures (BugsDecomp, High unless noted)
- **`CONFIG_PC`** (128 B, loaded raw from `..\bin\config.pc`; live at 0x9CA820, an "alt" copy at 0x9CA7A0). Layout:
  - version (=0x10);
  - renderer (0 soft 8-bit, 1 soft 24-bit, 2 OpenGL);
  - language (0 EN, 1 FR, 2 DE, 3 ES, 4 IT, 5 NL);
  - displayType (0–2, unknown);
  - width, height, fullscreen;
  - fogDist (0–2);
  - two doubles at 0x20/0x28;
  - soundVol%, musicVol%, useJoystick;
  - joyMap[10] at 0x3C: look, crouch, jump, kick, roll, action, cam L, cam R, ?, ?;
  - kbdMap[16] at 0x46 (DirectInput scancodes): up, down, right, left, cam R, cam L, roll, action, look, crouch, jump, kick, ?, ?, PSX select, PSX start;
  - two bytes at 0x56/0x57, four doubles 0x58–0x70, a byte at 0x78, and padding.
  - A bad size or version means the defaults are rewritten.
- **`GAME_DATA`** at `psxRam + 0x10000` (`psxRam` pointer at 0x52FD00, which emulates PS1 RAM at 0x80010000):
  - `currentLevel`, `checksum` (PSX only);
  - 28 shorts including button words and stickX/Y;
  - `logicGlobals[256]` at +0x40.
  - Each save slot is a raw dump of these 320 bytes.
- **`LOGIC_ITEM`** (8 B): condCode u8, condArg1 u8, condArg2 u8, actionCode u8, actionArg1 u16, actionArg2 u16. It comes from BZE tag 0x31 at offsets 12–19.
- **`LIST_NODE_HDR`** { next* }, plus pool functions.
- **GTE types** SVECTOR / VECTOR / MATRIX use the PS1 libgte layout (short m[3][3] Q12 plus long t[3]). `[LOCAL]` This matches the scale-matrix construction at LiT 0x408910 and TB 0x404D40.
- **TRACK** (DirectSound streaming state, ADPCM state per channel) and TRACK_PARAMS (buffer, convert and refill periods in ms).
- PS1 pad report:
  - byte0 = 0, byte1 = type << 4 | words;
  - keyboard → type 4 (digital, 16 buttons);
  - joystick → type 7 (analog, with one stick copied to both axes);
  - buttons are active-low, ordered L2, R2, L1, R1, △, ○, ✕, □, select, L3, R3, start, up, right, down, left.
- Launch options (internals.md):
  - `/b<N>` level id (2 digits; `/b00` = title, which the disc launcher `Datas/BugsBunny.exe` passes as `" /b00"`; with no `/b` LiT starts level 2 LANGUAGE.BZE, missing from the PC disc → "File not found" dialog, so `/b00` is required to reach the title on PC — runtime: RUNTIME_REFERENCE.md §1);
  - `/x /y /win /full`;
  - `/ogl|/opengl`, `/soft*`, `/soft8|/softpal`;
  - `/skip_intro /no_sync /no_skip`;
  - `/dll:`;
  - `/p: /r:` (dump/load);
  - `/q /r /w /lum /sat /mul /gam` (values ×1/100) and several unused flags.
  - `[LOCAL]` TB contains the same `/opengl /soft* /no_sync /no_skip /skip_intro /dll:` strings.
- `bugs.ini` keys: Language, FirstTime, DLL, CheckGDI.
- Level list: IDs 0–110 with file names (`title`, `loading`, `language` (absent), `ls01`, `l01a`…, `merlin`, `lb01`…, `cc*` cutscenes, `screen1-3`, `l_*` loading screens, `demo*` (absent on PC)). File name variants `_N` give the language for loading screens. `_8` is used for level ids 7, 8, 21–25, 34 and 54 (meaning unknown).

### 3.3 BZE (summary of `bze.md`; the full spec is owned by the formats docs)
- **Header** (0x800 B): u32 version?, u32 section count, entries {u32 id, u32 actual size, u32 file size}, and at 0x7FC a signed-byte sum of 0x000–0x7FB. Sections follow in header order, each padded to file size (2048-multiple).
  - The engine assumes the order 1, 2, 3, 6, then the other languages, then 4.
  - IDs: 1 load instructions, 2 ignored, 3 textures/SFX/text, 4 models, 5 FR, 6 EN, 7 DE, 8 ES, 9 IT, 10 NL.
  - `[LOCAL]` **TB's header starts differently** (u32 0, a size-like u32, then a count). CoreFormats is verifying this. An Ombelll note (web.md) says section 2 is unused PS1 MIPS code.
- **Compression:** LZSS.
  - Byte 0 holds osize (bits 0–2), giving offset bits = 9 + osize and length bits = 7 − osize, and step (bits 3–4).
  - Bytes 1–3 are a big-endian 24-bit item count − 1. That count is **sometimes off by one with no pattern**, so decoders must stop at the output size.
  - Flag byte per 8 items, LSB first; 1 = literal.
  - 0 = big-endian u16 (offset high bits, length-LUT index low bits), copied backwards with overlap allowed.
  - LUT: 3 + i up to a threshold (19 if the LUT has ≥ 32 entries, else size/2 − 1), then steps of `1 << step`.
- **Section 1** is a byte-tag stream:
  - top-level commands: `0x2C` (init node pools, load section 4 as active model section), `0x2D` chunk, `0x2F` end, `0x4B` (load language section);
  - the leading `2D 45` is a "pseudo-chunk" that skips one section per `45` tag;
  - chunks are `2D <type> {tag + fixed-length payload}* 2E`;
  - fixed tag lengths are listed per chunk type (0x00, 0x05, 0x07/08/0A entity, 0x09, 0x20 static world model, 0x22 entity asset, 0x29, 0x36, 0x3B camera, 0x47, 0x49).
  - Fixed point: positions are 20.12 longs; angles are 4.12 shorts with 0x1000 = 360°.
  - Entity tags: 0x10 position, 0x11 rotation, 0x12 scale, 0x13 == 1 → player, 0x27 entity id, 0x31 logic item.
  - Model tags 0x24 / 0x40 / 0x25 / 0x3F: offset and size into the active model section. 0x27 in chunk 0x22: asset id.

### 3.4 Windows / memory-layout dependence
Everything in BugsDecomp and BBLIT is tied to VAs, IAT slots, the injected-DLL model, DirectInput/DirectSound/WGL and a stock CRT. What carries over: the semantics of formats, logic opcodes, the config layout, pad emulation, fixed-point conventions and the GTE function semantics (which are the documented PS1 libgte API).

---

## 4. Web sources

See [research/web.md](research/web.md) for full detail and URLs. Key points:
- **Developers.** LiT is by Behaviour Interactive; TB by Artificial Mind & Movement. **A2M is the same studio, renamed in 2000** (https://en.wikipedia.org/wiki/Behaviour_Interactive).
  - Lead programmer Martin Ross, producer Denis Lacasse and designer Claude Pelletier worked on both. MobyGames says 6 of TB's 7 credited people are also credited on LiT.
  - Both games were published by Infogrames.
  - `[LOCAL]` TB's version resource still says **CompanyName "Behaviour Interactive"**, with **InternalName "bblit2"**.
- **PCGamingWiki** (https://www.pcgamingwiki.com/wiki/Bugs_Bunny:_Lost_in_Time, https://www.pcgamingwiki.com/wiki/Bugs_Bunny_%26_Taz:_Time_Busters):
  - both are locked at 30 FPS, use OpenGL plus software rendering, and take DirectInput only;
  - TB lists 2-player local multiplayer;
  - TB crashes on launch unless run as admin or with DDrawCompat at 1024×768.
- **WSGF**:
  - LiT: https://www.wsgf.org/dr/bugs-bunny-lost-time/en. Launcher hex edit of the resolution, plus a CE table: FOV default 144, stretch H = 50 / V = 37.
  - TB: https://www.wsgf.org/dr/bugs-bunny-taz-time-busters/en. CE table "Horizontal FOV", default 50, 35 for 16:9, 37 for 16:10. Warns of "pop-in due to culling", and particles stay stretched.
- **PCGW community file 2137** (LiT, killer-m): a DLL plus `Settings.ini` drop-in (resolution, fullscreen, font size, Hor+). The archive contents were not inspected.
- **PCGW community file 1705** (TB, uploaded by WSGF): the description is just `password = "pcgw"`.
- **The reported TB "object spawns/challenges break" problem: no public report found** (file page has no comments; WSGF mentions only culling pop-in). A plausible mechanism, `[INFERENCE]`, Low for TB:
  - LiT gates actor simulation on a screen test (±256 X, a PS1 512×240 screen; BBLIT H1 and BBLIT-Findings #297).
  - That test is computed through the camera projection.
  - Changing sx at runtime (which is what the CE table does, §5) would change which objects get simulated.
- **Other projects:**
  - bblit-remake (Rust/Bevy, LiT);
  - Ombelll LiT RE docs;
  - BBLIT-Findings (LiT, v1.0 plus PS1 addresses);
  - BBLIT-Viewer (GPL-3.0, `.bze` + TIM);
  - talaria0101/bblit (Ghidra dump);
  - BuggyBunny (PS1 `.bzz`);
  - a PS1 static recompilation;
  - an Archipelago randomizer.
  - Most have no license stated, so treat them as facts-only.
  - **No public TB reverse engineering exists.** Ombelll's linked `bbtrm` returns 404.

---

## 5. TB widescreen fix archive `[LOCAL]` (High)

- `/mnt/1st4TB/BugsBunny/Time Busters Widescreen V1.7z` is an encrypted RAR (unrar: "RAR 1.5" block format, method m5:17, CRC 9CC729C0).
- The archive comment, read with `unrar l -v`, says: "File origin : http://www.wsgf.org / Hosted by : https://pcgamingwiki.com / 7z Archive Password : **pcgw**". The same password is published on PCGW file 1705. No brute forcing was done.
- Extracted to `/mnt/1st4TB/BugsBunny/original/ws/Time Busters Widescreen V1.CT`: 714 B, dated 2019-02-11, Cheat Engine table version 18. It has two entries, both `Byte` at static addresses, with no stored values:
  - "Horizontal" @ **0x00639A8D**
  - "Vertical" @ **0x00639A8F**
- Both addresses are in `bblit2.exe` `.data` BSS (VA 0x484000 + VirtualSize 0x1B949C > raw 0x4D000).
- Code xrefs (byte search for the 32-bit address literal in `.text`, then objdump):
  - 0x639A8C is an int16 written at 0x406064 and read at 0x404D43 and 0x40647E.
  - 0x639A8E is written at 0x406086 and read at 0x404D4C, 0x406488 and 0x422E0D/26/4B.
  - At 0x404D40 they become m[0][0] and m[1][1] of a MATRIX whose m[2][2] = 0x1000, i.e. **diag(sx, sy, 1.0) in Q12**.
  - So **the CE table edits the high bytes of TB's projection scales sx (0x639A8C) and sy (0x639A8E)**.
  - The default H = 50 implies sx ≈ 0x32xx ≈ 3.13–3.19 in Q12 `[INFERENCE, Med]`.
- These are the exact TB counterparts of LiT 0x4EFB54/56 (see §2.6). They are produced by the projection-setup function TB 0x405EF0, which hard-codes the 3:4 factor 0.75 (rdata 0x48130C).
- Lowering sx widens the horizontal FOV. Every consumer of sx sees the change, including any screen-space culling. The spawn-test link is unproven (§4).
- For our engine this is a direct pointer: the TB projection is the same PS1-style sx/sy Q12 model as LiT. Hor+ must be done in the renderer while the **logic's** screen tests keep the original 4:3 sx.

---

## 6. Save and config files (LiT) `[LOCAL]`

Files in `lit/Datas/bin`, with the names the exe uses (`..\bin\Savegame%d.dat`, `..\bin\Savedata.dat`, `..\bin\config.pc`, `..\bin\bugs.ini`):

| File | Size | Content |
|---|---|---|
| `config.pc` | 128 | see below |
| `Savedata.dat` | 360 | **all zero** |
| `Savegame0.dat`…`Savegame5.dat` | 320 each | **all zero** (mtime 1999-09-13: shipped blank) |

- **Names.** BugsDecomp's internals.md calls them `Gamedata.dat` / `Gamesave1-6.dat`. The exe strings say **`Savedata.dat` / `Savegame%d.dat`**, and the files present are numbered **0–5** (High). The sizes match BugsDecomp: 360 and 320.
- **Savedata.dat** (360 B; BugsDecomp "Gamedata"):
  - +0 checksum (sum of signed bytes; scope **unverified**);
  - +4 active-slot bitmask;
  - +8 six 44-byte slot summaries, each with golden-carrot count u16 at +0 and clock count u16 at +2.
  - All-zero file = no active slots. Checksum 0 is consistent if the checksum excludes its own field `[INFERENCE, Med]`.
- **SavegameN.dat** (320 B) = raw `GAME_DATA`:
  - +0 current level, +4 checksum, +8…+0x3E shorts (buttons/stick/unknown), +0x40 `logicGlobals[256]`.
  - Known globals: 0x01 health (half-carrots), 0x02 max health (usually 6), 0x03 carrots, 0x05 clocks, 0x07 / 0xFC golden-carrot total low / high, 0x10 language, 0x25 world (1 Stone Age … 6 Nowhere), 0x9F slot, 0xB2/B3/B5 merlin GC mask/count, 0xC0/C1 merlin clock mask/count.
  - Confidence: Med (BugsDecomp; not cross-checked with a real save because all shipped files are blank).
- **config.pc decode** (High for the field map per BugsDecomp; values read directly):

  | Off | Raw | Meaning |
  |---|---|---|
  | 0x00 | 0x10 | version 0x10 ✔ |
  | 0x04 | 0 | renderer = soft 8-bit |
  | 0x08 | 0 | language = English |
  | 0x0C | 0 | displayType 0 |
  | 0x10/0x14 | 0x200 / 0x180 | **512×384** (matches Readme0.txt "default config … 512x384 fullscreen") |
  | 0x18 | 1 | fullscreen |
  | 0x1C | 2 | fogDist = far |
  | 0x20 / 0x28 | 1.0 / 1.0 | doubles, unknown |
  | 0x30 / 0x34 | 100 / 100 | sound / music % |
  | 0x38 | 0 | no joystick |
  | 0x3C | 00 01 … 09 | joyMap identity |
  | 0x46 | C8 D0 CD CB 10 11 12 13 1E 1F 20 21 0F 1C 0F 1C | DIK: ↑ ↓ → ← / camR = Q, camL = W, roll = E, action = R, look = A, crouch = S, jump = D, kick = F, ? = Tab, ? = Enter, select = Tab, start = Enter |
  | 0x56 / 0x57 | 0x10 / 0x01 | unknown bytes |
  | 0x58…0x70 | 0.0, 1.0, 1.0, 1.0 | four doubles |
  | 0x78 | 0 | unknown |

  Hypothesis `[INFERENCE, Low]`: the four doubles at 0x58–0x70 store the `/lum /sat /mul /gam` adjustments (each "scaled by 1/100" per internals.md). The values 0, 1, 1, 1 fit an identity colour transform. Unverified.
- **TB:** the exe uses the identical paths `..\bin\config.pc`, `..\bin\bugs.ini`, `..\bin\Savegame%d.dat`, `..\bin\Savedata.dat`, plus "LoadSaveGame/DeleteSaveGame/Could not find savegame!" strings. No TB config or save files ship in `tb/Datas/BIN` (they are created at runtime).
  - TB `README.txt`: you can save anywhere, but **position is not saved**; you restart in the central area of the last-saved level.
  - The TB save layout is **Research Required** (it may differ: TB adds a second player).

---

## 7. TB-specific observations `[LOCAL]`

- `README.txt` documents **two-player co-op on one keyboard**:
  - Bugs: G/B/V/N move, E jump, R kick, Q ears/sneak, A call friend, S switch camera, W action, D/F rotate camera.
  - Taz: arrow keys, P jump, `[` spin, I pull-a-face, K call friend, L switch camera, O action, `;`/`'` rotate camera.
  - Only player 1 can use a joystick.
  - This is the reference for the planned Bugs + Taz co-op.
- Level set (exe strings): era prefixes AR / AZ / FW / TR / VI…, families `*BOSS*`, `*CA*`, `*CASG*`, `*Q*`, `*R*`, cutscenes `CC*IN01` / `CC*BO02`, `DEMO1-4`, plus `MUSIC.XA` / `SPEECHES.XA` names under `..\BZE\`.
  - Speech paths: `..\speech\%.2d\speech%d.xa` (TB) vs `..\speeches\Speech%02d.xa` (LiT); both strings are present in TB.
- Menu assets are in `Datas/data` (50 files: `*.bmp` with `*D.bmp` variants, TTF fonts `Keystrok.ttf`, `adelaidn.ttf`, one `menu.men`). The exe references `..\data\red_ogl.dat`, `green_ogl.dat`, `lovometer_ogl.dat` and `black_ogl.dat`, but **none of them is in the installed `Datas/data`** (verified). They might be generated at runtime or sit in the install cabs.
- New imports compared with LiT:
  - AVIFIL32 (AVI video playback);
  - WINMM;
  - static DINPUT;
  - `WKeyKill.dll`, a small keyboard-hook DLL (SetWindowsHookExA, GetAsyncKeyState) that `[INFERENCE, Med]` suppresses the Windows key.
- The engine strings "Software  Renderer - Version 0.999", "Using SpanBuffer rendering - Version 0.2" and "Debug Engine Version %s" are TB-only.
- The TB disc folder `original/tb_iso/` is a third-party repack (`IGG-…`, `.mdf/.mds`). We cannot tell whether `bblit2.exe` is stock or patched (for example a disc-check removal). **Research Required:** compare against the exe in the InstallShield cabs of `tb.iso`.

---

## 8. Reusable for the native engine

1. **Fixed 30 Hz simulation tick, with rendering interpolated between the two latest ticks.** Animation partly steps at 15 Hz (High; BBLIT, PCGW both games).
2. PS1 fixed-point conventions:
   - Q12 matrices (0x1000 = 1.0);
   - 20.12 positions;
   - 4.12 angles (0x1000 = 360°);
   - libgte semantics for RotMatrix* (YXZ/ZYX variants), ApplyMatrix*, MulMatrix*, CompMatrixLV, rsin/rcos/ratan2/SquareRoot0 (High; BugsDecomp gte + Sony libgte docs).
3. Projection model:
   - sx/sy Q12 scale with a hard-coded 3:4 factor;
   - view divisor d(Z) = Z ≥ 0 ? Z + 1 : −Z;
   - world "up" is −Y;
   - an identical function in LiT and TB.
   - Do Hor+ in the renderer; **keep the logic's screen and activation tests at the original 4:3 / 512-wide values** (High / Med).
4. PS1 blending:
   - ABR 0–3 equations;
   - STP-bit opacity;
   - vertex colour 0x80 = neutral;
   - translucent geometry without depth writes;
   - draw-order decals;
   - quad zigzag order;
   - cull a quad only if both halves face away (High).
5. Fog: D = min(0xFFFF, b + ((9·fogDist) >> 1)·u_level); fade over the last 25 % of D; the sky backdrop (type 9) is exempt. Actor activation radius is `actor+0x3E`, separate from fog (High for LiT; TB **Research Required**).
6. Level logic VM: 8-byte items; the full condition and action opcode tables in `bze.md`; 256-byte scratch per entity; 256 globals persisted per save (High).
7. BZE container, LZSS, section-1 tag lengths, and the meanings of entity/model tags (High for LiT; TB header differs).
8. Input: emulate a PS1 pad report internally (buttons active-low, digital type 4 / analog type 7), run threshold bx ≥ 0x78 with bx = (|v| − 50)·2, camera turn 0x26 per tick on triggers (High).
9. `config.pc` 128-byte layout (shared paths with TB) and the save layout 360 / 320 B (Med), useful for importing original saves.
10. Level ID ↔ file table 0–110 (LiT) and launch-flag semantics, for debug options such as `/b<N>` (High).
11. Capacity limits to respect only where gameplay depends on them: 40 shadows, part pool, 2000 sort entries (Med).
12. Original defects to fix rather than replicate:
    - half-bright vertex colour;
    - fake-alpha transparency;
    - cliff cracks;
    - hard pop-in;
    - the 41st-shadow hang.
    - The stone collision offset is gameplay, so keep the logic and offset only the visuals, as BBLIT does (High).

Not reusable: every VA, IAT slot, hook or detour; the proxy-DLL and DLL-injection mechanisms; Wine/Proton sync workarounds; and BBLIT's snapshot-matching heuristics (unnecessary when we own the scene graph).

---

## 9. Remaining unknowns (Research Required)

1. Which `.text` bytes differ between our LiT v1.6 (CRC 0xc1d00325) and BBLIT's reference (CRC 0xecffb7c9). What "hpRT" is.
2. Whether TB `bblit2.exe` 1.0.3.0 in `original/tb` is stock; compare with the exe inside the `tb.iso` InstallShield cabs.
3. TB BZE header semantics (first u32 = 0, size-like second u32) and whether its LZSS and tag tables match LiT's.
4. LZSS item-count off-by-one rule. Currently we stop at the decompressed size.
5. Addresses of the BZE loader, LZSS decoder and section-1 interpreter in either exe.
6. Model / animation / collision formats and functions (only offsets and sizes are known from tags 0x24 / 0x25 / 0x3F / 0x40). The meaning of many entity tags (0x0B…0x46).
7. Audio codec of `track/*.xa` and `speeches/*.xa` on PC. The IMA-style tables in the track streamer vs the "XA-ADPCM" claims elsewhere.
8. `config.pc` unknown fields: displayType, doubles 0x20 / 0x28 / 0x58–0x70, bytes 0x56 / 0x57 / 0x78. Checksum scope in the save files. All 28 `GAME_DATA` shorts. TB save layout and its second player.
9. The constant pairs chosen by aspect threshold in the projection setup (2.0 / 1.0, 1.5 / 1.0, 1.0 / 1.0, 0.85 / 1.5). Their role is unconfirmed: perhaps pixel-aspect for software modes.
10. The TB widescreen side-effect on spawns/challenges: no primary report exists. The mechanism is unverified.
11. TB's `*_ogl.dat` files referenced by the exe; the HUD "lovometer" asset location.
12. Exact tick semantics of the logic clock 0x4B24A4: "+2 per tick" per static reading, unverified in BBLIT probes.
