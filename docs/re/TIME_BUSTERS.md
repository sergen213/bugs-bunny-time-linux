# Bugs Bunny & Taz: Time Busters — `bblit2.exe` static RE notes

Status: **Researched (static RE)**; the §9 projection/clip globals (except `0x639aa8`, Contradicted) and the §1
launch behaviour notes marked R are confirmed at runtime under Wine (runtime: RUNTIME_REFERENCE.md §4–§5).
Tags: *observed* = read from bytes / decompiled code at the cited address; *inferred* = interpretation.

| file | MD5 | SHA-256 |
|---|---|---|
| `original/tb/Datas/BIN/bblit2.exe` (880640 bytes) | `d7445216f8cabab1ea33ba1ad029ef7b` | `63483ea5205b8982e384e0b1863624cb8df4fce1601d0cd74393e5b69ecf78c1` |
| `original/tb/Datas/BIN/WKeyKill.dll` (4608 bytes) | `450180d360ec4e0d93531e8aa8e92b14` | `fb6d29de131ec3b59e74930e78debeeae34786830a3da1a8870473c121959f8e` |

Engine lineage, PE/import facts and the LiT↔TB function map are in [ENGINE_COMPARISON.md](ENGINE_COMPARISON.md).
Reproduction scripts: `tools/ghidra/` (see ENGINE_COMPARISON.md "Reproduction"). Addresses are VAs (base 0x400000).

## 1. Init and version strings

*Observed:*

* VERSIONINFO: `CompanyName "Behaviour Interactive"`, `FileDescription "Bugs & Taz: Time Busters"`,
  `FileVersion/ProductVersion "1, 0, 3, 0"`, `InternalName "bblit2"`, `OriginalFilename "bblit2.exe"`.
  PE timestamp 2000-11-17 08:22:00 UTC, MSVC 6 linker, VC6 debug CRT.
* Engine banner (logged through the stubbed logger 0x40ae00 = `ret`): `Debug Engine Version %s` with
  `V120` (0x484088), `Main looping starting` (0x484034). Level init 0x401000 logs, in order:
  `Initialysing System` → 0x459fc0, `Initialysing View` → 0x45a060, `Initialysing Pad` → 0x401ad0,
  `Beginning bouze ID %d loading` → BZE loader 0x4347c0, `Initialysing Plancher` → 0x445d00,
  `Initialysing Premier DeplaceObjet`, then timer init 0x415520.
* WinMain 0x4147f0: command line 0x415980, single instance 0x414370 (`CreateMutexA("A2m application
  mutex")` + `FindWindowA("BBLIT Game", <title>)`), main loop calling the per-tick function 0x4011c0.
* Registry (HKCU): key = resource string 7 `Software\Infogrames\Bugs Bunny & Taz - Time Busters`
  (string 11 is the `… DEMO` variant), values `installation path`, `language` (0x414550/0x4146d0/0x414770).
  Resource string 6 `:\Datas\`, string 5 `BUGS_TAZ`.
* Resource strings are multilingual `~`-separated lists in the order EN, FR, DE, ES, IT, PT, NL
  (e.g. string 1 title, 2 insert-CD, 1614 "desktop must be 16 bit").
* `WKeyKill.dll` is loaded at 0x42fc70 (`LoadLibraryA("WKeyKill.dll")`, export `Kill`). Its imports
  (`SetWindowsHookExA`, `GetAsyncKeyState`, `SystemParametersInfoA`) → *inferred*: suppresses the
  Windows/system keys while playing. Not needed natively.

### Command-line switches (parser 0x415980; the command line is `_strlwr`-ed first by 0x480020)

| switch | effect (global) |
|---|---|
| `/?` | `DAT_004d03f0 = 1` and return (same flag as `/cons`) |
| `/ask` | `DAT_004d1cc1 = 1` |
| `/bze:<name>` | start level = index of the first level-table entry containing `<name>` (written as decimal to `DAT_0063931c`) |
| `/b<NN>` | start level = 2 decimal digits (`DAT_0063931c`) |
| `/x<N>` `/y<N>` | config width/height (`u16 @0x639b50`, `u16 @0x639b52`) |
| `/no_sync` | `DAT_0048aff0 = 0` → pacer never waits (unthrottled) |
| `/no_skip` | `DAT_004d1ca8 = 1` → never skip rendering |
| `/full`, `/win` | fullscreen = `/win` absent (`DAT_00639b54`); `/full` is overwritten, i.e. redundant |
| `/ogl` `/opengl` | renderer = 2 (`DAT_00639b44`) |
| `/soft` `/soft8` `/softpal` `/softrgb` `/soft16` `/soft24` | renderer = 1 (all variants identical in TB) |
| `/skip_intro` | `DAT_0048aff4 = 0` → skips the logo AVIs **and** the GDI title menu: boot goes straight to CCINTRO → GRANNY (R; runtime: RUNTIME_REFERENCE.md §1) |
| `/cons` | `DAT_004d03f0 = 1` |
| `/fntp` | `DAT_004d03ac = 1` |
| `/dll:<name>` | GL library name → `DAT_00639460` |
| `/comp` | `DAT_004d03e8 = 1` → BZE sections are LZSS-compressed (default: raw) |

Without `/skip_intro` and `/b`, the logo AVIs play and the GDI title menu (1 Player / 2 Players / Load Game /
Configuration / Quit) appears; "2 Players" is reachable only there and sets `GD+0xA4` bit 0x10, read as 0x12 in play because the 2P switch rule 0x45eee0 adds 0x02 each tick (R; runtime:
RUNTIME_REFERENCE.md §1, §4 `tbplay/F_2p_granny.csv`).

## 2. Level table and files

*Observed:* level table at 0x4cd998, 96 entries × 16 bytes: `+0 char *path` (`..\BZE\NAME.BZE;1`),
`+4 u16` (= own index for SCREEN1-3/MKSCR1-3, else 0), `+6 u16` set by the loader to the `/comp` flag,
`+8..+15` runtime. Index → name: 0 TITLE, 1 LOADING, 2 LANGUAGE, 3 ZOO, 4 GRANNY, 5 RETRY,
6–16 AZ* (AZCA01, AZCASG01–05, AZR01, AZQ01–02, AZBOSS01–02), 17–27 VI* (same pattern),
28–43 AR* (ARCA01–06, ARCASG01–05, ARR01, ARQ01–02, ARBOSS01–02), 44–54 FW* (same as AZ), 55–66 TR*
(TRCA01–02, TRCASG01–05, TRR01, TRQ01–02, TRBOSS01–02), 67 CCERACUT, 68–72 TEST01–05, 73–75 SCREEN1–3,
76–78 MKSCR1–3, 79–82 DEMO1–4, 83 CCINTRO, 84–91 CC{AZ,VI,AR,TR}{IN01,BO02}, 92 CCEND, 93 CREDITS,
94 `MUSIC.XA`, 95 `SPEECHES.XA`.

Not present in `Datas/BZE` (38 entries): TITLE, LOADING, LANGUAGE, ZOO, ARCA06, ARCASG05, **every FW\*
level**, TRCASG02–05, TEST01–05, SCREEN1–3, MKSCR1–3, DEMO1–4, MUSIC.XA, SPEECHES.XA — *inferred*:
cut/unused content (the FW era is wholly absent). All 58 files on disk are in the table.
On-disk names are mixed case (`azca01.bze`, `Credits.bze`, `CCEraCut.bze`) → the port needs
case-insensitive lookup.

Other files (*observed* strings): `..\bin\config.pc` (0xF8 bytes, struct at 0x639b40),
`..\bin\bugs.ini` (`FirstTime=%i`, `Language=%i`), `..\bin\Savegame%d.dat` (0x428a50/0x428bf0/0x428fb0,
error `Could not find savegame!`), `..\bin\Savedata.dat` (0x428d70/0x428e50, version string `1.00`
@0x4cca08), `..\track\Track%d.xa`, `..\Speeches\Speech00.xa`, `..\speeches\Speech%02d.xa`,
`..\speech\%.2d\speech%d.xa`, `..\data\menu.men`, `..\data\bong.wav`, `..\data\{red,green,lovometer,black}_ogl.dat`,
fonts `Bradybun.ttf Adelaidn.ttf Keystrok.ttf` (`..\data\` + `\FONTS\`), AVI `..\data\Start%d.%s`,
`..\data\End%d.%s`, `..\data\demo%d.avi` (resource strings 8–10).

## 3. BZE loader (FUN_004347c0) — header layout as read by code

*Observed* (decompile of 0x4347c0; confirmed against all 58 TB files):

| offset | type | meaning |
|---|---|---|
| 0x000 | u32 | stored to `_DAT_004d9b00`, never checked; **0 in all files** (LiT: 1) |
| 0x004 | u32 | workspace offset `W`: the asset section is read to `secbuf + W − 0xA000`. Values are round decimals 160000…350000; in every file `W − 0xA000 > section-1 file size` (min slack 3152 bytes) |
| 0x008 | i32 | section count `n` (10 for levels, 3 for cut-scenes/credits) |
| 0x00C | `{u32 id, u32 actualSize, u32 fileSize}[n]` | copied to a stack table (`local_208`); the loader reads `fileSize` bytes per section |
| 0x7FC | i32 | checksum: Σ (int8) bytes[0x000..0x7FB]; mismatch → close file, return 1 |

Flow: open (0x414270) → size (0x414300); size > 0x600000 → `FATAL ERROR: Bouze trop grosse` →
read 0x800 header to `secbuf = DAT_004d03c0 + 0x180000` → checksum → read section 0 (ID 1) to
`secbuf` (via LZSS 0x436e90 from a 0x480000 temp buffer only when `/comp`). If section 1 starts with
`',' 'D'`, each further `'D' <byte>` until `0x2d` reads (and discards) one more section into
`secbuf + W − 0xA000`. Then the next section (ID 3 in all files) is read to `secbuf + W − 0xA000`.
Section-1 interpretation loop runs until token `0x2e`; command `0x2b` sets up pools and reads the
model section (ID 4, located via 0x433560); command `0x4a` reads the language section.

Section IDs (*observed* in headers + code): 1 load instructions; 3 shared assets; 4 models (always
last); 5–11 language assets selected by `language` byte (`*(u8*)(DAT_00639b30+0x93)`, clamped to 0..6):
lang 0→6, 1→5, 2→7, 3→8, 4→9, 5→10, 6→11. *Inferred* names from resource-string order:
0 EN, 1 FR, 2 DE, 3 ES, 4 IT, 5 PT, 6 NL. ID 2 does not occur in TB files. Section order in all level
files: 1,3,6,5,7,8,9,10,11,4; cut-scenes: 1,3,4.

File reads (0x414130) go in 4 KiB chunks with up to 20 retries, then seek to the next 2 KiB boundary
(*observed*); all TB section `fileSize`s are multiples of 2048 and the sections are contiguous from 0x800.

LZSS variant (0x436e90, *observed*): identical to LiT/BugsDecomp `bze.md` (header byte: `osize = b & 7`,
`step = (b >> 3) & 3`; 24-bit big-endian item count; LUT threshold 19 / `size/2 − 1`; flag byte LSB-first,
0 = 16-bit BE offset/length pair). Unused by the shipped data.

Section-1 token renumbering vs LiT: see ENGINE_COMPARISON.md "Incompatibilities".

## 4. Renderer

*Observed:*

* Renderer selection: config `renderer` (`DAT_00639b44`: 1 software, 2 OpenGL). Backend vtable
  `0x4d0330…0x4d0350` installed by 0x425290 (OpenGL: 0x424b90, 0x424e20, 0x4258e0, 0x422d70, 0x422da0,
  0x422dd0, 0x422df0, 0x4259a0, 0x41d9d0) or 0x426c50 (software, "Using SpanBuffer rendering -
  Version 0.2", "Software  Renderer - Version 0.999"; presents with `StretchDIBits` 0x427220). Texture
  manager vtable `0x4d0390…0x4d039c`: 0x42c510 (GL, `Error in Loadtexture …` messages, max 512×512) or
  0x42db90 (software).
* OpenGL is loaded dynamically in 0x410430 (opengl32.dll, 3dfxvgl.dll, or SGI opengl.dll; `/dll:` override;
  `GL_VENDOR/RENDERER/VERSION/EXTENTIONS` logged in 0x406560; vendor table `Direct3D … Voodoo3 3Dfx` at
  0x4ccd28 for driver quirks). Requires a 16-bit desktop for fullscreen (resource strings 1614/1640/1641).
* Vertex submission: CPU-projected screen-space `glVertex3fv` + `glColor3fv` + `glTexCoord2fv` or
  `glTexCoord4f(s/z, t/z, 0, 1/z)` (perspective-correct, chosen by `DAT_00639dc8`), under
  `glOrtho(−w/2, w/2, −h/2, h/2, 0, 65536)`. Details and evidence in ENGINE_COMPARISON.md.
* PSX primitive types: unsupported ones log `Type de polygone non supporte : %d` (0x4263f0, software);
  `CheckPrimValidity failed` (0x4055c0).

## 5. Main loop and timing

*Observed:* `period = QPF × 0.033333335f` (0x415520, f32 @0x48160c; `1000.0f / QPF` (f32 1000.0 @0x481610)
is kept as a ms factor). Per-tick function 0x4011c0: 0x401960 (frame counter `DAT_004d2708 += 2`, reset
per-frame state, call backend slot 0x4d0338) → update calls 0x42ae60, 0x42a030 (QPC-timed), 0x407460,
0x45ee90/0x45eee0 (per view), 0x4681c0 → render per view (0x4013b0; minimal when the skip flag
`DAT_004d03c8` is set) → present + message pump (0x4155d0) → pacer 0x4156c0. Which calls are game logic
vs. collision vs. animation is not yet identified.

**Simulation tick: 30 Hz fixed.** Render skipped (max 2 in a row; 4 when `DAT_004d24c2 != 0`) when more than
one tick late; busy-wait when more than half a tick early; resync after 30 ticks of lag. `/no_sync` and
`/no_skip` disable waiting and skipping respectively.

## 6. Input

*Observed:*

* DirectInput 3 (`DirectInputCreateA(hinst, 0x300)`, 0x40ee50): keyboard (`Keyboard Acquisition …`) and one
  joystick with dead-zone/range setup and force-feedback probing; joystick disabled on NT
  (`Joystick are not supported in WinNT.`). Teardown 0x40ec20. `GetAsyncKeyState` polling of Esc and F1–F7
  in 0x40f240.
* Input is mapped to emulated PSX pads. Key bindings live in config.pc and are labelled in 0x4187a0 with PSX
  pad names for **two players** (`P1_*`, `P2_*`: up, down, right, left, R2, L2, L1, Triangle, R1, Rond, Croix,
  Carre, select, start, View) plus `Joy_B1…Joy_B12`:

| config.pc offset (struct @0x639b40) | contents |
|---|---|
| 0x04 | renderer (1 soft, 2 GL) |
| 0x08 | passed to 0x414770 (registry `language` writer) — *inferred* language |
| 0x10 / 0x12 | u16 width / height |
| 0x14 | fullscreen flag |
| 0x18 | detail level 0 low / 1 medium / 2 high (0x409840: near slice = p1, (p1+far)/2 or far; tree C built only ≤ 1, MODELS.md) |
| 0x28 | double, `x·0.75 + 0.25` = gamma of table 0x639de0 (0x407580: `trunc(256·(i/256)^(1/g))`, identity at g = 0 or 1); default 1, never written by the menu |
| 0xd0 / 0xd8 / 0xe0 | doubles: hue shift (0), saturation × (1), **brightness** = intensity × (1; the Video slider, 0x639c20) of the HSI pass 0x407d30 on every CLUT colour at texture load (0x42d140 / 0x42df70, `0x639da4` = 1 always): RGB/255 → HSI (0x408270) → I × 0x639c20, ≤ 1 → RGB (0x407fe0, clamped) → ×255 truncated |
| 0xe8 / 0xf0 | double scale (1) / s8 offset (0) of table 0x63ae80 (0x407580, `clamp(i·scale + offset)`) |
| 0x40–0x4B | joystick button map (u8 × 12, `Joy_B1…B12`) |
| 0x4C–0x87 | P1 keys, u32 each: up 0x4C, down 0x50, right 0x54, left 0x58, L1 0x5C, L2 0x60, R1 0x64, R2 0x68, Triangle 0x6C, Rond 0x70, Croix 0x74, Carre 0x78, select 0x7C, start 0x80, View 0x84 |
| 0x88–0xC3 | P2 keys, same order: up 0x88 … View 0xC0 |

* Split-screen remnants: number of views `DAT_004d2794` drives per-view loops in 0x4011c0/0x4013b0 and
  0x422df0 doubles `sy` for 2 views, but the only static writers set it to 1 (0x4347c0, 0x459de0) →
  *inferred*: 2-view (two-player) mode exists in code but is unreachable in this build.

## 7. Bugs/Taz character switching

*Observed:* no string in `bblit2.exe` names Taz or Bugs except the window/registry titles; the exe has no
character-select text. *Inferred:* character identity/switching is driven by BZE section-1 data and the
logic VM (0x45d7a0), not by hard-coded strings. Needs dynamic or BZE-level analysis.

## 8. Audio / video / menus (brief, *observed*)

* XA music `..\track\Track%d.xa` and speech files streamed through DirectSound (init 0x429660,
  `DS Stream:` messages); sound effects are AIFF chunks (`SSND INST COMM MARK`, max 128 headers).
* AVI playback via AVIFIL32 + waveOut (0x409df0, 0x40a020).
* Front-end menus are a GDI/TTF system driven by `..\data\menu.men`, drawn with `BitBlt` / `DrawTextA`, not by the
  3D engine — §8.1.

### 8.1 Front end (`menu.men`, WinMain state 2 → 0x41c0f0) — O (code), R (Wine oracle `local/wine/tbmenu/`)

Implemented natively in `bb::game::parseTbMenu` (`engine/game/src/tb_menu.cpp`, `ctest -R game.tb_menu`) and
`apps/bbtime/tb_front.cpp` (`bbtime --game tb --menu`). Status: page 0 **Verified** (pixel compare below), other pages
Tested by eye against the oracle shots.

**Boot.** 0x41c0f0 probes the display modes, installs the three TTFs (0x416710: `Keystrok`, `adelaidn`, `Bradybun`,
copied to `\FONTS\` and `AddFontResourceA`), creates the fonts (0x416850), loads the menu (0x4182b0, else
"Couldn't load the menu file properly, check out your data directory"), reads `..\bin\Savedata.dat` (0x428e50) and
shows page 0. It never calls PlayMusic (GAMEPLAY_TB §6.3). **Attract** (R): the loop arms `GetTickCount + 30000`;
WM_KEYDOWN (`0x6396e8`) and WM_LBUTTONDOWN / DBLCLK / RBUTTONDOWN / MBUTTONDOWN (`0x6396a4`) re-arm it, mouse moves do
not. Once due, every loop pass formats resource string 10 `..\data\demo%d.avi` with `++n` (n is never reset by a
missing file) and opens it (0x4140e0); a found file is played by 0x409df0 / 0x40a220 / 0x40a4c0 until it ends (then the
next n at once) or a key / click stops it and repaints the page (n = 0). The PC disc ships no `demo*.avi`
(start1–3.bmp are the `Start%d.%s` boot logos, not used here), so after 30 s the exe only busy-probes the data
directory and **nothing on screen changes** until input re-arms the timer: Wine oracle `local/wine/tbmenu/w8_capture.sh`,
inotify on `Datas/data` (`w8_inotify_per_second.txt`: ~145 000 directory scans/s from 30 s after the last click,
through a mouse move at 61 s, stopping at the click at 67 s), screenshots `w8_idle_{00,25,35,65}s.png` /
`w8_idle_after_click_10s.png` pixel-identical. The window procedure 0x42e480 drives it with the **mouse only**:
WM_MOUSEMOVE → hover 0x41bc40, WM_LBUTTONDOWN / DBLCLK → click 0x41bf90 (+ hover), WM_KEYDOWN → key capture 0x41ba20
(Controls pages only). **WM_CLOSE** (also WM_QUIT, Alt+F4 via SC_CLOSE) in state 2: `MessageBoxA(text = string 4,
caption = string 1, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST)`, each string's '~' segment number config +0x08:
EN "Do you want to quit?" / "Bugs Bunny & Taz - Time Busters", FR "Veux-tu quitter ?" / "Bugs Bunny et Taz - La
Spirale du Temps", DE "Möchtest du abbrechen?" / "… - Im Wettlauf gegen die Zeit", ES "¿Deseas salir?" / "…: La
Espiral del Tiempo", IT "Vuoi uscire?" / "… - In Viaggio Nel Tempo", PT (5) "Você quer sair?" / "Pernalonga e Taz -
Time Busters", NL "Wil je stoppen?" / "… - Op avontuur door de tijd"; IDYES → state 0 (exit; config.pc is written
on the way out, R: +0xe0 kept), IDNO → the menu stays. The page-0 Quit image (`Cancel`) exits directly, without the
box. R: `w8_quit_box_root.png` (Wine, no window manager → no caption drawn; 173×82 client, Yes default/focused),
`n` → `w8_quit_no.png` (menu unchanged), Return → window gone. There is no keyboard or joystick navigation (bbtime
adds a focus on top: arrows / d-pad move to the nearest actionable item, Enter / ✕ clicks it, Esc / ○ = the page's
Back image, or the quit box on page 0).

**File.** Latin-1, CRLF, `//` comments, sections `[NAME]` (≤ 31 chars, split at every `[`). A value is found with
`strstr(section, "Key=")` — the first occurrence anywhere in the section, case-sensitive (0x416ba0) — up to the line
end, trailing spaces dropped; numbers are `atoi`. Keys:

| key | meaning |
|---|---|
| `Nopage` / `Group` | page 0–9 (−1 = no item; only `[PAGE_n]` and `[CHECKBOX_ITEM_0]` use it) / group within the page (absent = −1). Pages hold groups in order of first appearance, groups hold items in file order (0x416d00); drawing and hit tests walk that order |
| `[PAGE_n]` | page backdrop only: `Up<language>` (0x639b48), else `Up`; blitted 640×480 at 0,0. Its x / y / With / Height / Texte are **not** read |
| `x` `y` `With` `Height` | item rect (+0x70..+0x7c). A loaded `Up` image replaces With/Height with the bitmap size (ISSLIDE re-reads With/Height afterwards); ISTEXT with 0 takes the text extent |
| `Up` / `Down` | `..\data\<file>` BMPs, normal / hovered (`xxx` = none). Blitted opaque (SRCCOPY): the icons carry their own background crop |
| `Texte` | `~`-separated per language (EN FR DE ES IT – NL; index 5 is `xxx`), spaces trimmed (0x416a60); a text containing `xxx` is never drawn |
| `TypeTexte` | +0x6c: ISTEXT 0, ISCHECK 1, ISSLIDE 2, ISLABEL 3, ISIMAGE 4, ISKEY 5, ISNONE 6, ISLISTBOX 7 |
| `Align` | `_strlwr`ed: top 1, left 2 (default), bottom 3, right 4, center 5 (ISIMAGE: top / left only) |
| `Function` | `Done` / `Done2` → 0x41a6e0 with 1 / 2: write `config.pc`, registry language, `0x48b1bc` = players, state 5 (the game; R: CCINTRO → GRANNY, "2 Players" → GD+0xA4 bit 0x10, 0x12 in play). `Cancel` → 0x41a6e0 with 0 = quit. `Call(n)` → 0x41a860 page n (re-texts every item if the language changed). `LoadSaveGame` 0x41b520, `DeleteSaveGame` 0x41b810 |
| `Enable` | +0xac (`FALSE` = 0; every shipped section sets it) |
| ISCHECK: `Selected`, `Changeable`, `CheckType` | initial check, `FALSE` = click only runs Function (Controls player / control-type checks switch pages 2 ↔ 5 ↔ 6), box template `CHECKBOX_ITEM_<CheckType>` |
| ISSLIDE: `FillX` `FillY` | fill origin. `<P>_SLIDER` / `<P>_MIN` / `<P>_MAX` bind by prefix (0x418bd0): MUSIC (0x639b74 = v·80), SFX (0x639b70 = v·128), VOICES, CONTRASTE (double 0x639c20 = config +0xe0 = 0.25 + 1.75 v; v starts at 0x639c20 · 0.5, and 0x41adc0 stores 0.25 + 1.75 v on every draw, so merely showing the Video page turns 1.0 into 1.125 — R: peek 1 → 1.125, one `+` 1.34375, two `−` 0.90625) |
| ISKEY: `KeyId`, `LinkedKey`, `Flags` | `P1_`/`P2_` + up down left right Croix Carre Rond Triangle L1 R1 L2 R2 (+ select start View), `Joy_B1..12`; a linked item mirrors the key (First View = camera L2 + R2) |
| `LANGUAGE_n` `RESOLUTION_n` `RENDERER_n` `DETAILS_n` | name prefixes binding a check to language / mode (320×240 … 1024×768) / renderer / detail level |

The shipped file: pages 0 main (5 images), 1 Configuration (Controls / Video / Sound / Language / Back), 2 P1 keys,
3 Video (resolution, renderer, details, brightness), 4 Sound (music, effects), 5 P2 keys, 6 joystick buttons, 7 Load,
9 Language; 121 items.

**Drawing** (font 0x4d1dd8 = "brady bunch", lfHeight 23 pt · 96/72 = 30; list box font 0x4d1de0 = 26; R: the
oracle's text is 1-bit, matching FreeType at ppem 25 with coverage ≥ 240 — IoU 0.99 on the title labels; colours =
the RGB triples at 0x48b0d8 and immediates):

| type | draw |
|---|---|
| ISIMAGE 0x4195e0 | Up (hovered: Down) at x,y; Texte centred under it (x + w/2 − tw/2, y + h) in red (242,31,16), grey (128,128,150) when disabled (0x419c30) |
| ISTEXT 0x4197a0 | blue (8,25,204), hovered (9,128,237); center = centred on x |
| ISLABEL 0x419c30 | red / grey, aligned in the rect (center with With 0 = centred on x) |
| ISCHECK 0x419950 | box (coche1 checked / coche2) at x − 16 − 7, y + h/2 − 8 + 3; text at x,y: blue / checked (49,174,241), hovered (9,128,237) / checked (175,238,253), disabled grey |
| ISSLIDE 0x419df0 / 0x41adc0 | Texte red, centred 30 above; bar `Up` at x,y; fill (181,0,51) from FillX − 6 over With·v, FillY..+Height. `_MIN` / `_MAX` step ∓0.125 for every slider (the 0.05 meant for brightness is chosen by comparing the step with the 0x639c20 pointer, never true; R above); SFX / VOICES / MUSIC play `..\data\bong.wav` at the effects volume (0x41b060), CONTRASTE does not |
| ISKEY 0x41a2a0 | Texte orange (247,176,72) at x,y (top: centred 0x17 above); key name in the alignment: white, hovered (32,0,255), waiting (128,64,255), disabled (160,160,160); unbound `???` |
| ISLISTBOX 0x419f40 | Savedata texts, 22-px rows at x + 10, small font: (9,128,237), hovered (49,174,241), selected (248,230,0), both (175,238,253); UP / DOWN images scroll it |

Hover (0x41bc40): the first item under the mouse (image or its label, check box or text); a disabled key ends its
group. Click (0x41bf90): +0x90 (check toggle 0x41ab70, key capture 0x41b1f0, list row 0x41b120, slider step 0x41b060,
Done) then +0x98 (Call / Cancel / Load / Delete).

**Saves.** `..\bin\Savedata.dat`: u32 n, `"1.00\0"`, n × 0x54: u8 slot, text "date, time, n%" (0x4b B), u32 level
(+0x4c), +0x50 the in-memory next pointer. Load = `Savegame<slot>.dat` (0x2b0 bytes) → GAME_DATA, then 0x428bf0 clears
GD+0x14a, 0xfe–0x108, 0x13d, sets GD+0x93 = language, start level = the record's level, players = GD+0xa4 & 0x10.
Delete (0x428fb0) drops the record, renumbers the rest 0..n−1, rewrites Savedata (deleted when empty), deletes
`Savegame<slot>.dat` and renames the later files down by one.

**bbtime mapping.** Saves in `$XDG_DATA_HOME/bbtime/saves/tb/` (list, load, delete; written in play by the pause
menu's "save game", formats/SAVE_TB.md). Sound sliders = the audio device's music / effects
volume and `settings.ini` `music_volume` / `sfx_volume`. Controls: P1 / P2 keys rebind the session's TB keyboard maps
(a key taken from another action leaves it `???`); the joystick page shows bbtime's fixed SDL gamepad layout (A X B LB
RB Y LT RT) and is not rebindable. Video (`settings.ini`, LAUNCHER.md): a resolution sets the window size (`width` /
`height`; Hor+ framing as everywhere, fullscreen keeps the display mode; every size is enabled — the exe also greys
modes its probe 0x42f880 rejects); renderer and detail level are kept (`tb_renderer`, `tb_details`) and shown but
bbtime always draws with its own renderer at the high detail level (no software path, far-LOD tree C not drawn);
brightness is the exe's (`tb_brightness` = +0xe0, the showing-the-page drift included) and applies from the next TB
level load as `bb::tbAdjustColor` on every CLUT colour (both renderers: texture data). R: ARCA01 at +0xe0 = 0.25 /
1 / 2 (`w8_bright.sh`, `w8_arca01_b*.png`): mean ratio to 1.0 = 0.248 / 1.83, bbtime 0.249 / 1.822
(`local/shots/w8tbfront/cmp_arca01_brightness.png`, top oracle, bottom bbtime GL; VK the same). Quit box: drawn by us
in the Windows 9x classic look (caption bar, IDI_QUESTION, Yes / No with underlined mnemonics, dotted focus) with the
oracle's client layout; Y / N, Enter / Space, arrows / Tab, click on a button, d-pad + ✕; Esc does nothing (no
Cancel). Esc on page 0 opens it (Yes → the collection menu), a window close opens it too (Yes → bbtime exits; a
second close while it is open quits). Attract: the 30 s timer and its re-arm rules as above (key, mouse button, pad
button, `--autoplay` press); when due bbtime logs once that `demo1.avi` is absent (or present: no AVI decoder is
linked, so it is skipped like a missing file) and the menu stays. Language: menu texts and backdrop for the session.
Original Fidelity keeps the front end silent; Enhanced plays the collection menu's TB track.

## 9. Aspect ratio / FOV globals

*Observed* in screen setup 0x405ef0 (called as `(w, h, 0x200, 0xF0)` from 0x42eff0/0x42f930) and
`GsSetProjection` 0x4061f0 (called with H = 400 from 0x439570, 0x439700, 0x45a060, 0x465710; and with
`(int)DAT_004cf18c` from 0x465710):

| global | type | value / formula | used by |
|---|---|---|---|
| 0x639a84 / 0x639a86 | u16 | output width w / height h | glOrtho 0x424ae0/0x425290, viewport, 0x404d40 |
| 0x639a88 / 0x639a8a | u16 | w/2, h/2 (screen centre) | projection, clip |
| **0x639a8c** | s16 Q12 | `sx = 4096·w/512` | 0x404d40 (scale matrix), 0x406280 |
| **0x639a8e** | s16 Q12 | `sy = (4096·h/(0.75·w))·sx >> 12 = 4096·h/384` | 0x404d40, 0x406280; doubled for 2 views in 0x422df0 |
| 0x639a90 / 0x639a92 | u16 | reference size 512 / 240 | |
| 0x639a94 | s16 Q12 | `4096·240/(0.75·512)` = 2560 | 0x406280 |
| 0x639ab0 / 0x639ab4 | f32 | w/512, h/240 | tier select below; 0x401000 |
| 0x639ab8 / 0x639abc | f32 | (2.0,1.0) if w/512 ≥ 2; (1.5,1.0) ≥ 1.5; (1.0,1.0) ≥ 1.0; else (0.85,1.5); level 0 with w/512 < 1 → (0.65,1.0) | *inferred* LOD / sprite scale |
| **0x639a70** | i32 | projection distance H (default 400) | 0x402640, 0x4038f0, 0x403b30, 0x408b60, 0x408f20 |
| 0x639a74 / 0x639a78 / 0x639a7c | f32 | H, 1/H, −(H/2) | 0x408900, 0x408990, 0x408d00 |
| 0x639a80 | f32 | 0.0 → glOrtho near | 0x424ae0 |
| 0x639ac0 / 0x639aa8 | f32 | 10000.0 / **0.0** at runtime (not 5000.0, so not a far clip; runtime: RUNTIME_REFERENCE.md §5) | 0x639ac0 *inferred* far clip; 0x639aa8 role unknown |
| 0x63bc14, 0x63bb50, 0x63bc20, 0x63bc1c | f32 | −w/2, w/2−1, 1−h/2, h/2 | *inferred* 2D clip rectangle |
| 0x639aa0 / 0x639aa4 | f32 | screen offsets from 0x63bc04/0x63bc08 | 0x404d40 |
| .rdata 0x48130c / 0x481308 | f32 | 0.75 / 4096.0 | 0x405ef0 |
| .rdata 0x481318 / 0x481310 / 0x4812e0 | f64 | 2.0 / 1.5 / 1.0 | tier thresholds |
| .rdata 0x481668 / 0x4813a8 | f64 | −0.5 / +0.5 | glOrtho extents |

0x404d40 builds `diag(sx, sy, 0x1000)` (Q12), rotates it by `RotMatrixX(0x800)` (180°, 0x4042b0), multiplies
by the camera matrix (`MulMatrix0` 0x4048b0) into the GTE rotation matrix at 0x639940 — the screen scale is
baked into the view matrix. The f64 0.75 at 0x481340 is a gamma constant (0x407580), not aspect.

*Derived:* the 3D view always covers 512×384 projection units at H = 400 → horizontal FOV
2·atan(256/400) ≈ 65.2°, vertical 2·atan(192/400) ≈ 51.3°, independent of output resolution. A non-4:3
output stretches the 4:3 image (sx ≠ sy); there is no Hor+ logic. The third-party widescreen Cheat Engine
table pokes the high bytes 0x639a8d / 0x639a8f of sx / sy (reported by the Research agent; table itself not
decrypted here). A native widescreen mode should widen the horizontal extent (larger sx window /
wider clip rect 0x63bc14/0x63bb50) rather than scale sx.

## Appendix: logic-VM handler tables

TB tables (*observed*): conditions 0x4cd560 (83), actions 0x4cd6ac (142), extra 0x4cd8e4 (14) and
0x4cd91c (31), contiguous and terminated by the non-code value 0x4ce830; dispatched inline in 0x45d7a0
(table refs 0x45d8e8, 0x45e2e5, 0x45d9dc, 0x45e3cb). Slot 0 of conditions = `mov eax,1; ret`
(CondTrue). Positional alignment against LiT (`align_logic_tables.py`, similarity ≥ 0.8 shown in
`ghidra/diff/out/logic_align.tsv`) shows LiT condition 15/16 → TB 13/14, 30/31 → 24/25, 61–68 → 39–46,
72/73 → 49/50 and LiT actions 9, 11–16 → TB actions 1, 3–8 (e.g. `ActionOrgi` LiT act 12 → TB act 4,
`ActionRnds` 15 → 7), 20–22 → 12–14, 36 (`ActionAddsi`) → 21, 53 → 35: **opcode numbers differ between
the games**. A few tiny LiT actions align into the tail of TB's condition table (TB cond 76/79), so even
the table boundaries (taken from the code references) do not line up with LiT's. Each TB opcode still
needs semantic verification before the VM is implemented.
