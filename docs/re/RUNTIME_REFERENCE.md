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
| water zones + swimming (0x44f550, 0x453de0 swim branch; wave 12, Granny `/bze:granny /skip_intro`, RUNNER=wine, `local/wine/compare/w12/tb/water.sh`) | Bugs poked to (22000, −1600, 7700) above the pond zone (origin (19028, −320, 3455), 6000 × 8500, height −820): falls (23 → 52), at depth 90 +0x24 0x60 and +0x20 0x220; next tick state 222 (swim idle); plunge 0x1000000 vy 1600 → 864 → 448 → 176 → −48 (rise 0x800000), rise to depth 17 (y −1123), +0x24 0x40 / 0x60 (depth < 200); ↑ → 216 / 217 / 218, ↑← turns, ✕ → 232 jump out (+0x24 0) and re-entry. State id at record +6 (+8 = anim). Pre-poke camera eye (14937, −2758, 6988) look (16110, −2420, 7046) | `water.csv` (counter, state, anim, pos, vel, angles, flags, depth, pad, eye/look per tick; counters 1871 and 2111 sampled before the entity pass) | **Verified** (`game.tb` `water`, 264 ticks equal) |
| digital stick GD+0x14/0x15 | Right / Up 127, Left / Down −128 (also diagonals) | peek while `send_keys` held each arrow (w12) | **Verified** |
| turn step +0x18c (0x453de0 LAB_00455b16) | yaw 920 → 912, 906, 901, 897, 894 … under ↑ in the water; the steering trace E | `water.csv`, `tbplay/E_switch_turn_L2.csv` | **Verified** (a remainder whose quarter is 0 is applied whole, +0x18c = −1) |
| first tick after a poke into open air | grounded Bugs (+0x20 0x102) poked 500 up: vy 160 at once, +0x20 0 (0x100 at-rest bit cleared by the mover) | `water.csv` counter 1645 | **Verified** (0x443f40 only runs with collision candidates) |
| class-9 sky dome (0x465710) | drawn at the view eye (DAT_004d5124..) with matrix ty += +0x76 (tag 0x39 @2: VIQ02 −1750, VICA01 −5200, ARCA01 −7500, TRCA01 −10000, VIQ01 0); tag 0x10 position overwritten every draw, tag 0x11 rotation kept |  decompile; wave 13 peeks (RUNNER=wine, `local/w13/sky/domepeek.py`, `peek_*.txt`: list 0x4d4fd0, +0x31 9, +0xa4, +0xe4 → +0xec scale / rot / trans, draw items +0x9c with coordinates [2] → parent [0x12]) | **Verified** (eye + lift: ARCA01 root T y = eye − 7497) |
| dome placement: first draw item's coordinate (0x465710 → 0x405820(item[2]), then the 0x2000000 walk to the LOD item whose model 0x406840 draws) | VICASG04 / VIR01: item 1 (+6 8, no model) local T (0, 6000, 0), world = eye + (0, 6000, 0); item 2 (+6 4, the dome model) local (0, −6000, 0) under item 1, its own coordinate never composed. The two parts come from the class-1 skeleton (frame 0: 0x800 / 0xc00 create parts 1 and 2) and the class-0xb animation 0x44d6f0 plays at init (0x469770 class 0xb: packet 0xd00 flag 1 packed trans (0, ±6000, 0)); ARCA01 / VIQ02 items at (0, 0, 0) | `peek_{vicasg04,vir01,arca01,viq02}.txt`; side-by-sides at the oracle camera `local/w13/sky/side_sky_{vicasg04,vir01,arca01,viq02}.png` | **Verified** (tb_level.cpp: dome offset = first part after skeleton frame 0 + class-0xb frame 0; VICASG04's peach peaks drop to the horizon, MAD 23.8 → 20.7, rest = actors / dialog state) |
| dome view flags / far clip / background layer (0x465710, 0x45eee0, 0x422eb0) | tag 0x39 @0 → +0x6e: 0x20 = never listed by the view list (VIR01 dome 329, lift 5000, +0x6e 0x60: +0xa4 never written, all zero); tag 0x16 @4 → +0xc 0x2000000 = 64000 far clip (every dome peeked has it); layer DAT_004d2730 − 1 = 0 drawn first, then glClear(DEPTH) | peeks above; decompile | **Verified** (VIR01) / Implemented (`Mesh::background`, `Mesh::farClip`) |

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
texel, and the mode-2 CLUT rgb is then zeroed (0x420d10 after the gamma tables, renderer 2) — black texels blended
SRC_ALPHA / ONE_MINUS_SRC_ALPHA (`0x4ac028/0x4ac02c` = 0x302/0x303 at runtime defaults): a darkening blob by luma,
not a PS1 subtractive one; Original Fidelity uses this in both backends (Enhanced keeps the PS1 subtract; until wave
10b bbtime kept the grey texel rgb, §27). Side-by-side `compare/side_lit_L01A_shadow_{gl,vk}.png` (original `orig_lit_L01A_shadow.png`
after the walk-in, bbtime `--autoplay '-*90'` tick 90): MAD 1.58 / 1.58, gain 0.99/0.99/0.98; the soft blob under
Bugs' feet matches in place and size. Wave 13: the first 0x44b4f0 pass (projection onto the top plane of a non-ridden
class-2 platform below; L01A seesaw drop, `local/w13/lit/shadow/o_seesaw_drop.txt`: the 0x4efac0 records (20180, −2710,
7070) 2048, (21050, −2229, 7910) 2612 rot (0, 2104, 0), (20990, −1600, 9005) 3616 equal bbtime's, `testSeesawL01a`) and
0x44cb90 (the quad drawn with its owner's far value) are ported. Not ported: ordering-table depth (bbtime pulls the quad
24 u toward the eye).

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
| A44 (player +0x28) | +0x28 feeds only the shadow draw's sort bias (0x44bb50 → 0x411b10 float 0x621600 ×4, added to the max z of each translucent triangle at 0x415aa7): draw order of Bugs' shadow among blended faces, no gameplay | disassembly | Stored (`Player::shadowBias` → `LitWorld::Shadow::sortBias`). Shadows go to blended layer 2 (0x44bde9), which 0x4124b0 draws unsorted (§29), so the key changes no draw order (Inferred from §29; bbtime: same) |


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

## 25. LiT zone / portal visibility (wave 10; Proton-GE `/b03`, `local/wine/compare/w10/zones/`)

| What | Runtime (peek at ~20 s, LS01 standing at the time machine) | Status |
|------|-------------------------------------------------------------|--------|
| visible-zone table 0x4b3460 / count 0x4b39dc | count 2: `6 6` (entry 0 = zone 6, the camera zone appended again at the end), all flags 0 | **Verified** (bbtime overlay "zone table 6 6") |
| camera zone `[0x4b39b4]+0x26`, player zone `[0x4b324c]+0x26`, eye 0x4b38c0, flags 0x4b3940 | 6, 6, (28814, −1200, 1155), 0x340 (camera node: the player-zone pass is skipped) | **Verified** (same eye in bbtime at tick 200) |
| drawn world | only zone 6 (Nowhere); zones 1–5 (the eras' sets, no portals in LS01) and their `+0x3c & 1` actors not drawn | **Verified** visually: `ls01_side.png` (orig / bbtime Original Fidelity / diff) |
| L01A portal walk (`/b04`, Down / Left keys; `orig_l01a_{start,down1,down2,down3}.png`, `l01a_tables.txt`) | start eye (23613, −1000, 11550): `2`; Bugs running toward the camera, eye (23822, −1140, 9936) look (22455, −843, 9822): `2 3 4*` (zone 3 through the 2→3 portals, its 3→4 portals flagged: depth limit 1); two later views `2` | **Verified**: `render.test_zones` re-walks the four oracle cameras at 51.28° 4:3 and asserts the same tables (at 90° the third view also reveals zone 3) |

## 26. LiT hand-carried Bugs: LS01 time machine and CCMERLIN walk (wave 10; Proton-GE, `local/wine/compare/w10/ls01/`)

New Game → CCINTRO → LS01: Bugs flies in on carrier 620 (type 14, Bugs at its part 0) whose coordinate hangs on actor
89 (model 41, two parts). Selecting the era (✕ on "Nowhere") loads CCMERLIN, the walk to the castle (carrier 5's
part animated across the level). Scripts `tm_list.py`, `tm_coord.py`, `tm_hand.py` (throwaway pollers),
traces `b03.csv` (`/b03`), `new.csv` (New Game), `cycle.csv` (Right/Left), `walk.csv` (LS01 → CCMERLIN).

| claim | runtime | evidence | verdict |
|---|---|---|---|
| child coordinate super +0xc4 = the parent's +0x68 at the attach (0x440120 / 0x4393e0 f2 1), not re-read | 620's super = 89's part0 (id 1) coordinate throughout, although 89's anims 154 / 1 carry packet 10 on part id 2 (anim 1 jitters part 2 ±200); Bugs seated at (28800, −441…−491, 3800) yaw −1949 while cycling eras (state 501) | `tm_coord.txt`, `cycle.csv`; bbtime `t_ls01.csv` vs `b03.csv`: 522/522 samples from counter 245 equal (`cmp_ls01_ccm.txt`); before: Bugs ±200 jitter (part 1 of 89) | **Verified** (`testCarriedLs01`) |
| carried Bugs' heading +0xde = the hand coordinate's (atan2 of its z axis) | CCMERLIN −660 every counter (carrier 5's yaw 0) | `walk.csv` vs `t_ccm.csv`: yaw 335/335 equal | **Verified** (`testCarriedLs01`) |
| carried position vs the hand part (wave 11, `local/wine/compare/w11/carry/`) | the attach item (0x20000000 P2 = Bugs, 0x440120 → 0x443c20) recomputes Bugs' +0xe4 from the coordinate chain at once. During the LS01 fly-in (counters 3–239) carrier 620's step re-runs that item every tick, after 89 moved part 0, so Bugs = the part of the same counter; seated (no re-attach) only his own update (0x43a880 store 0x43de9b, list head) writes it: one tick behind 620 (`tm_hand.txt`). CCMERLIN is no exception: one tick behind carrier 5's hand (`carrier_b70.txt`); the w10 "same counter" there was `walk.csv`'s LS01 → CCMERLIN boot, whose counters run one tick ahead of the oracle's own `/b70` boot. The draw walks the chain (0x40a1f0 → GsGetLws 0x40a050, frame-stamped flg), so a carried Bugs is drawn at the part of this tick in every case (code reading) | z-store hooks on all 62 `mov [reg+0xec], reg` sites counting stores into Bugs (`hook.py`; `hook_b03.txt`: 0x443c40 + 0x43de9b every tick 3–239, only 0x43de9b from 241; `hook_b70.txt`: 0x443c40 once at the attach); `timing2*_b03.txt` write order; bbtime (re-attach recomputes `p.pos`, app draws `LitWorld::playerDraw`) vs oracle, last sample per counter (`cmp_ls01_ccm.txt`): LS01 `b03.csv` 120/120 fly-in + 522/522 seat counters equal, CCMERLIN `/b70` `b70.csv` 348/348 equal; captioned fly-in side-by-sides `side_flyin_{171,181}.png` (oracle frozen by NOPing the 0x447d40 call at 0x406617, bbtime Original Fidelity at the same Bugs position and the oracle camera: machine and seated Bugs coincide, MAD 7.3 / 7.7) | **Verified** (`testCarriedLs01`: fly-in, seat incl. draw pose, walk) |
| CCMERLIN environment, letterbox, moat | full set drawn in both. Bars: 0x44c4b0 draws two black 0x40c5e0 quads while G[0x36] (cut-scene lock) != 0, 480·s/240 tall reaching 5 past the edges (extents −1), then G[0x36] += 2 per pass while 0 < s < 30 or s < 0; oracle `/b70`: s = 31 from counter 43, bars 57 / 56 rows (`0x4efb4c` 640 480, `0x4efb5a` 240). Moat: black with the original's spikes once §29 is ported | w12: `w12/lit/side_ccm_o{15_b40,18_b120,21_b180,24_b300}.png` (MAD 3.2 / 1.46 / 1.37 / 1.27; 0.11.0 `side_ccm_o18_b120_before.png` 14.5, `side_ccm_o24_b300_before.png` 11.2). The oracle's `/b70` counter runs ≈ 2× bbtime's CCMERLIN tick (pairs picked by MAD) | **Verified** (bars, moat) |

## 27. LiT PC semi-transparency and sprite texture modes (wave 10b; Proton-GE `/b03`, `local/wine/compare/w10b/light/`)

Static (bugs.exe GL renderer 2): at level load 0x423eb0 walks every chunk 0x20 / 0x22 model (tag 0x24) and chunk 0x22
sprite asset (tag 0x40) in script order and ORs each texture id's face modes into `0x52fd84 + id·0x30` (ABR k of a
semi-transparent face / 0x64 sprite record: 1 << k; opaque: 0x10; types 0x4a / 0x4e assign 0x10; an unknown type
ends that asset). The upload loop gives the id's own GL texture the first of 0x10 (mode 4), 1, 2, 4, 8 (modes 0–3) and
every further mode a copy at slots 0x7ca, 0x7c9, … (`+0x14 + mode·4` = the variant used by faces of that ABR). 0x420d10
converts the CLUT per mode: 0 / 1 / 3: alpha = m/2, m − m/4, m/4 of m = max(r, g, b) (5-bit << 3), rgb += 255 − m
(grey becomes white); 2: alpha = luma, rgb then zeroed; 4: alpha 255 (CLUT 0 → alpha 0 in every mode); then the gamma
tables. One `glBlendFunc(0x302, 0x303)` call exists (GL init 0x40daf0): every blended list is alpha-blended, never
additive. Sprites 0x40a6c0 bind the id's own texture (no variant lookup) with vertex alpha `0x467730[abr]`.

| claim | runtime | evidence | verdict |
|---|---|---|---|
| blend factors, sprite vertex alpha | `0x4ac028/02c` = 0x302 / 0x303, `0x4ac030` 0, `0x4ac034` 1, `0x4ac094` 2; `0x467730[0..3]` = 1 1 1 1 | `o_texmodes.txt` (`texmodes.py`, LS01 at the seat ~30 s) | **Verified** |
| texture usage / base mode per id | bbtime's `pcTextureUse` (level.cpp) = `0x52fd84` for 618 of 644 LS01 ids; the 26 others are opaque-first in both (extra 0x02 on 470–525 from an unported source) or id 1 (0x04 vs 0x14, no sprite uses it); all 253 LS01 sprite frames have the oracle's base mode (321 → mode 1, 19 / 22 / 462 → 1, 25–28 / 352 → 0, the rest opaque). In LS01 every sprite record's ABR equals its texture's mode, so the sprite rule changes nothing there; over all 167 files it changes only L02A5 asset 80 (ABR-0 records, texture used opaque first → opaque) | `o_texmodes.txt` vs `t_use.txt` (`/tmp`-built dump of `pcTextureUse`) | **Verified** (LS01; `test_levels`: LS01 321 / 25, L02A5 80 Tested) |
| LS01 seat (bbtime `--level LS01 --autoplay '-*600'`, oracle `w10/ls01/o_b03_27.png`) | before: VK MAD 6.50, means 21.3 / 21.2 / 20.3 vs 17.6 / 17.2 / 16.1, gain 0.744 / 0.723 / 0.697 (vortex core 66 / 70 / 85 vs 37 / 39 / 48); after: MAD 3.78, means 17.9 / 17.5 / 16.5, gain 0.920 / 0.916 / 0.912 (vortex core 37 / 39 / 47); GL 7.00 → 4.03. Remaining gain < 1 = the vortex spikes' rotation phase and the floor pattern (animated), not brightness (signed mean diff +0.36) | `side_ls01_seat_{before,after}.png`, `side_ls01_seat_after_gl.png` | **Verified** |
| other captures (old = wave 10 build vs new, Original Fidelity VK) | TITLE light beams (orig 32.5 / 36.9 red): old 35.2 / 41.0, new 32.9 / 37.2 (`side_title_after.png`); L02C4 torches MAD 2.28 → 1.23; L01A play / shadow, ARCA01, pause veil: unchanged (old vs new MAD 0.00 / 0.00 / 0.00 / 0.04). The ABR 0/1/3 and sprite modes are LiT-only; the ABR-2 black texels apply to TB's Original Fidelity too (shared engine path, bblit2.exe not checked; ARCA01 has no visible ABR-2 face) | `regress.sh`, `regress/` | **Verified** (LiT) |
| dark bars under the LS01 time machine (original only) | glDepthMask is never called and alpha test GREATER 0 is on for the blended pass: translucent polygons write depth; the light sprites' quads (aid 321, alpha > 0 in their dark texels) hide the floor polygons sorted after them, showing the starfield | ported in §29 (wave 12) | **Verified** (§29) |

## 28. LiT children drawn under the parent part's matrix: LS01 clock gates, time-machine antenna (wave 10b; Proton-GE `/b03`, `local/wine/compare/w10b/parts/`)

Five type-14 actors at (29597, −225, 2925) (model 29) show an era's clock requirement right of "Nowhere": state 2
spawns digits 969–974 at parts 2 / 3 and clock 1317 at part 4 (items 0x81c0, p1 = part), state 1 holds anim 270,
Right / Left (cond 38 era == a) plays 199 (in), 1 (shown), 198 (out). `gate_peek.py` walks the live list and each
child's coordinate super chain (+0xc4 → GsCOORDINATE2 coord +4, work +0x24, super +0x48).

| claim | runtime | evidence | verdict |
|---|---|---|---|
| children under anim 270 | all 13 children present and drawn (+4 = 2), positions = the parent root, every coord and work matrix of their super chain (part 2/3/4 → 1 → 0) all zero: the original draws them through GsGetLws, so they collapse to a point. bbtime drew them with the parent's yaw only: the clock 1317 showed as a yellow "foot" bottom right (half sunk in the floor) | `gate_peek_nowhere.txt`, `o_nowhere.png`, `o_entry.png`; before: `w10/ls01/side_ls01_seat.png` | **Verified** |
| era 1 (Right) | that parent plays anim 1: part 3 coord diag(−2, 2, −2) → work 2·I (digit 970 "1" twice size), part 4 work I (clock); bbtime `LitWorld::parentFrame` (part matrix through the chain with `partOriginQ12`) gives the same; drawing = parent part matrix · RotMatrix(rot x, matYaw, rot z) · Scale | `gate_peek_era1.txt`; `side_r_2s.png` ("1x clock"), `side_rr_2s.png` ("30x clock"), `side_nowhere.png`, `side_back_nowhere.png` | **Verified** (`testGateLs01`) |
| crossed antenna on the time machine | drawn by the original too (two rods from the top cones to the electric arc); the remaining difference was the vortex sprite's brightness behind them (§27) | `side_antenna_zoom.png` (x4) | **Verified** (not an extra part) |
| other part-attached children (sweep L01A, TITLE, L02A2, L04A1, Merlin, L02C4: none or yaw-only frames) | L03ACOM cannon 398 on pirate 280's part 6 (chain scale 1.5 · 1.04): now drawn 1.56× like the oracle (crop MAD 5.93 → 5.54); l04e 607 on cart 109's part 15 (x tilt −94) now leans with it (no oracle shot); L04B3 548 has no model | `side_l03acom_cannon{,_before}.png` (oracle `w9lit5/o_L03ACOM.png`, counter 83) | **Verified** (L03ACOM size) / Inferred (l04e) |

## 29. LiT PC GL blended order and depth (wave 12; Proton-GE `/b03`, `/b70`, `local/wine/compare/w12/lit/`)

GL renderer 2 (bugs.exe; pointers named from the GetProcAddress table at file 0x66000): init 0x40daf0 `glDepthFunc(LEQUAL)`,
depth test always on, `glAlphaFunc(GREATER, 0)`, `glBlendFunc(SRC_ALPHA, 1−SRC_ALPHA)`, `glOrtho(−W/2, W/2, −H/2, H/2, 400,
65536)`: vertices arrive pre-projected with eye z (< 0), so the depth buffer holds eye depth interpolated linearly in
screen space. Per texture id four lists (`0x4e7dc0 + id·16`: opaque tris / quads, blended tris / quads) and a blended
layer per id (`0x621620`, last writer wins: 1 by default, 2 for Bugs' faces 0x44ca11 and ground shadows 0x44bde9, 4 for
type-0x15 sprites). Flush 0x4124b0: opaque lists (no blend, no alpha test; ids flagged `0x52fd69` = used with ABR 2 are
deferred); then blend + alpha test on: untextured blended quads / tris (id 0, e.g. the pause veil 0x40be10); then every
polygon of layer-1 blended lists plus the deferred opaque ones sorted by mean vertex z ascending (farthest first,
0x413440), alpha test off (`0x4ac030` = 0); then layers 2–4, alpha test on, quads only (their blended triangles are not
drawn). glDepthMask is never called: every pass writes depth.

| claim | runtime | evidence | verdict |
|---|---|---|---|
| blended faces write depth, per-polygon back-to-front order, screen-linear eye depth | bbtime (LiT Original Fidelity, GL + VK, `bb/render/pc_order.h`): untextured blended first, layer 1 sorted per triangle by centroid eye z, `Mesh::late` (Bugs, shadows) last, all depth-writing, alpha test > 0 except the sorted pass; vertex shaders write depth = eye depth linear in screen space (GL `uLinearZ`, VK push flag 256). LS01: the light sprites' dark bars under the time machine appear; the floor's coplanar layers (actor tex 338 opaque, 363 / 296 additive at y 290 / 320) occlude along clean polygon edges instead of z-fighting stair steps | `w12/lit/side_ls01_{683,1063}_vk.png` (MAD 3.75 / 4.01; 0.11.0 4.22 / 4.49 `side_ls01_*_before.png`), crops `side_ls01_{683,1063}_under_zoom.png`, GL `side_ls01_seat_gl.png`; mean brightness within 0.2–0.6 of the oracle (0.11.0: +0.8–2.0) | **Verified** |
| LS01 vortex spikes, light sprite positions (wave 13, rand hook `local/w13/lit/rand/rand_hook.s` at 0x450810, alias table capture `alias.py`) | the only rand() caller in LS01 is 0x4455e0's probability gate of type-2 actor 333 (2 draws a tick until one passes; bbtime's stream was already equal); the flashing lights are texture links 231–442 written by type-2 / type-20 actors into 0x4e5e80 (bbtime used a clock); the vortex is TOD-animated and deterministic (shots taken under SIGSTOP lag a tick) | `ls01_a.pkl.gz`, `ls01_alias.pkl.gz`; `local/wine/compare/w13/lit/side_ls01_{683,1063}.png` (MAD 1.32 / 1.31; w12 at 1063: 4.01), `crop_ls01_683.png` | **Verified** (`testLinksLs01`: seed + 13 aliases every counter 3–3001) |
| simplifications | quads split into triangles sort separately (exe: 4-vertex mean); layer per mesh (exe: per texture id, last writer); layer 2–4 blended triangles drawn (exe drops them); ABR-2-used textures' opaque faces stay in the opaque pass | static only | Not ported (no visible case found) |

## 30. LiT pad remap, demo / tape, pause-menu Video and Controls pages, music volume (wave 13; Proton-GE `/b04`, `local/w13/litin/`)

Key input under Xvfb reaches DirectInput only after a click into the client area (`xdotool mousemove --window W 320
240 click 1`): without it `0x9ca8c0` stays zero while Esc / F4 still arrive as window messages.

| claim | runtime | evidence | verdict |
|---|---|---|---|
| remap presets 0x4ae1f0 via G[0x11] at level init (0x448fa0 from 0x4474b0) | GD+0x51 poked 1 / 2 / 3 and 0 + level reload (0x4b39bc = 1): GD+0x2e..+0x3c = `128 4 32 64 8 1 16 2` / `4 8 16 128 32 1 64 2` / `32 8 128 16 1 2 4 64` / identity; holding f s a d e q r w (□ ○ △ ✕ L1 L2 R1 R2) gives GD+0x0c = the preset word, GD+0x10 = the physical bit | `remap.txt` | **Verified** (`testPad`) |
| pad-removed pause (report type ∉ {0x41, 0x73} → Start after 4 passes) | `0x402fe0` (PadGetState) is `mov eax, 6; ret`, so 0x449080 never clears the type; 0x403010 writes 0x41 / 0x73 every pass; peek `0x4b31c1` = 0x73 throughout | bytes `b8 06 00 00 00 c3`, peeks | **Proven absent on PC** (not ported) |
| demo stream chunk 0x47 tag 0x48 (G[0x0f]) | no PC LiT BZE has chunk 0x47 (269 files); the PS1's DEMO* levels are not on the PC disc | `game` sweep asserts `LitWorld::demo` empty | absent data; reader + 0x4491c0 playback branch ported (`testPad`) |
| `/r:FILE` record, `/p:FILE` playback (0x408f70, 0x4491c0, 0x409520 / 0x4095a0) | `/b04 /r:rec1.dat`, F4 → Yes: `Datas/bin/rec1.dat` 8432 B = 4216 passes × `ff ff` (report bytes 2 / 3, no DirectInput keys reached the exe in that run); 0x4b1d04 = 1, 0x4b1d0c counting by 2 | `local/wine/lit/Datas/bin/rec1.dat` | **Verified** (format); bbtime `--record` / `--playback` |
| Video page A106–A118 | desktop 800×600, window 640×480: resolution Right Right → item 5 (1024×768: windowed stops on a mode ≥ the desktop), fog Left → Normal, Apply → config 640×480 windowed, fog 1, 0x4b3e60.. = 640 480 1 0 0, 0x45f2b0 back to 1 after the re-init | `o_options.png`, `o_video_*.png`, `o_after_apply.png` | **Verified** (`testMenuConfig`) |
| Controls / Keyboard C94 | Look ← g: kbdMap slot 8 = 0x22; Crouch ← d: slot 9 = 0x20 and Jump (slot 10) takes Crouch's 0x1f | `o_keyboard.png`, `o_kcap*.png` | **Verified** (`testMenuConfig`) |
| music volume 0x9ca854 | 0x41d5f0 SetVolume((v − 100)·100) hundredths of a dB; below 50 0x41d900 / 0x41d630 / 0x41edb0 change nothing | static | Implemented (`litMusicGain`, `LitWorld::musicTick`) |

## 31. TB effects (wave 13; system Wine `RUNNER=wine`, `local/wine/compare/w13/fx/`)

Ported in `engine/game/src/tb_fx.cpp`: particle emitters 0x4d6ae0 (20 slots over a shared pool of 120 particles;
fields 0x452060 from logic S 118, emission 0x452ab0 from V 0x80200000, update + draw 0x453350 / 0x452e80 once per tick
after the entities and the camera, slot clear 0x4529d0 from S 138, actor touch 0x447ee0 → +0x28 0x4000 / R 0x48),
weather streaks 0x451370 (S 110), and the model / rig swaps 0x469320 (V 0x80020000, distance LOD of 0x45eee0).

| check | original (R) | evidence | verdict |
|---|---|---|---|
| particle update | Bugs dropped into the Granny pond: splash slot 6 (12 drops, gravity 195, drag 275) re-born in slot 4 (rings), ring slot 5, director sparkles 13; every sampled slot record and particle list per tick (`fx.sh granny`, `fx_capture.py`) | `granny.pkl.gz` → `engine/game/tests/tb_fx_granny.inc`, `ctest game.tb` `fx` (field for field) | **Verified** |
| emission rand() | 2 per particle (direction), +1 with slot flags 0x80, +1 without 0x800 | `emitcheck.txt` (seed fit, max dot error 0.00108) | **Verified** |
| draw | splash and rings at the oracle's screen spots | `side/side_granny_splash.png`, `granny*_*.png`, `arca_*.png` | **Consistent** |
| weather (VICASG04 snow) | drops in a 1500 × 1024 × 1500 box 1200 ahead of the eye, wind minus look-at motion, gouraud streaks 0x808080 → 0xefefff | `local/w13/sky/side_vicasg04_cam.png` | **Consistent** (size, density) |

## 32. TB cheat codes and the level select (wave 13; `RUNNER=wine`, `local/wine/tbintro/w13_levelselect.sh`)

0x40fbc0 (every input poll) matches the held DirectInput keys against "RULETHEEARTH" (table 0x48871c) and arms
0x4d095c for the session; 0x43bce0 (0x45eee0 tail) then steps 11 sequences (table 0x4ce8e8) on each new raw P1 word
GD+0xe: ○○○○ then □□□ (0: GD+0x8c 1, GD+0xab 0xff), □□△ (1: GD+0x58 999), □△□ (2: GD+0xa2 4, +0xad 0xfd, +0xae
0xff), □△△ (3: health, lives ≥ 3, downed partner revived at the focus), △□□ (4: GD+0xd2..0xdd 1); 5..10 set nothing.
GD+0x8c & 1 adds the pause menu's eighth entry (template 0x1d block 465: text 0x21 instead of 0x24, Up wraps to 7),
whose event 200 enters state 312: the level list (text 0x66), cursor S[0x21] 1..0x32, ✕ = V 0x1000 with the level.

| check | original (R) | evidence | verdict |
|---|---|---|---|
| arming | 0x4d095c 1 after typing, GD+0x8c 1 after ○○○○□□□ | script output | **Verified** (`game.tb` `levelSelect`) |
| page | level list, S[0x21] 1, Down → 2 | `w13_ls_page.png`, `w13_ls_page2.png` | **Verified** |
| pick | ✕: GD+0 7, 0x4d593a 1, S[9] 2 on one tick | `w13_ls_trace.csv` | **Verified** |

## 33. LiT attached steering, camera nodes, zone messages, checkpoint carry (wave 13; Proton-GE, `local/wine/compare/w13lit/steer/`)

Throwaway pollers there: `poke_trace.py` (w9lit3's per-tick trace + stick GD+0x12/0x14, camera yaw 0x4b39fe, local yaw
+0x19e, lean +0xe0), `drive.sh` (trace + keys at a counter), `zq.py` (one zone / queue snapshot).

| claim | runtime | evidence | verdict |
|---|---|---|---|
| stick steering while riding (0x43a880, +0xc4 set) | L01A `/b04`, Bugs poked to (20150, −2780, 7205) at 151, Left held from 173, Right after he walks off: on boarding +0x19e = −1024 (his +0xde 0 − the seesaw's 1024); turning goes to +0x19e (−673, −413 … 283), lean stays 0, the target is stick + camera yaw always (flagsA 0x8000 ignored), +0xde = atan2 of (0, 0, 1000) through the chain (local yaw, then the seesaw's RotMatrix incl. roll); he walks off its end at 203 | `o_steer.csv` | **Verified** (`testSeesawSteerL01a`: position, yaw, state, local yaw every sample 167–275) |
| same branch on ropes / carried | the branch keys on +0xc4 only (rope root, platform +0x7c, carrier hand +0x68); carried Bugs reaches a flagsA-2 move only on the drop tick (state 7 move 10) in the oracle tests | decompile; `test_game` probe | Implemented (rope / carried); the rope and carry traces stay equal |
| camera-node pass = 0x44c4b0 (full frames) | clears 0x300; per drawn actor (+4 & 2, type ≠ 7, box not all 0) a part with flags 0x40, no 0x20, not a mesh (0x11 unless hidden 0x8000): actor flags18 \|= 0x1000, 0x200 if flagsC 0x100, 0x44c070 eye / look / 0x100 / yaw 0x4b3e1a = 0x4b3e2c / distance 0x4b3da8, flagsC 0x10000000 also 0x4b3928 / 0x4b3954 / 0x4b3964 / 0x4b3974. Skipped frames run 0x40a530 (no visibility / box test, hidden parts excluded); bbtime draws every frame | decompile | Ported |
| timers under a camera node | L02A1 `/b09` intro: 0x4b3940 = 0x142 counters 3–265, 0x42 at 267, 0x40 at 269; Bugs' +0x22 = 2 throughout, then 3, 4 … 19 at 301; active type-14 timers the same. 0x447d40 counts +0x22 only if 0x100 is clear, 0x200 set, type 0x20 or flags18 0x1000 | `o_nodes_L02A1.csv` | **Verified** (`testCameraNodeTimersL02a1`) |
| first node pass after the counter-3 tick | at counter 3 the oracle already has camera flag 2 and Bugs' timer 2: the load update (counter 1) and the counter-3 tick run without a node (the loading screen is drawn, not the level) | `o_nodes_L02A1.csv` | **Verified** (`loadUpdate` has no node pass; title test asserts after one tick) |
| A17 zone messages | L04A1 `/b29`: queue 0x4b39de = 1, 0x4b2480 = {31} at every sample (counters 107–2051, Bugs in zone 30); start view table `30 31*` (31 flagged: its portal's u32 +0x10 is queued), eight later views `30` | `zq_L04A1.txt` | **Verified** (`testZoneQueueL04a1`, `render.test_zones` re-walks all nine cameras) |
| flagged entry vs actor visibility | 0x447d40: an actor whose zone's first table entry is flagged is hidden unless its flags8 has 0x10000; not listed: hidden | decompile | Ported |
| cell / hull culling (0x40fe90 types 0 / 4 / 8 / 0xc) | a rejected hull lies wholly outside one frustum plane (x / y screen edges, near −200 inside GL near 400, far −D beyond which everything is fogged to the black clear colour), and 0x40efb0 divides by z + 1 behind the eye (0x45c308 / 0x45c30c), so its polygons would be clipped or invisible anyway; no portal lies inside a hull's record range | decompile, level.cpp scan | Proven without visible effect |
| portal screen rects | 0x40fe90 writes the table entries +4..+0x10 (min / max x / y), nothing reads them | decompile (only writer / reader 0x40fe90) | Proven dead |
| respawn point across a level change | L01A sets 0x4b2460 = (23700, −640, 9655), 0x4b246c = 4096 by counter 105; GD+0 poked 18 with 0x4b39bc = 1 loads L02C2 (Bugs at 8970, −640, 3950): 0x4b2460 / 0x4b246c keep L01A's values at every sample | `ckpt.txt` | **Verified** (`testCheckpointCarry`; the app carries them, `carryCheckpoint`) |
| spawn yaw | Bugs' +0xde at counter 0 = 4096 (L01A tag 0x11 raw), stored as such into 0x4b246c | `o_seesaw_L01A.csv`, `ckpt.txt` | **Verified** (raw tag value, no wrap) |
| hand events / gun shots (LitFinal) | f3 4 / 2 add the hand part's rot y / x to the spawn (0x4393e0; no PC event uses them, 167-BZE scan); hand spawns copy +0xdc; f3 0x20 type-31 shots 130 / 131 / 225 (L04A1–A3, L04B1/B2, L04C3, L04D2, L05A5; type 14 in l04e / L04E2) keep the hand matrix (MulMatrix 0x408490) and 0x446d10 moves along it | decompile, BZE scan | Ported (no oracle capture of a shot) |
| TOD-less clips (LitFinal) | a clip without an asset returns 0x400 on its start tick, then 0x200 (held) as 0x4393e0 / 0x438970; no role-7 model asset at all dereferences null in the exe | decompile | Ported; crash path documented |
| live-list order (LitFinal) | 0x440120's arc-jump target = the first matching entity in the collision list 0x4b39ac (L03A id 715 ×4, L03C id 53 ×2) | decompile | Ported |
| language and speech (LitFinal) | config.pc +0x08 → G[0x10] each frame (0x4076f0); level speech section per language 6, 5, 7, 8, 9, 10, tags 0x4c / 0x4d (speech) and 0x4a (text) take that language's offset pair; clip table 0x45c340 (6 rows, each summing to its Speech0N.xa size, Speech00.xa 36102560 B); played only at music volume ≥ 50 (0x41e2b0) | decompile, L01a section dump | Ported (settings.ini `lit_language`); the PC disc ships languages 0, 1, 3 |

## 34. TB speech (wave 13; system Wine `RUNNER=wine`, `local/wine/tbintro/w13_speech*.sh`)

| item | exe | evidence | status |
|---|---|---|---|
| request by state | 0x469770: on a state's anim start with mode & 0xb and mode & 0x80, clip `0x63b73c` = anim + 0x9a (u8); a clip ≠ the last started (`0x4cca40`) stops the player (0x42b740) and sets S[0x22] (`0x4d24c2`) 2; the same clip only once S[0x22] is 0 | GRANNY: entity 297 state 394 (anim 360) → clip 2, oracle counters: ✕ closes "Welcome to Granwich!" at 329, S[0x22] 2 at 411, 4 at 413, ✕ at 1899 stops it (S[0x22] 0, clip 0, last −1) — Speech2.xa is 61 s; RETRY Daffy state 348 (anim 417) → clip 59 at 35, 4 at 37, ends at 403 (`w13_speech_trace.csv`). Counter c = tick (c − 1) / 2 after a load: every class-14 entity's +0x82 is 0 at counter 1 and 2 at counter 3 (`w13_retry82.{sh,py,csv}`), so the first entity tick runs at counter 1; Daffy (slot 22) leaves state 3 at counter 33 = tick 16 (+0x82 16 > 15) and asks at tick 17, Granny at tick 205 — both equal in bbtime | **Verified Against Original Behavior** (`game.tb_speech`, `game.tb`) |
| request by logic | action 62 (0x431ba0): X > 1 → clip X − 0x66, S[0x22] 8; X 1 → S[0x22] 1 (holds silent); X 0 → stop at once; both restore GD+0x98 from the load copy 0x63af80 | GRANNY's dialog blocks use X 418..422 (clips 60..64) and 0 (✕ skips) | Implemented |
| player 0x42a030 (start of every tick) | audio (`0x4d1f8c`): 8 → stop the music stream, 2; 2 → open `..\speech\%.2d\speech%d.xa` (GD+0x93, clip), play, 4 (open failure: the timed path); 4 → 0 when 0x40d9b0 reports the stream played out; 0 → stop, last −1. No audio: 2 → timer (table 0x481678 × 1000 / 44100 ms; 6000 for every clip), 4 → 0 when it ran out | decompile; oracle rows above | Tested |
| music while speaking | PlayMusic 0x4291e0 closes the music stream (0x40d3e0) while S[0x22] ≠ 0; afterwards 0x40d440 on the reset stream returns 0 and the track is reopened from its start | decompile | Implemented (app: no track while S[0x22] ≠ 0, then the level track restarts) |
| volume / pause | 0x40d440 sets every stream's volume from GD+0x98 (music volume); the pause (0x429180) stops only the music stream | decompile | Implemented (speech on the music player at the music volume, never paused) |
| files | `Datas/SPEECH/<NN>/Speech<n>.xa`, mono IMA ADPCM, 44100 Hz (AUDIO_XA.md); this disc has languages 00–04 and 06, clips 0–66 | data | Implemented (`bbtime::tbSpeechFile`) |

## 35. TB front-end quit, save slot, draw bias and distances, flashes, shake, look view (wave 13; decompile, `RUNNER=wine` `local/wine/tbintro/w13_flash.sh`)

| item | exe | bbtime | status |
|---|---|---|---|
| quit to the front end | S 119 0x432cc0 (the pause menu's Quit Game, template 0x1d in every level, and Credits): state 0x4d03d0 = 11, the streams stopped (0x429ce0); the main loop returns to the menu | `TbWorld::quitToMenu`; app: stop voices, leave the level, open the TB front end (smoke: pause → 5 × Down → ✕ → YES, `local/shots/w13/tb_quit_game_frontend.png`) | Tested |
| save slot | DAT_0048840c: −1 for a new game, the loaded record's slot otherwise; R 0x52 = slot ≥ 0 | `TbWorld::saveSlot`, set by the app at every level entry and after a save | Implemented |
| draw bias | +0x33 = tag 0x38 @0 + 0x10 (S 127 sets it) is written into the draw context's s16 +10 by 0x465130 / 0x465710 (0x8000 for sky domes), 0x464480 (−6) and 0x464b20 (+0x80); no PC draw path reads it (0x469aa0, 0x464820 / 0x464b00 / 0x464b20, the sprite draws 0x41c660 / 0x41db70, 0x406840, 0x406cd0 / 0x406d90 read the context's +4 / +6 / +0x10 / +0xe only): a PS1 ordering-table leftover | `TbActor::drawBias` kept by the world, not drawn | **Proven no effect on PC** |
| draw distances | context +0x10 = near 0x4d5954, +0xe = far 0x4d5980, +0x1a = 0x4d237a (chunk 0x3a tag 0x3c @0 / @2 / @10; S 109 / S 117 change near / +0x1a), each × projection / 400, capped at far. 0x406840 draws the world's tree A to near, tree C from near to far, tree B to far, with 0x409840 setting near = far at DETAILS 2 (config.pc +0x18; the oracle's is 2), where tree C is not even built (0x404f40); models draw with 0x409840(32000, 32000) (0x406d90). +0x1a has no reader | `TbWorld::nearClip`, `viewDist2` kept; Original Fidelity clips at `Scene::tbFarClip` (per pixel, actors included) | near / +0x1a **Proven no effect** at DETAILS 2; far Implemented |
| flash colour | 0x465130 runs 0x464ef0 (light colour 0x4883e0..e8 × (0x1000 − dark) / 0x1000) then 0x464c30, which with mode 0x40 sets the light colour to S 50's rgb (0x4d23c4 / 0x4d26ad / 0x4d26ac, set per channel by S 50 0x4319b0) · dark / 2²⁴; every glColor (world, sky, models, text) is multiplied by it, so a flash is a near-black blink tinted by its colour. While the fade mode is 0, 0x464ef0 clears the three colour bytes every frame: poke the mode first (`w13_flash.sh` poked the colour first, so only blue survived and its "white" flash was blue). Oracle `local/wine/tbintro/w13_flash_red.sh`: ARCA01 fly-over, held flash dark 0x800, colour (255, 0, 0): light (0.0311, 0, 0), frame mean R 2.10, G 0, B 0 at counter 921–925 | `fadeOverlay()` returns the per-channel multiplier (flash rgb · dark · 255 / 2²⁴, else (0x1000 − dark) >> 4) and the app multiplies the frame by it (`Renderer::multiplyRect`, DST_COLOR · src, GL and VK): at the oracle's camera and tick 450 R 2.37 (VK) / 2.37 (GL), G 0, B 0 (`local/wine/compare/w13/tb/side_flash_red.png` ×20, raw `side_flash_red_raw.png`; bbtime's frame is ~1.12× brighter in general, as in the look-view pair); `BBTIME_TB_FADE` holds a fade for such checks | **Verified** (per channel) |
| camera shake | 0x439650 at the end of 0x439700: look x += sin(phase) · amp >> 11, phase += speed (X · 11), amp − 3 per tick once phase ≥ 0x800; S 41 / S 49 start it | `TbWorld::camShake` → `cam.look` | Implemented (no oracle trace yet) |
| look view | 0x4d4fd4 & 2 (L2 + R2): first-person view; 0x465130 skips +0x24 & 4, which hides the focus; HUD sprite 29 is the cross-hair with GD+0xa2 & 4 | `lookMode()`; the app skips +0x24 & 4 for the players too. Oracle `local/wine/compare/w13/tb/look/run.sh` (ARCA01 after the ✕ skip, GD+0xa2 \|= 4, L2 + R2 = d + f): look on at counter 663 (Bugs +0x24 0x8800 → 0x8004, 0x4d4fd4 2), turn 0x4d9a9a −40 per tick while ← is held; per-tick Bugs / view block / 0x4d9a98 / 0x4d4fd4 in `look.pkl.gz`. bbtime gives the same view eye (−4366, −530, −7833) and look point, the player hidden and the cross-hair: `local/wine/compare/w13/tb/side_look_mode.png` (MAD 8.8; an NPC by the stairs differs by the run's timing) | **Verified** (view, hidden focus, cross-hair; no per-tick trace check yet) |
| frame brightness (Original Fidelity) | the light colour 0x4883e0..e8 is set to 1.0 by the GL renderer hook 0x4258e0 (pointer 0x4d0338) and otherwise only scaled by a fade / flash; with no fade every glColor is the vertex colour (world, models, sky, text) and the CLUT texels carry config.pc +0xe0 (§16). The oracle prefix's config.pc +0xe0 is 1.0; showing the Video page turns it into 1.125 in the exe and in bbtime (§16), and a `settings.ini` saved that way (`tb_brightness = 1.125`) made bbtime 1.12× brighter than the oracle (look view gain 0.89, red flash 2.37 vs 2.10). Oracle `local/wine/compare/w13/bright/run.sh` (`/bze:ARCA01` after ✕, `/bze:VIQ02`, `/bze:GRANNY`; camera, counter, `o_*.png`, `o_*.txt`), bbtime with `XDG_CONFIG_HOME` holding `tb_brightness = 1` at the oracle cameras: least-squares gain orig / bbtime above the text box (rows 70–330) VK 1.008 1.009 1.007 / 0.984 0.993 0.984 / 0.995 1.003 1.004, GL 1.008 1.008 1.006 / 0.982 0.988 0.978 / 0.995 1.002 1.004 (ARCA01 / VIQ02 / GRANNY); the look-view pair at tick 432 1.000 1.002 1.002 (MAD 8.79 → 2.57) in VK and GL (`side_{ARCA01,VIQ02,GRANNY,look}_{vk,gl}.png`, `side_look_vk_old1125.png`) | no change: compare with the oracle's `tb_brightness` | **Verified Against Original Behavior** |
| blob shadows / floor shade | 0x465130 lists drawn entities with +0x36 (tag 0x45, U 17) ≠ 0 and a model instance (≤ 40) and steps +0x124 2 per frame toward +0x125 while +0x28 & 0x1000 (else down by 2), drawing the entity at (32 − v) / 32 (0x488400). 0x464480 (after the particles) skips +0x24 & 0x40, takes the shadow entity's first role-12 link's model and draws it scaled ((ey − gy)·2 + 0x1000, ≥ 0x800) on x / z, matrix TR y / z += 5 / 9. 0x464090: x / z = world origin of the instance's head coordinate (the hips, TOD part 1); on a flags1 0x200000 parent y = ey with its angles; grounded (+0x20 & 2, !(+0x28 & 2), +0x108) y = ey − 4 with +0x108's normal; else a ray 0x8ca down from ey − 100 (query 0x105, flags1 0x200000 shapes), no hit = no shadow; rot (−atan2(nz, \|nxy\|), yaw, 0x800 − atan2(nx, ny)), y kept ≥ ey. flags10 0x200 (only GRANNY's players): floor surface flag 0x80 sets +0x125 0x10 / +0x28 0x1000 on it and its children (+0x1c & 0x1000 runs its program), else clears; others clear +0x28 0x1000. 0x450ed0's flags10 0x400 shade is in no level | `tb_shadow.cpp` (`TbWorld::shadows`, `TbActor::shade`), app `tbShade` and blob draw (GL + VK). Oracle `local/wine/compare/w13/shadow/run.sh` (code-cave hook on 0x464090's tail: `hook.s`, `capture.py`; `rootpeek.py` read the head coordinate). `game.tb` shadows: ARCA01's 7 listed shadows equal (players / guard x / z depend on the idle frame), GRANNY's poke onto the shade floor equal for 31 ticks (Bugs: shade 0 → 16, target, placement; Taz until its fidget starts). `side_{ARCA01_vk,ARCA01_gl,GRANNY_shade_vk}.png` (MAD 6.0 / 6.0 / 1.6). An entity-shape ray hit keeps a flat normal (rayHit exposes no shape normal) | **Verified Against Original Behavior** |
| pad rumble | S 39 / 40 / 42 / 43 / 48 / 130 / 131 / 134 drive force feedback 0x460e10, but 0x40fc30 returns 0, so the state is zeroed every poll and the actuator call is empty | not ported | **Proven absent on PC** |

## 36. TB classes, riding and pushing, S / R / U coverage (wave 13; `RUNNER=wine`, `local/wine/compare/w13/classes/`)

| item | exe | evidence | status |
|---|---|---|---|
| class 0 static model | 0x450890: first tick builds the model instance; with the first asset's static flag it draws the mesh under the entity matrix (0x44e5d0) and never animates | ARCA01 wall-disc panel (chunk 0x1584d: one model + rig + anim 0xd7, links 312–314) was posed only from animation parts, so it and the scene prop it replaced were both missing; `local/wine/compare/w13/tb/side_arca01_freecam.png` (MAD 7.6). It is the only class-0 entity with a model in the 58 levels (5474 entities and every spawn template scanned, 120 ticks each), so the rule draws nothing else | **Verified** (side-by-side) |
| class 2 texture-link writer | 0x4599e0 via 0x465dc0 / 0x465c80 | `arca01_links.pkl.gz`: 181 rows, GD+0xd8 / 0xd9 / 0xda pokes switch each link 344 → 343; drawn: the wall discs turn gold in both at the fly-over's tick-1220 camera (`local/wine/tbintro/w13_links.sh`, `local/wine/compare/w13/tb/side_arca01_links.png`, MAD 8.8 / 7.6; that run's alias words ([0x4d038c] + 0x270, links 312 / 313 / 314) go 344 → 343 at counters 145 / 147 / 145 for the sequential pokes d8, d9, da (the last landing a tick later), the order bbtime's mapping gives: d8 → 312, d9 → 314, da → 313, each switching the next tick) | **Verified** (`game.tb` classes, 0 mismatches; side-by-side) |
| classes 4 / 5 / 18 / 21 / 33 / 40 | 0x453b40 sprite, 0x4516e0 block, 0x44f7b0 modes 1 / 0x10 / 0x1000, 0x459b10 hung | `viq02_c4`, `azcasg02_push`, `vicasg04_seesaw`, `arcasg03_c33`, `viboss01_c40` (301 rows, `/skip_intro /b26`) | **Verified** (`game.tb` classes, 0 mismatches each) |
| ride / push | 0x453de0 ride (mount +0x90 / +0x13c, rider turn +0x12a) and push (+0x24 0x10, +0x58) blocks | `arca02_ride` 228 rows, `azcasg02_pushpoke` 151 rows | **Verified** |
| actor line of sight | W sub-mode 0x82000000 (0x4463e0 actor pass) | no W item of the 58 levels uses it (`game.tb` lineOfSightUnused) | **Proven absent** |
| S / R / U coverage | every handler of the tables | `game.tb` opsScan runs all 123 537 R / S / U items of the 58 levels: none unsupported | Tested |
| spawn ids 82 / 83 / 185 / 252 / 284 | 0x460b00 returns 0 for an id not in the level's chunk-0x08 list | no template with those ids in their levels | **Proven absent** (as the exe: nothing spawns) |


### 35.y Wave 13 pole climb, focus pointer, pool (TbCarry5; `local/wine/compare/w13/climb/` run.sh / run_b.sh, RUNNER=wine)

| item | finding | evidence | status |
|---|---|---|---|
| pole grab 0x456fb0 | contact with flags0 0x10000, own state flagsA 0x100, players only airborne (+0x20 0x80420), no parent; offset ahead ((atan2 − 0x400 − yaw) & 0xfff < 0x800), dy in (pole +0x6a − own +0x6a, pole +0x62): hang on the pole root at Ry(−yaw)·offset, +0x136 = atan2 − 0x800, move / velocity 0, +0x20 0x200; carried 0x2800 children let go | GRANNY pole 19: grab at counter 1825 / 1793, local (15, 710, 158) | **Verified** (`game.tb` climb) |
| climb tick (0x453de0 @0x45644a) + 0x44f690 | flagsA 0x200 with a parent: +0x1a0 = 0xc; heading = atan2(local x, z) + side·dt·0x80 / pole +0x68, local x / z on radius +0x68 + 10, +0x136 = heading + 0x800; pole limits: own +0x6a + local y < pole +0x6a → +0x1a0 &= ~4 (and a rising flagsA 0x800 state stops), past pole +0x62 → let go; after the mover +0x20 6 on a 0x10004 parent lets go | up to local y 374 (+0x1a0 8), circles 116 / 117, down 99 to the ground (counter 1885) | **Verified** |
| R1 pump / rope branch | +0x166 written only here, read nowhere in bblit2.exe; no class-8 entity and no non-pole climbable parent in any PC level | decompile grep, level scan | **Proven absent** |
| focus pointer | DAT_004d236c set by the view-list pass from GD+0xa4 & 4; R1 toggles GD+0xa4 after the pass, so the pointer moves next tick and 0x4d5028 (camera) one tick later; logic reads GD+0xa4 (director 338 `R 0x19` 0x4c & 4) | oracle eye at 2015 / 2017 (climb A) | **Verified** |
| entity pool | slots = instances + one per chunk 0x08 + 17 (0x460a40, 0x462030); min level slack 24 (TRQ02, VICA01); peak live spawns in 6000-tick sims ≤ 17 | ARCA01 oracle 338 used vs ours 344 at load (5 gdBit removals + director 264 on tick 1) | Implemented (not reached) |

### 35.z Wave 13 oracle checks: shake, glide, water snap (TbChecks; `local/wine/compare/w13/checks/` run.sh, RUNNER=wine; `game.tb` checks)

| item | finding | evidence | status |
|---|---|---|---|
| S 41 source | anim asset logic (chunk 0x22) of anim 75 = Bugs' slam landing 121 (also anims 146 S 41 50 / 25, 224 S 49 150 / 50); runs after the camera: the phase starts the next tick | A GRANNY slam: speed 330, amp 20, offset sin(phase)·amp >> 11 on look x | **Verified** (403 rows, 13 shaking) |
| S 49 player (130 / 131) | dig spot entity 260 sets +0x24 0x400 within 200; slam → 124 / 126 underground; under a flags0 0x10000000 block (contact, T 3) 131 / 130, ✕: S 49 100 / 30 → amp 26 by the look distance, block pushed up | B ARCA01 | **Verified** (283 rows) |
| S 49 boss | ARQ01 234 drops (364) on Bugs below it, lands (375): amp 40 | C ARQ01 counters 987–2021 | **Verified** (419 rows) |
| flagsA 0x8000 | Bugs 66 (vert −19) / 67 (vert +20, GD+0xad 2) glide; VIQ01 226 (333, vert 0): +0x20 0x80000 | A; D 40 steps of 226 | **Verified** (S-form vert of Taz 250–253 not traced: needs a 148 gum item; open) |
| V 0x800000 | templates 74 / 88 / 89 / 245 / 299 / 396 (water splashes): Bugs falling into a pond (394) spawns 88, its y set to the zone surface −1673 | D VIQ01 | **Verified** (76 rows) |
| spawn yaw | tag 0x11 stored raw (0x1000 stays 0x1000; Bugs VIQ01 2067) | D | **Verified** (fixed) |
| camera node 0x464820 | integer GsGetLw (RotMatrix / ScaleMatrix / CompMatrix) and ApplyMatrixLV for the look: the float chain was ±1 off; flags10 0x20 also sets the restored pose | VIQ01 / ARQ01 intro look | **Verified** (fixed) |
| Taz ignoring input (TRCA01 / TRQ02) | not a bug: the TRCA01 fly-over holds control until ✕ skips it, accepted from counter 181 (oracle: 177 no, 187 yes; bbtime the same); TRQ02's walk-in (545) refuses R1. App: ✕ at 120, R1, ↑ moves Taz (`local/shots/w13/checks/side_trca01_taz.png`) | `/tmp` probe + oracle TRCA01 trace | **Verified** |
| Taz gum float (S[0x67] vert) | □ into gum machine 307 spawns gum 148, △ on it: 147 → 161 → 245..248 → 250 → 252 / 251 / 253 (vert 0x4067), 249 → 50 | E TRQ02 (Taz), 1078 rows | **Verified** |
| TRQ02 309 lift | Taz beside 309: event 350 → 394 → 395 / 396 (flagsA 0x80000800, vert 5) up to −2277 → 514, 308 hangs on it | F TRQ02, 719 rows | **Verified** |
| load placement target | 0x4347c0 tag 0x16: every record (instance or template) with flags0 0x4000000 and flags1 0x400 becomes DAT_004d236c; template 181 (a Bugs copy) after Bugs' instance (VIQ01, GRANNY, TRCA01, 20 more) makes the placement (0x4395d0) target pos 0 / yaw 0: eye (0, −h, −100), the first follow tick comes in from there (vel (0, −221705, 0), flags 0x290) | `first.py` VIQ01 /b24 vs ARCA01 /b28 (eye at Bugs) | **Verified** (`TbWorld::loadAtOrigin`) |
| view-list sphere refinement | the box refinement of a cutting sphere needs the model instance +0x9c (built by the first anim tick, freed by a release) or +0x24 0x8000; before that the sphere alone lists it (VIQ01 226 listed at 515, not 517) | D VIQ01, 676 rows incl. the view from the level start | **Verified** (`TbActor::modelInst`) |
| capture reads | the counter 0x4d2708 moves at the tick start: `cap.py` / `first.py` re-read the player, seed and view until stable (a mid-tick seed broke rand replays) | E, F | Tooling |
| state entry clears +0x14e | 0x44d990 restarting a clip keeps only +0x14e 0x4000 (and the hit path's +0x14c 0: &= 0xcfff), so blocks run before the next anim tick see neither 0x2000 (last frame) nor 0x1000: ARQ01 boss 226's hit (+0x5e 0 → transition 3 → 377) runs 377's logic at once, its W mode 8 is false → N 0x79 → 189 jumps before the mover (1187, a tick "early" before) | C ARQ01 | **Verified** (fixed: `dispatch` clears `animLast`, +0x14e 0x3000) |
| partner out of the game | GD+0xa2 & 3: the partner's view-list slot stays −1 (0x45eee0 local_7d0[1]): not ticked, not drawn. A drowned / killed Bugs with Taz in stays in 8 (+0x82 frozen); before, ours kept ticking (394 re-entered, mover ground bit) | C ARQ01 1489–2021, D VIQ01 1577–2023 | **Verified** (fixed) |
| load box until the first tick | an entity keeps its tag 0x1b / 0x1c box (and radius) until a first tick selects tag-0x61 record +0x123 (0x450440(a, 0): classes 0, 5, 14, 33 only); the mover's sphere (0x4490a0: flags0 0x40000 box, 1 capsule, else sphere; unticked +0x24 0x8000 → angles, not +0xa4) is built from it. TRQ02 309 unticked: (−548, 0, −199)..(194, −519, 347) r 371 pushes a Taz poked inside by (−163, +171, −146); ARQ01 Taz starts inside Bugs' load box: pushed to (−14284, 600, −898) on the first tick | H TRQ02 (`--shapes`: oracle record type 1, centre, r 371), G ARQ01 (`early.py`), C | **Verified** (fixed: no box from tag 0x61 at load) |
| 0x44e6d0 nested release | VICASG04: Bugs carrying snowball 241 (from spawner 231; the level's text boxes lock the pad until ✕) beside 131: 131's grab hangs Bugs on its hand and Bugs' 0x8000 program (event 0x97 → 240) lets the snowball go (V 0x200) through that chain, so the throw (241) releases Bugs without children. A thrown entity with children needs a carried item that carries; none found | J VICASG04 (`game.tb`, whole capture, 1020 rows) | Verified: the grab waits for 131's frame with an attach point (frame 4, counter 1277) and its +0x7c goes through 131's 1.25 scale |
| pool limit (0x6c-dword slots, 0x460b00) | `--pool`: slot count 0x4d270e cut to the first free slot before GRANNY's slam, capture from the level start (whole history replayed): our slot table equals the exe's at the cut (258 of 305, first free 20), the slam's template 80 fails in both (made without the cut: `i0_granny_nocut`); trace-equal over 1200 rows. The earlier accounting gap (260 vs 258, 3 extra 420s) was the model: the 18 class 0 / 6 / 7 slots are view / world / 15 system / HUD holder, and the extra 420s came from spawns on frames without an attach point (see the entity-pool and attach-point notes below) | I GRANNY, P (GRANNY 61, ARCA01 201) | Verified |
| capture gaps | a row the reader missed (Wine stall) is replayed with the next row's pad | I | Tooling |


**Entity pool (w13 TbPool; `k_granny_spawns`, `l_arca01_spawns` spawnlog.py, `i_granny` cap.py --pool with the slot table).** 0x4d2704 slots (+4 bit 0 free), count 0x4d270e. Slots 0 view (0x460a40), 1 world + 2..16 (0x462030), then one per chunk 0x07 / 0x08 / 0x0a / 0x63 in file order (templates keep theirs), the HUD holder template 0x1f (0x462430, on the first world-entity tick). 0x460b00 takes the lowest free slot below the count. 0x460630 destroys an entity flagged +0x20 0x2000 right after its own turn (slot reusable later in the pass); 0x45eee0 destroys the other flagged ones in list order but continues with the next entity unchecked, so the one after each destroyed entity dies a pass later (GRANNY 371). Text 24 respawns into the two lowest free slots alternately. `game.tb` checks P: GRANNY counter 61 and ARCA01 201 equal slot for slot; I: the cut equals the exe's table and template 80 fails.

**Attach point (0x468280).** Every applied frame clears +0x84; a packet 0xa00 sets it and +0x7c, the part origin carried one coordinate at a time (0x403400, s16 steps) through the entity's root: the record matrix with its +0xec scale, or, hung, its record (+0x136, +0x13c) and the parent's chain (0x44e560). The asset logic of that frame reads the new point (GRANNY placers 39 / 71 spawn 420 / 173 only on frames with a 0xa00 packet). VICASG04 131 (scale 1.25) grabs on frame 4's point (counter 1277); Bugs' 0x8000 program releases the snowball through Bugs hung on 131's hand. Capture J trace-equal to its end.
### 35.x Wave 13 carry / throw / class 30 / homing / shape normals (TbCarry, TbCarry2, TbCarry3)
| Item | Finding | Evidence |
|---|---|---|
| Carry / throw | grab blocks of 0x453de0, release 0x44e6d0 (nested children: unreached, §35.z), clamp 0x450650, rayHit actor pass 0x4463e0 | `game.tb` carry 481 rows, throw 420 rows, checks J (VICASG04 grab / throw, whole capture) |
| Attach point +0x84 | cleared by every state entry (0x44d990) and set by frame packet 0xa00 (0x468280); a re-entered state whose clip restarts re-applies frame 0 | class 30: state 237 re-entered each tick by block 4 → event 149 |
| Class 30 (0x44d1f0) | template 151 from Bugs' 237 '^' (mode 0x2802) at the look point; flies, removed on a wall hit; slot freed at the tick end | `game.tb` look view + class 30, 270 rows, eye / body included after the release (the oracle's L2 goes down one tick before R2 and up one tick before it: view +0xe4 ±0x5000 on those ticks) |
| Idle fidget | block 15: last frame shown, +0x82 > 30, rand & 0xffff > 0x55f0 | `game.tb` GRANNY shade 50 ticks, `w13/shadow/run.sh` (Taz 20 → 21 sync, seed per tick) |
| Shape normals | 0x4463e0 query +0x10: polygon n; sphere/capsule inside: -delta (-push) · 0x4000/len; surface hit: test normal; box: normal through rec +0x60; shadow 0x464090 tilts by it, surface none | `w13/shapen/run.sh`, `game.tb` sphere shadow 11 ticks |
| Homing jump | flagsB 0x100000: target = selector +0x16; top = box +0x6a/2 + y; h = vert (or top − vert − y below it); vy = −10 − ((√((h·0x800+0x100)/10) − 16)·10 >> 5); n = ((√(((vy−5)² + (top−y)·20)·256) >> 4) − (vy−5))/10 − 1; vz = |d|/n along the yaw to the target | `w13/homing/trboss02.pkl.gz`; `game.tb` homing: TRBOSS02's own cannon-255 spawn (Bugs held at (1532, 0, −811)) enters 496 and matches rows 1587–1595 (yaw 1459, vel (2064, −1968, −1648), positions) |
| flagsA 0x4000 / flagsB 0x4000 at a jump | face the selector target (flagsC 0x100 lead) / yaw = contact yaw + 0x800; the jump detaches (0x44d870) | decompile 0x44d990 |
| Camera sphere sweep 0x440360 | follow work = scratch + 0x15c, look view + 0x10c; head: 0x446ea0 polygons and 0x448ce0 shapes within \|m\| + r; ≤ 11 passes: shapes solid to the camera (record 0x400 none; owner flags10 & 0x4000008, or flags0 0x4000000 unless the focus DAT_004d236c or DAT_004d2714 ≠ 0; 0x44c840 / 0x44c9b0 / 0x44cb50), push 0x443bf0 once \|+0x74\|² > 25, polygons (surface 0x32 none, 0x44bfd0); a contact slides the rest off the normal (Q14), best distance < 100 keeps it (≤ 8) and follows the crease with an earlier one, a rest against m stops | `game.tb` class 30 eye / body after the release, GRANNY shade chase → oracle eye (6201, −3258, 6399) look (5034, −2908, 6552) (`w13/shadow/o_GRANNY_shade.txt`) |

## 37. BBLIT-Remaster findings (LiT bugs.exe 1.6; from BBLIT-Remaster, MIT, InLineR495; `tools/re/verify_exe.py`)

Byte-checked on this project's disc by `tools/re/verify_exe.py` (all 27 hook-site signatures and the 1.6 version
qword 0x45ca00 = 0x3fe5555555555555 match; our .text CRC32 is 0xc1d00325, BBLIT's reference 0xecffb7c9: another 1.6
build with the same layout at these sites). TB bblit2.exe .text CRC32 0xd87e2a8f (the Ghidra export's build).

1. Projection: room emitter 0x40fa90, d(z) = z + 1 for z >= 0 else -z, H = [0x4efb30] (400); x_c = x_ndc·d,
   y_c = y_ndc·d, w_c = -z. Camera matrix C Q12 at 0x4efae0, -eye at 0x4efb20, eye / target 0x4b38c0 / 0x4b38cc,
   part GTE matrix 0x4e5e60.
2. Vertex pool 0x734de0..0x7bd960 (35000 × 16 B), cursor 0x623654. Writers: room 0x40fa90 (slot 0x4b1aa4), part
   0x411350 (0x7c9618), billboard 0x40a6c0 (0x7c624c), text 0x40b130 (0x7c6250), overlays 0x40b980 / 0x40be10
   (0x4b1a5c / 60), bars 0x40c5e0 (0x4b1a64), HUD bar 0x40c850 (0x4b1a68).
3. Actor on-screen cull: 0x4481c7 / 0x4481d1 (actor) and 0x4484b7 / 0x4484bd (part) compare projected x with ±0x100
   under the unsqueezed matrix 0x52fce0. Part pool 0x4b2240 / capacity 0x4b2244, stride 0xb8; exhaustion spins.
4. 41st shadow: 0x44c981 jge -> 0x44cb77 `jmp $` (eb fe, a hang); 0x44c9ae is the normal no-shadow path.
5. Portal test bounds 0x621610 (-w/2, read 0x410bbe), 0x621614 (+w/2, 0x410b62), 0x62161c (h/2, 0x410c15);
   per-polygon vertical tests read 0x4efb4e at 0x410070, 0x4100ba, 0x411c7d.
6. Backface 0x40fdc0 gets [v0, v1, v2, v3, nverts, 0]; quads zigzag, second half (v1, v3, v2); face list 0x621608,
   vertex pool pointer 0x623688 (s16 indices, 16-byte entries).
7. Fog writer 0x40cc80 also keeps float D at 0x4efb44 / 0x4efb68; 0x4b1ab0 is its call cache; level u 0x4b38a0,
   config fogDist 0x9ca83c. BBLIT's 200000 cap is its own.
8. No-fog (far 0x3fff) at 0x44cb90 is conjunctive: +0xc & 0x2000000 and (type 9 or +0xc & 0x20000000)
   (`main.cpp litFar`).
9. ABR variants: GL ids at 0x52fd74 + 48·id + 4·abr, max id 0x4b1c90; lookup 0x41493a, conversion 0x420d90 (gamma
   1.2 and offset; alpha maxima ABR 0 α·2, 1 α·4/3, 3 α·4; ABR 2 colour unchanged).
10. STP: converted alpha > 0.52 / 0.76 / 0.26 (ABR 0 / 1 / 3) marks opaque texels; PS1 0x0000 = no coverage, 0x8000 =
    opaque black (halves the backdrop under ABR 0); PC ABR 2 near-black textures (shadow 133) use dst·(1-α). The PC
    data lacks the STP bits of textures the port added (wave 14: LS01 tex 373 all-STP on PS1, black-only on PC).
11. TITLE depths (view z): logo -2160..-2120 (entity 20, def 36), machine -2546..-1746 (carrot to -2062), menu panel
    -2000, Bugs -1289..-1201, copyright plate -871.
12. Pushable stone (type 5): collision holds Bugs 170 from its centre on one side, 240 on the other (~65 sink, PS1
    too). BBLIT's 50 radius / 120 cap are mod choices (`push_r`).
13. Input: pad pointer 0x4b18d8 -> buffer 0x4b31c0; joyMap 0x9ca85c; extra Esc 0x4b18d0 bit 2; pause 0x4b226c;
    no-input 0x4b3e48; transition 0x4b39bc; camera yaw / step 0x4b3e1a / 0x4b3e10, manual-step site 0x4323ce honours
    the lock bh & 0x80 of 0x4b3940. Jump states 7, 8, 9, 26, 72, 78.
14. Counters: builds 0x4b18fc, skips 0x4b18f8, skip flag 0x4b1d00, primitives 0x4e5e40, arena 0x4e5e44 / cursor
    0x4efb8c, sort count 0x604130 (capacity 2000), BZE base / size / limit 0x4b2364 / 0x4b38a4 / 0x4b372c.
15. Video menu: Apply slot 0x4ac8b8 -> 0x42e050, WriteConfig 0x409a10; W / H / fullscreen / renderer 0x9ca830 / 34 /
    38 / 24; pending values 0x4b3e60..0x4b3e70; renderer selection 0x4ac094; reinit 0x45f2b0.
16. Vortices: full-ring fits give outer +140..165° and inner -80..-97° a tick; others step ~24° / ~48° with ×6 skips.
    Lerping world vertices collapses a vortex to ~35 % radius; interpolate part rotations (bbtime: no collapse,
    `tools/re/motion_check.py` min rms ratio ≥ 0.997).
