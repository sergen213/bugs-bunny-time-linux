# bbtime — Bugs Bunny: Lost in Time & Time Busters for Linux

A native C++ reimplementation of the Behaviour Interactive engine behind **Bugs Bunny: Lost in Time** (1999) and **Bugs Bunny & Taz: Time Busters** (2000).

## What it is

bbtime runs both PC games natively on Linux, with OpenGL and Vulkan renderers, a game-select collection menu, a Qt launcher, a mod manager and signed updates. Behaviour is checked tick by tick against the original PC executables running under Wine ([docs/re/RUNTIME_REFERENCE.md](docs/re/RUNTIME_REFERENCE.md)).

### Status

Work in progress.

- **Lost in Time** plays through its levels: scripts, enemies, pickups, platforms, ropes, HUD, and saves in the original format.
- **Time Busters** plays on its first levels: Bugs/Taz switching, 2-player co-op, NPCs, pickups, HUD, game over/continue. Level intros, menus and several move sets are still missing.

Full list: [docs/GAMEPLAY_STATUS.md](docs/GAMEPLAY_STATUS.md). History: [CHANGELOG.md](CHANGELOG.md).

Optional Enhanced mode (Vulkan) adds ray-traced shadows, ambient occlusion and reflections, stylised water, AMD FSR 3.1.4 upscaling and frame generation. Original Fidelity mode keeps the original look.

## You need the original games

**No game data is included.** You need your own copies of the original PC games; the launcher imports them.

This is an unofficial fan project, not affiliated with Warner Bros., Behaviour Interactive or Infogrames. Looney Tunes characters and the original games are the property of their owners. This repository contains only original code and documentation.

## Download

Download `bbtime-X.Y.Z-x86_64.AppImage` from [GitHub Releases](https://github.com/sergen213/bugs-bunny-time-linux/releases). Each release also has a source tarball and `SHA256SUMS`. The launcher updates itself from the latest release (signed manifest; see [docs/UPDATES.md](docs/UPDATES.md)).

## Requirements

- x86-64 Linux with glibc 2.39 or newer (the AppImage is built on Ubuntu 24.04).
- A GPU and driver with OpenGL 3.3 (core profile) or Vulkan 1.3. Ray-traced effects need Vulkan ray queries; without them Vulkan falls back to shadow maps.

## Quick start

1. `chmod +x bbtime-*-x86_64.AppImage` and start it.
2. In the launcher, click **Import original games…** and point it at the installed or mounted games.
3. Click **Play**.

### Keys in a level

| Key | Action |
|---|---|
| Esc | Pause (Lost in Time: the game's pause menu) |
| Backspace / gamepad Back | Leave to the collection menu (Lost in Time) |
| F1 / F2 / F3 / F4 | Lost in Time: help, sound on/off, music on/off, quit (as in `bugs.exe`) |
| F5 | Lost in Time: debug free-fly camera |
| F1 | Time Busters: debug free camera |
| F6 | Original Fidelity / Enhanced |
| F7 | Diagnostics overlay |
| F8 | Fog |
| F11 | Fullscreen |

Settings live in `~/.config/bbtime/settings.ini` (for example `lit_language`, `tb_language`, `lit_pad_preset`, `lit_fog_distance`). See [docs/LAUNCHER.md](docs/LAUNCHER.md).

### Mods

The launcher's **Mods** page enables mods, sets their load order, installs them from `.zip`, exports the original assets for editing, and has a 3-D model viewer with **Replace model…**. Mods can replace textures, models, animations, sounds and music. See [docs/MODDING.md](docs/MODDING.md).

## Build from source

C++26 (GCC 14+), CMake 3.30 or newer, Ninja, SDL3, Qt 6.4+ (launcher), Vulkan headers and loader + `glslc`, Python 3, libepoxy, OpenAL, FreeType, glm, OpenSSL, libsodium, libcurl, nlohmann-json.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
BBTIME_DATA=/path/to/data ctest --test-dir build   # data dir holds lit/ and tb/; real-data tests skip without it
./build/apps/launcher/bbtime-launcher
```

Release packaging: [docs/UPDATES.md](docs/UPDATES.md).

## Documentation

- [docs/LAUNCHER.md](docs/LAUNCHER.md) — launcher pages, settings.ini keys and command-line flags.
- [docs/MODDING.md](docs/MODDING.md) — making and installing mods.
- [docs/UPDATES.md](docs/UPDATES.md) — signed updates and release packaging.
- [docs/GAMEPLAY_STATUS.md](docs/GAMEPLAY_STATUS.md) — what is implemented and verified.
- [docs/ROADMAP.md](docs/ROADMAP.md) — plan.
- [docs/RESEARCH.md](docs/RESEARCH.md) — prior art, executables and original data.
- [docs/formats/](docs/formats/) — file formats: [BZE](docs/formats/BZE.md), [models](docs/formats/MODELS.md), [textures](docs/formats/TEXTURES.md), [audio](docs/formats/AUDIO_XA.md), [TB levels](docs/formats/TB_LEVELS.md), [LiT saves](docs/formats/SAVE_LIT.md), [TB saves](docs/formats/SAVE_TB.md).
- [docs/re/](docs/re/) — reverse-engineering notes: [Lost in Time](docs/re/LOST_IN_TIME.md), [Time Busters](docs/re/TIME_BUSTERS.md), [engine comparison](docs/re/ENGINE_COMPARISON.md), [runtime reference](docs/re/RUNTIME_REFERENCE.md).
- [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) — development environment.

## Contributing

Changes are accepted only as pull requests, reviewed and merged by the maintainer. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Source-available, all rights reserved. See [LICENSE](LICENSE). Versions published before this license under a different license remain available under that earlier license. Third-party components (AMD FidelityFX SDK, stb) keep their own licenses.
