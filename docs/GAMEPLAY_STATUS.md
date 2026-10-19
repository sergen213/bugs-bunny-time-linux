# LiT gameplay implementation status (bb_game, wave 3)

Code: `engine/game/` (`bb/game/{litmath,pad,heightfield,lit_world}.h`), app glue `apps/bbtime/play.{h,cpp}`.
Spec: [re/GAMEPLAY.md](re/GAMEPLAY.md) (§ numbers below). Vocabulary: Research Required / Designed / Implemented /
Tested / Verified Against Original Behavior. *Verified* = the same numbers seen in `bugs.exe` under Wine
(re/RUNTIME_REFERENCE.md §2–§3): §1.1 30 Hz tick / +2 counter, §3.3 jump arc (apex 389 @ 15, lands 26; tap apex 285
@ 8, lands 18; cut to 0x800; even descent steps), §5 parameters −200/600/1100 and target-y lerp; everything else in
the spec stays Researched (static).
"Tested" = covered by `ctest -R game.lit` (synthetic heightfield + real L01a with `BBTIME_DATA`, all 79 LiT
heightfield levels swept for 120 scripted ticks, ASan/UBSan clean).

| part | status | notes |
|---|---|---|
| 30 Hz tick, order pad → camera → player → timer (§1.1) | Tested; tick rate **Verified** | render locked to the tick in Original Fidelity; Enhanced lerps camera/Bugs between ticks only; original 30.005 ticks/s, counter +2 (runtime: RUNTIME_REFERENCE.md §2) |
| Q12 sin/cos tables, round(sqrt), atan2 (§conventions) | Tested | tables compared with `bugs.exe` bytes |
| Pad words, 3-tick ✕/□/L1 window, dead zone 50, digital ±0x7f/−0x80 (§2.2) | Tested | remap presets 1–3, demo playback, recording, pad-removed pause: not ported |
| Key map (config.pc defaults, settings.ini `bind.*`, Space/LCtrl aliases), SDL gamepad (§2.1) | Implemented | digital keyboard always runs (as the original: p = 128); walking needs an analog stick |
| Player state machine: tags 0x32/0x34/0x35, rule order, 0xfff1, −13, coyote redirect (§3.1) | Tested | held/support-actor masks fail (no carry/platform actors); held commands ignored |
| Steering, walk 40 / run 50, turn Δ>>2, accel ½, ice, momentum ×0.75, slide probes (§3.2) | Tested (walk/run/turn), Implemented (ice/slide) | exact integer port of 0x43a880 |
| Jump arc H/step, variable height, free fall 70+70·cos, glide 12 (§3.3) | Tested; arc **Verified** | apex 389 at arc tick 15, lands 26; tap apex 285 @ 8, lands 18 = original (runtime: RUNTIME_REFERENCE.md §3); fall 0,1,2,4,7,…; ±70 snap below 200 |
| Logic VM (lit_logic.cpp) | Tested (synthetic `testLogic`) | all gameplay conditions C0–C91 and actions A1–A92, A94–A101 (G index 7 = 16-bit golden-carrot total); not ported: front-end C92–C98 / A93 / A100–A128, overlay scripts C55/C56/A49/A51/A52, A44/A56/A69/A88/A91 stores, shake A59/A68, fades A70/A71, camera target A85; sound-playing C85/C87 always 'not playing' |
| Heightfield FindZone / GroundAt (§4.2–4.3) | Tested | exact |
| TraceGround 0x434cd0 | **Implemented (approximation)** | sub-cell walk with the documented block rules (wall except start, step-up 100, leave-all-zones) and entry-point clamping; not the exe's DDA with its 15-entry history buffer |
| MoveActor (wall slide retry, open → zone below, ceiling clamp, smoothing/landing) (§4.3) | Tested | port of 0x436ff0/0x4371f0/0x436de0 |
| Player box top +0x52 | **Implemented (approximation)** | TOD packet-9 box from the animation when set, else −400 |
| Camera follow 0x4318f0: recentre, L2/R2 ±0x26, distance band, defaults −200/600/1100, wall/yaw probes, eye-height/ceiling (§5) | Implemented; defaults **Verified** | original defaults −200/600/1100 and target-y lerp confirmed at runtime; the eye is placed 400 beyond the band candidate as in the original (rest xz 1200 + 400 = 1600; runtime: RUNTIME_REFERENCE.md §3); look mode (△), directors, scripted modes, focus actors, actor-box camera collision (no solids simulated) and shake not ported |
| Damage / death / respawn (§3.6) | Tested (death → respawn at checkpoint, health 6) | enemy contact damage absent (no AI actors); death via health 0, A35, A72, zone-trigger kill; unknown death state falls back to state 1 |
| Zone triggers 0x4384a0 (§7.5) | Tested (L01a exit → level 5 L01B) | run for Bugs and for actors with flags8 0x1000000 / step flagsB 0x800000; flags kill/checkpoint/default death/teleport/stop/continue/camera snap/force state/0x800 teleport/sound/**level exit 0x80000**; 0x800000 respawn point and 0x2000 (0x437510) not ported; box test yaw-only |
| Collectibles (§7.4) | Tested (L01a carrot row → carrots +4) | through the type-14 interpreter and data (touch flag 0x8000 → A12 bit, spawn fx template 37 → A48 gvcrt; golden carrots/clocks A23 incgg); Bugs' touch = approximated 0x433500 box test |
| HUD | Implemented | text counters, not the original sprite HUD |
| Bugs drawing | Implemented | ActorAnimLiT `animTick`/`poseActor` driven by the move's anim/mode, 0x100 clip end → sequence advance |

| Actors / activation 0x447d40 (§7.1) | Implemented (approximation) | update order type 0x15 → flags8 0x20000 → platforms → Bugs → rest; distance (+0x3e) and on-screen (centre only) activation; portal-zone visibility not simulated |
| Type 14 interpreter 0x440120 (§7.3) | Implemented, walk-in Verified | states/steps/items with all gates (masks, anim frame/end, random, contact 0x2000, distance/target), hurt → state 3/4, hand/body contact damage via 0x437d90 + knock-back, forward/turn/wander/bob/fixed hop, NPC steering 0x437f40, MoveActor gravity, spawn/kill/level exit/checkpoint/teleport/force state, carrying Bugs (0x20000000 P2 = Bugs, release 0x4000). **Verified Against Original Behavior**: L01a start walk-in (state 2 → 130 at tick 1 → 1 at tick 34; original counter 5 → 71 = ticks ≈2 → 35, 25 u/tick, WineRef trace). Not ported: hover 0x20000000/0x40000, player-steered 0x80000000, aimed arcs, carrying other actors, 0x434740, 0x437510 |
| Type 2 script graph 0x4455e0, directors 16/36 | Implemented | camera effects of type 36 not ported |
| Platforms 0x43f520 (types 12/13/18/19/22–29) | Implemented | spin, pendulum, rise/sink ridden, lift (A30/A31), slide Z, bob, conveyor, door, items, contact damage; seesaw tilt and rider rotation on pendulums not modelled; ride = box top within 100 |
| Texture links (TEXTURE_GAP.md) | **Verified Against Original Behavior** | loader resolves 5 271/5 271 LiT world faces; type 20 hold byte: L04C3 links 9/11 = 3/2 ticks per frame (original alias trace: 3/2); type 2 from the start step's tag-0x3f flipbook list: L02A1 link 16 = 5×2 ticks + 30 (original trace matches); unresolved ids not drawn |
| SFX | Implemented | sound table tags 0x04 + section-4 program table → section-3 AIFF blob; played at native rate (pitch/volume bytes ignored); mode 2 (release) ignored |
| Level change (§1.2) | Tested | `LitWorld::nextLevel` + exe level table names; app reloads and carries G[] (smoke: `BBTIME_LIT_START=3525,-3200,20326 bbtime --level L01A` → "level change -> 5 L01B"); loading screens / fades not shown |

Not started: pushables, ropes, swimming, saves, loading screens.

# TB gameplay implementation status (bb_game, wave 4)

Code: `engine/game/` (`bb/game/{tb_collision,tb_world}.h`), actors/animation `engine/core/{include/bb,src}/tb_actor.*`
(packed TOD through the shared `bb/actor.h` player via `Tod::packed`), app glue `apps/bbtime/play.{h,cpp}` (`TbPlay`,
`tbKeyMap`), `apps/bbtime/main.cpp` (`--players 1|2`, `--autoplay2`, Options "TB players", second gamepad), music
`apps/bbtime/music.cpp`. Spec: [re/GAMEPLAY_TB.md](re/GAMEPLAY_TB.md) (§ numbers below). "Tested" = `ctest -R game.tb`
(synthetic collision + real ARCA01 replays + all 58 levels × 120 scripted ticks, deterministic, ASan/UBSan clean).
"Verified" = the same integers seen in the original under Wine (WineRef harness `tools/wine/`, captures
`local/wine/tbplay/{A_idle_jump,B_run,C_switch_taz_jump,D_callfriend,E_switch_turn_L2,F_2p_granny}.csv` + peeks);
the test asserts those numbers where marked.

| part | status | notes |
|---|---|---|
| Tick order: pads → camera (focus latched first) → R1/L1 → Bugs, Taz (§4.1) | **Verified** | trace C: A4 0 → 12 on the R1 tick, camera target `0x4d5028` one tick later; test asserts `focus`/`camFocus` |
| Pad layer (GAMEPLAY.md §2.3) | Tested / **Verified** (parts) | edge mask chunk 0x3a tag 0x3c @12 = 0xd0 (peek `0x4d26c0` = 0xd0); keyboard stick ±0x7f → mag 127 (run 49 u/tick, trace B); P1/P2 keys = runtime `config.pc` defaults; analog dead-zone scaling Inferred; remap presets not ported |
| Collision mesh: locate / next leaf / segment query incl. first-hit-leaf quirk, filters (§1.2–1.4) | Tested | bit-exact port of `tools/re/tb_ground.py` (int64 like python); synthetic selftest; exact call-friend spot (below) exercises it on real data |
| Mover 0x4450c0 (§1.5–2.2) | **Implemented (approximation)** | gravity 5·dt, k-shift table, falling/descending flags, grip × friction: **Verified** (run ramp 13/14 → 33/36 u/tick, 7 ticks exact, trace B); swept sphere/capsule slide = own design (per-mode sweeps 0x44ba20/0x442430/0x44cf80 not transcribed), floor ny < −11000, crease stop, wall/ceiling flags; ledge/edge passes 0x4426a0/0x4434c0, actor pushes, bounce not ported |
| Collision box (§1.5 step 1) | **Verified** | from the entity's tag-0x61 record 0 (action 82 selects, ±20/tick morph): Bugs −70 0 −60 70 −400 80, Taz −85 0 −50 85 −240 125 = peek of `+0x60`. **Correction**: TOD 0x900 packets are all zero for the players and only apply when no record is selected (`+0x123` = 0xff); Bugs capsule r 70 half 130, Taz sphere r 120 |
| State records / transitions / logic VM at tag level (§3.1) | Tested | markers/skip slots per encoder 0x433900, chains, 'N' events, sequence/next/select events 0x44d990, walk/run split, anim (re)start rule; conditions O P Q X R(1,5,7,0x10,0x18,0x19,0x49) and ELSE; **W Z T [ ] ^ ` evaluate false** (unknown, listed in F3 overlay) → idle fidgets 17–31 never start (**Contradicted**: original idles 8 → 26 → 27 → 28, trace A); actions S10/S79/S82/S126, U28–30; others listed |
| Walk 26 / run 50 (S[0x66] = 25), run at mag ≥ 120 (§2.1) | **Verified** | peek S[0x66] = S[0x67] = 25; keyboard run = 49 u/tick (trace B, test asserts every velocity) |
| Steering (stick heading + camera yaw, turn Δ>>2, lean) | Tested | first tick exact vs trace E; later ticks ≤ 3 u/tick slower turning than the original (camera/yaw coupling not exact) |
| Jump (§2.3): vy0 = −0x50 − ((round√(80H+100) >> 1) << 4), ballistic +10/tick² | **Verified** | Bugs 548 → apex 550 @ 10, lands @ 21, landing state 50 the next tick; Taz 490 @ 10 (traces A, C); test asserts the whole 21-tick arc |
| R1 switch, 1P pad routing (§4.1) | **Verified** | as tick order; latch while held; partner idles (P2 record zero in 1P) |
| L1 call friend (§4.2) | **Verified** (xz, cooldown) / **Contradicted** (height) | first candidate camYaw + 0x100 → spot (326, 148) from Bugs, exact; cooldown 50 at level start, refused at 19, accepted at 45. **Correction**: the 50-unit cut precedes the ×0.8 scale. The original drops the partner from ≈1230 above the spot at ≈104 u/tick in template arrival states 260–263 (entity programs, not ported): ours places it at focus.y − 200 and lets gravity drop it |
| Camera follow (§4.3) | **Implemented (fit to traces)** | look-at f8/f4 smoothing **Verified** (exact after switch and during the jump); rest geometry **Verified** (eye 1203 from aim, D = 1200 peek, body = eye + 400·dir(L−eye) = peek `0x4d514c`). **Corrections**: the spring error is measured from the eye (B+4), not the body; the 20-tick vertical dead band is not observed (eye keeps its height after a landing) and is not applied; snaps use the observed rest pose. L2/R2 accumulator ramp 5, 9, 13 … and direction **Verified** (trace E). Camera collision = 300-unit sphere slide (approximation); look mode, overhead guard, occlusion impulses not ported |
| 2-player co-op camera (§4.3) | Tested / **Verified** (parts) | midpoint target, flags 0x300; framing ±190 × ±90 with projection distance 248 fitted to trace F (D +16, +28, +38, then +40/tick); split at \|Δxz\| > 2700 then snap (2500 rule) and D −= (D−1200)>>2 as trace F; re-merge after 30 ticks without the visibility test |
| Packed-TOD animation (§5) | Tested | decoder/applier = `tb_anim.py` port (`Tod::packed` → `applyTbFrame`); 137 skinned actors on ARCA01 animate (Bugs/Taz from the state machine, others loop their start-state anim, no behaviour); LOD rigs, root motion 0xa00, asset logic per frame, actor texture links (start frame only) not ported; stick anim pace Implemented |
| Music (§6) | Implemented | first action-126 setter in the level (ARCA01 n 26 → Track25.xa, test asserts n); Track14 fallback; later switches (TRQ02, VIQ02) not run; playback not verified headless (`--mute` smokes) |
| Fell out of the world | Implemented (approximation) | y > 32000 → back to spawn (exe: s16 wrap test → checkpoint) |

Not ported (TB): NPCs and items (no type-14 behaviour for non-players), damage/health/lives/death, water/swimming,
ledges/climbing, carry/throw, Taz spin damage effects, look mode, "switched" entity programs (`+0x28 & 0x100`),
level logic (directors, aux items), saves, SFX. Smoke screenshots: `local/gameplay/tb/{bugs_spawn,bugs_midjump,
taz_after_switch,taz_midjump,coop_2p}.png` (Xvfb llvmpipe GL; framing matches `local/wine/tbplay/A_spawn.png`).
