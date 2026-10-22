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
| `peek.py lit\|tb ADDR TYPE [N]` | one read; `ADDR` may be `[0x4b324c]+0xe4` (u32 deref); `--selftest`. `peek.py lit ADDR poke HEX` writes bytes (memsrv `W`; test-only, e.g. open a menu — needs an oracle started after the memsrv change) |
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
* **LiT exits/hangs at the title** (this pass, Proton-GE ntsync): left idle on the title logo (no menu open) the exe
  quit with the same X `BadWindow`/`X_UnmapWindow` ~10–20 s after boot (3×), and once froze (S[] static) after a
  Quit Game → title. Opening the slot menu promptly (Return until `S[3]` = `peek 0x4b2263` reads 2) avoids it.

### 1.1 PS1 originals (second oracle, `tools/psx/run.sh`)

PCSX-Redux HEAD AppImage (`~/.local/bin/PCSX-Redux-HEAD-x86_64.AppImage`, built-in OpenBIOS, no Sony BIOS needed)
runs the USA discs (`Bugs Bunny - Lost in Time (USA) (EnFrEs)/…cue`, `Bugs Bunny  Taz - Time Busters (USA)
(EnFrEs)/…cue` under the project root) headless via `-cli` under `xvfb-run`; one run = boot (or load a state) →
N vsyncs with a scripted pad → dump `ram.bin` (2 MiB, 0x80000000), `scratch.bin`, `shot.ppm`, optional save state.
Boot to Behaviour logo / TB language menu: ~1500 / ~2900 vsyncs, varying by a few hundred vsyncs between runs, so
boot scripts pulse buttons (`X*6,_*54` repeated) instead of timing single presses. ✕ picks the LiT language and
continues its memory-card prompts; Start opens the title menus. Save states (`PSX_SAVE` / `PSX_LOAD`) skip the boot;
a LiT state saved on the title / language page reloads into the language page (title XA stream), in-level states
reload exactly. States in `local/psx/states/`: `lit_game.sav` (hub "Nowhere" after New Game), `lit_x40.sav` (Bugs
in control on the Nowhere drawbridge), `tb_x40.sav` (GRANNY after "Welcome to Granwich!", Bugs + Taz in control);
`local/psx/tb_after_lang.sav` = TB title (`PSX_KEYS='_*30,ST*4,_*1' … tb 400` opens its menu).
`PSX_TRACE=name=0xADDR:s32*3,…` writes a per-vsync `trace.csv`; known addresses: `tools/psx/syms_{lit,tb}.txt`
(player positions, LiT camera look-at). Both games update actors every 2nd vsync (30 Hz, as on PC).
`PSX_LEVEL=0xLOADER:0xLEVELVAR:FROM:TO` swaps the level the loader is asked for (interpreter + breakpoint): LiT
`0x8004443c:0x80010000:2:4` boots into L01A (~40 s); PS1 level indices equal the PC table indices. In that run the
L01A walk-in matches §10's PC trace exactly (attach z 8830, +25 per tick, release at z 9655). The TB redirect
breakpoint never fired; the PS1 comparison was shelved at this point (ROADMAP.md, Wave 9).

What carries over from PC: the level container (PS1 `.BZZ` = same section table and LZSS as PC `.BZE`, same
section ids; PS1 TB files have first header word 1 = LZSS sections, read by `parseBze`) and the level-script format.
Script-entity comparison (chunk entities by id + position, PS1 vs PC, every level with a PC twin):

| game | PS1 scripts parse | levels whose PC entities all appear in PS1 | rest |
|---|---|---|---|
| LiT | 108/108 (`bzedump --script`) | 71/74 | TITLE 57/59, L02A2 124/126, L02A3 97/98 |
| TB | 58/58 (`tools/re/tb_sec1.py`) | 45/58 | ≥ 90 % each (e.g. ARCA01 244/249, TRCA01 148/162) |

PS1-only levels: LiT `L_*` loading / bonus scenes, `DEMO*` attract demos, `LANGUAGE`; TB `LOADING/SCREEN1–3`,
`LANGUAGE`, `TITLE`. L01A section 2 decodes byte-identical; geometry/model sections are a different encoding
(bbtime's model decoder rejects PS1 L01A). What does not carry over: the code — `SLUS_008.38` (LiT, load
0x80014a00) and `SLUS_011.44` (TB) are MIPS PsyQ builds, so PC Ghidra addresses and RAM layouts do not transfer; a
PS1 observation backs a PC claim only via behaviour (screens, values located anew).

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
| level start: state 2, then scripted walk-in | state 2 at tick 0, state 130 from counter 3 (attach, z 8830), Bugs +25 z/tick one tick behind carrier 246, state 1 at z 9655 when the carrier's 34th update releases him (counter 69→71) | `csv/litworld_L01A_idle.csv`, `csv/lit_spawn.csv`, `csv/litfid_walkin.csv`, `csv/litfid_walkin_list.txt` | **Verified** (bbtime: `replayL01a` per-tick asserts, §7) |
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
| AUDIO_XA.md levels 0x41f620 (distance from the target, angle from the eye, 4096 range, players 100/100) and frequency `rate · 2^(5 − note/12)` (0x41f04c) | 51 voice starts in 30 s (code cave at 0x41f094 before SetFrequency): every positional voice's +0xc/+0xd = `sfxLevels` of the hooked emitter/eye/target (26/14, 28/6, 23/2, 7/1, 26/28, 3/24, 9/3, 0/0 …), Bugs 100/100; SetFrequency 24750 / 23361 / 15592 / 13891 / 12375 for notes 70 / 71 / 78 / 80 / 82 of 44100 Hz blobs | `local/wine/sfxcheck/lit_starts.csv` (RUNNER=wine: Proton-GE without `AUDIO` gives LiT no DirectSound, voices stay 0x8001) | **Verified** (`game.sfx` asserts 8 level rows and 14 sound frequencies) |

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
| §4.2 call-friend arrival: partner program block 4 spawns carrier template 169 at the floor spot; partner rides its TOD 0xa00 part | carrier entity (id 169) at (16442, −2100, 6921) = floor point; Taz y − floor −1432, −1432, −1337, −1242, −1139 … −103, 0, states 261 261 260 262 … 263 50 142; Taz x/z = carrier + (4, −1) | `tbplay/D_callfriend.csv` ticks 481–527, `tbplay/H_callfriend_carrier.csv` (Taz `+0x90/+0xe8/+0x84`, carrier pos/frame/anim) | **Verified** (bbtime `game.tb` asserts all 17 heights and states; x/z ≤ 5: carrier face-camera yaw not modelled) |
| §3.2 idle fidgets: state 8 `ticks > 30 && rand() > 30000` → S7 `S[0x1fe] = rand() % 5` → 17/20/23/26/29 sequences; loops 18/21/24/27/30 exit at the last frame when `ticks > 90` or `ticks > 20 && rand() > 28000` | Bugs 8 → 20 (tick 449), 21, 22, 8, 29, 30, 31, 8, 26, 27, 28, 8 … 17, 18, 19, 8, 23 — 37 changes in 100 s; `+0x82` 7, 8, 9 … (not reset on entry); rand seed `0x4cf26c` from 0x64e07fb7 with 0–7 draws per tick; `+0x14e` 0x2000 on the last frame's hold-expiry tick and the loop-restart tick | `tbplay/G_idle_rng.csv` (ticks 379–1771; rows with Bugs state 383 = intro) | **Verified** (bbtime `game.tb`: traced seed set per tick, all 37 changes on the same tick); state 23 holds 10 ticks vs 6 from its 4-frame TOD (open) |
| TIME_BUSTERS.md §6 P1 key slots | runtime `config.pc`: arrows, L1 A, L2 D, R1 S, R2 F, △ W, ○ Q, ✕ E, □ R, select Tab, start Esc, View none; P2 numpad 8/5/6/4 … | `$W/tb/Datas/BIN/config.pc` | R (defaults of 0x415fd0) |
| §3.3 view list: +0x6e 2 culls beyond +0x76 from the camera, sleep +0x50 | guard 267 (0x41a, 7500) frozen (+0x82 390 constant over 600 ticks) ~7.3k from the camera; after Bugs is poked 1200 in front it ticks again | `tbnpc/A_guard_patrol.csv`, `tbnpc/B_guard_attack.pkl.gz` (Proton-GE oracle; pool `0x4d2704`, 465 × 0x1b0 B per tick + camera, S, GD) | **Consistent** |
| §3.4 guard logic (template 265): notice in front within 2500 → 497 504 502 501 364 (throw) | 357 522 497 504 502 501 364 435 502 501 364 435 …; 364 @55, 401 spawned @71 (state 395, 3 ticks still), out and back along its TOD 0xa00 point (dy −33, +6, +5 …; dx 103, 110, 105 …), `+0x20` 0x2000 @128 at the hand, 435 @133, 502 @181, 501 @182, 364 @188; Bugs hurt 189/190/191 by the thrown item, health 6 → 5 (@83) → 4 (@221); S[0x7a] 0 → 1 @84. Second run with every rand() traced (tbrand): 521 497 504 501 364 (no 502), the first throw misses → 502 @+78, 501 @+79, 364 @+85, the second hits | `tbnpc/B_guard_attack.pkl.gz` (401 state = 5th record of its `+0x110` list, read from the pool's template entity); `tbrand/states.csv` | **Verified** (bbtime `game.tb`: 497 504 [502] 501 364, ticks relative to 364 +16/+73/+78, then the hit branch (B: +126/+127/+133, flight dy exact / dx ±1) or the miss branch (tbrand: +79/+85), a later hit, S[0x7a]); 521/522 and the 401 variant 394/395 are rand() picks: the test runs the default seed 1 (521, miss) and seed 66 (B's 522 … 502 501 364, hit, 435), so both branches and the per-tick flight are checked |
| §3.5 melee hit / flash / death | guard 502 → 394 → 500; Bugs 208, health −1 per hit, flash `0x4d2058` 0x40 → 0x7c then 0x3c; Bugs at 0 health: f20 0x2000100, GD+0xa2 1, A4 4 (Taz), lives (GD+0xa0) 3; checkpoint `0x4d26e0` = (−2391, −52, −6293, 489) | `tbnpc/C_*`/`D_*` (kernel captures, saved B only), peeks | **Consistent** (bbtime test: melee 394 → Bugs 189, health 5, flash; death → partner; both down → lives 2) |
| §3.5 Taz spin into the facing guard (flagsA 0x1000000, contact damage 1) | Taz poked 173 in front of the guard on its 502 tick, □ held 40 ticks: guard 394 394, 433 ×17, 502, 394 ×24, then 500 ×56 (F) / 435 ×48 (F2, F9), 502, 501 ×6, 364 ×78 (throw: Taz 190 again @120), 435 / 500 …; Taz 135 ×6, 136 ×12, 140 ×5, 190 ×13, 10 ×12, fidgets; guard health stays 5, Taz −2; neither raises +0x20 0x40/0x80; guard +0x54 = Taz from the first tick. F9 (wave 9): Bugs and Taz idle (8) on the 502 tick, both `+0x82` poked 0 with Taz's 173 poke (guard yaw 781 then, offset (161, 0, 63)): guard 394 ×2, 433 ×17, 502, 394 ×24, 435 ×48, 502, 501 ×6, 364 ×78, 500 ×56, 502, 501 ×6; Taz … 21 ×45, 32, 17, 18, 190 @120 …; Bugs 8 26 27 28 8 23 24 25 8 17 18 19 8 29 30 | `tbnpc/F_taz_spin.pkl.gz`, `tbnpc/F2_taz_spin_rng.pkl.gz` (RUNNER=wine; scripts `tbnpc/spin_capture.py`, `spin_rng_capture.py`: per tick guard/Taz entity 0x1b0 B, state ids, F2 also the rand() seed `0x4cf26c`); `local/shots/w9tb4/F9_taz_spin_rand.pkl.gz` (+ F4/F5/F6; `tbnpc/spin_rand_capture5.py`, `spin_rand_report.py` → `F9_report.txt`: every rand() call with tick, caller, registers; per tick Bugs / Taz `+0x82`, guard / Taz 0x1b0 B) | **Verified** (bbtime `game.tb`, F9 seeds replayed per tick: guard, Taz and Bugs states match all 240 ticks; F2 cannot be: Bugs' states were not captured, and his draws shift the guard's). F5 (guard 500 → 502 → 501 ×7 at its tick 213, ours ×6; Taz and Bugs match all 240) is not asserted: Research Required |
| rand() 0x46afe0 callers (one shared stream) | 17 call sites; from ARCA01 after ✕ through the guard setup above: 465 calls in ~115 ticks, all at 4 sites: `S` 7/8 `rand() % X` 0x430f76 (255; director and asset logic every frame step), chance `W` 0x45a6df (108; guard 364, Taz fidgets, asset logic), particle effect 0x452ab0 2 per particle 0x452cb5 + 0x452ce2 (51 + 51: V 0x200000 of the class-16 director with slot 13, count 1, flags 0x92d). Effect slots (0x4d6ae0, 20 × 0x9c) read back: slot 3 count 20 flags 0x92d, slot 13 count 1 flags 0x92d bound to an id-0 entity, others the 0x451e40 defaults 10 / 0x40. Within a tick the callers come in entity-loop order: the focus player, the partner, then the others (F4/F5/F9: Taz before Bugs after the switch, Bugs before Taz before it); the view list `[0x4d2744]` read live is `B T 20 285 …` with Bugs in focus and `T B 20 174 285 …` after the switch (0x45eee0 leaves flags0 0x4000000 players out of its in-order append, 0x45ff56) | `tbrand/rand.csv` (seq, tick ×2, return address, seed before), `states.csv`; scripts `rand_capture.py` + `rand_hook.s` (cave 0x480240, ring 0x63d600); `local/shots/w9tb4/viewlist_switch.txt`, `tbrand/rand_hook2.s` (64 B records: + esi/edi/ebx/ebp and 9 stack dwords → caller entity) | **Verified** (sites, order); bbtime draws the asset logic on the shared stream, runs the director's block 0, simulates the effect draws (S 118 slot fields 4/7/10, V 0x200000) and ticks the focus, then the partner, then the rest (`game.tb` spin test, F9) |
| §3.4 item pickup by touch (ARCA01 item gdBit 473) | box `+0x60..` (−107, 0, −17, 0, 107, −215, 19), r 240, `+0x78` 0, `+0x123` 0; Bugs poked 150 beside it → next tick 2nd state record, `+0x82` 1, `+0x24` 0x400000, GD+0x5a/+0x6c/+0x84 0 → 1, GD+0x22f 0 → 2, S[0xa8] 1; 31 ticks later the 3rd record, y −55 → −205 … −955 (150/tick), `+0x20` 0x2100 @+38 at −1105 | `tbnpc/H_item_pickup.pkl.gz` (RUNNER=wine; script `tbnpc/item_capture.py`; per tick item + Bugs entity, GD, S) | **Verified** (bbtime `game.tb`: box, counters, state ticks, rise, removal tick) |
| §3.5 game over → level 5 (0x44ea50) and the reload 0x4014c0 | GD+0 poked 5 + `0x4d593a` 1 → RETRY loads (Daffy, then "GAME OVER / Do you want to continue? YES NO"); ✕ on YES → GD+0 4 (GRANNY), lives 3, health 6 6 | `tbnpc/retry/G0_retry_load.png`, `G1_game_over_prompt.png`, `G2_yes_granny.png` | **Consistent** (bbtime plays RETRY: close-up, prompt text, ✕ YES → GRANNY; `game.tb` + `local/shots/w6tb4/retry_*`; Daffy's pose / rock prop not compared) |
| HUD coin counter slide (0x463170, slide record 0x4d5da0 + 4·16) | ARCA01 item 473 pickup: x 86, 52, 18, −16, −41, −57, −64, −62, −55, −43, −26, −4, 16, 27, 29, 22, 10, −4, −9, −5, 1, 2, −3 from the pickup tick, 0 until tick 84 (60-tick hold), −12 … 105, flag 0x200 clear at 94 | `tbnpc/hud/slide.csv` (RUNNER=wine, `slide_capture.py`, start via `start_arca01.sh`) | **Verified** (`game.tb` tick for tick; HUD runs after the entity loop) |
| level-change fade (0x401910 / 0x4018a0, 0x464ef0) | out: level 3840 → 0 (−256) over 16 frames while the world ticks, then the load; in: speed 200, dark 4096, 3896 … 96, mode 0 at counter 45; RETRY the same | `tbnpc/hud/fade_trace.csv`, `tbnpc/retry/retry_trace.csv` | **Verified** (`game.tb`; app `tbFadingOut`) |
| entity register (0x45d7a0) | u16 S[0x1fe] (0x4d269e) = the running entity's +0x126, stored back on exit | disassembly 0x45d7a3 / 0x45ea5b | Implemented (RETRY needs it: the players' 6 stays in their register) |
| entity drawing at the simulated pose | guard 267 poked to its spawn (1145, −15, −5227), Bugs at (1545, −15, −5927) yaw 0: guard and the "1" token / item at the same screen spots as bbtime with the oracle camera (`cam.py`: `--camera 1312,316,7100,-0.19608,-0.00502`) | `compare/orig_tb_ARCA01_guard.png`, `compare/w6_tb_ARCA01_guard_{gl,vk}.png` (mean abs diff 4.4) | **Verified** (placement; the guard's yaw differs: the original was already turning to Bugs) |

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
| **Clear / fog colour (S, static, this pass).** LiT: `glClearColor` slot `0x7c6274` has two callers: GL init `0x40daf0` (0,0,0,0) and `0x40d910` (r,g,b bytes → gamma table `0x4b5b40` + HSV tweak `0x4250f0` → clear colour), reached only via SetBackColor `0x40cc10` (renderer & 2 → GL; renderers 0/1 → slot `0x565fe0`), whose three callers (`0x41ffac`, `0x4208de`, `0x44be5f`) all push **0,0,0**; no level chunk feeds it. Fog has no colour: the emitter (`0x4147b4`…`0x4156f9`) writes a vertex alpha ramp from `0x4efb60/64/68` (1 at 0.75·D, 0 beyond D) and moves such polygons to the blended lists `0x4e7dc8`/`0x4e7dcc` (flush `0x4124b0`), so far geometry fades into whatever is behind it — the black clear colour, or the fog-exempt type-9 sky (I: no other far colour). TB: `glClearColor` slot `0x4d16bc` has one call, init `0x422950` (0,0,0,0); `0x409840` only sets near/far clip slices `0x639b04..0x639b20` (I: the second slice draws the object's far-LOD model, `0x406840`); no fog colour writer exists; tag 0x66 (`84 84 84`) stays unread (TB_LEVELS.md). | R: `shots/lit_menu.png` (TITLE strip under the copyright and the attic gaps are black); `compare/orig_tb_ARCA01_spawn.png` (the sky is the dome mesh, no clear colour visible). bbtime `Settings::fogColor` = black (was a hard-coded light blue) for both games, renderers and fidelities | **Verified** (LiT TITLE) / Consistent (TB, L01A: no clear colour on screen) |

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
   the 2-player menu entry sets `GD+0xA4` bit 0x10; in play it reads 0x12 (the 2P switch rule 0x45eee0 adds 0x02 every tick; bbtime `game.tb` asserts 0x10 → 0x12 after one tick).
9. **LOST_IN_TIME.md / RESEARCH.md launch options**: LiT needs `/b00` to reach the title on PC (launcher passes it);
   with no `/b` the default level 2 (LANGUAGE.BZE) is missing from the PC disc.
10. **GAMEPLAY.md §8.1** (now §LiT saves below): the save checksum rule, slot naming, Savedata summary layout and
    the load path are **Verified**; the S[0x08] result reaches the logic as S[17] = 10 (the type-16 menu entity copies
    S8 → S17, L01a @0x1eb45).

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
same two-table stage (`0x639da4` = 1) but both tables (`0x639de0`, `0x63ae80`) are identity at runtime; its CLUT
colours then go through the HSI brightness pass 0x407d30 (config +0xe0, 1 by default; wave 8: `bb::tbAdjustColor`, §16).
bbtime Original Fidelity now uses colour scale 1.0 + `Scene::pcTexGamma` (LiT 5/6, TB 1); Enhanced keeps the PS1 ×255/128 look.
FOV: Original Fidelity is fixed at 51.28° in both backends (`bb/render/camera.h`); camera eye +400: the code already
matched (§6 item 2 was a doc-only error). §6 item 3: the play camera is initialised by the load-time update
(`LitWorld::loadUpdate`, 0x430f80) and the type-36 camera director 0x444b50 + camera 0xc00 mode are ported; first
frame now shows the spawn snap (eye 100 behind / 400 above Bugs), not the origin.
**Walk-in (fixed, Verified):** fresh capture `csv/litfid_walkin.csv` (player + carrier `+0x70` chain) and
`csv/litfid_walkin_list.txt` (live list `0x4b3740` poll, `local/litfidelity/walk_list.py`): counter 3 state 2 → the
director spawns template 246 and attaches Bugs (state 130, `+0x70` = carrier, flags14 stays 2) on the same tick;
the carrier moves +25 z/tick from its 2nd update; Bugs' world z = carrier z one tick late (he is updated before the
carrier: player is linked ahead of the rest list, 0x447d40) — 8830, 8830, 8855 … 9605; the carrier's 34th update
(z 9655) releases him (item flag 0x4000, 0x440120 `LAB_00443606`+: world position recomputed from the coordinate
chain = carrier position, `+0xdc/+0xe0` cleared, flags14 `& 0xfffff5f6 | 0x1000`) → state 1 at z 9655. Counter c ↔
bbtime tick (c−3)/2+1. Root cause of the old 2-tick/25-unit offset: bbtime copied the carrier position after the
whole actor pass (Bugs same-tick instead of one tick late) and released him at his own stale z. `replayL01a` asserts
every walk-in tick. **Verified**.
**Shadow (implemented, Verified visually):** 0x44c4b0 registers every drawn actor with tag `0x46` (`+0x3a` = shadow
entity id; L01a Bugs and 12 actors use entity 3) into `0x4b21a0` (max 40; Bugs skipped in first person, camera flags
0x20). 0x44bb50 draws the first role-6 model of that template entity (L01a entity 3: one 0x40 quad ±200, texture
133 = a soft 64×64 grey blob, flags 0x09 ABR 2) at: position = the actor's second part origin (0x44ba00 →
0x4375d0), ground via GroundAt 0x436c20 (open floor drops to the zone below), + hoff, clamped to `0x4b2378` (chunk
0x3b tag 0x3c s16, A88/A91) and not above the actor unless camera flags 0x100; scale x/z = `(y_actor − y_shadow)·4 +
0x1000` (min 0x800, 0x1000 when the ground is above); tilt from three box-corner ground probes kept within ±0x22
(0x44b4f0 second pass); ridden platforms (flags8 8 / flagsC 0x2000) put it at the feet with the platform's tilt.
PC GL draw of ABR 2 (0x420d10 case 2, 0x4122xx flush): texel alpha = trunc(0.299R + 0.587G + 0.114B) of the raw
texel, blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA (`0x4ac028/0x4ac02c` = 0x302/0x303 at runtime defaults) — a
lightening/greying blob, not a PS1 subtractive one; Original Fidelity uses this in both backends (Enhanced keeps the
PS1 subtract). Side-by-side `compare/side_lit_L01A_shadow_{gl,vk}.png` (original `orig_lit_L01A_shadow.png`
after the walk-in, bbtime `--autoplay '-*90'` tick 90): MAD 1.58 / 1.58, gain 0.99/0.99/0.98; the soft blob under
Bugs' feet matches in place and size. Not ported: the first 0x44b4f0 pass (projection onto a non-ridden class-2
actor below), the zone darkening 0x44cb90 tint, ordering-table depth (bbtime pulls the quad 24 u toward the eye).

## 8. Other captures (for LiTWorld / TBPlay)

`csv/litworld_L04C3_links.csv`, `csv/litworld_L02A1_links.csv` (texture-link alias table `0x4e5e80`),
`csv/litworld_L01A_idle.csv` (+ `shots/litworld_L01A_{2,8}s.png`); `tbplay/{A_idle_jump,B_run,C_switch_taz_jump,
D_callfriend,E_switch_turn_L2,F_2p_granny,G_idle_rng,H_callfriend_carrier,probe_start}.csv` + PNGs. Title screens: `shots/tb_title_menu.png`,
`shots/tb_title_sheet.png` (CCINTRO), `shots/tb_pause_menu.png`, `shots/lit_title.png`, `shots/lit_menu.png`; levels:
`shots/lit_L01A_spawn.png`, `shots/ge_lit_L01A.png`, `shots/ge_tb_ARCA01.png`.

## 9. LiT saves (GAMEPLAY.md §8.1, formats/SAVE_LIT.md)

Files (R): `local/saves/lit/save{1_merlin_new,2_merlin_cheats,3_gc333_credits}/{Savegame2.dat,Savedata.dat,
gd_before*.txt}` + PNGs; the prefix's `Datas/bin` blanks were restored afterwards. One session, Proton-GE:
title → New game → slot 2 (`G[0x9f]` = `[0x52fd00]+0x100df` reads 2 at slot select) → Start → CCINTRO (56) →
LS01 (3) → CCMERLIN (70) → MERLIN (51). The pause menu has no save entry; the prompt "Do you want to save your
game? Yes / No" is the type-16 menu entity of every playable level, active while `S[17]` = 1 (written by the
Merlin/LS01 save actors, e.g. MERLIN entity @0x104df state 129 on ✕ after its tutorial dialog). It was opened with
`peek.py lit 0x4b2271 poke 01`, then ← (S[18] = 1, Yes) and ✕ → `S[0x0a]` = 6 → 0x4074f0.

| save | trigger | observed | verdict |
|---|---|---|---|
| 1 | prompt Yes in MERLIN, new game | `Savegame2.dat` + `Savedata.dat` written (mtime), `Savegame1/0` untouched; GD+0 = 0x33 (51); +4 = 0x17a = Σs8 of the other 316 bytes (old +4 = 0); S[17] = 10 afterwards | **Verified**: slot file = `Savegame%d` with %d = G[0x9f] (1-based) |
| 2 | cheats `□R1✕L1 □R1✕L1` (G[3] = 99) and `□R1✕L1 ✕R1✕L1` (G[8] = 0x1f), prompt Yes again | +4 = 637 = 514 (rest) + 0x7a + 0x01 (old checksum bytes) | **Verified**: the old checksum is part of the sum |
| 3 | cheat `□R1✕L1 ✕□R1L1` (GC 333, clocks 124 → CCEND 71), then `poke 0x4b226a 06` | Savedata +0x34 = `4d 01 7c 00` = {u16 333, u16 124} at +8 + 0x2c·(2−1); mask +4 = 2; S[8] = 0x0a | **Verified**: summary layout, mask bit `1 << (slot−1)`, request 6 is processed in any level (CCEND has no save entity) |
| load | restart, title (`S[0x2b..]` = 0x14d / 0x7c copied from Savedata; menu shows "2. 333 124"), Load game → slot 2 (save 2 file) | GD = file bytes, G[0x0e] = 0x33, then level 51 with G[3] = 99, G[8] = 0x1f; Bugs at the level spawn (5940,−560,10050) | **Verified**: load continues in GD+0's level, position not saved |

bbtime: `ctest -R game.lit_save` (all three files parse byte-identically; `writeLitSave` from the pre-save state
reproduces each original `Savegame2.dat` **and** `Savedata.dat` byte for byte). Smoke: `XDG_DATA_HOME=local/saves/
smoke_xdg bbtime --import-saves local/saves/lit/save2_merlin_cheats --game lit --slot 2 --hidden --mute --frames 60
--screenshot local/saves/smoke/lit_slot2_loaded.png` → "slot 2 …: level 51 health 6/6 carrots 99", MERLIN, Bugs at
(5940,−561,10050), HUD CARROTS 99 (original after load: `save2_merlin_cheats/orig_after_load{,_pause}.png`).

Save prompt overlay (2026-10-19, Proton-GE, `run_original.sh lit /opengl /win /x640 /y480 /b51`, `peek.py lit 0x4b2271
poke 01`; shots `local/wine/compare/litsave/orig_{before,prompt,yes_1..3}.png`):

| claim | runtime | verdict |
|---|---|---|
| glyph list `0x4b373c` (count `0x4b39a8`) | 29 entries `{sprite, s16 x, s8 y, u8 pal}`: "Doyouwanttosaveyourgame?" x −141, −130, −114, … 125 at y 50 pal 1, then Yes x −50/−37/−29 and No 30/43 at y 70 pal 3; sprites = font base + 0x10·(glyph−1) of table `0x4ae3a8` | **Verified**: interpreter positions (advance table `0x4ae4a8`, centring by width >> 1, command byte skip) = `test_lit_overlay` |
| glyph scale `0x4efb88` | 1.0 at 640×480 | **Verified** |
| palettes / screen space | 1 yellow (207,201,0), 3 blue (0,0,200), 0 white (207,201,200) after ← (S[18] = 1) outside the S[11] ≤ 10 blink; text centre x = 320/640, y 50 → rows 344–353 of 480: 512×240 space centred, 16×16 cells | **Verified** (colours sampled, used by the app) |
| the prompt does not freeze Bugs | ← also walked/turned him | Observed |
| F1 help (L01A `/b04`, 2026-10-19; `local/wine/compare/litfont/orig_help{.png,_glyphs.txt}`) | `S[12]` = 3, 178 glyphs "Help Menu", "Look A", "Crouch S/Button1" … "Enter Select" (prefix config: use-joystick `0x9ca858` = 1, joyMap 0..7, kbdMap default); Bugs frozen | **Verified**: `testHelp` asserts all 178 glyphs (key names, `>DJ` suffix, spaces dropped, palettes 2/3) |
| font sprite size | `0x4efb84` = 1.0 at 640×480; glyph rows 344–353 for y 50 (cell 16×16 px, not ×2 vertically); x = 320 + ftol(x · 1.25) | **Verified**: bbtime 640×480 VK/GL prompt covers the original's yellow/blue/white pixels exactly (+7 edge px), colours ±1 (`compare/litsave/font_crop_compare.png`) |

bbtime: `ctest -R game.lit_overlay`; side-by-sides `local/wine/compare/litsave/{side_by_side_font,font_crop_compare}.png`
(original font sprites) and `local/wine/compare/litfont/side_by_side_help.png`; in-play save
smoke → `local/saves/smoke_prompt/bbtime/saves/lit/Savegame3.dat` + `Savedata.dat` (GAMEPLAY_STATUS.md Saves row).

## 10. LiT enemies (L01A, `/b04`, template 932) — GAMEPLAY.md §7.1–7.3

Captures (`local/litenemies/enemy_trace.py OUT SECS IDS [TYPES]`: one row per tick counter with Bugs, pad, eye, rand seed
`0x4ae610` and selected actors; rows sampled mid-tick repeat the previous seed = "torn"): `csv/litenemy_L01A_idle.csv`
(no input, 1263 counters), `csv/litenemy_L01A_idle2.csv` (+ type-2 actors, Bugs anim), `csv/litenemy_L01A_fight.csv`
(Bugs poked to (24500, −624, 14200) after counter 101 with `local/litenemies/poke_pos.py`, then □ kicks / ✕ jumps),
`csv/litenemy_L01A_fight2.csv` (the same fight replayed by counter with `local/litenemies/pad_play.py fight_pads.txt`,
identical except four key releases one tick later, plus `bbox`/`abox` columns: box `+0x48..+0x54`, radius `+0x2e`,
inflation `+0x20`, rotation `+0xdc` of Bugs and each traced actor; its pads: `local/litenemies/fight2_pads.txt`).
Comparison: `PROBE=1 PROBE_PADS=… [PROBE_TELEPORT=103,24500,-624,14200] [PROBE_IDS=932,39,542,37] [PROBE_BOX=1]
test_game` + `local/litenemies/cmp.py` (`seeds.py` separates torn rows from real rand differences).

| claim | runtime | verdict |
|---|---|---|
| tick mapping | bbtime tick t (after `loadUpdate`) = counter 2t + 3; rand seed is 1 at counter 0 (no srand) and identical across runs | **Verified** |
| zone-gated actors (+0x3c & 1) | start director fires on counter 5, not 3 (table 0x4b3460 empty before the first frame) → carrier spawn/attach and camera director one tick later than the load pass | **Verified** |
| spawn order 0x448d40 | spawned actor runs in the spawning tick (appended to the active list): 932 timer 1 on its first row (counter 9) | **Verified** |
| activation distance | 0x4078d0 estimate; 932 director (radius 6000) activates when the eye reaches z 9830 (counter 9) | **Verified** |
| type-2 sprite actors / anim events | L01A position-less type-2 props hold flipbook frames (0x438970); Bugs' fidget anim 72 event A19 sets G[176] = 3 at counter 89 → props state 361 | **Verified** |
| sound channels | without an audio device the 24 channel slots `0x4b3260` never free (14 busy at counter 237, 24 at 1739) → chance sounds stop consuming rand | R (tests set `sfxChannels = 24`) |
| enemy 932 idle | wander, ledge turn at counter 433 (+682 steer, +512 A38), random state 361 — all 1263 counters match | **Verified** (`testEnemyL01a`) |
| enemy notices / chases / attacks | kicks → state 236 (counter 603), chase 177 (659), attack 201 (725), step 217 (755) match | **Verified** |
| Bugs' box | x/z always (−70, −40)..(70, 40), radius 80, inflation 0; only the heights follow his packet-9 frames — 0x43a880's tail overwrites them each tick unless the move has flagsB 0x2000 | **Verified** (fight2 `bbox`, every row after the load) |
| hand hit / push-out | Bugs pushed away 757–763 by the growing attack box (TOD packet 9, 0x433500 face push), hand attach point (TOD packet 10, 0x434840) hits at 769: health 5, state 10 | **Verified** exactly (`testEnemyL01a`; the former z +1 came from his animated box) |
| knock-back after the hit | slides (0, −8) at 773 then −10 z/tick, no push: his nearest corner is 192 in front of 932's box face 177 | **Verified** (the extra (−9, −21) push came from his animated box: z1 83 instead of 40) |
| walking into the enemy | 881–921 Bugs walks into 932: the move is only deflected by the face push, never zeroed (z 14154 at 929) | **Verified** (bbtime used to stop him when his centre was inside the box) |
| stomp | ✕ at 967: at 1007 his corners miss the box (x 75 > 66), at 1009 one corner touches it with the feet 50 below its top → 932 flags14 0x80 → state 209, Bugs state 91 (bounce) | **Verified** (contact only through 0x433500's corner hits) |
| kick damage | □ at 1089: Bugs' attach point (packet 10 of anim 365, his `+0x58`) inside 932's box at 1097 (0x433380: radius² `+0x30`, box grown by inflation + 4) → health 9999, state 192 | **Verified** |
| enemy defeat / carrots | 1099 two carrots 39 and 1101 the golden carrot 542 spawn; 932 is flung (state 4) and freed at 1185; the carrots' n-th bounce steps the arc n × w[2] (0x440120 `+0x194`; second hop 1159–1175 twice as fast), flags14 0x800000 once ≥ 0x400 | **Verified** |
| golden carrot pickup | 542 lands (state 1) at 1143, is collected (state 129, at Bugs' position) at 1185 when its frame-11 box (−75, 0, −97, 140, −209, 40) + inflation 40 touches a corner of Bugs' box | **Verified**; its yaw: the exe's item flag 0x1000 only re-parents the coordinate (local position zeroed, yaw untouched), the world yaw becomes Bugs' (168) on the next update (1187), bbtime snaps it at 1185 — the one field differing in the 596 fight counters 970–2159 compared |

## 11. LiT pushable block (L01A, `/b04`, type 5 at (25215, −640, 10430)) — GAMEPLAY.md §4.4

Capture `csv/litpush_L01A.csv` (`local/litenemies/enemy_trace.py OUT 22 99999 5`, abox column + `+0x40/+0x44`): Bugs poked
to (24723, −640, 10774) yaw 1422 at counter 101 (`poke_pos.py 101 24723 -640 10774 1422`), pads
`local/litpush/l01a_pads.txt` (↑ from 201, ↑+R1 from 215, release 301) replayed with `pad_play.py`. Comparison:
`PROBE=1 PROBE_IDS=99999 PROBE_TYPES=5 PROBE_PADS=… PROBE_TELEPORT=103,24723,-640,10774,1422 test_game` + `cmp.py`.
Block tags: 0x1d = 0x89 (+0x17c: X only, keep on the floor), flags8 0x4308 (platform 8 + pushable 0x4000), box
(−243, 0, −230)..(190, −479, 233), radius 337 (tag 0x1c 4th u16), inflation −50, activation 0x3a = {0x22, 6000}.

| claim | runtime | verdict |
|---|---|---|
| touch / push state | 217: Bugs' corner touches the box (flags14 0x8000 on the block), the 150-ahead / 50-up probe (0x433380 mask 0x4000) finds it → flags14 0x20000 → R1 rule state 24 | **Verified** (`testPushL01a`) |
| pushing | from 219 move 23 (flagsA 0x4000000) hands his move to the block (+0x40/+0x44 = 24,−18 … 9,−7), flags14 0x4000; the block (platform group, before Bugs) moves X only one tick later and traces him back by (moved − pushed): z stays 10568 | **Verified** (block x 25239 at 221 … 25610 at 301; Bugs, eye, rand every counter) |
| release | ↑ alone at 301 → state 1, the block stays at 25610 | **Verified** |
| activation | counter 3 / 9–107 the block is skipped (flags18 0x100, on-screen test act3c 0x20); bbtime's on-screen approximation keeps it active (only its flags14 2 at counter 3 differs) | **Contradicted** (pre-existing 0x447d40 & 0x20 approximation; no effect on positions) |

## 12. LiT HUD, fades, loading screens (wave 6; `local/tmp/w6lit/oracle{1,2}.sh`, Proton-GE, `/b04`)

| item | observation | evidence | status |
|---|---|---|---|
| HUD at level start | load pass (counter 1) shows carrots 00, golden 03 / clocks 03 (G[0xb6] = G[0xb8] = 0 → G[0] = 3), 3 health carrots at z 1200; fly-out from counter 121: z 1700, 2325, 3106, 4082, 5302, 6827, hidden at 133 | `lithud/w6b/trace.csv`, `lithud/w6/change_6.png` vs `ours_c43_{gl,vk}.png`, `side_c43_{gl,vk}.png` | **Verified** (`testHud`; pixel positions equal) |
| level change fade | request tick: mode 2, dark 0; then dark 256 … 3840 one step per tick (16 ticks, world ticking), then mode 0; after the load: counters 3 and 5 unrendered, update 1 (dark 4096), counter 7 first frame dark 3840, … mode 0 at counter 37 | `lithud/w6b/trace.csv` (poked GD+0 = 5, 0x4b39bc = 1) | **Verified** |
| A71 flash (poked 0x41 / 0x400, A69 colour 0,0,255) | dark 4096, 3072, 2048, 1024, 0 (mode 0x42), 0, 1024, 2048, 3072, off | `lithud/w6b/trace.csv`, `flash_*.png` | **Verified** |
| loading screen | L01B loads in ~50 ms here: the loading TIM (L_SARUN_0, 512x512) is visible for the load only (no shot caught it); not a held id | trace gap counter 629 → 1 | Tested (image/flow from the decompile 0x430e60 / 0x423bd0) |

## 13. LiT ropes, floating, texture filtering (wave 6; Proton-GE; `local/wine/compare/litrope/rope_trace.py`)

| item | observation | evidence | status |
|---|---|---|---|
| L02A1 chain (/b09, type 8 at (13039, −5569, 23412), flags8 4, tag 0x1d 67 = swing sound +0x160) | inactive until the eye is within 5500; first update at counter 303: phase 280 (+4/tick, wraps at 360), amp 100 → clamped 128 (+0x1e default 0x80), segment 332 = TOD box 1328 / 4 nodes, pitch = table 0x4addc8[phase + 90]·amp >> 10 (−128..127), radius² from the bottom end | `litrope/r2.csv`, `r3.csv` | **Verified** (`testRopeL02a1`) |
| grab / hang | poked into the air at the chain (601): state 14 at 603, grab at 605 (state 31, flags14 4, local (0, 700, 0), yaw 141 = chain yaw); world = Ry·Rx(pitch of the previous chain update)·local + chain pos | `r3.csv` | **Verified** |
| pump / climb / jump off / re-grab | R1 681–685: amp 131, 134, 137 then −1/tick; ↑: local −32/tick, stops at 284 (hands at the top); ↓ +31/tick; ✕ 821: state 7, released at the next tick's jump start (x/z kept, yaw = local + chain yaw); ✕ let go at 841 cuts the arc → descending → re-grab at 843 (local 532) | `r3.csv`, pads `r3_rawpads.txt` (raw pad column; one key-up landed a tick after it was sent) | **Verified** (every counter 603–999) |
| L01D1 float column (/b07, type 14 at (21040, −2000, 7280), flags8 0x101000) | idle state 2 (zero-size box, radius 0) until S[45] & 1 (poked 0x4b228d at 351): state 1 at 353, 142 at 355 (TOD box ±382..413, radius 540..584, top −2000, −1280 from 415). Floating in the idle column crashed the exe (division by radius 0; trace ends at counter 437) | `litswim/s1.csv` (crash), `s2.csv` | R |
| floating | poked to (21040, −2100, 7280) at 401: state 25, flags14 0x404 at 403, plunge 6 → 0 over 16 ticks, lift to −3154 (507), bob −3133..−3269; ✕ 601–619: state 26, jump H80/60 at 629, descending above the column at 647, re-entry at 665 (arc vy 116 + plunge 86) | `s2.csv`, `s2_rawpads.txt` | **Verified** (every counter 403–759; rand seed replayed at 403 / 497 / 579 / 665) |
| texture filtering | bugs.exe sets `glTexParameterf(GL_TEXTURE_2D, MIN/MAG_FILTER, GL_LINEAR)`, wrap REPEAT (0x4220c4, 0x413ecc, 0x4236d2), no mipmaps. bbtime's nearest Original Fidelity made 3 texels of L01A floor texture 11 stand out at depth ~800: the bottom-centre artefact of `lithud/w6/side_c43_gl.png` | disassembly | bbtime LiT Original Fidelity now samples linear in GL + VK (TB not checked, unchanged) |

## 14. LiT title front end (wave 7; Proton-GE, `/b00`; GAMEPLAY.md §8.1.1)

Shots `local/w7litmenu/o_{title,menu,menu_new,slots,start,goback,back,quit}.png`; bbtime `t_*.png`, side-by-side
`local/w7litmenu/side_loadsel.png` (vs `local/saves/lit/save2_merlin_cheats/orig_title_load_select.png`, Savedata of
save3: slot 2 = 333 / 124).

| check | original (R) | verdict |
|---|---|---|
| view | eye (1, 146, 0), look (1, 146, 1000), flags 0x342 from the first frame; player at (50, 400, 1300) state 1 | **Verified** (`game.lit_title`: camera node of the actor at (0, 146, 0)) |
| Enter on the logo page | S[3] 0 → 2, S[0x1b] 0 → 14, S[0x24] = 1 (Load game) within one Return press | **Verified** (same S bytes in `game.lit_title`) |
| → / ✕ on page 14 | S[0x24] 2 (New game); ✕ → S[0x1c] = 2, slot cursor G[0x9f] = 1, ↓ → 2; ✕ → page 16 (Start / Go back, S[0x25] = 1); Go back → page 0 | **Verified** (flow; bbtime new game → CCINTRO 56) |
| Quit game | S[0x1c] = 3 → the Windows "Do you really want to quit? Yes / No" box (`o_quit.png`) | Consistent (bbtime returns to its collection menu by design) |
| slot menu text | panel, lines "N. Empty" / "2. ° 333 ± 124", green cursor line, Load / New / Quit row | **Verified** (glyph positions coincide in `side_loadsel.png`) |
| copyright plane | text spans x 5..634 at 640×480 | **Verified** after the fix: type-0 behaviour 0x43eee0 builds one rigid part (0x448980, model object, identity coordinate under the actor root) and never plays a TOD, so the plane's anim 163 root scale (6336, 4505) is not applied; only tag 0x12 (1.2, 1.2). bbtime had looped anim 163 (1.86× / 1.32× total) |

## 15. LiT music (wave 8; Proton-GE with `AUDIO=1` into a PulseAudio null sink, `/b04`; formats/AUDIO_XA.md)

Capture `local/wine/litmusic/capture.sh`: `trace.csv` polls `DAT_004b39d4` (wanted), `DAT_004abf94` (open track),
`DAT_00553394` (stream playing), fade `0x4b3232`, speech `0x4b2277`, player zone `+0x26`, `0x4b3b76`, GD+0;
`fds.log` lists the `track/*.xa` files the process holds open every 0.5 s; level 7 poked at ~15 s (GD+0, 0x4b39bc).

| check | original (R) | verdict |
|---|---|---|
| L01A track | zone 2 → wanted 2, `TRACK2.xa` open, playing | **Verified** (`game.sfx` table + `musicTick`; app opens Track2.xa) |
| level change | fade-out mode 2 → playing 0 the next sample; L01D1 loaded (counter 1): wanted 4, `TRACK4.xa` open and playing over the loading screen until the fade in (mode 1, ~3.0 s after the load) stops it; mode 0 at counter 37, playing again at 39 | **Verified** (`loadUpdate` → `musicTick`; app plays the loaded level's music during the hold) |
| speech | L01D1 intro speech S[0x17] 2 → 4 (counter 111–359): playing 0, then restarts at 361 | **Verified** (music 0 while S[0x17]; bbtime's speech length is the audio-less path's clip time) |

## 16. TB front end idle, quit box, brightness (wave 8; system Wine `RUNNER=wine`; TIME_BUSTERS.md §8.1)

Scripts `local/wine/tbmenu/w8_capture.sh` (menu) and `w8_bright.sh` (ARCA01 `/skip_intro /b28`, E, camera from
`cam.py` = `--camera -4928,369,8434,-0.75129,-0.04901`), both under `flock /tmp/bbtime-tb-oracle.lock`; config.pc
restored afterwards.

| check | original (R) | verdict |
|---|---|---|
| attract after 30 s idle (0x41c0f0, `demo%d.avi` absent) | inotify on `Datas/data`: none for 30 s after the last click, then ~145 000 directory scans/s (the per-pass `demo<n>.avi` probes) that a mouse move does not stop and a click does; screenshots at 0 / 25 / 35 / 65 s and 10 s after the click pixel-identical (`w8_inotify_per_second.txt`, `w8_idle_*.png`) | **Verified**: bbtime keeps the menu unchanged (`local/shots/w8tbfront/title_00s.png` = `idle_after30s.png`, taken at 31 s after the "idle 30 s" log line) |
| WM_CLOSE (Alt+F4) | MessageBox "Do you want to quit?", Yes (default, focused) / No, question icon; `n` → menu unchanged; Return → exit (`w8_quit_box_root.png`, `w8_quit_no.png`) | **Verified** text / buttons / outcomes; look = Windows 9x classic by choice (Wine draws no caption without a WM): `local/shots/w8tbfront/cmp_quit_box_full.png` |
| brightness slider | `0x639c20` 1 at the title, 1.125 once the Video page is shown, 1.34375 after `+`, 0.90625 after `−` `−` | **Verified** (`TbFront`: same values in `settings.ini` `tb_brightness`) |
| brightness in a level | ARCA01 mean at +0xe0 = 0.25 / 1 / 2: (15.1 15.8 20.9) / (60.7 63.5 84.2) / (110.3 116.3 154.6), ratios 0.248 / 1.83 | **Verified**: bbtime GL (Xvfb llvmpipe, 600 frames) 0.249 / 1.822, mean abs diff 1.2 / 4.8 / 7.5 (`local/shots/w8tbfront/cmp_arca01_brightness.png`); VK at 2 the same mean ±0.5 |

## 17. LiT pause menu, shake, poke side-by-sides (wave 9; Proton-GE, `local/wine/compare/w9lit/`)

| item | observation | evidence | status |
|---|---|---|---|
| Esc pause (L01A `/b04`) | S[12] = 1, S[14] = S[16] = 1; 44 glyphs "Game Paused / Continue / Video / Controls / Sound / Quit Game" (list at `[0x4b373c]`); HUD slots 0–10, 14–16, 22–23 at z 1200 (slot table `0x4b23a0`); menu item 1 = {0, 0, cursor, 5} (`0x4b3e80`) | `o_pause{.png,_glyphs.txt,_hud.txt}` | **Verified** (`testPause`) |
| pause totals with poked G | golden 300, clocks 57 → 3-digit counters at origins 48–55, 78 %, lines G[G[0xb6]]/G[0xb7], G[G[0xb8]]/G[0xb9], G[4]/G[0xba] in slots 29–46 at z 1500 (the exe reads GAME_DATA +0x100f6.. = G[0xb6..0xba]) | `o_pause2.png`, `o_pause_hud2.txt` | **Verified** |
| Quit Game | "Do you really want to exit the level?" Yes → GD+0 = 3 (hub), S[12] = 0 | `o_quitask.png`, `o_lvl3.png` | **Verified** |
| pause veil | GL 0x40be10: screen quad grey 0.2, alpha 0.5 under HUD and text (scene mean blue 5.7 → 29) | `side_o_pause_unpause.png`, bbtime `side_pause_vk.png` MAD 1.59 | **Verified** (visual) |
| A59 shake (L01A start) | counter 87: speed 1100, amplitude 35; look-at += sin·a >> 11, cos·a >> 12; decay 3/tick once the s16 phase ≥ 0x800 | `shake_L01A.csv` | **Verified** (`testShakeL01a`) |
| rope / float side-by-sides | `poke_shot.py` pokes as the tests' PROBE_TELEPORT, bbtime `BBTIME_LIT_POKE` at the same counters | `side_rope_vk.png` (MAD 6.3), `side_float_vk.png` (6.4, bob rand not replayed) | Consistent |
| L02A2 bird 1335 (`/b10`, Bugs poked to (5166, −1410, 4330) at 201, S[61] = 3 at 211) | spawns at counter 331, hovers toward Bugs, grabs him by anim-203 event f2 4 at 339 (Bugs state 47, flags14 4, at its hand part +0x6c/+0x70), flies toward actor 99 (riding the hand of the (0,0,0) path actor, hopping 5753,−1520,3750 → 6355,−1507,4585 …), A33 drops him at 1357 (state 39, flags14 0x200) | `w9lit2/o_bird_L02A2.csv`, `cmp_bird_L02A2.txt` (bbtime one tick early, ≤ 17 / 41) | **Verified** (`testBirdL02a2`) |
| l04e cart 109 (`/b40`, Right 87–135, Left+Up 159–219) | Bugs attached; pad-steered steps turn ±0x1e a tick at speed 116; wall cuts → flags14 0x100000 → bump 226 / 227 | `w9lit2/o_cart_l04e.csv`, `cart_pads.txt`, `cmp_cart_l04e.txt`, `o_cart_l04e.png` (tick-exact to 293, ≤ 9 to 503) | **Verified** (`testCartL04e`) |

## 18. TB ARCA01 intro, music switches, pause menu (wave 9; system Wine `RUNNER=wine`, `local/wine/tbintro/`)

Scripts (each starts `/ogl /win /x640 /y480 /skip_intro`, stops with `run_original.sh stop tb`): `capture.sh OUT
[SKIP_SECS]` (`/b28`: `trace.py` of counter `0x4d2708`, eye `0x4d5124`, look `0x4d5130`, body `0x4d514c`, flags
`0x4d515c`, GD+0x9f / +0xa9 every 10 ms for 45 s; screenshots at 1–25 s, or ✕ (`e`) after SKIP s and three
screenshots), `music.sh LEVEL SECS` (`/bze:LEVEL`: wanted track `0x63b9ec`, player positions / state), `pause.sh`
(`/b28`, ✕, Esc, screenshots `pause_menu.png`, `pause_menu_down.png`).

| claim | runtime | verdict |
|---|---|---|
| GAMEPLAY_TB.md §10 zone block 10 starts the intro (GD[0xa9] = 20) | `full/cam.csv`: counter 1 GD+0xa9 20, GD+0x9f 2; from counter 3 flags 0x150 (camera node 0x100) and the eye/look of template 425's node, letterbox 4 … 16; node ends at counter 1341 (GD+0x9f −14 → 0), rest eye (−4928, −369, −8434) from 1419 | **Verified** (`game.tb` `intro`: eye/look within 1 on all 670 ticks) |
| ✕ skips the fly-over | `skip/cam.csv`: ✕ at counter 441 → node gone at 443, same spring-out as the full run | **Verified** (state ≥ 90 ticks, raw edge) |
| camera after the node: follow 0x437800 (transcribed with 0x437180 / 0x437260 / 0x439570; prev-pose restore B+0x6c..0x94, hold sets 0x2000, vertical band B+0x128 once B+0xe8 > 20 ticks without contact and the spring is off rest) | eye springs out, look y climbs 1 per tick from −325, body rises to −349 / eye −369 at counter 1419 (oracle `full/cam.csv`, `skip/cam.csv`) | **Verified** (every tail sample equal: 68 rows `local/shots/w9tb2/intro_tail_compare.csv`, 80 rows `skip_tail_compare.csv`; `game.tb` intro asserts 6 rows + rest (−4928, −369, −8434). The level load places the camera (0x439570) and the follow skips the load tick. Counter 1: eye = body (−4176, −27, −7629), look (−4107, −27, −7555) — 0x437530 places (0x437180) before it sets B+0x134 = 300, so the session's first level places at the focus' height (B+0x134 still 0); asserted in `game.tb` intro, the tail unchanged) |
| §6 later music switches | `music_TRQ02.csv`: counter 3 → 46 (first visit, zone block 10); `music_VIQ02.csv`: 22, counter 55 → 8 | **Verified** (`game.tb` `music`) |
| op 0x14 key names, text panels S75 / R 0x3b / S32, pause dim | `pause_menu.png`: "player 1 paused", the menu, "⏎ select" / "'f4' exit menu", health bars over their panels, lives 3; frame undimmed (peek `0x4d4fc4` 0xe00, `0x4d2740` 0, `0x4d2764` 0) | **Verified** by side-by-side (`local/shots/w9tb/side_pause.png`, VK; text within 2 px) |
| pause pages 0x461db0 (S[4] 2 page mode, 4 / 8 next / previous era S[5] of GD+0xab = 0xff from 0x462430), page scroll 0x4d5960 (slide 0xa9 / 40 / 10 / 24 / 12), era models 5–13 placed by 0x462fd0, counters slides 5–7 (carrots, coins, GD+0x84·100/6010 %), text op 0xf | `pages.sh` (`local/shots/w9tb2/pages_trace.csv`): Inventory → scroll 4800 → 4000 (S[4] 0xa3 → 3 at counter 971), Right → S[5] 1, scroll 4000 → 3200 (0xc3 → 3 at 1117), Aztec page models | **Verified** (`game.tb` pauseMenu: model 5 x per tick = oracle for both slides, `pages_compare.csv`; side-by-sides `side_inv.png`, `side_inv_right.png`) |
| Level hints page | `hints.sh`: S[4] 0, hint texts, Right → next hint | **Verified** (`side_hints.png`, `side_hints_right.png`: texts identical; the arrows are director block 305's texts 109 (▸ at (210, −75)) while S[0x70] < S[0x71] — R 0x2d = 0x42ff80 `S[Y] < S[X]` unsigned, was unported — and 107 (◂) from the second hint on; `game.tb` pauseMenu asserts ▸ on hint 1, ◂ ▸ on hint 2; `local/shots/w9tb3/side_hints*.png`) |
| pause text panel slide-in (S75, 0x4d59a0 via 0x4667a0) | panel 0 x −520, −460 … 92, 80 … −40, −24, 2, 10, 0; panel 6 y 240, 250, 250 … −15, −5, 0, from the tick after S[4] = 1 | **Verified** (`game.tb` pauseMenu tick for tick, `panels_compare.csv`) |
| camera reset on a far focus poke (F3 = F2 procedure with B recorded, `F3_taz_spin_cam.pkl.gz`) | the original resets its camera (flag-4 placement + velocity (−25148, 0, −27070) one tick later) after Taz is poked ~4750 away; in `poke_probe.log` the 4000 poke (body 3200 from Taz) resets too, ±1500 (2300 / 700) do not. B+0x44 (class-16 camera target 0x4d5164) stays 0 throughout F3 | **Verified** (cause: 0x437800's snap `flags & 0x80` and (body − focus)² ≥ 0x5f5e11 = 2500², which bbtime had as 10⁸; `game.tb` spin: the same reset and velocity in our replay, guard all 240 ticks, Taz through 232) |

## 19. LiT seesaw, stationary actors, thrower, bird alignment (wave 9; Proton-GE, `local/wine/compare/w9lit3/`)

`poke_trace.py` (from w9lit2) adds Bugs' parent +0x70, local coordinate +0x1a4..+0x1ac and each actor's hand point
+0x58. Oracle pokes land **after** the tick sampled at that counter: an oracle poke shown at counter C equals a
`PROBE_POKE` / test poke before tick C + 2.

| item | observation | evidence | status |
|---|---|---|---|
| L01A seesaw (type 18 at (20150, −2460, 7705), yaw 1024, box x ±960, top −251, +0x1e 60; `/b04`, Bugs poked to (20150, −2780, 7205) at 151) | lands at 167 as a child coordinate: +0x70 = seesaw, local (500, −247, 0) = inverse rotation of the offset, y = top + 4; roll +0xe0 += 60·lx / 960 (31, 62, … 363, capped once past 0x164), lx += 60·roll / 960 (501, 504, … +22 / tick at 363); world = RotMatrix(rx, yaw, roll) · local; released at 245 (local 1240) by 0x434230 = his box corners/centre with the x/z offset only (no y) in the platform frame (1005 − 40 > 960), flags14 2 → 0x200; falls on with the phase kept from landing (0xa21: 24, 29, 35, 42 …); unridden roll −= roll >> 4 | `o_seesaw_L01A.csv`, `o_seesaw_L01A.png`, bbtime `t_seesaw_L01A.txt`, `cmp_seesaw.py` (counters 153–461: 0 rows differ) | **Verified** (`testSeesawL01a`) |
| L04A1 doors 165 / 177 (type 14, flagsA 0x404001; `/b29`, Bugs poked to (1400, −320, 11380) yaw 0 at 151) | stationary steps skip 0x440120's whole motion block (MoveActor too): flags14 never 2 / 0x200 (bbtime had 0x8002 / 0x200); door 165 hit by Bugs −1 hp a tick 157–161 (flags14 0x4008000 → 0x8000) | `o_L04A1.csv`, `L04A1_pads.txt` (the oracle's raw 0x80 pulses), `cmp_L04A1.py` | **Verified** (`testThrowerL04a1`) |
| L04A1 thrower (type 14 at (1589, −1400, 11595)) and projectile 100 | with the pad replayed the rand stream is the oracle's: the thrower turns toward Bugs (yaw 2398 at 199), the rand gate (P1 32600) starts state 225 at 201, anim 233 frame 27's event spawns 100 at 255 in hop state 171, then each hop step equals the oracle's (−6, −9, −9 …; second run without pad: thrower 225 at 173 in both). The spawn point: the first move goes to 0x433500 as (x, x >> 16, y) (§23), now equal | `o_L04A1.csv`, `o_L04A1b.csv` (hand points), `t_L04A1.txt`, `w9lit7/t_L04A1.txt` | **Verified** |
| L02A2 bird 1335 (§17 row) | with the pokes aligned (203 / 213) spawner 327 matches every counter to its end at 331 (state, anim frame, box), the bird spawns at 331 and matches exactly through 1441; Bugs ≤ 31 (spikes every 33 ticks while carried). The "one tick early" of §17 was the poke timing. The spawner's growing box pushing the standing Bugs 291–331 and his landing push-out at 231: Verified in §23 (merged push, collision-list order, wall re-trace) | `w9lit2/o_bird_L02A2.csv`, `w9lit3/cmp_bird_L02A2.txt` | **Verified** (`testBirdL02a2`) |
| Sweep | type 12/13/24/26/28/29 platforms exist in no LiT level (pendulum rider untestable); trigger 0x2000 aimed at Bugs only in LB02 (actor 1, state 1); type 31 now ported (0x446d10); left: actor type 4 (sprite effects, 38 levels), A56 (L03C1/C2), director flag 0x400000 (LB02), spawn template 214 (CC4D) | `sweep_before.txt`, `sweep_after.txt` | — |

## 20. LiT sprite effects and the animation rate (wave 9; Proton-GE, `local/wine/compare/w9lit4/`)

| capture | finding | evidence | status |
|---|---|---|---|
| L02C4 torches (type 4, flagsC 0x8000000, scale 3, `/b20`, Bugs poked to (2200, −1700, 4200) at 61, shot at 201) | flames at the wall torches: same position, size (2w × 2h × scale, centred), colours (whole TIM, white) in bbtime VK and GL; only the flipbook frame differs | `o_L02C4.png`, `side_L02C4_{vk,gl}.png`, `crop_L02C4_torches.png` (mean abs diff 2.5 / 2.7) | **Verified** |
| L02A2 lantern flames (type 4, flagsC 0, bottom-anchored, `/b10`, Bugs poked to (27600, −700, 2700) yaw 1024 at 61) | flame placement / size match; the lantern disc behind them is darker in the original (an alpha-blended glow face, not a type-4 sprite: bbtime draws it brighter, open) | `o_L02A2.png`, `side_L02A2_{vk,gl}.png`, `crop_L02A2_lantern.png` | **Verified** (sprites) |
| L03C1 intro actor (type 14 at (2000, −30000, 2000), step 488 anim 444 flagsB 0x2000000; `/b26`, traced with `poke_trace.py`, columns `rate` = 0x4b3b8c / 0x4b3c44) | head: acc clamped [0, 0x999], skip +0x148 = min(acc·3 >> 11 + 2, 5), acc −= 8; tail: acc += Δ TOD y of the list's second part (`**(+0x78)`) unless frame 1; 0x4393e0 skip: `skip` frames at once while frame + skip < last, else frame = last − 1 with nothing applied. bbtime equals the oracle every counter 3–307 (acc, y, frame 1 … 356; skip 2 → 3 at 253 → 5 at 303) | `o_L03C1.csv`, `t_L03C1_rate.txt` | **Verified** (`testRateL03c1`) |
| L03C1 after 307 | bbtime's carrier 268 hurt the carried Bugs by body contact every 60 ticks; the original never touches him: 0x433500 skips a candidate that is the caller's parent (+0x70) / carried (+0x6c), whose parent / carried is the caller, or whose parent chain holds it (fixed in wave 9, §21) | `o_L03C1.csv` (hp 6) | **Verified** (§21) |
| Sweep | type 4, A56 and director flag 0x400000 now interpreted; left: spawn template 214 (CC4D) | `sweep_after.txt` | — |

## 21. LiT carried contact, camera focus, cart lean, seesaw tilt (wave 9; Proton-GE, `local/wine/compare/w9lit5/`)

`poke_trace.py` here adds each traced actor's root part rot x,y,z / part flags (`r` token: `*(+0x78)` node → +0x10
coordinate → +0x44 param +0x10). `cmp_cam.py` compares Bugs / eye per counter with `test_game` PROBE rows.

| item | observation | evidence | status |
|---|---|---|---|
| L03C1 carrier 268 (`/b26`) | with the 0x433500 link skip (Bugs' contacts and type-14 body contact) Bugs keeps health 6 and state 1 carried through counter 669 (position ≤ 32, the carrier's one-tick lag) | `w9lit4/o_L03C1.csv`; `testRateL03c1` | **Verified** |
| L03ACOM focus actor 398 (type 14, flagsC 0x10; `/b22`, Bugs poked to (10940, −1824, 3200) at 81) | 0x440120 tail sets 0x4b3950 (flagsC 0x10 or step flagsB 0x40 always; flagsC 4 while the 0x407970 xz length < 2000); 0x4318f0 then skips the L2/R2 branch: yaw += (angle(focus → target) − yaw) >> 4, flags 2, distance max(0x4b2154, 1500), focus cleared (also by the A41 hold and camera nodes); a focus whose step has flagsB 0x200 becomes the followed target | `o_L03ACOM{,_poke}.csv`, `cmp_L03ACOM{,_poke}.txt` (eye equal every counter from 85; without the focus ≤ 1543 off: `cmp_L03ACOM_poke_nofocus.txt`), `o_L03ACOM_poke.png` | **Verified** (`testFocusL03acom`) |
| l04e cart 109 lean (`/b40`, oracle pads `lean_pads.txt`) | pad steering with flagsA 0x100000 sets the root part's flag 0x400 (0x44e400 then leaves its rot / scale / trans alone) and rot y −= (rot y − clamp(turn wanted, ±0x200)) >> 5 (attached: the stick power); no stick: rot y −= rot y >> 3 (whole when 0..7) | `o_lean_l04e.csv`, `cmp_lean_l04e.txt` (exact to 115, ≤ 5 to 127: our camera yaw) | **Verified** (`testCartL04e`) |
| L01A seesaw rider drawing (`/b04`, Bugs poked to (20150, −2780, 8205) at 151, shot 215) | Bugs is drawn under the seesaw's RotMatrix (child coordinate) | `o_seesaw_tilt2.png`, `t_seesaw_tilt_vk.png`, `side_seesaw_tilt_vk.png` (MAD 30: debug overlay, one tick apart) | Consistent |
| LB02 (`/b53`) | ski cutscene: Bugs (entity 1) stays at (0, 0, 0), state 2 → 166 at counter 209 in both; the only Bugs-aimed trigger 0x2000 (zone 50 at (4904, 27140, 4585), state 1) is not reached; 0x437510 on Bugs points +0x184 at move-list head + first move id (misaligned, not reproduced) | `o_LB02.csv`, `o_LB02.png`, `trigger2000_bugs.txt` (all levels) | Implemented |

## 22. LiT actor contact, actor riders, carried flags (wave 9; Proton-GE, `local/wine/compare/w9lit6/`)

`poke_trace.py` here adds actor pokes `A<eid>|<hex>` (flags14 |=), `Y<eid>=<yaw>`, `P<eid>=x,y,z`; `test_game` PROBE_POKE
takes the same tokens (at counter C + 2). `cmp_actor.py` diffs position / yaw / flags14 / state per counter.

| item | observation | evidence | status |
|---|---|---|---|
| L04B3 actor 98 (`/b34`, type 14, flags8 0x1040180, state 236 hop with absent target 99) | falls 0x200 from counter 15, at 25 0x440120's 0x433500 contact with pillar 57 (flags8 0x188, box top −200) boards it: parent 57, local = (x/z offset, top + 4), y −1276, flags14 0x14; 57 gets flags14 0x40000000 every tick; 269 stays attached to 98 | `o_L04B3.csv`, `t_L04B3.txt` (98 / 57 / 269 equal every counter 3–341) | **Verified** (`test_game` L04B3) |
| 98 turning on the pillar (state 225, flagsA 0x1000) | the turn writes the coordinate's local rot y; the world yaw +0xde comes from the matrix rebuilt after it is read: 1536 at 303, 1670 at 305; child 269 follows 98's matrix (1670 at 305) | same | **Verified** |
| ball 100 (spawned by 98's anim 233 frame 25 at its hand, state 2) | first tick inside 98's box: both push it out (98 flags14 0x40000000); the landing point is resolved in §23 | `o_L04B3_long.csv`, `o_L04B3_ball.csv` | **Verified** (§23) |
| ball thrown back (pokes 601: P100 = (5244, −1104, 1812), Y100 = 3738, A100 \| 0x1000 → state 200 hop at 98, flagsB 0x300000) | hop, bounce (171), rest and boarding pillar 57, 98's later pillar jumps; the ball's flags14 0x1000 clears at the anim loop wrap (611); the hop lands short of 98 (attach: §23) | `o_L04B3_attach.csv`, `t_L04B3_attach.txt` | **Verified** to 1079 (§23) |
| L03C1 carried Bugs flags14 | 0 while carried (counter 5 on): 0x43a880's tail clears 0x200 when +0x70 is set | `w9lit4/o_L03C1.csv` | **Verified** |
| CC4D spawn template 214 | anim event (actor 213, anim 163 f2 2) names template 214, absent from CC4D; 0x448d40 returns 0 and every 0x4393e0 spawn site checks it: the original spawns nothing either (bbtime: same) | `bugs_all.c` 0x448d40 | Verified (static) |
| A44 (player +0x28) | +0x28 feeds only the shadow draw's sort bias (0x44bb50 → 0x411b10 float 0x621600 ×4, added to the max z of each translucent triangle at 0x415aa7): draw order of Bugs' shadow among blended faces, no gameplay | disassembly | Not ported (no visible effect in bbtime's shadow pass) |


## 23. LiT spawn push, hand points, Bugs' merged push, attach on contact (wave 9; Proton-GE, `local/wine/compare/w9lit7/`)

`hook/h.s` (+ `hook_trace.py OUT SECONDS [EID]`) wraps 0x433500 in the original (jmp at its entry, cave 0x45b260, ring
0x9ccc80) and logs, for the actors of entity id EID, the caller, mask, move in → out, the mover's position / rot /
box and the returned contact; `push_sim.py` replays bbtime's boxPush on logged inputs; `cmp_norm.py` / `cmp_bugs.py`
compare per counter (yaw mod 4096).

| item | observation | evidence | status |
|---|---|---|---|
| 0x440120's move to 0x433500 (all type-14 actors) | the int move vector (+0x20) is copied dword-wise into the s16 vector +8: 0x433500 receives (x, x >> 16, y) — its "z" is the y move, so only the x move is pushed against; the pushed x / "z" become the move's x / z, "y" = the contact's box top − y. L04B3 ball's spawn tick: (32, 0, 0) → (−62, −31, −61) | `h_L04B3.txt`, `push_sim.py` (exact) | **Verified**: L04B3 ball 100 equal every counter 3–661 (`o_L04B3.csv`, `t_L04B3.txt`), L04A1 projectile 100 at 255 (1424, −1491, 11606) and its hops (`t_L04A1.txt`, max \|d\| 0 to 281) |
| hand point +0x58 / child at the hand | 0x44e400 packet 10: the part origin walked up the coordinate chain in integers (RotMatrix + ScaleMatrix per level, ApplyMatrix on the s16 vector, + translation); bbtime's float `partOrigin` was 1 unit off (L04B3 98 hand z 5718 vs 5717) — now `partOriginQ12` | `o_L04B3.csv` (h2692,−1721,5717 at 351) | **Verified** (also keeps L02A2's bird / Bugs carry exact) |
| attached actor's step (98 on pillar 57, counter 749) | the turn writes the coordinate's local rot y, the move is rotated by the world rot +0xdc of the previous tick: (8, −17) with 1738, not 1708 | `w9lit6/o_L04B3_attach.csv` | **Verified** (98 / 100 equal to 1079) |
| flags14 0x1000 / 0x80000 | 0x4393e0 clears them when a clip reaches its last frame (loop wrap or once end) | same, counter 611 | **Verified** |
| Bugs' 0x433500 (mask 0x9a690a, L02A2, `/b10`, pokes 201 Bugs → (5166, −1410, 4330), 211 S3d = 3) | one merged push over every candidate (non-solid pushes averaged) in the collision list 0x4b39ac, which 0x447d40 rebuilds each tick: type 21, solids, platforms, Bugs, the rest, each in reverse live-list order, spawns appended; the pushed x and z are then each traced against the walls (0x434cd0). Counters 227 / 229: planks push (−64, −53) / (−55, −46) but the wall stops it; 231 applied; 291–331 spawner 327's growing box shoves the standing Bugs | `h_bird_L02A2.txt`, `o_bird_L02A2.csv`, `t_bird_L02A2.txt` (Bugs equal every counter 203–341 but 343, the known carry lag) | **Verified** (`testBirdL02a2`, placement workaround removed) |
| attach on contact (L04B3, pokes 601: P100 = (3145, −1426, 5787), Y100 = 3072, A100 \| 0x1000) | the ball's hop touches 98 at 607 in both (contact push (−60, −1, 0) → (−33, −205, 18), identical), attaches to its hand, state 1; the post-attach position is resolved in §24 | `o_L04B3_attach.csv`, `h_L04B3_attach.txt`, `t_L04B3_attach2.txt`, `o_L04B3_attach.png` | **Verified** (§24) |
| platform step-off wall slide | see §24 (no reachable case found) | static sweep (bbtime heightfield) | no oracle capture |


## 24. LiT attached hand turn, attached MoveActor, flipbook explosion (wave 9; Proton-GE, `local/wine/compare/w9lit8/`)

`o_back.csv` = the §22 thrown-back run (`/b34`, pokes 601 P100 = (5244, −1104, 1812), Y100 = 3738, A100 \| 0x1000) to
counter 2325 with entity ids 8, 53, 57, 98, 100, 269 traced (`poke_trace.py`); `run.sh OUT POKES [TICKS] [IDS]` is the
matching `test_game` PROBE run (pokes at counter 603); `cmp_norm.py` / `cmp_bugs.py` from §23. The probe's actor token
now ends with `:p<eid>` (the +0x70 parent).

| item | observation | evidence | status |
|---|---|---|---|
| hand point of an attached actor (0x44e400) | walked up the coordinate chain with each level's *current* local rot: L04B3 98 on pillar 57 turns during 605 (local yaw 1670 → 1347) and its hand is already (2848, −1381, 5756) at 605, while the world yaw +0xde still reads 1670 until 607 | `w9lit7/o_L04B3_attach.csv` | **Verified** |
| attached move into the local frame (0x440120 tail) | RotMatrix(0, −parent +0xde, 0) · move (ApplyMatrixLV), i.e. the parent's stored world yaw, not its hand heading: the ball attached to 98's hand at 607 (it updates before 98) is offset by R(1347 − 1670) of its move | same, 607: (2795, −1352, 5727) | **Verified** |
| MoveActor on attached actors | 0x437390 also runs when +0xc4 is set (condition `flagsA ≥ 0 \|\| flagsB 4 \|\| +0xc4`), before the rider update; its y (ground − y for a flags14-0 child) enters the local translation: the ball takes y +29 at 607 and keeps it | same, 607–1505 | **Verified**: 57 / 98 / 100 equal every counter 3–1505 |
| 98's turn after the attach | follows from the two rows above: 1676 after 609 (was 1836), 1503 at 1193 | same | **Verified** |
| thrown-back run after 1079 | 98 leaves state 187 at 1081 (y −1234, flags14 0, still parented) and hops at 1095; it rides `0x317630c` = pillar 53 at (4340, −1080, 4300) from 1067 and again from 1119 (98's position lies in 53's box only; bbtime's parent there is 53) | `o_back.csv` | **Verified**: 8 / 53 / 57 / 98 / 269 equal every counter 3–2325 |
| flipbook actors (flags8 0x200000) | set at load by 0x42e7a0's tag 0x16 when the first listed asset has kind 0x180 (tag 0x40 sprite asset: kind 0x100); 0x4393e0 then runs 0x438970 | L04B3 entity 8 (first asset 5 = 64×64 sprite) | **Verified** |
| 0x438970 clip start (flags8 0x200000) | box ±(w/2)·scale x >> 12, top −h·scale y >> 12 (sprite record 0), flagsC 0x4000000: y += h/2, inflate += 2w·scale x >> 12, radius = that + the half width, +0x30 = radius² | entity 8 at 1013: box ±80, top −128, inflate 50 → 370, radius 400, y −1271 → −1239 | **Verified** |
| anim event spawn f2 2 | unless f3 0x20 the spawn takes the hand part's scale (+0xcc = `*(+0x68)->+0x44`): ball 100's death anim 201 spawns explosion 8 at (10240, 8192) | 0x4393e0, `o_back.csv` | **Verified** |
| explosion 8 vs pillar 57 (the 0x40000000 flag 1015–1045) | 8 (flagsA 0x20000 hurt, 0x800000 body contact, anim 201 = 8 frames × 2 ticks, flagsA 4 dies at the end) lives 1013–1043; its 370 inflate keeps touching 57 (contact flag every tick) and hurts it each time 57 is back in a vulnerable step: hp 10 → 8 (1015) → 6 (1031) → 4 (1047) | `o_back.csv` | **Verified** |
| single-axis TraceGround 0x434cd0 (x-only / z-only branches) | after reading a sub-cell at its entry point the walk steps on while the move's end lies strictly beyond that entry (`dz < entry − z0`), not while the next boundary is within the end: it always reads the sub-cell past the end one (moving −z, entries are boundary − 1), and that sub-cell's kind / height / zone is the result (a blocked one clamps the move to the previous entry). L04B3 Bugs knocked back −10 a tick (state 10, x 5750): from z 1168 the walk reads 1159 (floor) then 1119 (open, [1080, 1120)) → groundBase 1, flags14 0x200 at 1158 (bbtime had 1118); +0x100 / +0x10c read live (`o_att_g.csv`: 0/−1440 at 1039, 1/0 at 1041) | `o_att_g.csv` (`poke_trace.py` + column `bground`), `o_back.csv` | **Verified**: Bugs equal every counter 3–2325 (thrown-back) and 3–1505 (attach run); diagonal traces unchanged (not re-derived) |
| trigger entry flag 0x10000 (0x4384a0) | leaves the entry loop into the function's tail: the actor in the volume gets flags14 0x100 (freed). L04B3's floor volume (0, 0, 0) 9000 × 9000, y 0..−400 (entries for actor 100: A19 / A18 with 0x8000, then 0x18000) frees the second thrown ball when it falls to y −391 at 2067 | `o_back.csv` | **Verified** |
| L02A2 / L02A5 "spawn template 508" | Bugs' own animation events name template 508 at the hand (f2 2: anim 17 = move 8 frames 0 / 1, used by states 7 / 9 / 91 / 98 / 101; also anims 77 / 97 / 99 / 350 in other levels); 69 of 167 BZEs define 508, L02A2 and L02A5 do not. 0x448d40 walks the level's template list and returns 0 when the id is absent, and the event spawn site (0x4393e0 f2 2) checks the result, so the original spawns nothing there (as CC4D 214, §22); bbtime's spawn() returns −1 the same way and now reports "spawn template N absent (none, as the exe)". `test_game`'s sweep runs 900 ticks with the app smoke's input (was 240) and lists it for L02A2 (`sweep_900.txt`) | `bugs.exe.c` 0x448d40 / 0x4393e0 | Verified (static) |
| platform step-off wall slide (0x434cd0 from the platform centre) | swept every LiT level for exit points 60 past a platform edge that are cut from the platform centre but reachable by a walk from 60 inside: only the hinge ends of L03A / L03A2 type-14 platforms (f8 0x388, box z 0..1420 from the hinge; Bugs poked above them falls through in bbtime, presumably drawbridges not yet lowered) and the wall-side pivots of L02A2's type-18 planks (Bugs' own walk stops at local x −48 before his box leaves); L01b / L01d2's block and L04A1's (2640, −330, 12160) actor give no cut at the rider's height (on L04A1's Bugs does not walk) | `test_game` sweeps (removed) | no reachable case; Implemented |
