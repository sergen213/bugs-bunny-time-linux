# Runtime reference — the original games under Wine/Proton (reference oracle)

Status vocabulary as ROADMAP.md. This document records **observed-runtime** facts read from the running original
exes (LiT `bugs.exe` v1.6, TB `bblit2.exe` 1.0.3.0) and ties each one to the Ghidra address it confirms or
contradicts. Wine is a reference tool only; nothing in bbtime may depend on it.

Tags: **R** = observed at runtime (memory read / screenshot), **S** = static claim being checked (doc + address),
**I** = inferred. Verdicts: **Verified** (runtime equals the static claim), **Contradicted** (runtime differs → doc
correction listed in §6), **Consistent** (agrees within what the input could exercise), **Not exercised**.

All game-derived evidence is under `/mnt/1st4TB/BugsBunny/local/wine/` (below `$W`); CSV rows are 5 ms asynchronous
polls — use the **last row per tick-counter value** (a row can still land mid-tick).

## 1. Environment (how the originals run)

| item | value |
|---|---|
| runner (default) | **Proton-GE** `GE-Proton11-9` (wine-11.0 staging) from `~/.local/share/Steam/compatibilitytools.d/Proton-GE Latest`, run as `proton run` with `STEAM_COMPAT_DATA_PATH=$W/ge` → prefix `$W/ge/pfx` (`RUNNER=ge`) |
| runner (alternative) | system Wine 11.20 (pure wow64; `WINEARCH=win32` is refused by this build), prefix `$W/prefix` (`RUNNER=wine`) |
| install trees | `$W/lit/Datas`, `$W/tb/Datas`: exe dir copied (writable `config.pc`, `bugs.ini`, saves), every other dir a symlink into `original/` (read-only use) |
| CD check | both exes scan drives for `GetDriveType == 5` + volume label (LiT `"BBLIT"` @0x45f360, WinMain 0x405950; TB resource string 5 `BUGS_TAZ`, case-insensitive) → `D:` = `$W/cd_lit` (`.windows-label` BBLIT), `E:` = `$W/cd_tb` (Bugs_Taz), registry `HKLM\Software\Wine\Drives` = cdrom |
| install path | `HKCU\Software\Infogrames\Bugs Bunny Lost In Time\installation path` = `C:\BBLIT\Datas` (exe chdirs to `<path>\bin`, 0x405950), TB `…\Bugs Bunny & Taz - Time Busters` = `C:\BBTB\Datas` |
| renderer | LiT `config.pc` patched to renderer 2 (OpenGL), 640×480, windowed; switches `/opengl /win /x640 /y480 /skip_intro`; TB `/ogl /win /x640 /y480 /skip_intro` (TB writes its own 0xF8-B `config.pc` on first run: 640×480, windowed, renderer 2) |
| display | one private Xvfb per game (800×600, GLX = Mesa llvmpipe); the 640×480 client sits at (3,29) under system Wine, (3,22) under Proton |
| memory access | `kernel.yama.ptrace_scope = 1` → only an ancestor may read `/proc/<pid>/mem`; `memsrv.py` launches Wine itself (child subreaper) and serves reads on `$W/run/<game>.sock`. No ASLR on either exe (image base 0x400000, DllCharacteristics 0) → Ghidra VAs are live addresses |
| audio | `winepulse.drv,winealsa.drv` disabled by default (`AUDIO=1` keeps sound); both games run without audio devices |

Scripts (`tools/wine/`):

| script | use |
|---|---|
| `setup_prefix.sh` | idempotent: prefix, install trees, CD trees, drives, registry, LiT `config.pc` patch |
| `run_original.sh lit\|tb [switches]` / `run_original.sh stop lit\|tb` | start headless (returns when the window exists; state in `$W/run/<game>.env`, log in `$W/logs/`) / stop. Env `RUNNER=ge\|wine`, `SYNC=off` (Proton: `PROTON_NO_NTSYNC/FSYNC/ESYNC=1`), `RES`, `AUDIO` |
| `shot.sh lit\|tb OUT.png` | 640×480 client-area screenshot |
| `send_keys.sh lit\|tb STEP…` | XTEST keys: `Up+d@10t` (hold chord 10 ticks), `_@500` (wait ms) |
| `peek.py lit\|tb ADDR TYPE [N]` | one read; `ADDR` may be `[0x4b324c]+0xe4` (u32 deref); `--selftest` |
| `trace.py lit\|tb OUT.csv SECS name=ADDR:TYPE[*N]…` | poll to CSV (default 33 ms, `--period`) |
| `cam.py lit\|tb` | live eye/look-at → bbtime `--camera X,Y,Z,YAW,PITCH` (PS1 → engine `(x,−y,−z)`, play.cpp yaw/pitch); `--selftest` |

Level boot: LiT `/bNN` = index in level table 0x4ad380 (parser 0x408f70, 2 chars after `/b` or `/B`; L01A = `/b04`,
L02A1 `/b09`, L04C3 `/b37`); **`/b00` = title**, which is what the disc launcher `Datas/BugsBunny.exe` passes
(string `" /b00"`); without any `/b` WinMain 0x405950 sets `GD.currentLevel = 2` = LANGUAGE.BZE, which the PC disc
does not ship → "File not found: ..\BZE\LANGUAGE.BZE" Retry/Cancel dialog (WB/Behaviour logos play first).
`/b00` boot: `shots/lit_boot_sheet.png`; title → Enter → slot menu (Load / New / Quit): `shots/lit_title_menu.png`.
TB `/bNN` or `/bze:name` (parser 0x415980; ARCA01 = `/b28`).
TB ARCA01 starts with a fly-over intro that ✕ (key E) skips. Without `/skip_intro` and `/b`, TB shows the logo AVIs
and the **GDI title menu** (1 Player / 2 Players / Load Game / Configuration / Quit); with `/skip_intro` it goes
straight to CCINTRO → GRANNY. "2 Players" is only reachable from that menu (clicked with xdotool at client
(130,385)); in-level Esc opens the pause menu, whose Quit Game restarts CCINTRO.

Runner notes (R):
* Both runners run both games. Ticks: LiT 30.005 t/s (Proton-GE, ntsync), TB 29.99 t/s (system Wine).
* **One hang** observed: LiT under Proton-GE with ntsync froze at level tick 3480 (~116 s after load, after scripted
  input), 100 % CPU, tick counter 0x4b24a4 stuck. Not reproduced in 300 s idle with ntsync, 200 s after the same Up
  input with ntsync, nor 240 s with `SYNC=off` (→ consistent with, but does not prove, the BBLIT README "freezes
  with newer sync primitives"; `SYNC=off` is the safe setting for long captures).
* **TB exits intermittently** with an X `BadWindow` on `X_UnmapWindow` (resource 0x400001), seen 3× (system Wine and
  Proton-GE, ntsync and `SYNC=off`) minutes after a level was left running; a dedicated 240 s idle run after the
  intro skip did **not** reproduce it, so the trigger is unknown (Research Required). Restart TB per capture.

## 2. Tick rate (GAMEPLAY.md §1.1/§1.3, ENGINE_COMPARISON.md pacer)

| claim (S) | runtime (R) | evidence | verdict |
|---|---|---|---|
| LiT frame counter `DAT_004b24a4 += 2` per tick in 0x4065e0, 30 Hz pacer 0x406ee0 | every change is +2; 30.005 ticks/s over 20 s; 30.04 t/s over 240 s soak; 0 → … after `/b` load (counter reset) | `csv/tickrate_lit.csv`, soak log in this doc §1 | **Verified** |
| TB `DAT_004d2708 += 2` in 0x401960, 30 Hz pacer 0x4156c0 | every change +2; 29.99 ticks/s over 20 s | `csv/tickrate_tb.csv` | **Verified** |

## 3. LiT player and camera (L01A, `/b04`)

Player `P = *(0x4b324c)`; pos `+0xe4` s32×3, rot `+0xdc` s16×3, flags14 `+0x14`, state `+0x17c`, jump `+0x104` H /
`+0x106` step / `+0x108` phase; pad `GD = *(0x52fd00)+0x10000`, `+0x0a` edge, `+0x0c` held.

| claim (S, GAMEPLAY.md) | runtime (R) | evidence | verdict |
|---|---|---|---|
| level start: state 2, then scripted walk-in | state 2 at tick 0, state 130 from tick 5 walking +25 z/tick (8830 → 9655), state 1 from tick 71 | `csv/litworld_L01A_idle.csv`, `csv/lit_spawn.csv` | R (new fact) |
| §3.3 jump start: H 39, step 70, phase 0x400 (+step on first arc tick), flags14 bit 1 | `+0x104..` = 39 70, phase 1094 (0x446) on arc tick 1, flags14 2 → 1 → 9 (descending) → 2 | `csv/lit_jump_hold.csv` ticks 135–187 | **Verified** |
| §3.3 standard jump (✕ held) apex −389 at arc tick 15, lands tick 26 | y −640 → −1029 (389) at tick 165 = arc tick 15; lands tick 187 = 26 | same | **Verified** |
| §3.3 variable height: release in 0x600–0x800 → phase 0x800; early-release apex −285 @ 8, lands 18 | 2-tick tap: phase 1584 → 2118 (0x800 + 70), apex −925 (285) at arc tick 8, lands arc tick 18 | same, ticks 255–291 | **Verified** |
| §3.3 descent twice as fast (`>> 11`, `& ~1`) | per-tick descent 2, 10, 18, 26, 34, 42, 48, 54, 60, 64 (even steps) | same | **Verified** |
| §3.2/§3.7 keyboard = p 128 → run 50 u/tick | Up held: state 5, steady 49.25–49.5 u/tick while yaw still converges (Δ ≠ 0 → ×(0x800−\|Δ\|)/0x800) | `csv/lit_run.csv` ticks 165–181 | **Consistent** |
| §3.2 turn `Δ >> 2` per tick | yaw 882 → 1165 → 1377 → 1536 → 1654 → 1741 (steps shrink ×≈0.75) | same | **Consistent** |
| §3.7 walk 40 u/tick | keyboard cannot produce p < 0x78 | — | **Not exercised** |
| §5 defaults: target offset `0x4b215c` −200, eye height `0x4b2150` 600, follow distance `0x4b2154` 1100 | −200, 600, 1100 | `csv/lit_spawn.csv` | **Verified** |
| §5.8 target y = player.y − 200 via `>> 2` lerp | rest target y = player.y − 203 (lerp residue 3 >> 2 = 0) | `csv/lit_spawn.csv`, `lit_run.csv` | **Verified** |
| §5.8 eye y pulled into ±100 of min(ground, player.y) − 600 | rest eye y = player.y − 500 (band edge) | same | **Consistent** |
| §5.7 eye = target + R(yaw)·(0,0,distance), distance band 1100 ± 100 (`0x4b3da8` "distance cache") | at rest `0x4b3da8` = 1200 (band edge) but the **eye–target xz distance is 0x4b3da8 + 400** (1600 / 1596 / 1602 in three captures; 3-D ≈ 1627); `0x4b3e1a` = atan2(eye − target) once settled | `csv/lit_spawn.csv`, `lit_run.csv`, `lit_jump_hold.csv` (last rows) | **Contradicted** (extra 400; §6) |
| §5 initial eye from chunk 0x3b tag 0x2b | L01A 0x3b/0x2b = (0,0,0); live eye after load (23613, −1000, 11550) | `tools/re/sec1.py` on `L01a.bze`; `csv/lit_spawn.csv` | R (eye comes from the reset/walk-in, not the tag) |
| §5 camera flags `0x4b3940` 0x40 recentre request after load | 0x440 during walk-in, 0x40 for 10 ticks, then 0 | `csv/lit_spawn.csv` | **Consistent** |

## 4. TB players and camera (ARCA01, `/b28`, after ✕ skips the intro)

Bugs `*(0x4d26cc)`, Taz `*(0x4d2768)`, focus `*(0x4d5028)`; pos `+0xfc` s16×3, velocity `+0x70` s16×3 (×16),
flags `+0x20`, state id `[+0x17c]+6`; `GD = *(0x639b30)`; camera view 0 block `0x4d5120`.

| claim (S, GAMEPLAY_TB.md) | runtime (R) | evidence | verdict |
|---|---|---|---|
| §2.3 jump `vy0 = −0x50 − ((round(sqrt(80H+100)) >> 1) << 4)`; Bugs H 548 → −110 | stored vy −1760 (= −110 × 16) | `tbplay/A_idle_jump.csv` tick 541 | **Verified** |
| §2.2 gravity +10 u/tick² (5·dt), no terminal velocity | vy +160 (×16) every tick, symmetric arc | same | **Verified** |
| §2.3 Bugs apex 550 @ 10, lands 21 | y −10 → −560 at jump tick 10, back at −10 on tick 21; state 44 | same | **Verified** |
| §2.3 Taz apex 490 @ 10, lands 20 (vy0 −104) | vy −1664, y −10 → −500 at tick 10, lands tick 20 | `tbplay/C_switch_taz_jump.csv` ticks 507–547 | **Verified** |
| §2.1 Bugs run 50 u/tick (state 37, `S[0x66]` = 25) | `S[0x66]`,`S[0x67]` = 25, 25; Up held → state 37, steady velocity (528, 0, 576)/16 = **48.84 u/tick** | `tbplay/B_run.csv`, peeks | **Consistent** (≈ 50·(0x800−\|Δ\|)/0x800 with a residual heading error) |
| §2.2 ground grip Bugs run 1500/4096 | speed ramp 19.1, 31.1, 38.2, 42.5, 45.3, 47.4, 48.8 → error ×≈0.6 per tick (g ≈ 0.4) | same | **Consistent** |
| §2.1 walk 26 u/tick | keyboard = full stick | — | **Not exercised** |
| §4.3 camera constants `B+0x118..0x148` = 1200 500 20 400 50 5000 800 300 300 30 10 15 400 | exactly 1200 500 20 400 50 5000 800 300 300 30 10 15 400 | peek `0x4d5238` s32×13 | **Verified** |
| §4.3 spring rest `0x4d9ae0` = 1200, blend `0x4d9acc` 0x1000 | 1200, 4096 | peeks | **Verified** |
| §4.3 look-at = T.y − 300 | look (−4107, −310, −7555) for Bugs at (−4107, −10, −7555); eye–look 1204 at rest | `cam.py tb` | **Verified** |
| §4.3 2P framing: `e > 15 → D += e`, e clamped 40 | 2P (A4 = 0x12): D 1200 → +40 per tick while the players separate | `tbplay/F_2p_granny.csv` ticks 765–795 | **Verified** |
| §4.1 R1 switch toggles `A4 & 4`, focus follows one tick later | A4 0 → 12 → 4, focus pointer changes on the next sample | `tbplay/C_switch_taz_jump.csv` ticks 387–397 | **Consistent** (TBPlay analyses) |
| §4.2 call friend refused while cooldown < 45 | L1 at +20 ticks refused, at +80 accepted; cooldown `0x4ceb08` resets to 0 | `tbplay/D_callfriend.csv` | **Verified** |
| TIME_BUSTERS.md §6 P1 key slots | runtime `config.pc`: arrows, L1 A, L2 D, R1 S, R2 F, △ W, ○ Q, ✕ E, □ R, select Tab, start Esc, View none; P2 numpad 8/5/6/4 … | `$W/tb/Datas/BIN/config.pc` | R (defaults of 0x415fd0) |

## 5. Projection, fog, texturing

| claim (S) | runtime (R) at 640×480 | verdict |
|---|---|---|
| LOST_IN_TIME.md: `0x4efb4c..` w, h, w/2, h/2, sx = 4096·w/512, sy = 4096·h/384, 512, 240, `0x4efb5c` 2560 | 640 480 320 240 **5120 5120** 512 240 2560 | **Verified** |
| LiT `0x4efb30` H = 400 (int), `0x4efb34` (float)H, `0x4efb38` −H/2 | 400, 400.0, −200.0 | **Verified** |
| LiT `0x4efb7c/80` w/512, h/240; `0x4efb84/88` tier (1.0, 1.0) for w/512 ≥ 1 | 1.25, 2.0, 1.0, 1.0 | **Verified** |
| RESEARCH.md §2.7 fog 0x40cc80: D = min(0xFFFF, b + ((9·fogDist)/2)·u), `0x4efb60` = −0.75·D, `0x4efb64` = 1/(0.25·D) | fogDist (`config.pc`+0x1c, live 0x9ca83c) 2, u (`0x4b38a0`) 1764, **D (`0x4efb40`) 39876 ⇒ b = 24000** for L01A; `0x4efb48` −39876.0 (float), `0x4efb60` −29907.0, `0x4efb64` 1.00311e-4 | **Verified** (b now known) |
| TIME_BUSTERS.md §9: `0x639a70` H 400; `0x639a74..` H, 1/H, −H/2; `0x639a84..` as LiT; `0x639a94` 2560; `0x639ab0..` 1.25 2 1 1 | 400; 400, 0.0025, −200; 640 480 320 240 5120 5120 512 240 2560; 1.25 2 1 1 | **Verified** |
| TB `0x639aa8` f32 = 5000.0 "inferred far clip" | **0.0** | **Contradicted** |
| TB `0x639ac0` 10000.0; clip rect `0x63bc14/0x63bb50/0x63bc20/0x63bc1c` = −w/2, w/2−1, 1−h/2, h/2 | 10000.0; −320, 319, −239, 240 | **Verified** |
| TB draw distances `0x4d5954` / `0x4d5980` (chunk 0x3a tag 0x3c, × H/400) | ARCA01: 12000 / 32000 | R |
| TB number of views `0x4d2794` = 1 | 1 (also 1 in 2-player mode is not re-checked) | **Verified** (1P) |
| ENGINE_COMPARISON.md: TB perspective-correct `glTexCoord4f` path when `DAT_00639dc8 != 0` (writer unknown) | **0x639dc8 = 1** at runtime (GL renderer, both runners) → the shipped TB GL path is perspective-correct | **Verified** (flag set) |
| ROADMAP caveat: original FOV ≈ 65.2° × 51.3° (H 400 over 512×384) | vertical FOV = 2·atan(240 / (1.25·400)) = 51.28°; bbtime renders at the same camera line up with the originals at `fov=51.28` (LiT and TB, §7) | **Verified** |

## 6. Corrections for other documents (integrator)

1. **GAMEPLAY.md** header + **GAMEPLAY_STATUS.md** "Nothing is Verified": §1.1 30 Hz/+2 counter, §3.3 jump arc
   (H39/70 apex 389 @ 15, lands 26; tap apex 285 @ 8, lands 18; cut to 0x800), §5 parameters −200/600/1100 are now
   *Verified Against Original Behavior* (this doc §2–§3).
2. **GAMEPLAY.md §5 step 7**: at rest the eye's xz distance from the target is `0x4b3da8 + 400` (1600 when
   `0x4b3da8` = 1200), not `0x4b3da8`/`0x4b2154`; and `0x4b3da8` rests at the band edge 1200, not 1100. The live
   follow also restores `0x4b38c0..d4` from the saved copies `0x4b3954..0x4b397c` (decompile 0x432438) — the
   "distance cache" naming needs re-deriving. Owner: LiT camera (`play.cpp` camera uses the documented 1100 band).
3. **GAMEPLAY.md §1.2 / §5**: L01A's chunk 0x3b tag 0x2b initial eye is (0,0,0); level start runs state 2 → 130
   (scripted walk-in, +25 z/tick for ~66 ticks) → 1 — bbtime has no walk-in and its play camera stays at the origin
   for the first frames (`--frames 10` play screenshot is all sky: `compare/lit_L01A_bbtime_play.png`).
4. **TIME_BUSTERS.md §9**: `0x639aa8` is 0.0 at runtime (not 5000.0).
5. **RESEARCH.md §2.7**: fog base b = 24000 for L01A (D = 39876 with u = 1764, fogDist 2); `0x4efb48` is a float.
6. **ENGINE_COMPARISON.md** (TB texturing): `DAT_00639dc8` is 1 at runtime → TB's GL path is perspective-correct
   in the shipped game; ROADMAP "affine texturing (LiT)" stays LiT-only.
7. **ROADMAP / renderer**: Original Fidelity still uses `fov = 60` (`apps/bbtime/main.cpp:56`, settings `fov`);
   the originals' vertical FOV is 51.28° (H 400, sy 1.25 at 480 lines) — with 51.28 the framings match (§7).
8. **TIME_BUSTERS.md §1**: `/skip_intro` skips the logo AVIs **and** the GDI title menu (goes to CCINTRO → GRANNY);
   the 2-player menu entry sets `GD+0xA4 = 0x12`.
9. **LOST_IN_TIME.md / RESEARCH.md launch options**: LiT needs `/b00` to reach the title on PC (launcher passes it);
   with no `/b` the default level 2 (LANGUAGE.BZE) is missing from the PC disc.

## 7. Side-by-side with bbtime (same camera, 4:3, `--fidelity original`)

Commands: `python3 tools/wine/cam.py lit|tb` → `bbtime --data …/original --game lit|tb --level L01A|ARCA01
--fidelity original --width 640 --height 480 --hidden --mute --frames 10 --camera <cam.py output> --screenshot …`
(`build/apps/bbtime/bbtime` of 367a11e; `fov=51.28` via a scratch `XDG_CONFIG_HOME=$W/compare/xdg`
`bbtime/settings.ini`).

| file | content |
|---|---|
| `compare/lit_L01A_side_by_side.png` | original (eye (23613,−1000,11550) → look (23700,−1039,9655)) / bbtime fov 60 / bbtime play camera |
| `compare/lit_L01A_side_by_side_fov51.png` | original / bbtime fov 51.28 |
| `compare/tb_ARCA01_side_by_side.png` | original (eye (−4928,−369,−8434) → look (−4107,−310,−7555)) / bbtime fov 51.28 / fov 60 |
| `compare/tb_blend_sheet.png` | 50 % blends original+bbtime at fov 51.28 (edges coincide) and 47.5 (doubled edges) |

Differences seen (R, both at fov 51.28 unless noted):

* **FOV**: bbtime's 60° default is visibly wider; 51.28° lines up wall edges, arch, palm, cactus (LiT) and the carpet,
  walls and stall (TB).
* **Missing actors** (bbtime free-fly/`--camera` view): LiT — the boulder blocking the L01A arch (so bbtime sees
  through to trees and a second arch), the cactus in front of the right-hand bush, Bugs; TB — Bugs, Taz, the palm
  tree left of centre, the small far NPC/pickup by the steps.
* **Brightness/colour**: the PC originals are darker (PC port divides vertex colour by 255; bbtime uses PS1
  ×255/128 modulation, MODELS.md) — LiT ground/walls and TB stone are clearly brighter and lower-contrast in bbtime.
  Choose deliberately for Original Fidelity.
* **Shadows**: LiT original shows a dark blob shadow on the ground (0x44bb50 shadows/particles) — absent in bbtime.
* **Fog**: no visible fog at these spawn distances in either game (D = 39876 on L01A).
* **Texturing**: TB original is perspective-correct (flag above); LiT original affine (static) — not distinguishable
  at these views.
* Reference originals render with Mesa llvmpipe in Xvfb (no hardware GL), so filtering/precision details are
  llvmpipe's.

**After the fixes** (`build-polish`, `--no-hud`, GL = llvmpipe in a private Xvfb like the originals, VK = radv via
XWayland; `tools/wine/compare.py ORIG BB OUT` writes `[original | bbtime | |diff|×4]` and prints mean abs diff (MAD,
0..255) and the least-squares gain original/bbtime):

| compare (`compare/`) | MAD GL / VK | notes |
|---|---|---|
| `after_lit_L01A_play{,_vk}.png` (play mode, `--autoplay '-*70'`, no `--camera`) | 1.56 / 1.55 | gain 0.99/0.985/0.975; camera = director point (23613,−1000,11550) as the original; boulder, cactus, sky dome present |
| `after_lit_L01A_spawn_{gl,vk}.png` (`--camera` from `cam.py`, free-fly: actors not simulated) | 5.10 / 5.08 | before: 21.15 (colour ×255/128, no texel curve); residual = missing boulder/cactus in the free-fly view |
| `after_tb_ARCA01_spawn_{gl,vk}.png` (`--camera`) | 5.86 / 5.16 | gain 1.00–1.04; residual: sky gradient (top right), Bugs/Taz/palm/NPC not posed in free-fly, uniform ≈ 2.5 % darker |

Brightness rule (R + S): LiT `bugs.exe` GL renderer (`0x4ac094` = 2) multiplies by colour byte/255 (no doubling) and
uploads CLUT texels `<< 3` through table `0x4b5b40` = trunc(256·(i/256)^(5/6)) (`0x422a40`, exponent `0x45ca08`;
enabled `0x4ac034` = 1 at runtime; read live: 0,25,45,…,255) then identity table `0x4d5d40`. TB `bblit2.exe` has the
same two-table stage (`0x639da4` = 1) but both tables (`0x639de0`, `0x63ae80`) are identity at runtime. bbtime Original
Fidelity now uses colour scale 1.0 + `Scene::pcTexGamma` (LiT 5/6, TB 1); Enhanced keeps the PS1 ×255/128 look.
FOV: Original Fidelity is fixed at 51.28° in both backends (`bb/render/camera.h`); camera eye +400: the code already
matched (§6 item 2 was a doc-only error). §6 item 3: the play camera is initialised by the load-time update
(`LitWorld::loadUpdate`, 0x430f80) and the type-36 camera director 0x444b50 + camera 0xc00 mode are ported; first
frame now shows the spawn snap (eye 100 behind / 400 above Bugs), not the origin. Residual: bbtime's walk-in moves
Bugs 2 ticks earlier and stops at z 9630 (original 9655, last step +50) — carrier timing not resolved.
Shadow blob (S, Research Required): 0x44c4b0 registers every drawn actor with tag `0x46` (`+0x3a` = shadow entity
id, parsed 0x42e7a0 case 'F') into `0x4b21a0` (max 40; Bugs skipped in first person); 0x44bb50 draws, per actor, the
part of type 6 of that entity's model on the ground (0x44b4f0 probe; on platforms scaled `(y−ground)·4 + 0x1000`,
min 0x800) darkened by the zone `+0x28` (0x44cb90). Not small (entity lookup, ground probe, part-type-6 mesh, blend):
not implemented.

## 8. Other captures (for LiTWorld / TBPlay)

`csv/litworld_L04C3_links.csv`, `csv/litworld_L02A1_links.csv` (texture-link alias table `0x4e5e80`),
`csv/litworld_L01A_idle.csv` (+ `shots/litworld_L01A_{2,8}s.png`); `tbplay/{A_idle_jump,B_run,C_switch_taz_jump,
D_callfriend,E_switch_turn_L2,F_2p_granny,probe_start}.csv` + PNGs. Title screens: `shots/tb_title_menu.png`,
`shots/tb_title_sheet.png` (CCINTRO), `shots/tb_pause_menu.png`, `shots/lit_title.png`, `shots/lit_menu.png`; levels:
`shots/lit_L01A_spawn.png`, `shots/ge_lit_L01A.png`, `shots/ge_tb_ARCA01.png`.
