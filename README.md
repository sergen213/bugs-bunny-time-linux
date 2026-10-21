# bbtime — Bugs Bunny: Lost in Time & Time Busters for Linux

A native C++ reimplementation of the Behaviour Interactive engine behind **Bugs Bunny: Lost in Time** (1999) and
**Bugs Bunny & Taz: Time Busters** (2000), with OpenGL and Vulkan renderers, a game-select collection menu, a Qt
launcher and signed updates. Behaviour is checked tick by tick against the original PC executables running under
Wine (`docs/re/RUNTIME_REFERENCE.md`).

**No game data is included.** You need your own copies of the original PC games; the launcher imports them.

## Status

Work in progress. Lost in Time plays through its levels (scripts, enemies, pickups, platforms, ropes, HUD, saves in
the original format). Time Busters plays on its first levels (Bugs/Taz switching, 2-player co-op, NPCs, pickups,
HUD, game over/continue); level intros, menus and several move sets are still missing.
Full list: [`docs/GAMEPLAY_STATUS.md`](docs/GAMEPLAY_STATUS.md), history: [`CHANGELOG.md`](CHANGELOG.md).

## Run

Download `bbtime-X.Y.Z-x86_64.AppImage` from the [releases](https://github.com/sergen213/bugs-bunny-time-linux/releases)
(x86-64, glibc 2.39 or newer), `chmod +x` it and start it. In the launcher, **Import original games…** and point it
at the installed or mounted games, then **Play**.

## Build

C++26 (GCC 14+), CMake ≥ 3.30, Ninja, SDL3, Qt 6.4+ (launcher), Vulkan + `glslc`, libepoxy, OpenAL, glm,
libsodium, libcurl, nlohmann-json.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
BBTIME_DATA=/path/to/original ctest --test-dir build   # real-data tests skip without BBTIME_DATA
./build/apps/launcher/bbtime-launcher
```

Release packaging: [`docs/UPDATES.md`](docs/UPDATES.md). Launcher and settings: [`docs/LAUNCHER.md`](docs/LAUNCHER.md).

## Legal

Unofficial fan project, not affiliated with Warner Bros., Behaviour Interactive or Infogrames. Looney Tunes
characters and the original games are the property of their owners. This repository contains only original code
and documentation.
