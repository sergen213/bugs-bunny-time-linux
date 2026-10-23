# Changelog

All notable changes to bbtime. Versions follow `MAJOR.MINOR.PATCH` (the updater refuses anything that is not
strictly newer). Entries before 0.1.0 are reconstructed from `git log`.

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
