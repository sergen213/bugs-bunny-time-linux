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
| §3.4 guard logic (template 265): notice in front within 2500 → 497 504 502 501 364 (throw) | 357 522 497 504 502 501 364 435 502 501 364 435 …; 364 @55, 401 spawned @71 (state 395, 3 ticks still), out and back along its TOD 0xa00 point (dy −33, +6, +5 …; dx 103, 110, 105 …), `+0x20` 0x2000 @128 at the hand, 435 @133, 502 @181, 501 @182, 364 @188; Bugs hurt 189/190/191 by the thrown item, health 6 → 5 (@83) → 4 (@221); S[0x7a] 0 → 1 @84. Second run with every rand() traced (tbrand): 521 497 504 501 364 (no 502), the first throw misses → 502 @+78, 501 @+79, 364 @+85, the second hits | `tbnpc/B_guard_attack.pkl.gz` (401 state = 5th record of its `+0x110` list, read from the pool's template entity); `tbrand/states.csv` | **Verified** (bbtime `game.tb`: 497 504 [502] 501 364, ticks relative to 364 +16/+73/+78, then the hit branch (B: +126/+127/+133, flight dy exact / dx ±1) or the miss branch (tbrand: +79/+85), a later hit, S[0x7a]); 521/522 and the 401 variant 394/395 are rand() picks |
| §3.5 melee hit / flash / death | guard 502 → 394 → 500; Bugs 208, health −1 per hit, flash `0x4d2058` 0x40 → 0x7c then 0x3c; Bugs at 0 health: f20 0x2000100, GD+0xa2 1, A4 4 (Taz), lives (GD+0xa0) 3; checkpoint `0x4d26e0` = (−2391, −52, −6293, 489) | `tbnpc/C_*`/`D_*` (kernel captures, saved B only), peeks | **Consistent** (bbtime test: melee 394 → Bugs 189, health 5, flash; death → partner; both down → lives 2) |
| §3.5 Taz spin into the facing guard (flagsA 0x1000000, contact damage 1) | Taz poked 173 in front of the guard on its 502 tick, □ held 40 ticks: guard 394 394, 433 ×17, 502, 394 ×24, then 500 ×56 (F) / 435 ×48 (F2), 502, 501 ×6, 364 ×78 (throw: Taz 190 again @120), 435 …; Taz 135 ×6, 136 ×12, 140 ×5, 190 ×13, 10 ×12, fidgets; guard health stays 5, Taz 6 → 5 → 4; neither raises +0x20 0x40/0x80; guard +0x54 = Taz from the first tick | `tbnpc/F_taz_spin.pkl.gz`, `tbnpc/F2_taz_spin_rng.pkl.gz` (RUNNER=wine; scripts `tbnpc/spin_capture.py`, `spin_rng_capture.py`: per tick guard/Taz entity 0x1b0 B, state ids, F2 also the rand() seed `0x4cf26c`; 2–5 rand() calls per tick) | **Verified** (bbtime `game.tb`, F2 seeds replayed per tick: guard through tick 176, Taz through tick 229 (fidgets 20/21/22 @48 and @146, the 190 @120); 500 vs 435 at tick 44 = block 404's rand() % 2) |
| rand() 0x46afe0 callers (one shared stream) | 17 call sites; from ARCA01 after ✕ through the guard setup above: 465 calls in ~115 ticks, all at 4 sites: `S` 7/8 `rand() % X` 0x430f76 (255; director and asset logic every frame step), chance `W` 0x45a6df (108; guard 364, Taz fidgets, asset logic), particle effect 0x452ab0 2 per particle 0x452cb5 + 0x452ce2 (51 + 51: V 0x200000 of the class-16 director with slot 13, count 1, flags 0x92d). Effect slots (0x4d6ae0, 20 × 0x9c) read back: slot 3 count 20 flags 0x92d, slot 13 count 1 flags 0x92d bound to an id-0 entity, others the 0x451e40 defaults 10 / 0x40 | `tbrand/rand.csv` (seq, tick ×2, return address, seed before), `states.csv`; scripts `rand_capture.py` + `rand_hook.s` (cave 0x480240, ring 0x63d600) | **Verified** (sites); bbtime now draws the asset logic on the shared stream, runs the director's block 0 (it was skipped as 'no block') and simulates the effect draws (S 118 slot fields 4/7/10, V 0x200000) |
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
same two-table stage (`0x639da4` = 1) but both tables (`0x639de0`, `0x63ae80`) are identity at runtime. bbtime Original
Fidelity now uses colour scale 1.0 + `Scene::pcTexGamma` (LiT 5/6, TB 1); Enhanced keeps the PS1 ×255/128 look.
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
