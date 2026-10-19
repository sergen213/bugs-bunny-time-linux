# LiT gameplay implementation status (bb_game, wave 3)

Code: `engine/game/` (`bb/game/{litmath,pad,heightfield,lit_world}.h`), app glue `apps/bbtime/play.{h,cpp}`.
Spec: [re/GAMEPLAY.md](re/GAMEPLAY.md) (§ numbers below). Vocabulary: Research Required / Designed / Implemented /
Tested / Verified Against Original Behavior. Nothing is *Verified* (no runtime traces of the original exist).
"Tested" = covered by `ctest -R game.lit` (synthetic heightfield + real L01a with `BBTIME_DATA`, all 79 LiT
heightfield levels swept for 120 scripted ticks, ASan/UBSan clean).

| part | status | notes |
|---|---|---|
| 30 Hz tick, order pad → camera → player → timer (§1.1) | Tested | render locked to the tick in Original Fidelity; Enhanced lerps camera/Bugs between ticks only |
| Q12 sin/cos tables, round(sqrt), atan2 (§conventions) | Tested | tables compared with `bugs.exe` bytes |
| Pad words, 3-tick ✕/□/L1 window, dead zone 50, digital ±0x7f/−0x80 (§2.2) | Tested | remap presets 1–3, demo playback, recording, pad-removed pause: not ported |
| Key map (config.pc defaults, settings.ini `bind.*`, Space/LCtrl aliases), SDL gamepad (§2.1) | Implemented | digital keyboard always runs (as the original: p = 128); walking needs an analog stick |
| Player state machine: tags 0x32/0x34/0x35, rule order, 0xfff1, −13, coyote redirect (§3.1) | Tested | held/support-actor masks fail (no carry/platform actors); held commands ignored |
| Steering, walk 40 / run 50, turn Δ>>2, accel ½, ice, momentum ×0.75, slide probes (§3.2) | Tested (walk/run/turn), Implemented (ice/slide) | exact integer port of 0x43a880 |
| Jump arc H/step, variable height, free fall 70+70·cos, glide 12 (§3.3) | Tested | apex 389 at arc tick 15, lands 26; fall 0,1,2,4,7,…; ±70 snap below 200 |
| Logic VM subset for player rules / triggers | Implemented | C0–C2 C4 C11 C12 C14 C21–C23 C30 C31 C34 C35 C37–C40 C44 C46 C47 C90; A3 A11–A14 A18 A19 A25 A28 A35 A36 A39 A58 A72 A74 A92 A94; others are listed in the F3 overlay and return false / do nothing |
| Heightfield FindZone / GroundAt (§4.2–4.3) | Tested | exact |
| TraceGround 0x434cd0 | **Implemented (approximation)** | sub-cell walk with the documented block rules (wall except start, step-up 100, leave-all-zones) and entry-point clamping; not the exe's DDA with its 15-entry history buffer |
| MoveActor (wall slide retry, open → zone below, ceiling clamp, smoothing/landing) (§4.3) | Tested | port of 0x436ff0/0x4371f0/0x436de0 |
| Player box top +0x52 | **Implemented (approximation)** | TOD packet-9 box from the animation when set, else −400 |
| Camera follow 0x4318f0: recentre, L2/R2 ±0x26, distance band, defaults −200/600/1100, wall/yaw probes, eye-height/ceiling (§5) | Implemented | look mode (△), directors, scripted modes, focus actors, actor-box camera collision (no solids simulated) and shake not ported |
| Damage / death / respawn (§3.6) | Tested (death → respawn at checkpoint, health 6) | enemy contact damage absent (no AI actors); death via health 0, A35, A72, zone-trigger kill; unknown death state falls back to state 1 |
| Zone triggers 0x4384a0 (§7.5) | **Implemented (approximation)** | point-in-box with yaw-only rotation; flags kill/checkpoint/default-death/teleport/stop/continue/camera-snap; level exit 0x80000 and 0x80000000/0x800/0x800000/0x200/0x2000 not ported |
| Collectibles: carrots (heal else +1, cap 99), golden carrots, clocks (§7.4) | **Implemented (approximation)** | classified like `tools/re/entities.py`; touch = player centre (r 80) vs pickup box inflated by tag 0x38 instead of 0x433500 + the type-14 item interpreter; reward applied on the touching tick (original: fx template / clock celebration state 95 / camera close-up) |
| HUD | Implemented | text counters, not the original sprite HUD |
| Bugs drawing | Implemented | ActorAnimLiT `animTick`/`poseActor` driven by the move's anim/mode, 0x100 clip end → sequence advance |

Not started this wave: other actor behaviours (enemies, platforms, pushables, ropes, swimming), level change
requests, saves, SFX.
