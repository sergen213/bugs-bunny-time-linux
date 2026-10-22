# Launcher (`bbtime-launcher`) and settings

Qt 6 Widgets front end next to the game. Status: **Implemented**, **Tested** (unit tests below; manual desktop
smoke on KDE Wayland). The game does not depend on Qt: both share `apps/bbtime/settings.{h,cpp}` (`bb_settings`).

## Files it touches

| Path | Written by | Content |
|---|---|---|
| `$XDG_CONFIG_HOME/bbtime/settings.ini` (default `~/.config/…`) | launcher (Save / Play / close) | settings below |
| `$XDG_DATA_HOME/bbtime/{lit,tb}/Datas/` | import wizard, only if "Copy" is chosen | copy of the original `Datas/` |
| `$XDG_DATA_HOME/bbtime/saves/{lit,tb}/` | the game | native saves (`bbtime::saveDir()`); LiT: `Savegame<N>.dat`, `Savedata.dat` in the original format (+ `.bak` of the previous file; `docs/formats/SAVE_LIT.md`, `bbtime --slot N`, `--import-saves DIR`) |
| `$XDG_DATA_HOME/bbtime/backups/<UTC stamp>/` | Saves tab | full copies of `saves/` |
| `$XDG_DATA_HOME/bbtime/mods/<id>/` | Mods page (New mod, Install from .zip, Remove) | mods (`docs/MODDING.md`) |
| `$XDG_DATA_HOME/bbtime/modkit-export/` (default, any folder can be picked) | Mods page "Export assets…" (`bbtime-modkit export`) | the user's own exported originals |
| `$XDG_CACHE_HOME/bbtime/viewer/<game>/<LEVEL>/` | Mods page 3-D viewer "Show originals" (`bbtime-modkit export --what models`) | cached original models for the viewer; safe to delete |
| `$XDG_DATA_HOME/bbtime/app/` | updater | see `docs/UPDATES.md` |
| `$XDG_STATE_HOME/bbtime/logs/bbtime-*.log` | launcher | stdout+stderr of games started from the launcher |

Original installs, disc mounts and ISO-extracted folders are **only read** (`bb::scanInstall`, `copyDatas` reads;
a destination inside the source is refused). XDG variables that are unset or relative fall back to the spec
defaults.

## settings.ini

Plain `key = value` lines; `#` / `;` start a comment line; whitespace around key and value is trimmed. The
launcher rewrites the whole file (comments are not kept). Unknown keys are preserved; malformed lines and
out-of-range values are skipped with a `line N:` warning and the default stays. Precedence in the game:
**defaults < settings.ini < command-line flags**; `--data DIR` or `$BBTIME_DATA` replace both game roots by
`DIR/lit` and `DIR/tb`.

| Key | Values (default) | Used by |
|---|---|---|
| `lit_root`, `tb_root` | folder containing `Datas/` (empty → `<data root>/lit|tb`) | game data per game |
| `renderer` | `gl` \| `vk` (`gl`) | backend select |
| `fidelity` | `original` \| `enhanced` (`enhanced`) | `bb::render::Fidelity` |
| `width`, `height` | 320–16384 / 200–16384 (1280, 720) | window |
| `fullscreen`, `vsync` | `true/false`, `1/0`, `yes/no`, `on/off` (false, true) | window |
| `fov` | 30–120 degrees vertical at 4:3, Hor+ wider (60) | `Settings::vfovDeg` |
| `msaa` | 0, 2, 4, 8 (4) | `WindowConfig::msaaSamples` |
| `shadows` | `off` \| `map` \| `rt` \| `rt_ao` (`rt_ao`) | Vulkan + Enhanced dynamic shadows (`rt`/`rt_ao` fall back to `map` without ray queries) |
| `reflections` | boolean (`true`) | Vulkan + Enhanced ray-traced reflections on water / ice / polished floors (off without ray queries) |
| `upscaling` | `off` \| `fsr_quality` \| `fsr_balanced` \| `fsr_performance` \| `native_aa` (`off`) | Vulkan + Enhanced AMD FSR 3.1.4 (render scale 1/1.5, 1/1.7, 1/2, 1); replaces MSAA while on |
| `frame_generation` | boolean (`false`) | Vulkan + Enhanced FSR frame generation (one interpolated frame per rendered frame; with `upscaling = off` it runs FSR native AA) |
| `music_volume`, `sfx_volume` | 0–100 (100) | audio |
| `tb_brightness` | 0.25–2 (1) | TB texture brightness = `config.pc` +0xe0 of the TB Video page (`bb::tbAdjustColor`) |
| `tb_renderer`, `tb_details` | 1–2 (2), 0–2 (2) | TB Video page renderer / detail choice, kept and shown only |
| `bind.<action>` | comma-separated SDL key names | keyboard map |
| `mods` | comma-separated mod ids in load order, later wins (empty) | game: mods applied to every level (`docs/MODDING.md`); `--mods ID,…` / `--no-mods` override |
| `update_url` | https URL of `manifest.json` (empty = the GitHub release channel, `docs/UPDATES.md`) | launcher updater only; tests/dev override, not in the UI |

Actions (`bbtime::kActions`) and defaults = LiT `config.pc` map (`docs/re/GAMEPLAY.md` §2.1): `up` Up, `down` Down,
`left` Left, `right` Right, `camera_left` Q, `camera_right` W, `roll` E, `action` R, `look` A, `crouch` S, `jump` D,
`kick` F, `select` Tab, `start` Return.

Game side (`apps/bbtime/main.cpp`, owned by gameplay): `bbtime::loadSettings()` before parsing flags, then flags
override. Verified 2026-10-18: with a launcher-written `settings.ini` (`width/height = 800x600`, `lit_root`/`tb_root`
= wizard copies) `bbtime --game lit --hidden --screenshot` loaded 167 LiT / 58 TB levels from the copies and rendered
at 800×600 logical (1120×840 at 1.4× scale); `--width 640 --height 480` overrode it (896×672).

## Look

The launcher wears the imported games' own front-end art, only read from `tb_root` / `lit_root` (case-insensitive
paths): the TB `menu.men` spiral page `Datas/data/pc03.bmp` as window background (LiT only: `Datas/ScrnBgEn.bmp`
darkened), the TB menu picture buttons (`Play2`, `Video`, `Controls`, `LoadGame`, `Load`, `Config`, `Language`,
`Quit`, `Play1D`; hover/selected = the `…D.bmp` variant) as the page column, TB's Brady Bunch font
(`bradybun.ttf`) with the TB text colours (red labels, yellow hover/selection, orange keys), and pages inside the LiT
title slot-menu panel (navy, two gold rules) — the same palette as the collection menu. Both title screens
(`ScrnBgEn.bmp`, `english.bmp`) are the game cards on the Games page; clicking one selects what Play starts (again:
collection menu). Without an import it paints the spiral as rings, uses the system font and text-only buttons;
the wizard re-dresses it on Finish. Not used: the LiT overlay glyph font (it lives in `TITLE.bze` and needs the
game's level loader). Screenshots: `local/shots/w9launcher/`.

## Pages

* **Games** — background `checkInstall` of the configured roots on start; "Import original games…" wizard:
  1. pick a folder per game (disc mount, install dir or extracted ISO; the one with `Datas/`);
  2. validation: detected game, matched executable + version (`bugs.exe` "LiT 1.6", `bblit2.exe` "TB EN ISO"),
     executable SHA-256, file count/size, BZE level count, decode failures. A game is usable only if the
     detected game matches the slot, an executable matched, `Datas/` exists and nothing failed to decode;
  3. use in place (read-only) or copy `Datas/` to `$XDG_DATA_HOME/bbtime/<game>/` with a progress bar
     (`<dst>.partial` then rename; cancel removes the partial copy). Finish writes `lit_root`/`tb_root`.
* **Graphics & audio** — renderer, fidelity, resolution (+ presets), fullscreen, vsync, FOV, MSAA, shadows, reflections, upscaling, frame generation
  (Vulkan), music/effects volume.
* **Controls** — editable key table, restore defaults; connected gamepads via SDL3 (Qt 6 has no gamepad module).
* **Saves** — back up now, restore selected (the current saves are backed up first as `…-before-restore`).
* **Mods** — installed mods of `$XDG_DATA_HOME/bbtime/mods/` (name, version, author, game); the tick enables a
  mod, ▲ / ▼ set the load order (top to bottom, later wins; saved as `mods = …`). "New mod…" creates the folder
  with its `mod.json` and the empty `textures/ models/ animations/ sounds/ music/` folders, "Open folder",
  "Install from .zip…" (archive with `mod.json` at its root or in one top folder; extracted with `unzip` or
  `bsdtar`, which refuse `../` paths; the folder is named after the `mod.json` id), "Remove" (deletes the folder
  after a confirmation), "Export assets…" (game / level / kind → `bbtime-modkit export`, next to the launcher or in
  the build tree), "Refresh". Below: the selected mod's details and validation (`bb::mods::validate`, run in the
  background; it decodes the named levels to check texture ids) and the conflicts of the enabled set (keys that
  several enabled mods provide, and which one wins). Screenshot: `local/shots/w9mod/launcher_mods_page.png`.
  The **3-D model viewer** tab lists the selected mod's `models/` and `animations/` glTF files and, after "Show
  originals" (game + level), that level's original models (exported once into the cache above; originals the mod
  replaces are marked). Selecting one shows it textured (base colour texture × vertex colour, nearest sampling, as
  the game): drag = orbit, wheel = zoom, double-click = reset. Static rest / bind pose only (no animation, no
  skinning). `QOpenGLWidget`, `bb::mods::loadPreview`. Screenshot: `local/shots/w10/viewer/`.
  **Replace model…** (button or right-click) copies a picked `.gltf` / `.glb` into the selected mod at the
  selected model's path (`models/<game>/<model id>`, `models/<game>/<level>/<asset id>` or `…/world`, the
  extension of the picked file; a file of the other format for the same model is removed) together with the
  buffers / images its relative URIs name, at the same relative place, so nothing is rewritten. URIs that are
  absolute or leave the mod folder are not copied (warning). The file must parse (`loadPreview`) and have
  triangles; missing `part<N>` / `<index>#` nodes of the original, a mod for the other game and every overwritten
  file are listed in one confirmation. No mod selected → offers **New mod…** first. **Remove replacement** deletes
  the mod's file for the selected model (mod entry or the marked original) and its side files that no other glTF
  of the mod refers to, after a confirmation listing them.
* **Logs** — game logs, newest first; live while a game started from the launcher runs.
* **Updates** — signed update check/install/rollback against the GitHub release channel; after an install that
  brings a newer launcher it restarts into it (`docs/UPDATES.md`).
* **About** — version and `CHANGELOG.md` (compiled in as a Qt resource).
* **Play** — saves settings, starts `bbtime` (`--game lit|tb` unless "Collection menu"): first
  `$XDG_DATA_HOME/bbtime/app/current/bbtime`, then next to the launcher, then the build tree
  (`../bbtime/bbtime`), then `PATH`.

## Build and tests

`bbtime-launcher` is built when Qt6 Widgets + Concurrent and SDL3 are found; `bb_settings`, `bb_updater`,
`bb_launcher_files` and the tests build regardless.

| ctest | Checks |
|---|---|
| `launcher.settings` | malformed lines (no `=`, empty key, bad bool/enum/range, trailing junk, unknown action), unknown-key preservation, format→parse round trip, atomic save, missing file, XDG fallbacks |
| `launcher.files` | Datas copy from a read-only source (progress, replace, cancel, refuse destination inside source), save backup/restore |
| `launcher.install_realdata` | `checkInstall` on `$BBTIME_DATA/{lit,tb}` (skips with 77 without it) |
| `launcher.updater` | see `docs/UPDATES.md` |
