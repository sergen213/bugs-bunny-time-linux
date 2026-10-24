# Changelog

All notable changes to bbtime. Versions follow `MAJOR.MINOR.PATCH` (the updater refuses anything that is not
strictly newer). Entries before 0.1.0 are reconstructed from `git log`.

## 0.12.0 — 2026-10-24 (Waves 12–14: TB sky domes, water/swimming, effects, entity classes, carrying/climbing, exact hand-ported TB mover; LiT render order, input, actors; BBLIT-Remaster-inspired Enhanced features)

- Enhanced (ideas from BBLIT-Remaster, MIT, InLineR495; no code copied): poses interpolate between ticks (part
  matrices by shortest-arc rotation, TOD frames glide over their hold so 15 Hz clips and the LS01 vortices no longer
  step; non-player actor / entity roots too, teleports snap); right-stick LiT camera through the L2 / R2 rotation
  (`stick_camera`, `camera_speed` 52, `camera_invert`); 4-tick LiT jump buffer (`jump_buffer`, taken by jump states
  7/8/9/26/72/78, dropped on pause); HUD counters at wide-screen edges in levels (`hud_edges`); centroid UVs under
  MSAA and a half-texel UV clamp on non-tiling translucent meshes. Attached actors follow their parent's interpolated
  part, ground shadows glide with their owners (LiT and TB), a stalled clip no longer saw-tooths (ARCA01), LiT
  teleports / camera cuts are shown at once (LS01 tick 22 used to smear 7000 units). Fog fades into the sky backdrop
  instead of black (Vulkan; LiT's sky is the background layer in Enhanced); ground haze in L01A / L01B / L01D1 /
  L01D2 (`haze`, Vulkan); the TITLE logo drawn at `title_logo` 85 % of its depth; a pushed stone drawn clear of
  Bugs (`push_r` 50). `tools/re/motion_check.py` (BBTIME_MOTION trace) and `tools/re/verify_exe.py`. Settings in
  settings.ini and the launcher (input / HUD options also in the in-game Options). Original Fidelity unchanged
  (LS01, L01A, L03A, ARCA01 `--no-hud` pixel-identical to the wave 13 build).

- TB mover: the collision code is now entirely our own C++ (`tb_gte.h`, `tb_shapes.cpp`, `tb_sweep.cpp`,
  `tb_mover.cpp`; all 118 functions under 0x4450c0 plus the camera sweep): the software GTE and vector / matrix
  helpers, every sphere / cylinder / capsule / box / segment primitive, the shape and polygon tests of the camera
  sphere and the sphere / capsule / box / point movers, their passes, the response 0x443f40, the polygon gather, the
  shape cache, the ledge / climb / overlap / hover passes and the mover. It runs on typed bodies of the entities and
  a copy of the collision block; the Ghidra transcription (`tb_mover_exe.c`), its generator and the exe-address
  arena are gone. `tools/re/tb_shapes_fuzz.py` runs random records through the exe (unicorn) and the port for every
  primitive and test: all equal. Fixed on the way: a negative square root (0x4040b0) is 0x80000000 as on the x87.
- TB: the entity pool is modelled per slot (0x460b00 / 0x4608c0): view / world entities 0..16, one slot per chunk 0x07 / 0x08 / 0x0a / 0x63 in file order, the HUD sheet holder (0x462430), spawns in the lowest free slot below the count; a destroyed entity frees its slot right after its own tick (0x460630), the others go in the next list walk (0x45eee0), which spares the entity after each one it destroys. GRANNY and ARCA01 slot tables equal the original's slot for slot; with the GRANNY pool cut the slam's template 80 now fails as in the original. Each applied anim frame resets the attach point (+0x84) and sets it with +0x7c before the frame's asset logic (0x468280), through the entity's scaled root and, for a hung entity, its parent's chain (0x44e560): GRANNY's coin placers skip frames without one, VICASG04's 131 (scale 1.25) grabs Bugs on the original's tick and his snowball flies from its hand. Oracle checks: GRANNY pool cut (whole capture from the level start) and VICASG04 grab / throw (whole capture) are trace-equal.
- TB: a state entry that restarts the clip clears the anim's last-frame / restarted bits (0x44d990), so a hit's logic runs as the original: ARQ01's boss hit now knocks Bugs back on the same tick. A player out of the game while the partner plays is no longer ticked or drawn (0x45eee0): a drowned Bugs stays down instead of re-entering 394. Entities keep their tag 0x1b / 0x1c box until their first tick selects the tag-0x61 box record (0x450440): the mover pushes a player out of an unticked entity, and Taz out of Bugs at the ARQ01 start, as the original. Oracle checks ARQ01 (whole capture), VIQ01 (whole capture), TRQ02 inside-309 push, GRANNY slam with the pool cut, VICASG04 pick-up / carry are trace-equal.
- TB: the level-load camera placement targets the last focus-flagged record, which is template 181 in VIQ01, GRANNY, TRCA01 and 20 more levels (view starts at the origin; the first follow camera after an intro now matches the original); the view list refines a cutting sphere only once the entity has a model instance (entities at the view edge listed on the original's tick). Oracle checks added: Taz's gum float (S[0x67] vertical move, TRQ02) and the TRQ02 309 lift are trace-equal.
- TB oracle checks (`game.tb` checks, RUNNER=wine captures in `local/wine/compare/w13/checks/`): camera shake S 41 (slam landing, anim 75) and S 49 (underground lift, ARQ01 boss drop), the flagsA 0x8000 glide states 66 / 67 and VIQ01 entity 226, V 0x800000 water-surface snap of splash template 88 are trace-equal.
- TB: spawn yaw (tag 0x11) is kept raw as the exe does; camera nodes use the exe's integer matrix chain (the look was ±1 off) and flags10 0x20 nodes set the follow camera's restored pose.
- TB pole climb: a state with flagsA 0x100 grabs a pole (flags0 0x10000: GRANNY 19 / 115, AZCASG05 236) it touches
  ahead (0x456fb0), the climb tick keeps the climber on the pole's radius (side speed circles it, +0x136 faces it),
  0x44f690 stops climbing at the top and the mover's ground / wall contact lets go at the bottom; +0x1a0 (R 0x42) now
  round-trips through the mover image (0x4434c0's ledge probe bits). Trace-equal with two GRANNY oracle runs
  (`game.tb` climb: grab, ↑, top, ← / →, R1, ↓ to the bottom).
- TB focus: GD+0xA4 is GAME_DATA (logic reads it: GRANNY's camera director 338), the focus pointer DAT_004d236c moves
  in the next tick's view-list pass and the camera follows it a tick later (R1 on the pole: camera and the
  eye-facing pole now match the oracle). The entity pool limit also counts the template and system slots.
- TB follow / look camera: the sphere sweep is the exe's 0x440360 (up to 11 passes over the gathered polygons and
  the shapes of nearby entities solid to the camera, push-outs, slides along up to 8 kept normals) instead of an
  approximate swept sphere (`TbCollision::sweep` / `gather` removed). After the look view is released the eye and
  body now match the original tick for tick (class 30 check compares them throughout; the oracle presses and
  releases L2 one tick before R2); the GRANNY shade chase ends on the oracle's camera (eye (6201, −3258, 6399)). The
  homing jump is checked in `game.tb`: TRBOSS02's cannon spawn launches at Bugs and flies as the oracle's rows
  1587–1595 (position, velocity, yaw).
- TB: a state re-entered with a restarted clip re-applies frame 0's attach packet (0x468280), so the look view's
  class 30 shot (state 237 re-entered every tick by block 4's event 149) spawns as in the original; the look view +
  class 30 check runs in the normal `game.tb`. Homing jumps (state flagsB 0x100000, 0x44d990: apex over the target's
  middle, flight ticks, |d| / ticks, flagsC 0x100 lead), face-the-target (flagsA 0x4000), back-to-contact (flagsB
  0x4000), the jump's detach and pitched impulses are ported. Shadow rays that hit an entity shape take its normal
  (rayHit returns the 0x4463e0 shape normals; TRCA01 sphere oracle). The GRANNY shade check is rand / state synced:
  Taz's idle fidget (block 15 roll) now matches the original tick for tick.
- TB blob shadows (0x464480 / 0x464090) are drawn in GL and Vulkan: each listed entity with a shadow id (tag 0x45,
  U 17) gets its shadow entity's role-12 model on the floor 2250 below (or its platform), turned onto the floor
  normal, x / z under the hips, shrunk with height, shifted 5 down / 9 away in view space. GRANNY's shade floors
  (surface flag 0x80) darken Bugs and Taz to half brightness, 2 steps per tick. Trace-equal with the original
  (hooked 0x464090, `local/wine/compare/w13/shadow`, `game.tb` shadows). `BBTIME_TB_START` also places Taz.
- TB brightness vs the original: no renderer change; the earlier 1.12× gap came from `settings.ini`
  `tb_brightness = 1.125` (the Video page sets it, as in the exe) against the oracle's 1.0 (RUNTIME_REFERENCE §35).
- TB throw: □ while carrying a flags0 0x800 item (state 160, flagsA 0x40) throws it; the 0x450650 clamp pulls it
  back from behind a wall. Trace-equal with the original (Granny, item 267 made throwable, 420 ticks incl. the
  landing on Bugs' head, `game.tb` throw). The camera sweep 0x440360 now caches the shapes of nearby entities
  (0x448ce0) as the exe does; players' +0x24 0x800 follows their state's flagsB 0x1000. Class 30 (0x44d1f0, the
  look-view shot, template 151) is ported but not yet oracle-checked.
- TB carrying (0x453de0): Bugs / Taz pick up carriables (state flagsB 2 probe 0x44e7d0 and carriable contacts raise
  +0x20 0x200000; △ grab block hangs the item on the hand part; put-down / throw release block with the 0x450650 wall
  clamp and the nested release 0x44e6d0; V 0x200 with 0x20000000 clamps too). Trace-equal with the original (Granny
  item 267, 481 ticks, `game.tb` carry). Also: the per-tick +0x20 head clear, flagsA 0x8000 vertical move, V 0x800000
  (snap to a water zone's surface, keep-world attach with 0x40000), the entity pool limit on spawns, S 74 on the
  camera-script flags; 0x4463e0 queries take entity masks / no self and return the shortened ray.
- TB logic ops: every R / S / U item of the 58 levels now has its exe handler (`game.tb` opsScan, 123 537 items). S 119
  (pause menu Quit Game) quits to the front end, the save slot 0x48840c is known to the world (R 0x52), camera shake
  (S 41 / S 49, 0x439650) moves the look point (no oracle trace yet), and L2 + R2 opens the look view: the focused
  character is hidden and HUD sprite 29 is the cross-hair (with the ability cheat), from the oracle's view eye
  (`local/wine/compare/w13/tb/side_look_mode.png`). Proven to have no effect on PC and
  so not drawn: the draw bias (S 127, a PS1 ordering-table value no PC draw path reads), the near / entity draw
  distances (S 109 / S 117) at the high detail level, pad rumble (0x40fc30 reports no force feedback), the actor
  line-of-sight W mode (no level uses it) and spawn ids 82 / 83 / 185 / 252 / 284 (no such template in their levels).
- TB fades and S 50 flashes multiply the frame per channel as the original does (every colour × flash colour ·
  dark / 2²⁴, a near-black tinted blink): a red flash leaves green and blue at 0 (oracle
  `local/wine/tbintro/w13_flash_red.sh`, `local/wine/compare/w13/tb/side_flash_red.png`). Both renderers gained a
  frame-multiply 2D draw.
- TB classes 2 / 4 / 5 / 18 / 21 / 33 / 40 (texture-link writer, sprite, pushable block, seesaw, swinging platform,
  hung entity, VIBOSS01 plank) and the 0x453de0 ride / push blocks equal the Wine oracle row for row (`game.tb` classes).
- TB class-0 entities draw their static model: ARCA01's three wall discs and the wall around them were missing
  (`local/wine/compare/w13/tb/side_arca01_freecam.png`); it is the only class-0 entity with a model in the 58 levels.
- TB speech plays: a state with mode 0x80 (Granny's welcome, her dialog, Daffy in RETRY) or logic action 62 starts clip
  `Datas/SPEECH/<language>/Speech<n>.xa` on its own stream at the music volume; the level music stops while it speaks
  and restarts from its start after (0x42a030 / 0x4291e0). Without a sound device or file the exe's timer ends it.
  `ctest -R game.tb_speech` checks Granny's clip against the Wine oracle.
- Install manifests (`bbimport`) escape path bytes that are not well-formed UTF-8 as `\u00XX`, so the JSON stays valid.
- Launcher: after a hand-off from a pre-0.10.1 launcher, the user's AppImage is found by its AppImage magic as the FUSE
  child of the launcher's process chain, so renamed AppImages are refreshed too. Replacing a glTF model also takes the
  external files a `.glb` names.
- Tools: `export_level.py` walks TB levels with `tb_sec1`'s tag tables instead of a byte pattern; `tb_world.py` refuses
  a world chunk with a non-identity rotation (none exist: `--check`).
- TB cheat codes (0x40fbc0 / 0x43bce0): typing RULETHEEARTH arms the pad sequences ○○○○ + three of □ △ ✕; ○○○○□□□ unlocks the pause menu's level select (template 0x1d state 312, oracle `local/wine/tbintro/w13_levelselect.sh`: Down + ✕ loads level 7), the others give clocks 999, abilities, health / lives / partner revival, and GD+0xd2..0xdd.
- TB language (settings.ini `tb_language`, the front end's Language page, GD+0x93): each level's language section (EN 6, FR 5, DE..NL 7..11) and its entry of the speech and text tables.
- TB pause-menu key names follow the live key maps (captured keys included).
- TB free camera (F1 / `--camera`): the view list and portal walk cull from the free camera, so the ARCA01 palms it sees are drawn (`local/wine/compare/w13/tb/side_arca01_freecam.png`).
- TB actor faces bound to a texture link animate like the world's linked faces.
- TB CameraTrackPath pool (steering flagsB 0x100, U 20): slots taken and freed as the exe; an entity without one ends its tick.
- TB 0x453de0 tail clears +0x20 / +0x24 / +0x28 with the exe's masks; sky domes rotate their first part inside the entity yaw.
- LiT language (settings.ini `lit_language` = config.pc +0x08, G[0x10]): the level text block and the speech section / bank of that language (sections 6, 5, 7, 8, 9, 10), and the speech clips themselves from `speeches/Speech0N.xa` (clip table 0x45c340, played on the music stream; the track restarts after).
- LiT gun shots (type 31 spawned by an f3 0x20 hand event, 1930s levels) fly along the gun's hand matrix instead of aiming at Bugs; f3 4 / 2 hand spawns add the part's yaw / pitch; hand spawns take the actor's rotation.
- LiT respawn point 0x4b2460 is no longer reset at a level load: 29 levels never set it, and keep the previous level's (0 after start-up).
- LiT arc-jump targets are looked up in collision-list order (L03A, L03C share ids); clips with no asset return 0x400 then finished as the exe.
- LiT stick steering while attached (riding a platform, on a rope, carried) follows the exe's +0xc4 branch: no lean, always camera-relative, the turn goes to his local yaw and his heading comes from the coordinate chain (L01A seesaw ride with Left / Right trace-equal, `testSeesawSteerL01a`).
- LiT camera nodes are found as 0x44c4b0 does (drawn actors only, flag 0x200 only for the camera actor, camera yaw / distance updated), start after the first tick, and stop every actor timer while a non-camera node holds the view (L02A1 intro trace-equal, `testCameraNodeTimersL02a1`).
- LiT A17 zone messages: a queued zone (L04A1 zone 31) is flagged in the portal walk, so it is neither drawn nor walked, as in the original (`testZoneQueueL04a1`, `render.test_zones`); actors in a flagged zone stay visible only with flags8 0x10000.
- LiT respawn point carried into the next level checked against the original (L01A → L02C2); Bugs' spawn yaw keeps the raw level value.
- TB camera script points ('_' op 0x15): 0x439700's scripted branch now places the camera at the eye (B+0x48, flag 0x400) and look (B+0x58, 0x800) points, easing by 1/4 with 0x20; 0x4000 / 0x8000 make them the restored pose, and the one-shot bits 0x10000 / 8 drop them after one tick. The camera track-path ops are proven inert on PC: no level has a path recorder (flags1 0x40), and `TbWorld::load` flags one if it appears.
- TB Controls page: the key-capture prompt line had garbled glyphs, because FreeType's .fon face has no charmap and glyph = code − first + 1. It now matches the Wine oracle pixel for pixel (952/952 green pixels; `local/w13/tb/front/side_capture_jump.png`).
- TB idle fidget test now replays trace G on GRANNY, the level it was recorded on. That level's role-40 asset 144 has 6 frames (oracle peek), so state 23 holds 10 ticks; the test now runs through tick 900 (23 → 24 → 25 → 8 → 26/27/28).
- TB pause menu "Quit to Granwich" → YES: test against the oracle (`local/wine/tbintro/w13_quit_granwich.sh`: GD+0 = 4 and restart on the same tick).
- LiT carrying vs the original: pick-up links the actor without placing it (its own update puts it at the hand the next tick), and Bugs at rest with no contact probes (0, −50, 100) in his frame (0x433380: 0x4b3c00, flags14 0x40000, the actor's 0x8000). L01a crate 234 pick-up / carry / put-down / respawn is trace-equal to the Wine oracle for counters 105-1285 (`testCarryL01a`).
- LiT trigger 0x2000 on Bugs (0x437510): the misaligned "move" at move-list head + first move id is read from the exe's in-memory move list (`LitWorld::moveImage`, layout read under Wine in LB02); it clears his clip and state id unless that byte has 0x40 and its u16 equals his anim.
- LiT camera reset (0x431160) clears the cut-scene lock G[0x36]: leaving L02A1 mid-intro no longer carries permanent letterbox bars into L02A2 (oracle: 31 at the exit, 0 at L02A2's first counter; `testExitL02a1`).
- LiT ground shadows: 0x44b4f0's first pass puts a shadow onto the top plane of a platform (flags8 8 / flagsC 0x2000, class 2) between the caster's feet and the ground (L01A seesaw shadows equal the oracle's 0x4efac0 records); actors riding a platform take its tilt; each shadow quad uses its owner's far value (0x44cb90).
- LiT window keys as bugs.exe: F2 sound on/off, F3 music on/off (volume kept, sounds paused / the track restarted), F4 quit (to the collection menu, as A128). bbtime's own keys moved: F6 fidelity, F7 diagnostics overlay, F8 fog (F5 free-fly unchanged).
- TB world zones and view flag 1 (0x45eee0 @0x45f5d2): chunk-0x20 zone models (tag 0x21 slot, octree root box, portal list at model +0xc) are parsed; each tick the world entity takes the zone of the camera body (FindZone 0x460520, else the focus's) and the focus its own; the render point rebuilds the visible-zone table 0x4d56a0 with the portal walk 0x465710 / 0x406840 (640×480 float projection, y up; ARBOSS01 tables equal the Wine oracle within 2 px). Entities with view flag 1 tick only when their zone byte (tag 0x1f, spawns take the spawner's, children their parent's) is listed, and outside the camera's zone only when their GTE screen box meets that zone's portal rectangle: ARCA01's zone-2 directors no longer tick (capture B). Culled flags0-0x1000 entities release their model instance (anim restarts, LOD 0), and the app draws listed entities only when in view (+0x24 & 4, 0x465130).
- TB mover glue: attached entities hang on their parent part's coordinate (+0xe8) with a local +0xfc, as the shape cache composes them; the view list's corner test chains the parent part too.
- TB classes 9 / 39 (first-tick model setup, 0x44d6f0 / 0x450e00) and 10 (0x450df0: write-only global, no effect) leave the unsupported list.
- TB pause pauses the sound like the exe: while S[9] = 1 the music stream stops in place (0x429180) and every playing effect buffer stops in place (0x42b5c0); on resume the music goes on where it stopped and copy 0 of each stopped sound plays on, as do looping sounds (0x42b680). Music changes made while paused wait for the resume (0x407460). Start on either pad record opens the pause, as in 0x4014c0.
- TB pad layer transcribes 0x461100 / 0x460f80: the remap words of preset GD+0x95 / +0x96 (0x460d80 at every level load, table 0x4ceb14) apply to □ ○ △ ✕ L1 L2 R1 R2, not on the digital branch while paused; the joystick axis goes through DirectInput's −127..128 range with dead zone 2000 (0x40ecd0), sets the d-pad bits past 0x10 / 200 (0x40e710), and otherwise scales by the exe's 0x4d..0xb1 dead zone (trunc((b − 0x4d)·128/78)).
- TB front end Controls pages: waiting for a key shows the exe's green status line ("Set a keyboard Key or click Back to cancel !", string 0x64f, 0x416590) in the stock DC font at the bottom; a gamepad button shows string 0x671 and keeps waiting; the mouse keeps working, so Back cancels.
- TB demo playback is proven absent on PC: it needs level 0 (TITLE) or 2 (LANGUAGE), which the PC disc lacks and no level change targets; the exe has no record / playback switch. The 8 levels that still carry PS1 demo streams are listed by game.tb.
- LiT Bugs carries actors (0x43a880 +0x6c): pick-up move flagsA 0x20 (last contact 0x4b3c00, big flags8-0x2000 actors re-probed with 0x433380), put down flagsA 0x40 / throw flagsB 0x40000 (0x433500 mask 0x20100 + wall trace of the release point), the carried-box wall clamp and the tail box variants (longer z1 / the carried box with flagsB 0x2000), rule held-actor mask +0x1c and the +0x0c carried-state command, hand hits with the carried actor, releases on death / float / grab; carried children hang on his packet-10 part chain. New type 34 held item (0x446990). Pushable blocks (0x445a60): box scaled at the first call, the 0x432fe0 solid-overlap trigger, their own 0x433500 push, free mode 4, trigger list +0xfc, push sounds (0x4467cd), zone triggers. Bugs' body attack uses 0x432fe0, 0x433380 walks the collision list with the full rotation; 0x433500's corner-in-quad pass is compiled out in the exe (proven, the stand-in removed). LS01 releases sound 0x4a on Bugs' clip ends (0x43ea74 / 0x43eb31); LiT sound entries with flag 0x100 add the emitter's first-part translation (0x41f620). 0x437b90 unlinks Bugs from his parent and keeps the running clip.
- TB anim logic lists run on the tick that counts the frame hold down to 0 (0x469770 @0x469a41), not on the frame step. The swim and jump traces now replay the oracle's held pad word (GD+0xa; the old rows carried the pressed edge): swim 899/899 and jump 479/479 rows equal the oracle.
- TB view list honours the level's list sizes (chunk 0x3a tag 0x3d @0xc view, @0x20 collision; 0x45eee0): an overflowing view list ends the pass.
- game.tb guard test replays capture B's per-tick seed (like the F9 Taz spin) and checks each 401 variant against the capture that shows it; the F9 run starts Taz at the oracle's health 4.
- LiT pad remap presets 1–3 (0x448fa0, table 0x4ae1f0) applied at every level start from G[0x11]; settings.ini `lit_pad_preset`, Options "LiT controller". Paused, the pad is not remapped and has no ✕/□/L1 press window, as in the exe. Verified against the Wine oracle's remap words and held words.
- LiT pause menu Video and Controls pages work: resolution list, fog distance, display and renderer (A106–A108, A111–A118) with the exe's Apply rules (windowed modes step below the desktop size), key capture with swapping (C94) and joystick button capture (C95). Changes go to settings.ini (`bind.*`, `lit_joy_buttons`, `width` / `height` / `fullscreen`, `lit_fog_distance`, `lit_renderer`) and resize the window like the exe's display re-init; the fog distance feeds the Original Fidelity draw distance. The gamepad follows the joystick button map.
- LiT music volume uses the exe's DirectSound attenuation ((v − 100) dB) instead of a linear gain, and below 50 the zone music no longer changes; the pause menu's volume changes are saved.
- LiT demo playback of a level's demo stream (chunk 0x47; no PC level has one) and the exe's `/r:` / `/p:` pad tape as `--record FILE` / `--playback FILE` (same file format as bugs.exe writes).
- The exe's "controller removed" pause is dead code on PC (PadGetState always reports a pad) and stays unported.
- LiT texture links follow the world (alias table 0x4e5e80 written by type-2 / type-20 actors, 0x438830 / 0x445500): LS01's time-machine lights and vortex line up with the original counter for counter (`testLinksLs01`: rand seed and 13 links equal every counter 3–3001).
- LiT TraceGround 0x434cd0 transcribed exactly (cell / sub-cell walk with the 16-entry history) instead of the sub-cell approximation.
- LiT player box top +0x52 starts at the entity's tag 0x1c / 0x42 value instead of −400.
- TB particles are simulated and drawn (tb_fx.cpp): emitter slots 0x4d6ae0 with every S 118 field (0x452060), emission 0x452ab0 (V 0x80200000), update and draw 0x453350 / 0x452e80 (sprites, water-surface rings, gouraud lines, re-birth on the surface), S 138 slot clear, actor touch 0x447ee0 and R 0x48. Granny pond splash: `game.tb` fx replays 433 oracle particle states, all equal (local/wine/compare/w13/fx); emission rand use checked against the oracle seed (emitcheck.txt). Drawn in GL and VK; side-by-side local/wine/compare/w13/fx/side/side_granny_splash.png.
- TB weather streaks (S 110, 0x451370 / 0x451200: VIR01, VICASG03/04) and '^' effect spawns (bits 1, 2 with 0x2000, 8; the other bits occur in no level).
- TB rig swaps (V 0x80020000, 0x469320; TRCA01) and the distance-LOD rig swap (0x45eee0 view flag 0x400, `lodTick`).
- TB sky domes are a background layer as in bblit2 (`0x465710` layer 0, `0x422eb0` depth clear before the world): behind
  everything at any distance, own far clip (64000 with entity flag 0x2000000 `Mesh::farClip`, else the level's), no
  fog; Enhanced water reflects and fades into the dome. A dome whose view flags (tag 0x39 @0) have 0x20 is never
  listed and never drawn (VIR01's second dome no longer covers the sky). Original Fidelity VK draws domes with the
  exe's CPU projection (`0x408990`, glOrtho 0..65536). Octree primitives with a vertex beyond model x/y ±32000 are
  dropped as `0x408990` / `0x41eee0` do. Each dome sits where the exe draws it: at its first draw item's coordinate
  (`0x465710` composes the matrix from item 1, then draws the LOD item's model without item 2's own offset), i.e. the
  first part of its class-1 skeleton after the class-0xb animation the init `0x44d6f0` plays — VICASG04 / VIR01 domes
  6000 lower (the peach peaks / big mountains now sit on the horizon as in the oracle), AZCASG05 / AZQ01 rotated
  (oracle peeks `local/w13/sky/peek_*.txt`, side-by-sides `local/w13/sky/side_sky_*.png`).
- Enhanced water fills TB water zones (the exe's 0x1000 trigger zones, `0x44f550`) that hold the level's own water
  layers, at the surface the game measures depth from: the Granny pond (only scattered additive patches in the data)
  is now stylised water, and Bugs swimming in it leaves a wake (`local/shots/w13/side_granny_wake.png`).
- Translucent meshes are sorted per polygon by mean eye depth (`0x413440`) in GL and VK.
- Original Fidelity draw distances from the exe: LiT per-object fog end `0x44cb90` / `0x40cc80`; TB clips at the
  world's far slice (chunk 0x3a tag 0x3c @2; the exe culls world octree nodes there and draws models to 32000, bbtime
  clips every pixel at it).
- LiT ground shadows use the exe's view-space offset (`0x44bb50`: 5 down, 9 away) instead of a 24-unit pull
  (bugs.exe's view keeps PS1 y down: live view matrix `0x4efb00` row 1 (3, 2558, −70)).

- TB: Bugs dives when □ is pressed in water. The move vector now goes through the exe's yaw · roll · pitch matrix
  (`0x453de0`), so he swims down along his pitch to the pond floor as in the original (Granny pond oracle
  `local/wine/compare/w13/tb/swim_sfx`). The whole steering block of `0x453de0` is transcribed into
  `tb_steer.cpp`: wander, face/flee pitch, sidestep, hover, lean/flight/roll steering, camera-capped turns, face
  camera, pitch level-out and roll reset.
- TB swim sounds match the original: entry and dive splash (id 7) and jump-out (id 19) start on the same ticks at
  levels 100/100. The SFX config volume now defaults to the exe's GD+0x97 = 0x80 (was 127). New `game.tb` checks
  `swim` / `jump`.
- TB logic `T` conditions 5–13 (`0x4cd8e4`): surface flags of the last floor (+0x144), state flags, box index,
  link id and camera path node. The Credits level shows its three HUD sprites (`0x4622d0`).

- LiT Original Fidelity draws blended faces the way bugs.exe's GL renderer does (`0x4124b0`): they write depth, are
  sorted per triangle back to front, Bugs' and shadow faces come last, and depth is the exe's screen-linear eye
  depth. The dark bars under the LS01 time machine now show as in the original, the LS01 floor's glow layers meet
  along clean edges instead of stair steps, and the CCMERLIN moat is black like the original's (GL and Vulkan).
- LiT cut scenes get the original's black letterbox bars (`0x44c4b0` while the cut-scene lock `G[0x36]` is set;
  CCMERLIN walk to the castle: 57 / 56 rows at 640×480, as in the Wine oracle).
- TB sky domes follow the camera as in the original (bblit2 `0x465710`: the dome is drawn at the view eye, lifted by
  its per-level offset) instead of standing at the entity's map position. The sky now matches the original from
  anywhere in a level, and with Enhanced water VIQ02's distant icebergs sit on the horizon instead of sinking into the
  sea.
- LiT: a carried Bugs' ground shadow follows him through his carrier's part this tick, like his model (`0x44ba00`
  walks the coordinate chain), instead of trailing a tick behind.
- TB swimming: water trigger zones (chunk 0x09 with +0xc 0x1000: Granny's pond, VIQ01/VIQ02/VICA01, AZQ01, ARCA04,
  TRQ01/TRQ02 and others) now work as in the original. Bugs and Taz fall in with a plunge, float up to the surface,
  swim with the stick and jump back out with ✕. The water depth, the "shallow" flag and gravity-off follow the exe
  (`0x44f550`, `0x453de0` swim branch). Matches the Wine oracle tick for tick: 264 ticks of falling in, swimming,
  turning and jumping out in Granny's pond (`ctest -R game.tb`). `TbWorld::waterSurface` reports the surface above a
  swimmer for the renderer.
- TB steering now matches the original exactly (Wine trace E: every tick equal, was up to 4 units/tick off).
  - The turn step applies a remainder smaller than 4 at once.
  - Keyboard Left / Down give a stick of −128, not −127.
- TB movement details from the original:
  - A character moved into open air falls with full gravity on the first tick.
  - The at-rest flag follows the mover.
  - Jump states mark falling at the apex (the no-grab tails of the edge and ledge probes).
  - Zone blocks 8 / 9 match the player in control and the partner, not Bugs and Taz.

## 0.11.0 — 2026-10-23 (Wave 11: stylised interactive water, LiT carry fixes)

- Vulkan Enhanced: stylised interactive water (`water = enhanced`, `--water`, Options, launcher; ported from the Joan
  of Arc port): teal-to-deep-blue water over the refracted bed, shoreline foam, ray-traced / screen-space reflections,
  sun glints and waves the characters make (wakes, bobbing, splashes). LiT L03A's sea and L02A1's river become real
  water surfaces. `water = cartoon` is the same water toon-shaded (flat colour bands, crisp foam rims and glints);
  `water = original` and Original Fidelity render exactly as before.
  Open seas drawn as void + surf lines (TB VIQ01, VIQ02, VICA01, LiT L03A2) become water out to the horizon — told
  from a river or moat (CC2A, TRCA01, Merlin) by how far one sees over the water — and keep the original's glowing
  surf / ripple lines; LiT L02A5's sloped stream is water over its whole width; the water takes the colour the
  original gave it (TB TRQ01's pink fountain, AZCASG05's lavender stream, LiT L02A5 / L03A navy), translucent
  sheens keep the teal look.
- LiT: Bugs no longer trails the time machine during the LS01 fly-in or his carrier on the CCMERLIN walk. The
  carrier's attach item now places him on its hand part at once (the fly-in re-runs it every tick, so he follows the
  machine on the same tick as the original), and a carried Bugs is drawn through his carrier's part chain like the
  original's renderer. Positions equal the Wine oracle every counter (`testCarriedLs01`, RUNTIME_REFERENCE §26).
- LiT: level files missing from the game's level table (debug list variants such as L03a_8, L03acom_8, L_30Bank_0)
  were treated as the TITLE screen, so they ran 4:3-framed without Enhanced effects (no stylised water, FSR or far
  drawing) and with the title's Esc rules. They now take the id of the level they are a variant of.

## 0.10.2 — 2026-10-22 (fix: launcher crashed on Wayland)

- The 0.10.x AppImage launcher crashed at start on Wayland ("QOpenGLWidget is not supported on this platform",
  SIGSEGV): the AppImage lacked Qt's `wayland-egl` client buffer integration, so Qt had no OpenGL there, and the
  model viewer's GL widget made the whole window require it. The plugin is now bundled, and the viewer's GL widget is
  created only when a model is first previewed, after a GL context probe; without OpenGL the viewer says so and the
  rest of the launcher works.

## 0.10.1 — 2026-10-22 (fix: app menu entry kept starting the old AppImage)

- An AppImage installed through an AppImage manager (Gear Lever `~/AppImages`, Shelly `~/.local/bin`,
  AppImageLauncher) or a hand-made menu entry stayed at its old version after a launcher update: the menu entry
  started it, and it only handed off to the updated launcher (and older launchers did not even do that). The updated
  launcher now replaces that AppImage file in place with itself (atomic copy + rename, mode kept, symlinks followed),
  so the menu entry, desktop icons and the manager start the new version directly. The file is found through
  `BBTIME_ORIGIN_APPIMAGE` (set at the hand-off), `APPIMAGE`, or for a hand-off from a 0.9.x / 0.10.0 launcher the
  AppImage runtime process still mounting it (`bbtime` in the file name). A rollback never touches it (no hand-off
  to an older launcher).

## 0.10.0 — 2026-10-22 (Wave 10: LiT zone/portal visibility, PC blend modes, wide-FOV culls, draw distance, LS01 fixes, model viewer)

- LiT Original Fidelity semi-transparency as bugs.exe's GL path draws it (GL + VK): ABR 0 / 1 / 3 textures are
  alpha-blended with the PC texel conversion (rgb raised to a 255 max channel, alpha = max/2, max − max/4, max/4)
  instead of PS1 additive / 50 % blending, ABR-2 texels are black with luma alpha, and type-4 sprites take their
  texture's PC mode from the level's face / sprite usage scan (0x423eb0), not the record's ABR. LS01 "Nowhere" is no
  longer ~1.4× too bright with a white vortex and trails (vs Wine: MAD 6.5 → 3.8, mean 21.3 → 17.9, orig 17.6); the
  TITLE light beams now match. Enhanced keeps the PS1 look.
- LiT actors attached at a parent's animated part are drawn under that part's full matrix (rotation and scale
  through the chain, as GsGetLws), not just its heading: the LS01 clock-gate digits / clock no longer show a stray
  yellow "foot" bottom right at "Nowhere" (their parts are zero-scaled there, like the original), and cycling eras
  shows "1x" / "30x" with the clock assembled as in the original; L03ACOM's cannon has its original size. The
  crossed antenna on the time machine is drawn by the original too (unchanged).
- LiT carried actors / Bugs stay on the hand part their parent had when they were attached (the exe's +0xc4 =
  +0x68 at attach time), not on whatever part a later animation's attach packet names: Bugs no longer jitters out
  of the LS01 time machine seat while eras are cycled (seat = oracle every counter). Bugs carried at a hand part
  takes the part's heading: he faces his walk to Merlin's castle (CCMERLIN) instead of the camera.
- Launcher Mods page: **3-D model viewer** tab — previews a mod's model / animation glTF files and a level's
  original models (exported once by `bbtime-modkit` into `$XDG_CACHE_HOME/bbtime/viewer/`, replaced ones marked),
  textured, orbit / zoom / reset; static rest pose (`bb::mods::loadPreview`, `QOpenGLWidget`; docs/LAUNCHER.md).
  **Replace model…** copies a picked glTF (+ its relative buffers / images) into the selected mod at the model's
  resolver path after a parse check and part-name / overwrite confirmation; **Remove replacement** undoes it.
- Wide FOV: the actor screen culls follow the render view instead of the exes' fixed 4:3 / 51.3° frustum (LiT
  `+0x3c & 0x20` half-width `LitWorld::viewHalfX`, TB view list 0x45eee0 `TbWorld::viewTanX/Y`), so objects no longer
  vanish at the left / right edges with FOV 90 / Hor+. 4:3 at 51.28° gives exactly the exe values (Original Fidelity
  with pillarbox unchanged).
- Enhanced draw distance: actors / entities hidden only by their activation range (LiT `+0x3e`, TB `+0x76` range and
  its sleep) are still drawn, frozen and not ticked, so the simulation is unchanged (`far` flag). Original Fidelity
  now also hides LiT range-culled actors without `+0x3c & 0x40` (the exe clears their draw bit; they were drawn).
- LiT zone / portal visibility (exe 0x44c270): only the camera's and Bugs' zones and the zones their portals reveal
  in the render frustum are drawn (GL + VK, both fidelities), so LS01 "Nowhere" no longer shows the other eras' sets;
  zone-gated actors (`+0x3c & 1`) follow the same table.

## 0.9.2 — 2026-10-22 (fix: release channel stuck on 0.8.0)

- A manifest URL saved by the pre-0.9 launcher (e.g. `…/releases/download/v0.8.0/manifest.json` in settings.ini
  `update_url`) pinned the update channel to that release. Any URL into this repository's releases now follows the
  latest release, and the stale value is dropped from settings.ini on the next save.

## 0.9.1 — 2026-10-22 (fix: launcher ran an older installed game)

- The launcher started the game from an earlier updater install (`~/.local/share/bbtime/app/current/bbtime`) even
  when it was older than the launcher itself, so a freshly downloaded 0.9.0 AppImage ran the 0.8.0 game without ray
  tracing, FSR or the mod kit. It now uses an installed game / modkit only when it is at least as new as the launcher,
  otherwise the copy bundled with it.
- `bbtime --version`; the game prints its version at start and on the F3 overlay.

## 0.9.0 — 2026-10-22 (Wave 9: Vulkan RT shadows/AO/reflections + FSR 3.1 upscaling and frame generation, modkit, restyled launcher with auto-updates, TB intro/camera/pause, many LiT behaviours)
- Mod kit for both games (`docs/MODDING.md`): mods in `~/.local/share/bbtime/mods/<id>/` replace textures (PNG,
  any size — upscaled packs work), character / NPC / prop / pickup models (incl. TB entities the game builds from
  level data) and world meshes (glTF with the original part
  names; edited geometry, vertex counts and textures allowed; collision stays the original's), animation clips
  (replace or add by anim id, glTF), sounds (WAV) and music (Ogg / WAV); `mod.json` can swap a whole model for
  another (`"swap": {"tb:taz": "tb:bugs"}`) and alias clips. New launcher **Mods** page (enable, load order, new mod,
  install from .zip, remove, conflicts, validation, "Export assets…"), `bbtime-modkit` exporter / validator
  (originals as PNG / glTF / WAV in the mod layout, editable in GIMP, Blender, Audacity), `mods` setting and
  `--mods` / `--no-mods` flags, F3 line `mods: N active, M overrides`. With no mod enabled the output is unchanged.
- LiT attached actors and explosions (Verified against the original, RUNTIME_REFERENCE §24): an actor caught on another's hand now sits where the original puts it (the hand turns with its owner at once, the catch move is turned by the owner's heading and resolved against the ground), so L04B3's thrower keeps facing the ball like the original; sprite-built actors (e.g. the ball's explosion) get their hit box, reach and size from their sprite and the spawning hand, so L04B3's ball explosion now lasts its 16 ticks and damages the pillar and Bugs as in the original. Ground traces along one axis look one floor cell ahead like the original, so Bugs knocked back toward a ledge starts falling where the original does; actors falling into a level's kill volume are removed (trigger flag 0x10000).
- LiT actor collision fidelity (Verified against the hooked original, RUNTIME_REFERENCE §23): a spawned projectile's first move is pushed out of its thrower like the original (the exe hands 0x433500 the move as (x, x >> 16, y)), so the L04A1 / L04B3 throws now leave from the original's spot; attach / hand points use the exe's integer coordinate chain (no more 1-unit drift); Bugs gets the merged push of every actor he touches, in the original's collision-list order, re-traced against the walls (L02A2: the spawner's growing box now shoves him exactly as the original); attached actors step with their world yaw of the previous tick; flags14 0x1000 / 0x80000 clear when an animation reaches its last frame.
- LiT actor-vs-actor contact (0x433500 for moving actors): actors push each other, ride platforms (L04B3's thrower now lands on and stays on its pillar instead of falling out of the level, Verified), attach to their target on contact (flagsB 0x200000) and hurt actors (flagsA 0x20000); attached actors turn through their local yaw (Verified); Bugs stepping off a platform slides along walls; a carried Bugs no longer reports falling (Verified, L03C1).
- LiT: a carrier no longer hurts the Bugs it carries (L03C1 intro killed him every 60 ticks; the contact test now skips parent / carried links like 0x433500, Verified); camera focus actors (0x4b3950: the follow camera turns toward them, L03ACOM, Verified); the l04e cart leans into its turns (Verified); Bugs is drawn tilted with the platform he rides; trigger 0x2000 on Bugs ported.
- LiT sprite effects (behaviour 4: torch and lantern flames, dust puffs, sparkles in 38 levels) now animate and are drawn in both renderers as the original's camera-facing sprites (Verified against the Wine oracle in L02C4 and L02A2).
- LiT animation-rate steps (flagsB 0x2000000, A56; L03C1/L03C2 intro rides) and the TOD frame skip run as the original (Verified, L03C1); director flag 0x400000 (start another actor's state, LB02) ported.
- TB trigger zones (chunk 0x09, 0x44f550) run: ARCA01's zone starts the original intro fly-over (template 425's camera node, 0x464820) with its letterbox and caption, skippable with ✕ after 3 s, the camera springing out from behind Bugs afterwards as the original's reset — Verified against the Wine oracle (camera within 1 unit for all 670 ticks). `BBTIME_TB_GD=a9=20` no longer needed.
- TB music now follows the level logic at runtime (TRQ02 first visit → Track14 fallback, VIQ02 switches to Track7 at tick 27; Verified).
- TB follow camera transcribed from the original (0x437800): after the ARCA01 intro or its skip the camera now springs out and rises to the original's rest height exactly (Verified, every oracle sample); level scripts' camera settings ('_' sub-ops) apply.
- TB pause Inventory / era pages: page scroll, era page models (coins, medals, crates, statues), carrot / coin / % counters and Left/Right era paging as the original (Verified tick for tick); Level hints pages; the pause text panels slide in on the original's ticks (Verified).
- TB pause screen as the original: title, hint lines with key names (op 0x14), text panels (S75 / R 0x3b / S32), health bars over their panels, glyphs at the TB 512x240 scale, no dimming (Verified by side-by-side).
- TB camera and view list closer to the original: the follow camera resets behind the player after a far jump as the original does (2500-unit snap), class-16 directors can take over the camera target, the first level's camera starts at the player's height, and entities tick by the original's 51.3° view frustum with its box refinement. Entities now tick in the original's order — the player in control first, then the partner, then the rest (Verified: view list and every rand() call traced in Wine) — so idle fidgets and NPC random picks come out as in the original: Taz's spin into the ARCA01 guard matches the original for the whole 240-tick capture for the guard, Taz and Bugs (was 240 / 232 / unchecked); the knock-back slide stops dead as in the original. The Level hints page shows the original's ◂ / ▸ arrows (Verified).
- LiT pause menu: Esc pauses levels as the original (Backspace / gamepad Back leave to the collection menu); pause totals 0x449d80 (golden carrots, clocks, completion %, level lines), the menu's item-table / volume opcodes (C92, C93, C97, A102–A105, A109, A110, A119–A127) and the grey pause veil; Quit Game → hub. Verified against the Wine oracle (L01A).
- LiT camera shake A59/A68 (Verified, L01A start), camera target A85, type-36 eye point relative to an actor (0x20000).
- `BBTIME_LIT_POKE="C:x,y,z[,yaw];C:S2d=1"` replays oracle pokes at a level counter (rope / float side-by-sides).
- LiT actors carry each other and Bugs (coordinate chain at the hand part), anim-event grabs / releases, hovering flyers, pad-steered rides, slope rollers, aimed arcs, the 0x434740 side test and trigger 0x2000 (Verified: L02A2 carrier bird, l04e cart).
- LiT seesaws: Bugs rides platforms as the original's child coordinate, tilts the seesaw and slides off it — L01A
  matches the original counter for counter. Stationary type-14 actors (doors, props) no longer run the motion block
  (flags14 as the original), the 0x434740 side test uses the right box face, type-31 projectiles fly, hit and expire
  (0x446d10). The L02A2 bird is now tick-exact (the old one-tick lead was the test's poke timing). L04A1 thrower /
  doors checked against the original (`docs/re/RUNTIME_REFERENCE.md` §19).

- Collection menu navigation follows its layout: Left / Right picks the game, Up / Down moves to Options and Quit;
  the debug level list moved from Right to L (gamepad □ / X).
- PS1 originals as a second reference: `tools/psx/run.sh` runs the USA discs headless in PCSX-Redux (scripted pad,
  RAM / screenshot dumps, save states); the level loader also reads PS1 Time Busters `.BZZ` files (LZSS sections).
- Launcher in the games' own style: the TB menu spiral as background, the original TB menu picture buttons (hover
  glow), the Brady Bunch font and TB text colours, pages in the LiT title panel, and both title screens as game
  cards that pick what Play starts. Without imported games it falls back to painted rings and the system font.
- Updates need no setup: the launcher checks the signed manifest of the latest GitHub release (no URL field;
  `update_url` stays as a dev override). Releases now also ship the AppImage as the update artifact
  `bbtime-launcher`; after installing it the launcher restarts into the new one, and an older launcher hands off
  to a newer installed one on start (`docs/UPDATES.md`).
- Launcher: "Shadows (Vulkan)" choice for the new `shadows` setting.
- Vulkan, Enhanced mode: real-time sun shadows for the world and every character (Bugs, Taz, NPCs), with ray
  tracing where the GPU has ray queries — ray-traced shadows plus ray-traced ambient occlusion — and cascaded shadow
  maps otherwise. New `shadows = off|map|rt|rt_ao` setting (default `rt_ao`), `--shadows` flag and a "Shadows" row
  in the collection Options; the F3 overlay shows the mode in use. LiT's blob shadows give way to the real ones in
  that mode; Original Fidelity and the OpenGL renderer look exactly as before. `BBTIME_VK_NO_RT=1` forces the
  shadow-map path.
- Vulkan, Enhanced mode: ray-traced shading now uses smooth vertex normals (hard edges stay hard), so characters,
  rocks and creatures get round light/shadow edges and ambient occlusion instead of faceted ones; the AO is free of
  grain.
- Vulkan, Enhanced mode: ray-traced reflections on water, ice and polished floors (e.g. LiT pirate-ship water, the TB
  Viking ice rink, the Arabian marble square), new `reflections` setting (on by default, needs ray tracing), Options
  row and launcher checkbox.
- Vulkan, Enhanced mode: AMD FSR 3.1.4 upscaling (`upscaling = fsr_quality|fsr_balanced|fsr_performance|native_aa`) and
  FSR frame generation (`frame_generation = on`, one interpolated frame between each pair of rendered frames), as
  Options rows, launcher settings and `--upscaling` / `--frame-generation` flags. Off by default; the FidelityFX SDK
  (MIT) is bundled under `third_party/fidelityfx-sdk`.
- Frame generation is paced: a present thread shows the generated frame and then the real one half a frame later, so
  shown frames are evenly spaced with or without vsync (before, both went out back to back). FSR now also gets a
  reactive mask for glows, translucent water, sprites, animated textures and reflections, so they no longer smear or
  trail behind moving things.

## 0.8.0 — 2026-10-21 (Wave 8: TB saves + dialog boxes, LiT level music, TB menu extras, new collection menu)

- Time Busters saves in play: Start / Esc opens the original pause menu (save game, inventory, level hints,
  options, quit), and "save game" writes `Savegame<N>.dat` + `Savedata.dat` in the original's exact byte format
  (checked against saves made by the original under Wine), asking "use the same savegame file?" on later saves; the
  front end's Load page lists them and loading continues the game. Old files are kept as `.bak`.
- Time Busters dialog boxes: the paged speech / hint / caption boxes (e.g. "Welcome to Granwich!" over the GRANNY
  fly-over, Granny's briefing, key hints) are drawn as in the original — word-wrapped, centred, dark rounded panel,
  page arrows, ✕ / → paging — together with the cinematic black bars, which now also open and close so pausing,
  switching and calling the partner work again after a cut-scene.
- Lost in Time levels play their music: each level's zone table picks `Datas/track/Track<n>.xa` for Bugs' zone
  (L01A Track 2, L01D1 Track 4, LS01 by zone, …), switching when he changes zone; music stops during fades and speech
  and restarts afterwards, and the next level's track starts over the end of its loading screen (checked against the
  original under Wine); the help screen pauses it. The title keeps Track 25.
- New collection menu in the games' own style: both games side by side with their title art, named in their own font
  (LiT's in-game font, TB's Brady Bunch), the highlighted one bright; Options, Quit, the level list and the button hints
  wear the highlighted game's look (LiT: the title slot-menu panel and colours; TB: its configuration / load pages,
  colours and Back hand). Works at any window size / aspect in GL and Vulkan. `--autoplay` now also drives the
  collection menus (menu smoke screenshots).
- Time Busters front end, Video page: the resolution choices resize the window, Renderer and Level of details are
  remembered (`settings.ini` `tb_renderer` / `tb_details`; bbtime keeps drawing with its own renderer at high detail),
  and Brightness now works like the original's: it brightens or darkens every Time Busters texture from the next level
  (same colour maths as the exe, checked against the original at three slider values; `tb_brightness`). The slider
  steps in eighths like the original (it was finer before) and, as in the original, merely opening the page nudges an
  untouched brightness from 1 to 1.125.
- Time Busters front end: Esc on the title page or closing the window asks the original's "Do you want to quit?"
  (Yes / No, in the game's language) — Yes from Esc returns to the collection menu, Yes from a window close exits.
  The title page's Quit icon still leaves at once, as in the original. After 30 s without a key or click the original
  looks for attract videos (`demo1.avi`, …) that the PC disc does not include, so nothing happens; bbtime now does
  the same (and reports it once in the log).

## 0.7.0 — 2026-10-21 (Wave 7: original menus, original sound mixing)

- Both games clear the 3-D view and fade fog to black like the originals (was a hard-coded light blue: blue strip under
  the Lost in Time title copyright and blue gaps in the attic).
- Time Busters: the original front end (GDI title menu from `Datas/data/menu.men`, Brady Bunch text via FreeType).
  Enter on the collection menu's TB entry (or `bbtime --game tb --menu`): 1 / 2 Players → CCINTRO, Load Game (list /
  load / delete `saves/tb/Savedata.dat`), Configuration (key rebinding, sound volumes + bong, language), Quit → the
  collection menu; mouse as the original plus keyboard / gamepad focus; silent in Original Fidelity. Right / X on a game
  entry opens the level list. Builds now need FreeType (`libfreetype-dev`).
- Sound effects follow both originals' mixer rules: 24 voices, distance fade and pan from the camera, LiT pitch from the
  program's PS1 centre note (sounds played ~2.8× too high before), looping sounds with release/stop, C85/C87 and TB
  0x35/0x37 'sound playing', in-level speech bank; LiT levels and pitch match the original voice for voice (Wine
  oracle, L01A). TB sound effects now play, including footsteps, jumps, spins and NPC sounds from the
  animation-asset logic (checked against the TB oracle). settings.ini volumes apply at start-up.
- Time Busters: one shared rand() stream as in the original (animation-asset logic and particle-effect draws
  included, every call site traced in Wine), and class-16 director logic whose first block is block 0 now runs
  (ARCA01's particle setup); NPC random picks (patrol direction, throw variant) now follow the same stream.
- Lost in Time: the original front end. Enter on the collection menu's LiT entry (or `bbtime --game lit --menu`)
  runs TITLE.BZE with its own logic: camera-node view, Taz's spin and Bugs out of the hole, PRESS ENTER, the slot menu
  with Savedata summaries, Load (skips empty slots) / New game (→ CCINTRO) / Quit (→ collection menu), Esc back to the
  slot menu. Right / gamepad X opens the old level list. Fixes on the way: C79/C80 pad masks, C69 Start mapping,
  type-14 forward speed gated by flagsB (not flagsA) sign, item flag 0x400000 (set another actor's state).
  Type-0 static props are one rigid object without animation (0x43eee0), which fixes the stretched title copyright
  text. Autoplay keys P (Start) and E (Esc).
  The title keeps the original 4:3 framing (51.28°, pillarbox) in Enhanced too, so its 3-D slot panel lines up with
  the 4:3 menu text (`Settings::frame43`).

## 0.6.0 — 2026-10-20 (Wave 6, first public release)

- Release: portable AppImage (`tools/release/build_appimage.sh`, built in an Ubuntu 24.04 container, glibc ≥ 2.39)
  with the Qt launcher and the game, plus the signed update manifest for the game binary; published at
  https://github.com/sergen213/bugs-bunny-time-linux.
- Enhanced fidelity: overlay text glyphs scale with the window height (the original's 2× sprite cap left glyphs tiny
  and widely spaced at 4K); the debug overlay names LiT's Original Fidelity filter "linear".
- Time Busters: the original in-play HUD (coin counter / lives / health sprites and spinning models, slide timing
  matches the original tick for tick), overlay text (S30 / R 0x22 / R 0x23), screen fades (S51 / S52) and the
  level-change fade (16-frame fade-out, fade-in at speed 200). RETRY shows the game-over prompt in the simulation;
  YES loads GRANNY (`ctest game.tb`). The logic VM keeps S[0x1fe] per entity (+0x126) like the original.
- Lost in Time: ropes and chains (swing, grab, hang, R1 pump, climb with limits, jump off) and floating in buoyant
  columns (states 25/26: plunge, lift, bob, water jump); both match the original tick for tick (`ctest game.lit`).
- Lost in Time, Original Fidelity: textures are bilinear-filtered like the PC exe (was nearest; removes the stray
  texel blocks on near floors).
- Lost in Time: the original 3-D HUD (carrots, golden carrots, clocks, health carrots fly in when a counter changes;
  stopwatch timers, boss bar, item icons), drawn from the level's HUD template models in GL and Vulkan; matches the
  Wine oracle at L01A (`local/wine/compare/lithud/w6/side_c43_gl.png`). The text counter line is gone.
- Lost in Time: screen fades and colour flashes (logic actions A69/A70/A71), the level-change fade out, loading
  screen (the target's loading picture, 3 s minimum where the original holds it) and fade in, tick-exact to the
  original. Level entry now draws its first frame at the original's first presented tick, so app frame N shows
  simulation tick N + 1.

- Time Busters: NPCs, enemies, items and spawned objects are now drawn at their simulated position and pose (the
  entities the view list ticks); rigid props the simulation owns leave the static scene, so removed ones disappear.
  Placement matches the Wine oracle (`local/wine/compare/w6_tb_ARCA01_guard_*.png`).
- Time Busters level changes: a level exit or game over reloads like the original (level GD+0, GAME_DATA kept);
  game over loads the RETRY level, cut-scene exits continue (CCARIN01 → ARCA01). RETRY's continue dialog is not
  drawn yet.
- Time Busters combat: the attach point is cleared on every state change and mover contacts follow the original's
  contact-list rule, so Taz's spin into the ARCA01 guard now plays out tick for tick as in the original (the guard
  blocks it; `ctest -R game.tb`).
- Time Busters pickups: walking into an item collects it like the original — the item's collision box now follows
  its animation (it kept a degenerate box from the level tags), and the touched entity's program runs on contact,
  so the GAME_DATA counters and collected bit update, the item plays its pick-up and rise and then vanishes, all on
  the original's ticks (ARCA01 item 473 against a Wine oracle capture; `ctest -R game.tb`).
- Time Busters thrown objects: the ARCA01 guard's boomerang (template 401) flies out along its animation, hurts the
  player and comes back to the guard's hand; the guard's attack cycle after the throw now matches the original tick
  for tick (including its random pick, checked with the original's rand() seeds; `ctest -R game.tb`).

## Unreleased

- LiT overlay text in play: the original's script interpreter (C55/C56, A49/A51/A52) draws the level's text with
  the original font sprites (template 68, GL and Vulkan) at the exe's glyph positions, sizes and palette colours,
  including `>D`/`>J` key names. "Do you want to save your game?" now appears at the Merlin/LS01 save points
  (speech timing S[0x17] ported) and Yes writes Savegame/Savedata. F1 opens the original help screen (pause state 3:
  only menu actors run); the LiT debug free-fly camera moved to F5. Matches the Wine oracle glyph for glyph
  (`ctest -R game.lit_overlay`, `docs/re/RUNTIME_REFERENCE.md` §9).
- Time Busters logic VM completed for the players: conditions W, T, Z, [, ], ^, ` and the full R table, the
  0x45a120 target selector, spawn/attach/detach/kill actions, entity programs and created entities (attached to
  animation attach points). Bugs' idle fidgets now play and match the original tick by tick when its rand() seed is
  replayed (Wine trace G); after L1 the partner rides the arrival carrier down from 1432 above the spot exactly as
  the original (traces D/H). Status: `docs/GAMEPLAY_STATUS.md` (TB section).
- LiT enemies match the original tick by tick on L01A (template 932, `testEnemyL01a`): actor update order with
  same-tick spawns, the original's activation distance and zone-gated start, TOD models for templates and
  position-less entities, sprite flipbook animation, animation frame sounds and logic events, sound-chance rand,
  transcribed NPC steering, the actor box and hand attach point from the animation, and the box push between Bugs
  and actor boxes. The whole fight matches: knock-back, stomp, kick damage through Bugs' attach point, the enemy's
  defeat, bouncing carrots and the golden-carrot pickup (Bugs' fixed box, contacts only through the original's
  corner test). Wine captures and verdicts: `docs/re/RUNTIME_REFERENCE.md` §10.
- LiT pushable blocks: Bugs pushes type-5 blocks along their allowed axes (R1 while walking into one), matching the
  original on L01A counter for counter (`testPushL01a`, `docs/re/RUNTIME_REFERENCE.md` §11).
- Lost in Time play mode (`bb_game`, 30 Hz): choosing a LiT level plays it. Pad emulation (3-tick press window,
  dead zone 50), the per-level player state machine, walk/run/turn/acceleration, jump arc and fall, heightfield
  collision, follow camera with L2/R2 rotation, zone-trigger deaths and checkpoints, carrots / golden carrots /
  clocks with a text HUD. F1 debug free-fly camera, F2 fidelity, F4 fog. Enhanced mode interpolates between ticks,
  Original Fidelity does not. `--autoplay KEYS*TICKS,...` for scripted runs. Status: `docs/GAMEPLAY_STATUS.md`.
- Launcher `bbtime-launcher` (Qt 6): import wizard with install validation (game, executable version and SHA-256,
  file counts, BZE health) and optional copy of `Datas/` into `~/.local/share/bbtime/<game>/`; graphics, audio
  and key-binding settings; gamepad list; save backup/restore; game logs; changelog; launch button.
- `settings.ini` (`$XDG_CONFIG_HOME/bbtime/`) shared by the launcher and the game; command-line flags override it.
- Signed updater: HTTPS-only release manifest, Ed25519 signature (libsodium), per-file SHA-256, versioned install
  with atomic symlink swap, rollback, downgrade refusal. Release tools in `tools/release/`.
- Vulkan 1.3 backend `bb_render_vk` (`bbtime --renderer vk`, `--vk-validation`): dynamic rendering, sync2,
  bindless textures with GPU-blit mipmaps, same blend passes/fog/MSAA/pillarbox as GL, 2D layer, swapchain
  resize, vsync modes, pipeline cache in `$XDG_CACHE_HOME/bbtime/`, screenshot readback, ray-tracing probe in
  the F3 overlay (not used yet).
- Lost in Time saves in the original format (`docs/formats/SAVE_LIT.md`): `bbtime --game lit --slot 1-6` loads
  `$XDG_DATA_HOME/bbtime/saves/lit/Savegame<N>.dat` (or starts a new game in that slot), the in-level save prompt
  writes `Savegame<N>.dat` + `Savedata.dat` byte-compatibly with `bugs.exe` (old file kept as `.bak`),
  `--import-saves DIR` copies saves from an original `Datas/bin`.

## 2026-10-18 — f1fddad

- Time Busters C++ level loader.
- Gameplay reverse engineering: main loop, input, player controller constants, heightfield collision, camera,
  TOD animation, entities, saves, Bugs/Taz switching; roadmap status matrix.

## 2026-10-18 — 4f56e7d (wave 2)

- Real Lost in Time level rendering, TIM texture decoder.
- `.xa` / AIFF audio decoding and collection-menu music.
- Time Busters section-1 and world format reverse engineering.

## 2026-10-18 — b9ee0ac (wave 1)

- BZE container and LZSS decoder, read-only install importer (`bbimport`).
- SDL3 + OpenGL collection app with game-select menu.
- Reverse-engineering documentation for Lost in Time and Time Busters.
