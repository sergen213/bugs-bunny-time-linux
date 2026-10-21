# Changelog

All notable changes to bbtime. Versions follow `MAJOR.MINOR.PATCH` (the updater refuses anything that is not
strictly newer). Entries before 0.1.0 are reconstructed from `git log`.

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
