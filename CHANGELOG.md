# Changelog

All notable changes to bbtime. Versions follow `MAJOR.MINOR.PATCH` (the updater refuses anything that is not
strictly newer). Entries before 0.1.0 are reconstructed from `git log`.

## Unreleased

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
