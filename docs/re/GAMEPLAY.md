# Gameplay RE — Lost in Time 1.6 (`bugs.exe`) first, Time Busters (`bblit2.exe`) deltas

Status: **Researched (static RE)**, except these LiT items, which are **Verified Against Original Behavior** by
runtime traces of `bugs.exe` under Wine (R; [RUNTIME_REFERENCE.md](RUNTIME_REFERENCE.md) §2–§3): §1.1 30 Hz tick
and the +2 frame counter; §3.3 jump arc (H39/step 70 apex 389 @ arc tick 15, lands 26; early-release apex 285 @ 8,
lands 18; cut to 0x800; descent in even steps); §5 parameters −200/600/1100 and the target-y `>> 2` lerp. Runtime
corrections are marked "(runtime: RUNTIME_REFERENCE.md §N)". Tags: *observed* (O) = read in the decompile
`ghidra/diff/out/{bugs,bblit2}.exe.c`, in exe bytes, or counted in game data by a probe; *observed-runtime* (R) =
read from the running original; *inferred* (I) = interpretation. Confidence H/M/L. Addresses are VAs (image base 0x400000);
LiT unless prefixed "TB". Conventions: Q12 (0x1000 = 1.0), angles 4.12 (0x1000 = 360°, `rsin` table `0x45f3f0`, `rcos`
`0x4633f0`, `atan2` = `0x4079e0` = round(atan2(a,b)·4096/2π)), integer world units, PS1 axes (+Y **down**),
**30 Hz fixed tick**. C `/` and `%` truncate toward zero; `>>` is arithmetic.

Companion documents: logic VM opcode tables and script-node layouts → [LOGIC.md](LOGIC.md); engine lineage and the
LiT↔TB function map → [ENGINE_COMPARISON.md](ENGINE_COMPARISON.md); container/model formats →
`docs/formats/{BZE,MODELS}.md`. BugsDecomp (no license) is used for names/addresses only; BBLIT-Remaster (MIT)
runtime-verified addresses are cited as "BBLIT".

Probes written for this document (python3 stdlib + `tools/re/bze.py`/`sec1.py`; each has `--selftest` or a
no-argument self-check):

| probe | purpose |
|---|---|
| `tools/re/ground.py` | LiT collision heightfield (chunk 0x36): decode zones, stats over all BZEs, OBJ export, world-floor agreement check (`--check`), TB header (`--tb`) |
| `tools/re/anim.py` | LiT TOD animation blocks (tag 0x25): `stats DIR`, `dump`, `pose LEVEL ACTOR_OFF ANIM FRAME OUT` |
| `tools/re/player_states.py` | dump a level's player state/move/transition tables |
| `tools/re/entities.py` | entity list per level with decoded logic items, collectible statistics (`--stats DIR`) |
| `tools/re/tb_players.py` | TB player entities (ids 2/3), state records, transitions, pad conditions |

---

## 1. Main loop and tick order

### 1.1 LiT per-tick function `0x4065e0` (called from WinMain `0x405950`) — O/H; tick rate and +2 counter R Verified (30.005 ticks/s; runtime: RUNTIME_REFERENCE.md §2)

| # | step | address / globals |
|---|---|---|
| 1 | frame counter `DAT_004b24a4 += 2`; `dt = DAT_004b2384 = counter − last` (= **2** per tick; used as a multiplier in speed formulas) | 0x4065e0 |
| 2 | **input**: message pump, pad emulation, GAME_DATA pad words (§2) | `0x4491c0` |
| 3 | **actor update** `0x447d40(liveList = DAT_004b3740)`: (a) camera update `0x4318f0(camTarget = DAT_004b39b8, P+0x1b4)` unless paused, (b) cheat-code scan `0x447b80`, (c) activation/culling pass building the active list `DAT_004b39ac`, (d) behaviour calls (§7.1) | 0x447d40 |
| 4 | SFX voices (24 channels `0x4b3260`, 20 B each) | `0x41edb0` |
| 5 | XA music/speech stream | `0x41e2b0` |
| 6 | autosave/save-request handling `0x407190` (only when no level change pending) — §8 | 0x407190 |
| 7 | pause/menu state machine (scratch `S[0x0c]` = `0x4b226c`: 0 run, 1 pause menu, 2 resuming, 3 other) | inline |
| 8 | if level change requested (`DAT_004b39bc == 1`) → §1.2; if `== 2` → reset pause/rumble state, `G[1] = 6`, `G[0] = 3`, no reload (*inferred* in-place restart; no static writer of 2 found) | inline |
| 9 | **render** unless skip flag `DAT_004b1d00`: `0x40ceb0(&eye 0x4b38c0)` (GsSetRefView2L), `0x40a1f0(activeList)` (pose: part world matrices), `0x44b370(0x4b2164)` (draws one actor's parts; `0x4b2164` is also passed to the pause code `0x449d60` → *inferred* HUD/pause overlay actor), `0x44c4b0(activeList)` (world cells via `0x44c270→0x40fe90` + actors via `0x411b10`), `0x44bb50(&0x4b21a0)` (shadows/particles), `0x44cfa0(0x4b373c)` (2-D sprites); skipped frame: `0x40a530` + `0x44bef0` only | inline |
| 10 | present (`WM_PAINT` → `0x4124b0` GL flush) and frame pacer `0x406ee0` (30 Hz, ENGINE_COMPARISON.md) | |

Consequences (O): **the camera is updated before the player** in the same tick (it follows the player position of
the previous tick), activation uses the new camera eye/target, the player behaviour runs first among actors (§7.1),
and rendering uses the camera computed at step 3a.

### 1.2 Level load and transitions — O/H

* Request: any of action A42 (`GD.currentLevel = G[0x0e]`), script item flag 0x80000 (`currentLevel = P1`), zone
  trigger flag 0x80000, cheat 6 (level 0x47 credits) sets `DAT_004b39bc = 1`.
* Handling in 0x4065e0: reset pause/rumble state, fade-out (`0x44bec0(2,0x100)`), keep rendering until the fade ends, then
  `0x430e60(newId)`: free all actors (`0x4474b0`), look up the level table (`0x4ad380`, 16-B entries, `+4` s16 =
  loading-screen level id); if one exists, load the loading-screen BZE first (input disabled `DAT_004b3e48 = 4`); for
  level 3 (`ls01`, time-period hub) the loading screen depends on world `G[0x25]`: 1→0x68 `l_saload`, 2→0x5f
  `l_mload`, 3→0x64 `l_pload`, 4→0x54 `l_30load`, 5→0x59 `l_fload`, else 1 `loading`.
* The tick loop keeps running while the loading screen shows; for target level ids {2, 6, 7, 0x28, 0x2c, 0x31, 0x32,
  0x34–0x36, 0x39–0x43, 0x45, 0x46} (mostly `cc*` cut-scenes) a **minimum 3000 ms** (`GetTickCount`) is enforced.
* Then `0x430f80(id)`: BZE load `0x42e7a0(id, psxRam+0x200000)`, collision init `0x434c30(DAT_004b2490)`, counter
  reset `DAT_004b24a4 = 1`, then one load-time actor update `0x447d40` (initialises behaviours, camera reset
  `0x431160` via the player), audio restart, fade-in `0x44be90(1,0x100)`, two unrendered ticks (loop `iVar1 < 2`
  at 0x406a3a.. calling `0x4065e0` + `0x40a530`), then normal rendered play.
* L01A start (R; runtime: RUNTIME_REFERENCE.md §3, `csv/lit_spawn.csv`, `csv/litworld_L01A_idle.csv`): player state
  2 at counter 3, **scripted walk-in** state 130 from counter 5 (+25 z/tick, z 8830 → 9655), state 1 at counter 71.
  Camera flags `0x4b3940` = 0x440 during the walk-in, 0x40 for 10 ticks, then 0 (§5).

### 1.3 TB tick `0x4011c0` — O/H

input `0x4615b0` (keyboard `0x40e0c0` + joystick `0x40e710` → PSX buffers `0x4d2380` P1 / `0x4d23a2` P2 → GAME_DATA
pad records) → `0x401960` (`DAT_004d2708 += 2`) → SFX `0x42ae60` → speech `0x42a030` → music `0x407460/0x4291e0` → per
view (`DAT_004d2794`, always 1): `0x45ee90` (per-tick actor resets), `0x45eee0` (camera `0x439700→0x437800` follow /
`0x4373c0` first person, activation, draw list, **player identity/switch/call-friend**), `0x460630` (behaviour dispatch)
→ `0x4681c0` deferred actor pass → render `0x4013b0` → pause/level reload `0x4014c0` (`DAT_004d593a == 1` ⇔ LiT
`DAT_004b39bc`; demo idle timer 901 ticks) → pacer `0x4156c0`.

**Reimplementation notes.** One `tick()` at exactly 30 Hz: `pad → camera(prevPlayerPos) → cheats → activation →
behaviours (player first) → sfx/music → save requests → level-change state machine → render`. Keep `dt = 2`
as a literal multiplier where the original uses `DAT_004b2384`. Level change = free actors → optional loading-screen
level (min 3 s for the listed ids) → load target → run one update with rendering suppressed → fade in.

---

## 2. Input

### 2.1 Pad emulation (LiT) — O/H (BugsDecomp `pad.c` facts + decompile)

Sources are mapped onto a **PS1 pad report** at `0x4b31c0` (pointer `0x4b18d8`): byte0 0, byte1 type
(`0x41` digital / `0x73` analog), byte2 = `~(Select, L3, R3, Start, Up, Right, Down, Left)` (bit0..7), byte3 =
`~(L2, R2, L1, R1, △, ○, ✕, □)`, bytes 4..7 analog RX, RY, LX, LY (0..255, 0x80 neutral; the PC joystick value is
copied to both sticks). Order in `0x4491c0`: `MapJoystickInput 0x403010` first, then `MapKeyboardInput 0x4023e0`
(normal run state `0x45f2b0 == 1`) or `MapAltKeyboardInput 0x402290` (other window states: fixed arrows/Tab/Enter),
**OR-ing** keys on top.

Default `config.pc` keyboard map (`config+0x46`, 16 DirectInput scancodes; shipped values in RESEARCH.md §6):

| kbdMap slot | default key | PSX bit | logical use (preset 0) |
|---|---|---|---|
| 0 / 1 / 2 / 3 | ↑ ↓ → ← | Up/Down/Right/Left | move |
| 4 | Q | L2 | camera rotate (+0x26/tick) |
| 5 | W | R2 | camera rotate (−0x26/tick) |
| 6 | E | L1 | roll |
| 7 | R | R1 | action: pick up / push / dig / put down |
| 8 | A | △ | look (first person) |
| 9 | S | ○ | crouch / sneak / helicopter glide |
| 10 | D | ✕ | jump |
| 11 | F | □ | kick / attack |
| 14 / 15 | Tab / Enter | Select / Start | |

Extra keys (`0x4b18d0`): bit0 Return, bit1 Esc (cond C96), bit2 Space. Joystick map `config+0x3c` (10 button
indices: look, crouch, jump, kick, roll, action, camL, camR, ?, ?).

### 2.2 GAME_DATA pad words (`GD = *(0x52fd00) + 0x10000`) — O/H

| GD offset | meaning |
|---|---|
| +0x10 | raw buttons: low byte = `~report[3]`, high byte = `~report[2]` (physical bits) |
| +0x0c | **held, logical**: high byte raw (dirs/start/select); low byte remapped: physical △→word GD+0x32, ✕→+0x34, □→+0x2e, ○→+0x30, L1→+0x36, R1→+0x3a, L2→+0x38, R2→+0x3c (8 remap words; preset table `0x4ae1f0`, 8×u16 per preset, index GD+0x51 = `G[0x11]`, written by `0x448fa0`; presets 0 identity, 1 `(□,○,△,✕,L1,L2,R1,R2)→(0x80,4,0x20,0x40,8,1,0x10,2)`, 2 `(4,8,0x10,0x80,0x20,1,0x40,2)`, 3 `(0x20,8,0x80,0x10,1,2,4,0x40)`) |
| +0x0e | previous +0x0c masked by the repeat mask `DAT_004b3228` |
| +0x3e = +0x0a | **"pressed" word** used by player transitions and C49/C81: `+0x0c & ~+0x0e` |
| +0x12 / +0x14 | stick X (right +) / Y (up +): analog `raw − centre` outside dead zone, 0 inside; digital ±0x7f / −0x80 from the D-pad |
| +0x1a / +0x20 | analog centre X / Y (new game 0x7f), +0x16/+0x18 and +0x1c/+0x1e min/max (0/0xff) |
| +0xe7 bit7 | analog pad present (`G[0xa7]`) |

Repeat mask (O): logical ✕ (0x40), □ (0x80) and L1 (4) have counters `0x4b39b2`, `0x4b238a`, `0x4b28cc`, reset to 2
while released; while held they count down, and at 0 the bit enters `DAT_004b3228`. Net effect: **✕, □, L1 stay in
the pressed word for the first 3 ticks of a press, then drop out** (an edge with a 3-tick buffer); all other buttons
are level-triggered. Dead zone `DAT_004b3234 = 0x32` (50), set in `0x4474b0`. With an analog pad and no D-pad input
the stick also sets D-pad bits (beyond the dead zone).

Other input paths (O): **demo playback** when `G[0x0f] ≠ 0` (set by chunk 0x47 tag 0x48, stream at `DAT_004b249c`,
4 u16 per tick = {+0x3e, +0x0e, +0x0c, +0x10}; a real Start press is turned into Select 0x100, *inferred* demo skip); `S[0x17]` (`0x4b2277`)
∉ {0, 4} zeroes all words; input recording to a buffer (`/r:` switch, `DAT_004b1d04`); pad absent for 4 ticks → Start
injected (PS1 "controller removed" pause); no pad processing while `DAT_004b3e48 ≠ 0` (loading) or without a player;
all words zero while a level change is pending or the player is scripted (flags14 & 0x1000000) and not paused;
frozen player (flags14 0x80000000 or camera flags 0x100) keeps only Start (player code). Rumble emulation `0x4b3240..46` → `PadSetAct` (no-op on PC).

Cheat codes `0x447b80` (O/H): while raw △+○ are held, enter 8 more raw buttons (table `0x4ae130`, 7 × 12 u16
`{id, progress, seq[≤10], 0}`); all start `□ R1 ✕ L1`:

| id | rest of sequence | effect |
|---|---|---|
| 0 | □ R1 □ L1 | `G[0x24] |= 1` (I: unlock) |
| 1 | □ R1 ✕ L1 | carrots `G[3] = 99` |
| 2 | ✕ R1 ✕ L1 | `G[8] |= 0x1f` |
| 3 | ✕ R1 □ L1 | health `G[1] = 6` |
| 4 | □ ✕ R1 L1 | `G[8] |= 0x80` |
| 5 | ✕ □ R1 L1 | golden carrots = 333 (`G[7]=0x4d, G[0xfc]=1`), clocks `G[5] = 124`, **falls through** to 6 |
| 6 | R1 ✕ □ L1 | goto level 0x47 (credits) |

Each sets bit `id` in `G[0x24]`.

### 2.3 TB input — O/H

`config.pc` is 0xF8 B (version 0x14) at `0x639b40`, defaults `0x415fd0`. Key slots are u32 DirectInput indices
(≥ 0x100 → extended table). **Two players**, slot order up, down, right, left, L1, L2, R1, R2, △, ○, ✕, □, select,
start, View:

| PSX bit | P1 slot / default | P2 slot / default | README meaning (Bugs / Taz) |
|---|---|---|---|
| Up/Down/Right/Left | +0x4C/+0x50/+0x54/+0x58 arrows | +0x88/+0x8C/+0x90/+0x94 Num8/5/6/4 | move |
| L1 0x04 | +0x5C A | +0x98 L | **Call Friend** |
| L2 0x01 / R2 0x02 | +0x60 D / +0x68 F | +0x9C ; / +0xA4 ' | rotate camera |
| R1 0x08 | +0x64 S | +0xA0 K | **Switch Camera / character** |
| △ 0x10 | +0x6C W | +0xA8 O | Action |
| ○ 0x20 | +0x70 Q | +0xAC I | Ears-Sneak / **Pull-a-face** |
| ✕ 0x40 | +0x74 E | +0xB0 P | Jump |
| □ 0x80 | +0x78 R | +0xB4 [ | Kick / **Spin** |
| Select / Start | +0x7C Tab / +0x80 Esc | +0xB8 RCtrl / +0xBC RShift | |
| View | +0x84 unset | +0xC0 unset | toggle that holds L2+R2 = first-person view |

Pad records: GD+0x08 (P1) and GD+0x30 (P2), 0x28 B each: +0 pressed (`held & ~(prev & edgeMask)`, edge mask per
level from chunk 0x3a tag 0x3c), +2 held (remapped), +6 raw, +0xC/+0xD s8 stick (dead zone 0x4D..0xB1, scale 128/78),
+0x18 8-word remap (presets `0x4ceb14`, preset byte GD+0x95/+0x96). The P2 report is read only in 2-player mode
(GD+0xA4 & 0x10); the joystick drives P1 only and can never press Select/Start; P2's View latch only toggles on a P1
View edge (bug, O). Discrepancy (O): README movement keys (G/B/V/N; Taz K/L swapped) differ from the exe defaults.

**Reimplementation notes.** Build the 8-byte PS1 report from SDL keyboard/gamepad, then run the GD word logic
verbatim: remap via preset words, 3-tick press window for ✕/□/L1, dead zone 50, stick → D-pad bits, digital stick
±0x7f/−0x80. Gameplay reads only GD+0x0a/+0x0c/+0x10/+0x12/+0x14. TB: two 0x28-B records, focus-dependent routing (§9).

---

## 3. Player controller (LiT behaviour type 1 `0x43a880`)

### 3.1 Architecture — O/H

**The state machine is level data.** `0x43a880` has no per-state switch; each level's player entity (tag 0x13 = 1)
ships three tables, loaded into per-actor lists by `0x42e7a0`:

| list | tag (payload) | node | role and layout (payload offset → meaning) |
|---|---|---|---|
| +0xf4 | 0x32 (12 B) | 0x10 | **state**: `@0 u16 id`, `@2 5×s16 move ids` (−16 = end) |
| +0xf8 | 0x34 (24 B) | 0x1c | **move**: `@0 id`, `@2 anim id`, `@4 s16 anim mode` (1 loop, 2 once, 0x802 once+hold), `@8 u32 flagsA`, `@12 u32 flagsB`, `@16 u16 link` (transition group), `@18 s16 speed`, `@20 s16 jumpH`, `@22 s16 jumpStep` |
| +0xfc | 0x35 (40 B) | 0x2c | **transition rule**: `@0 group`, `@2 buttonsAll`, `@4 buttonsAny`, `@6 heldCmd`, `@8 s16 nextState` (−13 = stay), `@10..17 logic item {cond,A,B,act,X,Y}`, `@20 invert bits`, `@24 heldMask`, `@28 parentMask`, `@32 flags14Mask`, `@36 flags18Mask` |

State ids are global (same meaning in all levels), but each of the 79 LiT player chunks carries its own subset and
variants (25 variants of state 1) → **load per level**. Runtime: P+0x17c state id, +0x17e index in the move sequence,
+0x180 state node, +0x184 move node, +0x188 first rule of the move's group, +0x158 current anim id, +0x144 anim state.

Per tick (O/H):
1. Resolve state/move; move id −16 or index > 4 → state 1 index 0 (clears hurt flags 0x2000020, drops rope).
2. Physics from move flagsA/flagsB (§3.2–3.4).
3. **Transitions**: rules of group `move.link` in file order; the **first** rule passing all tests fires: `heldMask`
   (vs held actor `P+0x6c` +8), `parentMask` (vs support `P+0x70` +8), `flags14Mask`, `flags18Mask` (invert bits),
   `buttonsAny` on GD+0x0a, then `(GD+0x0a & all) == all` **or `all == 0xfff1`** (authoring value for "no button",
   6183/9436 rules), then TestLogicCond → RunLogicAction. nextState −13 = stay. On a change: index 0; **states 5 and
   0x12 while falling (flags14 0x200) are redirected to 7** (coyote jump); move flagsB 0x800 clears 0x209.
   heldCmd drives a held actor (0xd3 = free it).
4. Animation: `0x4393e0(P, P+0x144, move.anim, move.mode, work)` when the anim changes, else `(…, 0, 0, …)`;
   return 0x100 (clip ended) → index += 1 (state 1 first sets index = `rand() & 3` → random idle fidget); flagsA 0x80
   also advances on landing.

### 3.2 Steering and ground speed — O/H

* `mag = round(sqrt(sx² + sy²))` from GD+0x12/+0x14; `p = min((mag − 50)·2, 0x80)`.
* flagsB 0x4000000: sequence index = `(p > 0x77)` → **run at p ≥ 0x78 (mag ≥ 110)**, seq[0] walk / seq[1] run.
* flagsB 0x8000000: animation pace `acc(0x4b3bc0) += p` (×0x1111>>12 = 1.0667 when walking), `P+0x149 = 5 − acc/32`,
  `acc %= 32` (frame-skip byte for the anim player).
* `v = max((speed · p · dt) >> 7, 1)`, dt = 2 → **walk 40, run 50 units/tick** (speeds 20/25 in data).
* Target yaw `t = atan2(sx, sy) + camYaw` (`camYaw = atan2(target.x − eye.x, target.z − eye.z)`, refreshed only
  while grounded, flags14 & 0x209 == 0; flagsA 0x8000 = absolute steering); `Δ = wrap(t − yaw)`;
  **forward = v·(0x800 − |Δ|) >> 11**; lean `+0xe0 = Δ >> 3`; turn `step = Δ >> 2` per tick (exponential, ¼ of
  the remaining error, final snap when step == 0).
* flagsA 0x100000: constant forward `speed · 2`.
* World velocity `w = RotMatrix(+0xdc) · s` (local s = (x, y, z-forward), `0x407a50`+`0x408a60`).
* **Acceleration** (when flags14 & 0x201 == 0): `vs += ((target − vs) · k) >> 12`, `k = DAT_004b3b90` = 0x800 (½ the
  error per tick; ramps back +0x10/tick after a freeze); ice (slide surface or standing on an actor with +0xc
  0x800000): vs moves 1 unit/tick toward target; slope/momentum term `0x4b3c30/38` decays ×0.75 per tick
  (`c −= c >> 2`); **in the air: no smoothing** (w = target) and horizontal momentum is copied into the decay term,
  carrying into the landing.
* Slide surfaces (positive collision bytes, §4): flags14 0x10000000; four ground probes at ±0xa0 (160) in X and Z,
  valid when −0x65 < h − y < 200: `slope += (Δh · 3000) >> 12` (gain 0.732), low-passed by `>> 4`.

### 3.3 Jump, fall, gravity — O/H (`0x43a250`, `0x436de0`)

* Jump start (move flagsA 0x10, flags14 & 0x209 == 0 or swimming): `+0x104 = H (jumpH)`, `+0x106 = step`,
  `+0x108 = 0x400` (phase), `flags14 = (flags14 & 0xeffff9f1) | 1`. Bouncy support (support +8 & 0x8000, or a type-14
  whose move flagsB 0x10000000): step = support `+0x196`/`+0x1e` or `step·0xa28 >> 12` (×0.635 → higher jump).
* Arc tick (flags14 bit 1): rising `vy = −(sin(ph)·H) >> 12`; descending (ph ≥ 0x800, flag 8) `vy = (−(sin(ph)·H)
  >> 11) & ~1` (**twice as fast down**); `ph += step`; ph > 0xc00 → arc ends (free fall flag 0x200 if airborne).
* **Variable height**: when ✕ is no longer held (`GD+0x0c & 0x40 == 0`), not descending, 0x600 < ph < 0x800 and
  move flagsB lacks 0x40000000 → `ph = 0x800` (apex now).
* Free fall (floor > 200 below, not in an arc): `vy = 70 + 70·cos(ph)` (`70 − ((sin(ph+0x400)·−70) >> 12)`), ph
  from 0x800 (landing value), `+= 0x44`/tick, capped 0xc00 → per-tick vy 0,1,2,4,7,10,14,18,24,29,35,42,49,56,63,
  **70 terminal** (tick 16+); glide (flags14 0x10000) vy = **12**.
* Grounded vertical snapping: dy > 1 → `dy >>= 1`, −70 < dy < 0 → `dy >>= 1` (exponential step smoothing); airborne
  snap clamped to ±70. Landing: phase 0x800, flags14 |= 2, clear 0x200|0x10000.
* **No fall damage** exists (no distance tracking); pits kill through zone triggers (§7.5).
* Simulated arcs (exact integer math, flat ground): H39/step70 (standard) apex −389 at tick 15, lands tick 26; early
  ✕ release apex −285 tick 8, lands tick 18 (both **Verified**: same ticks and heights, descent steps 2, 10, 18, …
  even, release → phase 0x800 + step; runtime: RUNTIME_REFERENCE.md §3, `csv/lit_jump_hold.csv`); bounce H50/70
  −495 @15; high jump H50/32 −1059 @32 (lands 55); dig hop H50/100 −354 @11; run-attack hop H10/128 −59 @8; water
  jump H80/60.

### 3.4 Player flags — O (names I unless obvious), H

flags14 = `P+0x14`:

| bit | meaning |
|---|---|
| 0x1 | jump arc active |
| 0x2 | on ground |
| 0x4 | attached to parent frame (rope/platform/carried) — no gravity |
| 0x8 | arc descending |
| 0x10 | standing on platform actor `P+0x70` |
| 0x40 | attack active (move flagsA & 0x810000) |
| 0x200 | free fall (ground > 200 below or arc ended airborne) |
| 0x400 | floating on buoyant actor (swim) |
| 0x4000 / 0x20000 / 0x40000 | pushing / blocked by pushable / touching pickable (cleared each tick) |
| 0x10000 | gliding (move flagsA 0x2000) → fall speed 12 |
| 0x100000 | blocked by a wall this tick |
| 0x1000000 | dead / scripted, no control (`0x437b90`) |
| 0x2000000 (+0x20) | hurt |
| 0x10000000 | on slippery ground |
| 0x80000000 | frozen (A86, camera hold); cleared at end of tick |

flags18 = `P+0x18`: 0x1 dig allowed (A47, re-set by level logic each tick), 0x2 (move flagsB 0x1000; C51), 0x4
underground (flagsB 0x2000; C52), 0x40 bounced off a stompable, 0x200 walked off a ledge this tick, 0x100 from
`0x4b3748 & 2`.

Move flags (O/H for tested bits): flagsA 0x2 stick steering, 0x10 start jump arc, 0x20 pick up, 0x40 throw, 0x80
advance on landing, 0x100 grab rope while airborne (`0x43a310`), 0x200 climbing, 0x400/0x800 stick → vertical (climb,
0x400 = up), 0x2000 glide, **0x4000 vulnerable** (only then can enemies damage the player), 0x8000 absolute steering,
0x10000 hand-point attack hitbox (`0x433380` at P+0x58, radius 0x80), 0x100000 constant forward, 0x800000 body
contact attack (`0x432fe0` radius 0x80), 0x4000000 may push objects. flagsB 0x10 face camera, 0x800 clear 0x209 on
entry, 0x1000/0x2000 → flags18 0x2/0x4, 0x40000 put down held, 0x4000000 walk/run split, 0x8000000 anim pace from
stick, 0x40000000 disable variable-height cut, 0x80000000 position from parent part chain.

### 3.5 State table (ids global; L01a data; names I/M)

| id | name | moves (anim) | entry → exits (pad = GD+0x0a) | physics |
|---|---|---|---|---|
| 1 | idle (fidgets) | 4, 70..73 | default / sequence end → R1+touching pickable 41, ○ 61, L1 86, ○+dir 46, ✕ 7, □ 21, dir 5, falling 14 | none |
| 2 | level-start idle | 4 | first tick | as 1 |
| 5 | walk/run | 34 (anim 11, spd 20) / 6 (13, spd 25) | dir → R1+pickable 41, L1+dir 86, □+dir 21, R1 & blocked-by-pushable 24, ○+dir 46, ✕+dir 9, run+□ 90, no dir & running > 30 ticks 56, else 1 | steer, walk/run split, pace |
| 56 | run stop / skid | 93, 94 | from 5 | const fwd 10 |
| 7 / 9 | jump standing / moving | 10 (anim 15, H39/70) / 12; then 17 fall, 8 land | ✕ (/✕+dir) → descending&bounce 91, R1 descending 27, ○ descending 30 | arc, air 32 u/t, rope grab |
| 91 | bounce jump | 100 (H50/70) | flags18 0x40 | fixed arc |
| 98 | high/spring jump | 108 (H50/32) | other levels | |
| 14 | fall | 17 | flags14 0x200 → bounce 91, ○ 30, float 25, rope 31 | fall, air 32 |
| 30 | helicopter glide | 37 (spd 1) | ○ while descending/falling; hold ○ | vy 12, sound 0xd |
| 27 | dig-dive (R1 in air) | 28 (H50/100), 29, 31 | R1 descending → landed & dig allowed 38 | hop |
| 38 / 124 / 125 / 127 / 128 | underground (enter/move/idle/turn) | 30, 136–140 | ✕ & dig allowed → 7 | 24 u/t, sound 0x1b |
| 61 / 62 / 87 | crouch / rise / crouch→sneak | 66–68 | ○ | none |
| 46 | sneak | 47 (spd 6) | ○+dir | 12 u/t |
| 21 | attack (□ kick) | 20 (anim 365, flagsA 0x14001) | □ | hand hitbox |
| 90 | run attack / lunge | 99 (H10/128) | □ while running | arc + fwd 20 |
| 86 / 81 | roll (L1) / roll end | 58, 69 / 59 | L1 | fwd 30 + steer |
| 24 | push | 23 (spd 6) | R1 & flags14 0x20000 | pushes block |
| 41 / 42 | pick up / fail | 51–53 | R1 & 0x40000 | |
| 40 / 6 / 8 / 45 / 43 / 44 | light object: hold idle / walk / jump / fall / throw (□) / put down (R1) | | | |
| 74–83 | heavy object family | 80–88 | | spd 10 |
| 68–72 | carry family for held flags 2 (L02 levels) | | | |
| 25 / 26 | float-swim / water jump | 27, 24 / 25, 26 (H80/60) | flags14 0x400 | buoyancy |
| 31 / 32 / 33 / 39 | rope hang / climb up / down / let go | 38, 40, 39, 50 | rope grab (`0x43a310`) | climb ≤ 31 u/t |
| 88 | run on ice | 97 | flags14 0x10000000 | ice accel |
| 10 / 15 / 103 | hurt knockback | 13 (fwd −10), 113, 114 | forced by enemy (`0x4377a0`) or A26 | backwards |
| 94 / 95 | face-camera poses (95 = clock celebrate, I) | 103 / 104 | logic | |
| ≥ 105, 129–131, … | scripted / cut-scene / death | | A26, zone triggers, `0x437b90` | |

### 3.6 Health, damage, death — O/H

Health `G[1]` (half-carrots), max `G[2]` = 6. Damage paths: `0x437d90(attacker, step)` (enemy contact; only when the
player's current move has flagsA 0x4000, no invulnerability, no level change, `S[3] == 0`, not scripted; damage 1, or
the attacker step `+0x16` clamped to 2, projectiles their own `+0x1e`), A72 (`G[1] -= X`). Both set `P+0x38 = dmg`,
flags14 0x2000000 and **invulnerability `0x4b3160 = 0x1e` (30 ticks)**; the caller then forces the knock-back state
`step+0x1c`. Death (`G[1] < 1`, HUD `0x44acc0`; or A35; or zone trigger 0x200000): `0x437b90(deathState =
0x4b39d8 (A92) ?: 0x4b2370 (zone trigger 0x400000), pos)`, flags14 = 0x1000004; near the clip end fade
`0x44bec0(2,0x200)`; at the end the player respawns at the **checkpoint `0x4b2460..68` / yaw `0x4b246c`** (set by
zone trigger or item flag 0x100000), health 6, invulnerability 30, camera snap, fade-in.

### 3.7 Constants (per tick at 30 Hz) — O/H

| what | value | where |
|---|---|---|
| dt multiplier | 2 | `0x4b2384` |
| stick dead zone / strength | 50 / `p = (mag−50)·2 ≤ 128` | `0x4b3234`, 0x43a880 |
| run threshold | p ≥ 0x78 (mag ≥ 110) | |
| walk / run | 40 / 50 u/tick (1200 / 1500 u/s) | speeds 20/25 × p·2>>7 |
| sneak / push / swim | 12 u/tick | speed 6 |
| air control | 32 u/tick, instant | speed 16 |
| glide | 2 u/tick horizontal, 12 u/tick fall | |
| roll | 30 + steer ≤ 30 | flagsA 0x100000 |
| accel | ½ error/tick (k = 0x800, +0x10/tick recovery); ice 1 u/tick² | `0x4b3b90` |
| momentum decay | ×0.75/tick | |
| turn | Δ>>2 per tick; speed ×(0x800−\|Δ\|)/0x800; lean Δ>>3 | |
| jump | H 39, step 70; phase 0x400→0xc00; cut window 0x600–0x800 | data / 0x43a250 |
| fall | phase +0x44/tick from 0x800; terminal 70 u/tick | 0x436de0 |
| ledge fall threshold | floor > 200 below | 0x436de0 |
| step-up limit | 100 above current y | 0x434cd0 (§4) |
| platform boarding | dy > −100 | 0x43a880 |
| default player box | +0x48 −70, +0x4c −40, +0x50 70, +0x54 40, radius 80 (r² 6400) | 0x43a880 |
| invulnerability | 30 ticks | `0x4b3160` |
| camera snap at spawn | target = pos − (0,400,0); eye 100 behind | 0x4373f0 |

### 3.8 TB player — O/M

TB slot 1 (LiT's player) is a no-op; **Bugs and Taz are behaviour type 14 `0x453de0`** (generic character shared
with NPCs, 6263 instructions; type byte actor+0x31, table `0x4cea60`, 41 entries, dispatch `0x460630`). Same
algorithms recognisable (run threshold `> 0x77`, pace ×0x1111>>12, `5 − acc>>5`, speed `(spd·dt·mag)>>7`, buoyancy
constants). Deltas: stick magnitude smoothed `m −= (m − mag) >> 3` (1/8 per tick) with no dead-zone subtraction here;
speed may be indirect (`spd & 0x6000` → table `0x4d24a0`); moves are tag-**0x2F** state records (48 B) + tag-0x31
event transitions (dispatcher `0x44d990`) + logic pad conditions ('X' = VM opcode 0x0E), not the LiT 0x35 rule table.
TB gravity/jump constants are not decoded (arc fields actor+0x194..0x19a).

**Reimplementation notes (player).** Load tags 0x32/0x34/0x35 of the player entity per level; implement one
interpreter: pad words (§2) → steering (`p`, run split, `v = spd·p·2>>7`, yaw += Δ>>2, speed scale (0x800−|Δ|)/0x800)
→ constant-forward → rotate → accel (½ error, ice ±1, air instant + ×0.75 carry) → jump arc (`−sin·H>>12` up,
`>>11` down, cut to 0x800 on ✕ release in 0x600–0x800) or fall (`70+70·cos`, +0x44, cap 70, glide 12) → collision
(§4: X-only, Z-only, full move, then solid actors) → transitions (first match, 0xfff1 = no buttons, −13 = stay,
coyote redirect 5/0x12→7) → animation (advance on clip end / landing). Use 30-tick invulnerability and the move
vulnerability bit 0x4000. Exact integer math (Q12 tables, truncating shifts) is required for bit-identical arcs.

---

## 4. Collision (LiT)

### 4.1 What geometry is used — O/H

**No polygon collision.** The world collision is a separate **heightfield block**: section-1 chunk **0x36 tag 0x37**
`{u32 offset, u32 size}` into decompressed section 4 (→ `DAT_004b2490`, init `0x434c30`). World-model cells, hulls
(MODELS.md §7 type 0x08) and portals are **render-only**; no collision function reads section-4 model data. Dynamic
collision uses per-actor oriented boxes. Chunk 0x22 tags 0x3f/0x40 and the 0x25 blocks are animation/sprite data
(§6), not collision. 79 of 167 LiT BZEs carry chunk 0x36 (all levels with world geometry).

Probe validation (`ground.py --check`, vertical rays vs chunk-0x20 triangles): L01a |dy| median 1.0, 98.9 % within
16 units; L04C1 median 0.0; LB02 (slide level) median 9.6, 89 % within 40 → the formula below is confirmed (O, H).

### 4.2 Block format — O/H

```
u32 count
ZoneRecord[count]      0x44 B each; records with equal id are contiguous
u16 cellmap[…]         one per zone (rec+0x1c)
s8  tiles[n][8][8]     ONE tile pool shared by all zones (every rec+0x20 identical)
```

| off | type | field |
|---|---|---|
| 0x00 | u16 | zone id (< 0x3c, trap otherwise); = world chunk 0x20 tag 0x21 id (`DAT_004b37a0[id]` world model of that zone) |
| 0x02 | u16 | height scale `hscale` (16: 348 zones, 20: 127, 10: 4) |
| 0x04 | u16 | `cols` (cell-map stride) |
| 0x06 | u16 | rows (unused by code) |
| 0x08 / 0x0a | u16 | size_x / size_z (= cols·320 / rows·320) |
| 0x0c / 0x10 | s32 | min_x / min_z (multiples of 320, ≥ 0) |
| 0x14 | s32 | `y_bottom` floor base (inclusive) |
| 0x18 | s32 | `y_top` ceiling (exclusive) |
| 0x1c / 0x20 | u32 | cell-map / tile-pool offsets (block-relative; relocated at init) |
| 0x24..0x43 | | redundant bbox, no reader (ignore) |

Cell word: `tile = w & 0xfff`, `flags = w & 0xf000` (only 0 or 0x2000 in data; no LiT reader). Sub-cell byte
`b = tiles[tile*64 + (lz%320/40)*8 + (lx%320/40)]`:

| byte | meaning |
|---|---|
| ≤ 0 | floor, `ground_y = y_bottom − |b|·hscale` |
| 1..125 | floor (same formula) with **slide** flag (result kind \| 0x20) |
| 0x7e | open: no floor in this zone here → re-query at `y_bottom + 1` (zone below) or fall |
| 0x7f | wall |

Heights are **piecewise constant per 40×40 sub-cell** (no interpolation). Cells are 320×320.

### 4.3 Queries — O/H

* **FindZone `0x436b60(pos, hint)`**: `inside(r) = r.y_top < y ≤ r.y_bottom && min ≤ x,z < min + size`; scan the
  hinted id's group first (`DAT_004b3c60[hint]`), then every record in file order; NULL if none. Hint = actor+0x26.
* **GroundAt `0x436c20(pos, res, work)`**: zone → cell → byte → result (kind 1 no zone, 2 floor (\|0x20 slide),
  6 wall, 8 open); writes absolute ground y to res+0x10 and overwrites pos.y with y_bottom.
* **TraceGround `0x434cd0(pos, delta, res, work)`** (XZ move): walks **every 40-unit sub-cell** the segment touches
  (X-only, Z-only, or diagonal DDA by nearest boundary); blocks when the byte is 0x7f (except the start sub-cell),
  or the ground there is **more than 100 above the start y** (`h(b)·hscale + y_bottom − y0 < −100`), or the path
  leaves all zones. Blocked → delta clamped to the point where the path **entered the last passable sub-cell**
  (snap back up to 40 units, not slid to the wall); kinds 6/0xc (single axis) or 0x16/0x1c (diagonal, bit 0x10).
  Result short[] (byte offsets): +2 kind, +4 hoff previous, +6 hoff final (→ actor+0x10c), +8 cell flags, +0xa zone id
  in/out, +0xc ceiling y, +0x10 ground y (GroundAt), +0x14 zone record ptr. Kind bits: 1 no zone, 2 floor, 4 blocked,
  8 open, 0x10 diagonal-blocked, 0x20 slide.
* **MoveActor `0x437390(actor, delta, res, work)`** = `0x436ff0` (trace; diagonal block → retry X-only then Z-only =
  wall slide, zero the blocked axis speed `+0x110/+0x112`, flag 0x100000; open → floor of the zone below) →
  `0x4371f0` (vertical target: no zone → dy = 200; jump rising clamps the head at the zone ceiling `y_top − box_top`
  unless the zone above is open there) → `0x436de0` (smoothing/fall/landing as §3.3). Ground y of an actor =
  `+0x100 + +0x10c`.
* **Slopes**: no slope-angle limit; walkability = 100-unit step rule + walls; slide bytes push downhill (§3.2).
* End of the player tick: GroundAt at the new position; inside a wall (kind & 4) → restore the last safe position
  `DAT_004b3780..88`.

### 4.4 Actor-vs-actor — O/H (M for push geometry)

Actor box: `+0x48/+0x4a/+0x4c` = (minX, **maxY**, minZ) (tag 0x1b), `+0x50/+0x52/+0x54` = (maxX, **minY = top**, maxZ)
(tags 0x1c/0x42; +Y down so +0x52 is the top), inflated by s16 `+0x20` (tag 0x38), radius `+0x2e`, `+0x30` = r² (XZ
pre-filter only); TOD packet type 9 rewrites the box every animation frame (§6). Collision class `+0x2a` (tag 0x15)
**== 2** = member of the collision list; class bits `+8` (tag 0x16) tested against caller masks. Decisive test =
point/box in the target's local frame (RotMatrixZYX(0x1000 − rot) `0x407b90` + ApplyMatrix `0x4081c0`), then AABB.

| fn | role |
|---|---|
| `0x433500(actor, delta, work, mask)` | push the move out of other solids' boxes; returns the hit actor; `delta.y` = top-of-box height; masks: player `0x9a690a`, pushable `0x2690a`, carry `0x20100`; target +8 bit 0x2000000 = sensor (no push), 8 = platform |
| `0x432fe0(self, mask)` | self centre inside a target box (attack overlap, mask 0x80) |
| `0x433380(self, pt, mask, excl)` | point inside target box ⊕ (r+4) (hand attacks, pick-up probes, projectiles mask 0x180) |
| `0x434840(actor, pt)` | point in a given actor's box (enemy hand hits) |
| `0x434230(a, b)` | box overlap (still standing on platform?) |
| `0x4313a0(pos, move, work, mask)` | camera box collision (camera only) |
| `0x437f40` | NPC steering: 9 heading offsets (table `0x4add98` ±341/±682/±1023 or `0x4addb0` ±256/±512/±768), reject walls/cliffs (> 200 drop), else turn 180° |
| `0x4376f0` | release a carried actor without dropping it through a wall |

Player contact results (from `0x433500`, hit actor +8 bits): 8 platform (ride if dy > −100: `P+0x70` = platform,
y = plat.y + plat.+0x52 + 4, local offset `+0x1a4..`, flags14 0x14; each tick `0x434230` checks it still overlaps),
0x4000 pushable (probe ahead `(0,−50,150)`; pushing writes the block's `+0x40/+0x44`), 0x2802 / 0x20100 pickable /
pickup (sets the target's flags14 0x8000 "touched", §7.3), 0x900000 buoyant (swim bob), 0x80000 radial push.
Pushable block (type 5 `0x445a60`): `+0x17c` (tag 0x1d) bits 1 allow X, 2 allow Z, 0x100/0x200 forbid −/+, 0x80 keep
on floor (cancel when a corner would drop > 200). BBLIT: the stone's box is offset from its mesh (PS1 bug; keep).
Runtime (RUNTIME_REFERENCE §11): the block runs in the platform group (flags8 8) before Bugs, so it applies the
push he wrote on the previous tick; it traces each corner of its box (grown by inflation / 4) for the X then Z move,
the leading face for walls unless flagsC 0x80, then moves Bugs by (moved − pushed) with a trace. The push branch
needs his move flagsA 0x4000000 and the probe (0, −50, 150) inside the same block (else the face push stops him);
the probe hit alone sets flags14 0x20000 (the R1 push rule). The radius `+0x2e` comes from the 4th u16 of tag 0x1c.

Trigger volumes (section-1 chunk 0x09, 0x3c-B nodes, list `DAT_004b3238`; tags 0x10 pos, 0x11 rot, 0x16 flags,
0x1c {sizeX, minY, sizeZ}, 0x1f zone, 0x33 32-B action entries): point-in-box test `0x434b00`, actions in
`0x4384a0` (§7.5).

### 4.5 TB — O

TB uses **a different, polygonal floor**: chunk **0x35 tag 0x36** → `0x445d00` ("Initialysing Plancher"); header +4
surface table, +8 surface count, +0xc vertex table (8-B s16 vertices), +0x10 polygon table (I), +0x14 corner, +0x1c
size, +0x20 octree nodes queried by `0x445d90`; surface record 12 B {u16 value, u16 id @4, u32 flags @8 (bits 1,
0x30, 0x200 tested by movers)}. No `% 0x140` code exists in TB. Needs its own RE pass.

**Reimplementation notes (collision).** Load zones (keep file order + `first[60]`), cell maps, shared tile pool.
GroundAt/FindZone exactly as above (y_top exclusive, y_bottom inclusive, hinted group first). Movement per tick:
trace X-only, trace Z-only, MoveActor(full delta) with diagonal retry, then `0x433500`-style box push vs solids
(r² pre-filter, oriented AABB inflated by +0x20, class 2 + mask), re-trace with the clamped delta. Constants: cell
320, sub-cell 40, step-up 100, no-zone dy 200, fall threshold 200, snap clamp ±70, grounded smoothing >>1, fall
`70+70·cos`, slide gain 3000>>12 at ±160, low-pass >>4. Keep 32-bit ints and s16 truncation where noted.

---

## 5. Camera (LiT `0x4318f0`, called first in `0x447d40`) — O/H unless noted

Globals: eye `DAT_004b38c0/c4/c8`, target (look-at) `DAT_004b38cc/d0/d4`, flags `DAT_004b3940`, yaw `DAT_004b3e1a`,
`0x4b3da8` (called "distance cache" here; naming needs re-deriving: the live follow also restores `0x4b38c0..d4` from
saved copies `0x4b3954..0x4b397c`, decompile 0x432438 — runtime: RUNTIME_REFERENCE.md §3), camera target actor
`DAT_004b39b8` (default player; A85 sets self), focus actor
`DAT_004b3950` (type 16 directors), parameters `DAT_004b215c` (target height offset), `DAT_004b2150` (eye height above
target), `DAT_004b2154` (follow distance). Level defaults (camera reset `0x431160`, also A77): **−200, 600, 1100**
(`0x4b3da8 = 1100`, flags = 0x40; the three parameters are **Verified** at runtime). Section-1 chunk **0x3b tag 0x2b**
(3 × s32 → eye) is the static initial eye, but on L01A it is **(0,0,0)**; the live eye after load (23613, −1000,
11550) comes from the walk-in (§1.2): flags 0x440 → the type-36 camera director `0x444b50` sets the eye point to its
own position (23610, −1000, 11550) via item flags 0x1081 and flag 0x400 lerps the eye `>> 2` (runtime:
RUNTIME_REFERENCE.md §3).
Spawn snap `0x4373f0`: target = player − (0,400,0), eye = target + R(yaw)·(0,0,−100), yaw = player yaw + 0x800.

Per tick (normal follow, flags & 0x300 == 0):
1. Zones: player zone → `P+0x26`, camera zone `0x436b60(eye)` → selects the world model `DAT_004b37a0[zone]` for the
   camera pseudo-actor (`DAT_004b39b4`).
2. Hold timer `DAT_004b3dac` (A41): camera frozen, player frozen. Scripted modes (flags 0x300 cut-scene, 0xc00 move
   to fixed point `0x4b3994..` or relative, 0x1000 fixed distance `0x4b39a4`, 0x400 eye lerp, 0x4000 snapshot) lerp
   eye/target by `>> 2` toward the scripted points (camera directors type 36 write them).
3. **Look mode** (logical △ held, no invulnerability, player not busy (flags14 & 0x1015609 == 0), not paused,
   flags 0x2000/0x10 clear): first-person: target = player + 8-direction probe average (table `0x4adad8`, ±200),
   pitch/yaw accumulators `0x4b3e20/22 += stick·0x28 >> 7`, clamped ±0x14a (≈ ±29°); eye lerps `>> 1`.
4. **Manual rotation**: L2 → yaw += 0x26, R2 → yaw −= 0x26 per tick (≈ 3.3°; flags |= 0x82; disabled by flag 0x8000
   (A83/A84) or L1 held or player scripted).
5. Focus actor present: yaw += (angle(player − focus) − yaw) >> 4 and distance ≥ 1500 (0x5dc).
6. **Auto-recentre**: `moved` = the target actor's position changed this tick (or it is type 0xe, or a recentre
   request flag 0x40 is pending ≤ 10 ticks); timer `0x4b3e28` is reloaded with 60 while moving and counts down
   otherwise; while (`moved` or timer == 0) and no manual hold (flag 2), yaw approaches `player.yaw + 0x800` by
   `diff·0x1e >> 0xb` per tick (≈ 1.5 % of the error). L2/R2 set the hold (flags |= 0x82); moving without L2/R2
   clears it.
7. Eye placement: candidate from target, `R(yaw)·(0,0,r)`; `r` (`0x4b3da8`) converges toward `0x4b2154 ± 100` (`>> 3`
   when closer, `>> 1` when farther) and rests at the band edge (**1200**, not 1100); the eye sits **400 beyond the
   candidate**: at rest the eye–target xz distance is `0x4b3da8 + 400` (1600 when `0x4b3da8` = 1200; runtime:
   RUNTIME_REFERENCE.md §3, three captures 1596–1602). Collision: ground traces `0x434cd0` along the eye path, `0x4313a0` actor boxes
   (masks 0x24000/0x20000) in 200-unit steps, `0x431770` 25 rotated probe traces (table `0x4adaa8`) to swing the yaw
   out of walls (`>> 2` of the probe result); if probes hit at > 500, the last manual step is undone.
8. Target follows the player: `t += (p − t) >> 2` with target y = player.y + `0x4b215c` (−200 = above the feet;
   **Verified**: rest target y = player.y − 203, runtime: RUNTIME_REFERENCE.md §3);
   eye y is pulled (`>> 3`) into ±100 of `min(ground, player.y) − 0x4b2150` (600 above), ceiling checks with 600-unit
   probes (`0x436c20`).
9. Camera shake `0x4312c0` (A59/A68): target x += `sin(phase)·amp >> 11`, y += `cos(phase)·amp >> 12`, phase +=
   `X·11` per tick, amplitude `0x4b3948` decreases by 3 per cycle (I: decompile control flow garbled).
10. View matrix: `0x40ceb0(eye/target)` → Q12 `diag(sx,sy,1)·RotX(0x800)·R` (`0x408910`), projection H = 400.

Flags `DAT_004b3940` (O for writers, I for names): 0x1 (A94/A95), 0x2 yaw held, 0x4 (A101), 0x8 cleared by directors
(A ← type 36 0x4000), 0x10 (A96) disables look, 0x20 (A98/A99), 0x40 recentre request, 0x80 manual step this tick,
0x100/0x200 cut-scene (actors freeze unless 0x200), 0x300 scripted (A90 clears), 0x2000 look disabled (A73/A74),
0x8000 manual rotation disabled (A83/A84).

TB (O/M): `0x439700 → 0x437800` follow, `0x4373c0` first person (L2+R2 / View key), per-view camera blocks 0x2C0 B at
`0x4d5120`; L2/R2 add ±0x5000 to a yaw accumulator; 2-player mode targets the **midpoint** of both players
(`0x4d5200` bit 0x100, lerp `0x4d9acc`).

**Reimplementation notes (camera).** Port the follow algorithm as listed with the exact shifts: target lerp `>>2`,
eye lerp `>>1`/`>>2`, distance band ±100 around 1100 (rests at 1200) with the eye 400 beyond that candidate (rest
xz 1600; runtime: RUNTIME_REFERENCE.md §3 — bbtime `lit_world.cpp` already uses `rotY(a, 0, 0, r + 400)`), height 600, target offset −200, L2/R2 ±0x26/tick, recentre after
60 idle ticks at 30/2048 of the error per tick, look clamp ±0x14a, focus-actor yaw `>>4`. Logic actions A41, A59,
A73/74, A77–A80, A83–A85, A90, A94–A101 drive the parameters. The 4:3 projection (sx, sy, H = 400) is shared with
the renderer (ENGINE_COMPARISON.md); keep logic visibility tests at 4:3.

---

## 6. Animation (LiT) — O/H

**Rigid hierarchical Sony TOD animation; not vertex morph.** Chunk 0x22 assets (0x58-B nodes): tag 0x27 `{u16 role/
anim id, u16 asset id}` (role 7 = actor model, 4 = skeleton TOD, 8 = flipbook sprite model, others = animation ids),
0x24/0x40 models, **0x25 = TOD** `{off, size}`, 0x3f flipbook list `{u16 nframes, u16 0, off, size}` (`nframes ×
{u8 sprite, u8 ticks}`), 0x28 sound event `{u8 id, u8 frame, u8 period}` (≤ 4), 0x0d logic event 12 B (≤ 3), 0x26
(absent in LiT). Actor tag 0x0f lists the asset ids an actor may use.

TOD (LE): header `u8 0x50, u8 0, u16 resolution (1/60 s), u32 nframes`; frame `u16 size_words, u16 npackets, u32
frame_no`; packet header u32 `id = w & 0xffff` (k-th created part, 1-based; 0 = actor), `type = w>>16 & 0xf`,
`flag = w>>20 & 0xf`, `len_words = w>>24`. Types: 1 coord (flag 2 rot 3×s16+pad 4.12, 4 scale 3×s16+pad Q12, 8
trans 3×s32), 2 TMD object (n−1, 0 = none), 3 parent (0/0xffff = actor root `actor+0x7c`), 8 object control (create
part / light-direction node / flag 0xb **hide** = scale 0), 9 actor bbox+radius (→ `+0x48..+0x54`, `+0x2e`, `+0x30`),
0xa attach point (→ `actor+0x58`, coord `actor+0x68`). A one-frame role-4 skeleton TOD builds the part list
(GsCOORDINATE2 per part, pool `0x4b2240`, 0xb8 B, list `actor+0x78`); animation TODs write sparse absolute channels
that persist across frames and animation switches.

Player `0x4393e0(actor, state, animId, mode, work)` (frame applier `0x44e400`): mode 1 loop, 2 once (returns
**0x100** when done), 0x800 hold last; 0x10 pause, 0x20 stop, 4 resume. **Each frame is held `resolution >> 1`
ticks** (res 2 → 30 fps: 8309 TODs; res 4 → 15 fps: 1582; skeletons res 1), **no interpolation**, and the last TOD
frame is never displayed. Sound events fire on the tick their frame is applied (every `period+1` passes); logic
events (`{u16 id, u16 flags, s16 frame, logic item}`) fire on frame or with flag 0x800 and can spawn (0x1/0x2),
drop (0x20/0x40), grab/release/knock the player (0x4/0x8/0x100; release subtracts `(actor+0x184)+0x16` health).
Anim state (`actor+0x144`): +0 last, +2 frame (`+0x146`), +4 frame-skip, +5 ticks/frame, +8 countdown (`+0x14c`),
+0x14 anim id (`+0x158`), +0x18 flags (`+0x15c`: 0x100 finished, 0x200 playing, 0x2000 last frame).

Posing: `coord.R = Rx·Ry·Rz·diag(scale)` (RotMatrix `0x407a50`, ScaleMatrix column scale), `coord.t = trans`;
`workm = super.workm ⊗ coord` (CompMatrixLV `0x408690`), root = actor (rot `+0xdc`, scale `+0xcc`, pos `+0xe4`, or a
carrier's attach coord2); built each render frame by `0x40a1f0 → 0x40a050`. **Skinning seams**: in actor TMDs the
vertex word 3 is a slot index (`& 0x7fff`) into a per-actor shared vertex buffer, bit 0x8000 = average (×0.5, after
projection) with the value already written by another part (`0x411350`); primitives index slots. Verified on all
4284 actor models (Bugs: 445 vertices → 313 slots) — **correction to MODELS.md §2** ("pad 0", plain concatenation),
which only holds for models without blend vertices. World models ignore word 3.

Animation selection: state → move node → anim id (`+6`) and mode (`+8`) → asset with matching role in the actor's
tag-0x0f list (§3.1). Flipbook actors (tag-0x16 flag 0x200000): `0x438970` copies sprite records per frame
(type 0x64 prims of the role-8 model).

TB (O/M): counterparts `0x469770` (player), `0x468280` (applier), `0x465dc0` (flipbook), `0x405820` (GsGetLw); actor
offsets +0x24; only the 8-byte TOD header is shared — TB packs frames/packets into 16-bit words with packed
rotation/translation types (not fully decoded).

**Reimplementation notes (animation).** Per actor: build parts from frame 0 of the role-4 TOD; per tick advance
`countdown`, apply the next frame's packets (persist channels), fire events, loop/once/hold per mode, report
finished; render with the parent chain and slot-buffer skinning (average flagged slots). Integer Q12 tables only if
pixel parity matters (`anim.py` uses floats and 3-D averaging).

---

## 7. Entities (LiT)

### 7.1 Actors, lists, activation — O/H

Actor = 0x1f4 B (pool `DAT_004b24a0`, capacity from chunk 0x3b tag 0x3e + 100); section-1 chunks **0x07 / 0x0a →
live list `DAT_004b3740`**, **0x08 → template list `DAT_004b2390`** (spawned by `0x448d40(entityId, pos, rot)`,
copying 0x1f4 B). Tag → field map (loader `0x42e7a0`, O/H):

| tag | field | tag | field |
|---|---|---|---|
| 0x0b/0x0c | +0x170..0x176 (texture link) | 0x1b | box min +0x48/4a/4c |
| 0x0f | asset ref list +0x74 | 0x1c / 0x42 | box max +0x50/52/54 (+ radius +0x2e, r² +0x30) |
| 0x10 | pos +0xe4/e8/ec (+0x100 = y) | 0x1d | type-specific u16 (+0x17c; type 14/0x1f → +0x196; type 8 → +0x160) |
| 0x11 | rot +0xdc/de/e0 | 0x1e | s16 +0x1e (type 14: hit points) |
| 0x12 | scale +0xcc/d0/d4 | 0x1f | zone +0x26 (default 0xffff) |
| 0x13 | behaviour type +0x2c (1 = player → `0x4b324c`, +0x3c = 0x40, +0x3e = 0x3fff) | 0x27 | entity id +0x1c |
| 0x15 | collision class +0x2a (2 = collision list) | 0x30/0x34, 0x31/0x35, 0x32 | script steps / items / states (LOGIC.md §2) |
| 0x16 | flags8 +0x08, flagsC +0x0c | 0x38 | box inflation +0x20 |
| 0x17 | +0x104..0x127 (platform params) | 0x39 | +0x28 = v + 0x10 |
| 0x18 | +0x128.. (two asset refs) | 0x3a | activation flags +0x3c / radius +0x3e |
| 0x1a | +0x100 | 0x41 | u16 list +0x178 (count +0x14a) |

Activation pass in `0x447d40` (O; bit names M): reference point = camera **eye** `0x4b38c0` (player position only
while the eye is at the origin); sleep timer `+0x24` decrements. Always active: `+0x3c == 0`, type 5, type 0x14,
actors linked to the player (`+0x6c`/`+0x70`), actor `+4 & 0x20`. Otherwise: `+0x3c & 2` → **inactive beyond
distance `+0x3e`** (sleep `(d − r) >> 7` ticks unless bit 0x40); `& 1` → zone visibility (portal zone table `0x4b3460`
must contain `+0x26`, entry flag 1); `& 0x20` → on-screen test (projected centre within ±0x100 × ±0x78 of the 512×240
reference, BBLIT H1 sites; multi-part actors test 8 box corners). Inactive actors with flags8 0x1000 also release
their parts (`+0x15c |= 0x4000`). Active actors are linked through `+0x10` into `DAT_004b39ac` in this order: type 0x15 actors, flags8 0x20000
actors, flags8 0x8 actors (platforms), **the player**, then the rest — and behaviours run in that order; then
`+0x22` (tick counter) increments. Killed actors (flags14 0x100) are freed by `0x447a50`. The distance test uses the 0x4078d0 estimate
`max + (mid + min) >> 2`; the zone table is filled by drawing (empty for the load pass and the first tick). Spawned
actors (0x448d40) go to the end of the live list **and of the running active list** (they update in the spawning tick).
Animation frames carry sound records (asset tag 0x28 {sound, frame, repeat}) and logic events (tag 0x0d, 12 B {u16
template, u8 flags, u8 flags, u16 frame, cond, A, B, act, u8 X, u8 Y}) run on the frame boundary by 0x4393e0 /
0x438970 (R: L01A Bugs anim 72 → A19 G[176] = 3). Steering 0x437f40 picks its offset table by step **flagsB**
0x80000; an open trace end is re-probed with GroundAt, whose result replaces the trace's (RUNTIME_REFERENCE §10).

### 7.2 Behaviour types `PTR_FUN_004ae098[actor+0x2c]` — O (roles H/M as marked)

| type | fn | role |
|---|---|---|
| 0 | 0x43eee0 | static prop (build model once) H |
| 1 | 0x43a880 | player (§3) H |
| 2 | 0x4455e0 | script-graph actor (LOGIC.md §3.1) H |
| 4 | 0x4452a0 | flipbook sprite effect (frame every 2 ticks) H |
| 5 | 0x445a60 | pushable block H |
| 6 | 0x447490 → 0x44acc0 | HUD: health `G[1]`, carrots `G[3]` (clamped 99), death detection H |
| 8 | 0x43efc0 | swinging rope/chain (sine table `0x4addc8`, amplitude 100 decaying, clamp [+0x1e, 300]) M |
| 9 | 0x4471c0 | follows the camera eye (+0x1e Y offset; sky/ambient emitter) M/L |
| 10 | 0x447280 | volume tagger (sets flags18 8 on actors inside; mirror plane?) L |
| 12, 13, 18, 19, 22–29 | 0x43f520(mode) | moving platforms: 2 spin (yaw += +0x17c), 8 pendulum (sin, speed +0x1e default 0x22, carries the rider), 1 seesaw (tilts with rider offset, ±0x164), 4 rises while ridden / 0x80 sinks while ridden (5 u/tick), 0x10 oscillate along local Z, 0x20 bob in Y, 0x22 bob + spin 0x44/tick, 0x40 two-position lift toggled by A30/A31, 0x100 conveyor, 0x200 conveyor when active, 0x400 sliding door; rider link player `+0x70` / platform `+0x6c` H |
| 14 | 0x440120 | **generic AI / pickup interpreter** (§7.3) H |
| 16 | 0x4445e0 | logic director (no model; camera focus when player < 2000) H |
| 20 | 0x445500 | looping flipbook M |
| 31 | 0x446d10 | projectile (speed +0x20/tick, 60-tick life, damage via 0x437d90) H |
| 32 | 0x4470e0 | UI overlay script (`0x44d100`, scripts `0x4b3a04`; spawned by `0x44e310`) H |
| 34 | 0x446990 | object attached to a parent (held tool/weapon; killed when detached) M |
| 36 | 0x444b50 | camera director (distance-gated items drive camera points) M |
| 3, 7, 11, 15, 17, 21, 30, 33, 35 | 0x417010 | no-op |

### 7.3 Type 14 and the item dialect — O/H

No hard-coded sub-kinds: behaviour = static flags8/flagsC (tag 0x16) + current script step flags (tag 0x30 node
`+0xc` flagsA / `+0x10` flagsB, params `+0x14..+0x1c`) + `+0xfc` items. Start state id 2 if present else 1; step
end/0xfff0 → state 1. Hurt: if hit bits (`flags14 & 0x6000000`) and the step is vulnerable (flagsA 0x4000 vs player
attack, flagsB 0x8000 stomp, flagsB 0x8 objects/projectiles): `HP(+0x1e) −= +0x38` → **state 3**; HP < 1 (unless
flags8 0x200 immortal) → **state 4**. Key flagsA bits: 0x1 stationary, 0x2 forward speed, 0x4 die at anim end, 0x10
ballistic jump, 0x20/0x40 pick up/drop, 0x80 advance on landing, 0x4000 vulnerable, 0x8000 turn toward target
(Δ >> `+0x196`, default 4), 0x10000 hand attack on player, 0x20000 contact damage to actors, 0x400000 no actor
collision, 0x800000 body-contact damage to player, 0x2000000 random wander (±10 yaw/tick), 0x20000000 hover,
0x80000000 fly. Item gates/effects (beyond LOGIC.md §3): gate14 0x8000 = touched by player (3498 items), 0x80 stomped,
0x40000000 actor contact; item flags 0x40100 spawn template P2, 0x4000000 kill id P2, 0x80000 goto level P1,
0x80000000 force player state P2, 0x100000 set checkpoint, 0x800 teleport player, 0x2000000 kill self. Contact bits
are cleared at the end of the actor update (`flags14 &= 0xb76f777f`). Worked example (L01d1 template 932): wander →
face (C19, dist ≤ 1600) → chase (speed 30) → attack (dist ≤ 600, hand hit dmg 1, player state 10) → stomped: sound
105 → hurt: spawns template 39 ×2 and golden carrot template 542 (once, guard `G[227] & 64`).

Player attacks: flagsA 0x800000 body overlap / 0x10000 hand point (mask 0x80) set the target's flags14 0x4000000,
`+0x38 = 1` (held object: 0x6000000 and its step damage); landing on an actor sets its 0x80 (stomp), flagsB 0x10000000
→ player bounce (flags18 0x40 → state 91).

Bugs vs actor boxes (0x43a880, runtime-verified on the L01A fight, RUNTIME_REFERENCE §10): his box x/z and radius are
fixed at (−70, −40)..(70, 40) / 80 after every animation tick unless the move has flagsB 0x2000 (only the packet-9
heights animate). An actor is touched only when 0x433500 reports a contact (a corner of his box past one of its
faces, or its second test — the actor's corners inside his quad, whose call the decompile lost); its w.y result is the
box top minus his new feet height. The face push is kept only for non-sensors when he is more than 30 below the top
and the move lacks flagsA 0x40; a stomped type-14 actor (flags14 0x80) skips it. The hand attack tests his `+0x58`
attach point (packet 10) with 0x433380 (radius² `+0x30`, box grown by inflation + 4 horizontally). Type 14 flagsA
0x8000000 hops count bounces in `+0x194` (reset only by the first call): hop n steps the arc n × w[2]; at ≥ 0x400 the
hop ends with flags14 0x800000.

Carrying (item flag 0x20000000, R Verified on the L01a walk-in): the target (P2; Bugs = his entity id) becomes a child
coordinate of the carrier (`+0xc4` = carrier `+0x7c`, `+0x70` = carrier, local offset `+0x11c` = 0, the carrier's
`+0x6c` = target); the target's flags14 are not changed. Its world position is evaluated when it updates, so a carrier
later in the update order leads it by one tick. Release (flag 0x4000): world position recomputed from the chain (=
the carrier's current position), `+0xdc/+0xe0` cleared, flags14 `&= 0xfffff5f6, |= 0x1000`, `+0x70 = 0`.

Ground shadows (R Verified visually, details RUNTIME_REFERENCE.md §7): tag 0x46 (`+0x3a`) names a template entity
whose first role-6 model is drawn under each registered actor (0x44c4b0 → `0x4b21a0`, 0x44bb50 / 0x44ba00 /
0x44b4f0): second-part origin, GroundAt with open-floor drop, clamp `0x4b2378`, x/z scale `(y − ground)·4 + 0x1000`
(≥ 0x800), tilt from three box-corner probes (±0x22); PC ABR 2 = texel-luma alpha, SRC_ALPHA / 1 − SRC_ALPHA.

### 7.4 Collectibles — O/H (data validated over 51 LiT level files)

Detection: the player's box collision (`0x433500`, mask 0x9a690a) marks touched actors with flags8 0x100 (pickup;
pickups use flags8 0x02001100 / 0x02000100 / 0x02000180, class 2, step flagsA 0x400001 or 0x1400000 clocks) by
setting their **flags14 0x8000**; the pickup's items gate on it. No separate radius.

| kind | mechanism | reward | persistence |
|---|---|---|---|
| carrot (759 placed) | `C31 G[Y]&X → kill` (already taken); touch: `A12 orgi G[Y] |= X`, spawn template **37** (fx), kill | template 37 at anim end: **A48 gvcrt** — `G[1] < G[2]` ? health +1 : carrots `G[3]` +1 | bit X of G[19..31], G[146..151] |
| golden carrot (236 + 69 templates) | init state: taken → kill; touch: **A23 incgg** total `G[7]|G[0xfc]<<8` and per-level counter, `A12` bit, collect anim (state 129), die | total +1, level counter +1 | bits in G[55..82], G[154], G[170–174], G[187..222], G[227], G[236], G[249] |
| clock (89 + 48) | as GC, plus sound 34, **player state 95** (celebrate, I), camera A79/A80 close-up, then A77 reset | `G[5]` +1, level counter +1 | bits in G[103..121], G[189], G[208–209], G[223–224], G[232], G[237–239] |
| carrot exchange (LS01 hub) | five actors 542–546: `C2 G[3] == 99` → spawn 507 | `G[3] = 0`, golden carrot +1 (5 times) | G[204] |

Counters: carrots clamped to 99 (HUD, cheat 1), **no extra-life rule**; lives `G[0] = 3` at new game but never
decremented. Game totals **333 golden carrots, 124 clocks** (cheat 5; pause-screen completion `(GC + clocks)·100 /
0x1c9`, 457 = 333 + 124; `credits.bze` checks GC ≥ 333 and clocks ≥ 124). Hub gates: clocks
{0,1,3,5,11,15,25,30,35,40,50,60,65,70,73,80,90,100,117,120}, golden carrots 50/150/250. Per-level display: LS01
logic sets `G[0xb6]` = GC counter index, `G[0xb7]` max, `G[0xb8]/G[0xb9]` clocks; HUD shows `G[G[0xb6]]`.

### 7.5 Level logic, zone triggers — O/H

Logic VM, scratch/global storage and all 99 conditions / 129 actions with semantics: **LOGIC.md**. Zone triggers
(chunk 0x09 volumes, run for the player each tick by `0x4384a0` unless scripted): node `[1]` actor id (6 = loop end
rule), `[2]` flags14 mask, logic item, `[5]` flags, args `[6..8]`; flags 0x80000000 force state, 0x800 `0x4379b0`,
0x800000 respawn position `0x4b2180`, 0x40000000 teleport, 0x80000 level exit (`GD+0 = [6]`), 0x200000 kill, 0x400000
default death state, 0x100000 checkpoint `0x4b2460..6c`, 0x10000 stop, 0x200 sound, 0x2000 `0x437510`, 0x40 camera
snap; continue only with 0x8000.

Per-level globals chunk 0x3b (O): tag 0x2b initial camera eye (L01A: (0,0,0); the live start eye comes from the
walk-in director, §5; runtime: RUNTIME_REFERENCE.md §3); 0x3c `0x4b2378` (shadow/ground clamp) + two bytes;
0x3d fog distance base (`0x4b38a0 = v/4`, `0x40cc80`; RESEARCH.md §2.7); 0x3e pool sizes (+100 each; vertex/part pools,
arena limit `0x4b247c = v + 20000`, BZE size limit). Chunk 0x47 tag 0x48 = demo input stream (sets `G[0x0f] = 1`);
chunk 0x49 tag 0x4a = 6 per-language `{offset, size}` pairs selected by `G[0x10]`: the overlay text scripts (§7.6).

**Reimplementation notes (entities).** Implement the actor record with the tag map above, live/template lists,
activation radius/zone tests and the exact update order (directors → solids → platforms → player → rest). Implement
type 14, type 2 and the platform family as data interpreters; collectibles need nothing beyond type 14 + logic
(route G writes through the golden-carrot-aware helper, HUD clamp 99, 30-tick player invulnerability). Keep logic
visibility tests at the original 4:3, 512-wide reference.

### 7.6 Overlay text (type 32) and speech status — O/H; save prompt Verified at runtime (RUNTIME_REFERENCE.md §9)

**Scripts** (D, all LiT levels): chunk 0x49 tag 0x4a pairs index the decompressed **section 3**, languages EN FR DE
ES IT NL; a block is a u16 offset table (entry count = first offset / 2; per level, 96–257 scripts; MERLIN EN 233,
0x2a54 B) followed by NUL-terminated text with `>` commands. The loader keeps the `G[0x10]` block in `0x4b3a04`.

**Spawner `0x44e310(script)`** (C55 / A49 X, A51 `S[Y]`, A52 `G[Y]`): spawns template entity **68** (type 32; 68
levels have one, CC2A/CC2B/CC2C/CC3B/CC3C have scripts but no template → nothing happens), `+0x17e = script | 0x8000`,
`+0x180 = 0`, zone = Bugs', flagsC `|= 0x40000000` if `S[12]` ∈ {1, 3}, then runs the interpreter at once and returns
its status nibble (C55: stored in `0x4b3fc4`, true if `A & status`; C56 tests `A & 0x4b3fc4`).

**Interpreter `0x44d100`**: pen (−200, −60), line width 0. A byte with a font glyph (table `0x4ae3a8`) appends
`{font sprite +0x10·(glyph−1), s16 x, s8 y, u8 palette}` to the 2-D list `0x4b373c` and advances pen and width by
`0x4ae4a8[byte]` × `0x4efb88` (1.0; 1.5 below a low-resolution threshold, `0x408100`); `' '` +6, `'\t'` +24, `'\n'`
ends the line, y += 13, x = line x. Commands (one byte after each operand is skipped): `>CX n` pen and line x, `>CC n`
the same + centre (at the line end the line's glyphs move left by width >> 1), `>CY n`, `>CP n` palette, `>CL` pen to
the line start + status 0, `>P d` status bits of digit d, `>W n` (n ≠ 0: `+0x22 = 0`, `+0x17c = n`, the rest →
`+0x180`, return 0; n = 0: status 4 if 0, `+0x17c = 2`), `>ID b` entity id `+0x1c` (for A50), `>B[G]b` decimal `S[b]`
(u16 for 0x2b..0x35) / `G[b]` (7 = golden-carrot total), `>D n` / `>J n` / `>DJ n` key names (below). `0x44cfa0`
draws each glyph's sprite record with the top-left at (x, y) of a 512×240 space centred on the screen, tinted by
palette (R colours): 0 white, 1 yellow, 2 green, 3 blue, 4 red; glyphs 0x67–0x6e (button icons) use palettes 5–12.
The list restarts at the top of `0x447d40`, so text shows on the tick it is emitted.

**Font** (D 63/63 levels with template 68; R): `0x44ccd0` / `0x44e310` take template 68's first listed asset of class
8 (`+0x4e`), a sprite-sequence model (chunk 0x22 tag 0x40) of 130 type-0x64 records, one per glyph, each naming a
whole TIM (u, v 0; 16×16, icons 20×16 / 64×32 / 32×16). GL draw `0x40b130`: x = `__ftol(x · 0x4efb7c)`, y =
`__ftol(y · 0x4efb80)` (width / 512, height / 240, from the centre) and size (w, h) × `0x4efb84`, which steps with
the width scale (≥ 2 → 2, ≥ 1.5 → 1.5, ≥ 1 → 1, else 0.85 with advance scale `0x4efb88` 1.5; `0x408100`); colour =
texel × palette mask (`0x40b130` +0x20, 0 = white).

**Key names**: `>D n` / `>DJ n` = DIK name (table `0x45e030` {name[32], scan code}, `0x402250`) of `config.pc`
kbdMap entry {11, 9, 8, 10, 6, 5, 7, 4}[n] (kick, crouch, look, jump, roll, camera W, action, camera Q); `>J n` =
"BUTTON%i" of joyMap {3, 1, 0, 2, 4, 6, 5, 7}[n] (`0x403260`); `>DJ` appends " / " + " Button " + DIK name of scan
code joyMap + 1 (a digit) when use-joystick (`config+0x38`) is on and the button is ≠ 0. Only glyph bytes are
emitted (spaces vanish: "S/Button1"), at most 20, then the pen advances 6; `>D` uses the `>CP` palette, `>J` the
last glyph's. C98 tests use-joystick (hint scripts pick `>D` or `>J`).

**Pause / help** (`0x4065e0` step 7, window messages `0x40e4a1`): F1 (VK 0x70) with `S[12] = 0` requests help
(`0x4b1a2c`), otherwise `S[12] = 2`; Esc requests the pause menu (`S[12] = 1`). Step 7 with `S[0] = 0`: from 0 the
request opens 3 (help) / 1 (menu) unless the camera is scripted (flags 0x300 without 1), `G[0x36]` or speech
`S[0x17]` blocks it; 2 → 0 once `0x4b3fc0` = 0. While `S[12] ≠ 0` `0x447d40` skips the camera and runs (and ticks)
only actors with flagsC 0x40000000 — the help director (L01A entity 0x1d99b: C1 `S[12] = 3` → A49 210) and the
overlays it spawns (`0x44e310` sets that flag while `S[12]` ∈ {1, 3}). R: F1 in L01A = 178 glyphs ("Help Menu" … with
"S/Button1"; RUNTIME_REFERENCE.md §9).

**Behaviour `0x4470e0`**: `+0x17c < 1` → redraw script `+0x17e` every tick; else when the timer reaches `+0x17c`
resume `+0x180` (fresh pen) or free the actor, otherwise redraw from the script start (the first `>W` page; timer
kept). The first call after the spawn only clears 0x8000. A `>W1` script re-spawned every tick by a director is thus
steady text.

**Save prompt** (D; R in MERLIN): the type-16 entity (chunk 0x0a) runs only while `S[17] = 1`: `S[11]` blink counter
0..19; ← / → (C57) set `S[18]` = 1 / 2 with `S[20]` as debounce (C69 none-of clears it); scripts 68 "Do you want to
save your game?" (yellow, centred, y 50), 69 / 71 Yes / No blue at x −50 / 30, y 70 (while `S[11]` ≤ 10 or not
selected), 70 / 72 white (selected); ✕ → Yes: `S[0x0a] = 6` (save, §8.1), No: `S[3] = 0`, `S[17] = 20`. The save
actors (MERLIN 0x104df, LS01 template 1316, type 14) start a speech (A81 47 / 50), enter state 129 once `S[0x17] = 4`
and open the prompt with Yes selected (`S[17] = S[18] = 1`) on ✕ or when the speech ends (`S[0x17] = 0`); after the
save `S[8] = 10` → `S[17] = 10` (entity 0x51c5) → "Game saved" (script 73).

**Speech status `S[0x17]`** (A81 `0x42d750`, update `0x41e2b0` after `0x447d40` each tick; path without an audio
device, as under Wine): A81 X > 1 → 8 (id `0x4b322e` = X), next update 2, then 4 with the clock started; length =
table `0x45c338[G[0x10]·64 + X]` samples × 1000 / 44100 ms (MERLIN 47: 4824 ms); 0 once elapsed. X = 1 → 1, X = 0 → 0.

---

## 8. Save and config

### 8.1 LiT files and the name mismatch — O/H, Verified at runtime (RUNTIME_REFERENCE.md §9, formats/SAVE_LIT.md)

The exe uses **`..\bin\Savegame%d.dat`** (320 B = `GAME_DATA[0..0x13f]`) and **`..\bin\Savedata.dat`** (360 B),
plus `..\bin\config.pc` (128 B) and `..\bin\bugs.ini`. BugsDecomp's "Gamedata.dat / Gamesave%d.dat" names are wrong
for v1.6. The slot number is **`G[0x9f]` (1-based, 1..6)** (`%d` = `*(u8*)(GD+0xdf)`, active-slot bit `1 << (slot−1)`),
so the game reads/writes `Savegame1.dat … Savegame6.dat` (R: title slot 2 → `Savegame2.dat`); the shipped blank files
`Savegame0..5.dat` include a `Savegame0.dat` that only a `/bNN` direct boot (`G[0x9f]` = 0) would write (I).
The save prompt ("Do you want to save your game?") is the type-16 menu entity of every playable level, opened by
`S[17] = 1` (Merlin/LS01 save actors); Yes = `S[18] = 1` → `S[0x0a] = 6`. The pause menu has no save entry (R).

* Writer `0x4074f0`: `GD+4 = Σ (s8) GD[0..0x13f]` (old checksum byte values included — R: 637 = 514 + 0x7a + 0x01;
  **never verified on load**), write 320 B; then update `Savedata.dat`: mask `0x4b28e4 |= bit`, slot summary
  (+8 + 0x2c·(slot−1)): `+0 u16` golden-carrot total, `+2 u16` clocks `G[5]` (R: `4d 01 7c 00` = 333/124 at +0x34);
  write 0x168 B from `0x4b28e0`, then `mask ^= bit` in memory only (`0x4076cc`). Failure → `S[0x1b] = 7`.
* Savedata reader `0x4076f0` (on the title, `S[3] == 1`): creates a zeroed file if missing; copies slot summaries into
  `S[0x2b..0x40]` for the title menu; `GD+0x50 = G[0x10]` = config language.
* Requests via `S[0x0a]` (`0x407190`): **4 new game** (`G[] = 0`, `G[1] = G[2] = 6`, `G[0] = 3`, `G[0x10]` =
  language, `G[0x11] = 0`, analog calibration defaults, **next level `G[0x0e] = 0x38` (56 `ccintro`)**); **5 load**
  (slot must be in the mask; copy 320 B back; next level = saved `currentLevel`, or 0x38 if 0); **6 save**
  (`0x4074f0`; `S[0x08] = 0x0a` ok / `0x14` fail).

GAME_DATA (LiT, `*(0x52fd00) + 0x10000`): +0 current level, +4 checksum, +0x0a..+0x3e pad words (§2.2), +0x40
`G[256]` (LOGIC.md §1).

`config.pc` (128 B, live copy `0x9ca820`, BugsDecomp layout confirmed by readers here): +0x08 language (→ `G[0x10]`),
+0x1c fogDist, +0x30/+0x34 volumes (logic A122–A127 change them in steps of 10/5 and call WriteConfig `0x409a10`),
+0x38 joystick enabled (C98), +0x3c joyMap[10], +0x46 kbdMap[16] (§2.1), +0x10/+0x14 resolution (A108/A114/A118),
+0x18 fullscreen (A111). Front-end option menus are BZE logic (LOGIC.md C92–C98, A100–A128).

### 8.2 TB — O/H

GAME_DATA = `*(0x4d03c0) + 0x10000` (cached `0x639b30`). `Savegame%d.dat` = raw GAME_DATA[0..0x2af] (0x2b0 B), signed-
byte checksum at +4 (not checked on load); before writing +0x14b = completion % (`u16 GD+0x5a · 100 / 6010`).
`Savedata.dat` = u32 count, `"1.00\0"`, count × 0x54-B slot records {u8 slot, char text "date, time, n%", u32 level
@+0x4c, next ptr}. Map: +0x08/+0x30 pad records, +0x58.. logic variables (operand base; LiT +0x40), +0x93 language,
+0x95/+0x96 remap presets, +0x97/+0x98 volumes, +0x9f dialog lock, **+0xa0 lives (3)**, +0xa2 down flags, **+0xa4
co-op mode flags**, **+0xa5/+0xa6 Bugs/Taz health (6)**, +0x1f4.. bitset of removed/collected entity ids. README:
position is not saved (restart in the level hub).

**Reimplementation notes (save).** Keep the 320-B LiT `GAME_DATA` image for original-save import (slot files 1..6,
checksum computed as above but not enforced), the 360-B `Savedata.dat` summary, and the new-game defaults (health
6/6, `G[0]` 3, start level 56). TB: 0x2b0-B image with per-character health/lives.

---

## 9. Time Busters deltas — O/H unless noted

* **Characters**: every TB level contains exactly two player entities: **entity id 2 (Bugs) and id 3 (Taz)**, both
  behaviour type 14 (`0x453de0`), player flag tag-0x16 word0 0x4000000; tag-0x16 word1 bit 0x400 → pad 1, 0x800 →
  pad 2 (loader `0x4347c0`); class templates via tag 0x27 u16[1] (Granny: Bugs template 181, Taz 182). Bugs = id 2
  / Taz = id 3 is *inferred* (M-H) from the README key lists matching P1/P2 defaults.
* Globals: controlled/camera player `0x4d236c`, partner `0x4d2368`, fixed Bugs `0x4d26cc` / Taz `0x4d2768`; GD+0xA4
  bits 0x02 two players active, **0x04 focus is Taz**, 0x08 switch latch, **0x10 two-player mode**, 0x20 call-friend
  latch.
* **Character switching (the mechanism)**: no strings, no logic opcode needed — `0x45eee0` each tick assigns the
  pointers from GD+0xA4 bit 4 and gives the focused character pad bit 0x400. **1 player**: R1 ("Switch Camera") edge,
  partner alive, no dialog lock → `A4 ^= 4` → control *and* camera move to the other character; both players run
  their "switched" logic (`0x45ea90`, +0x28 bit 0x100). Logic action 79 (`0x432320`) forces a switch. **2 players**:
  P1 always Bugs, P2 always Taz; R1 only moves the camera focus.
* **Call friend** (L1 edge, `0x45eb10`): partner teleported to a spot found by `0x44ecd0` relative to the focus
  (offset {0,0,−400}); refused when paused, dialog lock, airborne > 2 ticks, partner dead, or **cooldown < 45 ticks**
  (`0x4ceb08`). In 2P the non-focus player teleports himself.
* Death/lives: partner alive → auto-switch; both down → lives (GD+0xA0) −1, respawn at checkpoint `0x4d26e0..ec` with
  both health 6; lives 0 → level 5 (`RETRY`) reload, lives 3. Action 78 revives the partner.
* **Co-op / split screen**: views `DAT_004d2794` (.bss) have only two writers, both `= 1` → two-view rendering (per-
  view camera blocks 0x2C0 B, `sy` doubling `0x422df0`, view masks actor+0x6E) is **unreachable**; the menu's "2 Players"
  (`menu.men [TWO_PLAYER]` → `0x41a6e0(2)` → `0x4370c0` sets A4 |= 0x10) is **single-screen co-op** with a midpoint
  camera.
* **Abilities**: data-driven — tag 0x2F state records (state id, anim, logic lists on entry, flag words, forward
  speed, damage dealt, vertical speed, target selector), tag 0x31 event transitions (`0x44d990`: events 0x10000000
  death, 0x8000000 hit, combo indices), pad conditions = VM opcode 0x0E ('X' tag {flags, required mask, any-of mask}).
  Button mapping (README ↔ pad): □ Kick / **Spin**, ○ Ears-Sneak / **Pull-a-face**, ✕ jump, △ action, L2+R2 first
  view. Code-level Bugs/Taz differences are only hit flashes, health mirrors (+0xA5/+0xA6), Taz-hit victim flag
  +0x28 0x20000, and per-character interactables (object +0xC 0x10000 Bugs / 0x40000 Taz). Which state id is which
  ability is not decoded (`tb_players.py --states`).
* Logic VM selectors (`0x45a120`): 2 Bugs, 3 Taz, 8/0xE focus, 9/0xF partner, 4–7 nearest/farthest player, ≥ 0x11
  entity id. TB opcodes are renumbered vs LiT (LOGIC.md §6).
* Collision floor is polygonal + octree (§4.5); animation TOD packing differs (§6); camera §5; input §2.3; saves §8.2.

**Reimplementation notes (TB).** Two player actors always exist; keep focus/partner/Bugs/Taz pointers, route pads by
the 0x400/0x800 bits, implement R1 switch / L1 call-friend (45-tick cooldown, ≤ 2 airborne ticks) in the per-tick
view update, one viewport with a midpoint camera in 2-player mode. Player movement = the generic type-14 state-record
interpreter; abilities come from the class-template data.

---

## 10. Residual unknowns (Research Required)

1. Runtime confirmation of everything above (no traces taken): especially tick order effects, jump arcs, camera
   lerps, and the 3-tick press window.
2. Player: flagsA bits 0x1, 0x8, 0x400000, 0x40000000, 0x80000000; the carry family 68–72; cut-scene states ≥ 129;
   writer of `P+0x68`.
3. Collision: cell flag 0x2000 (12 685 cells in 8 levels, no reader); zone record +0x24..+0x43; `0x433500` edge-push
   branch (`0x4b3d60..98`) and per-bit meaning of class masks 0x9a690a / 0x2690a.
4. Animation: writer of part flag 0x400; packet types 0 and 6 consumers; logic-event flag gameplay meanings.
5. Entities: carrot bits G[19..31]/G[146..151] reused across levels with no clear found (do carrots stay collected in
   levels sharing bits?); 10 golden carrots and 1 clock not found in the per-level maxima (333/124 vs 323/123);
   consumer of type-10 flags18 8; type 9 role; `0x4377a0`; pause-menu gate `0x4b3fc0`.
6. Camera: exact names of flags 0x1/0x4/0x20; type 36 director item semantics beyond distance gates.
7. Logic: C69 tail, A24 tail, C82/C83 B-byte bit map (LOGIC.md §8).
8. Save: whether any path uses `Savegame0.dat`; `config.pc` unknown fields (RESEARCH.md §9.8).
9. TB: polygon floor table and surface flags; TB jump/gravity constants; Bugs/Taz state id → ability names;
   'X' flag bit 0x08; call-friend placement rule `0x44ecd0`; partner movement in 1P; TB TOD packing; roles of TB
   behaviour types 2, 4, 5, 30, 31, 33.
