# Changelog

All notable changes to bbtime. Versions follow `MAJOR.MINOR.PATCH` (the updater refuses anything that is not
strictly newer). Entries before 0.1.0 are reconstructed from `git log`.

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
