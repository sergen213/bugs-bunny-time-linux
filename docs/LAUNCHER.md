# Launcher (`bbtime-launcher`) and settings

Qt 6 Widgets front end next to the game. Status: **Implemented**, **Tested** (unit tests below; manual desktop
smoke on KDE Wayland). The game does not depend on Qt: both share `apps/bbtime/settings.{h,cpp}` (`bb_settings`).

## Files it touches

| Path | Written by | Content |
|---|---|---|
| `$XDG_CONFIG_HOME/bbtime/settings.ini` (default `~/.config/…`) | launcher (Save / Play / close) | settings below |
| `$XDG_DATA_HOME/bbtime/{lit,tb}/Datas/` | import wizard, only if "Copy" is chosen | copy of the original `Datas/` |
| `$XDG_DATA_HOME/bbtime/saves/{lit,tb}/` | the game | native saves (`bbtime::saveDir()`) |
| `$XDG_DATA_HOME/bbtime/backups/<UTC stamp>/` | Saves tab | full copies of `saves/` |
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
| `music_volume`, `sfx_volume` | 0–100 (100) | audio |
| `bind.<action>` | comma-separated SDL key names | keyboard map |
| `update_url` | https URL of `manifest.json` (empty) | launcher updater only |

Actions (`bbtime::kActions`) and defaults = LiT `config.pc` map (`docs/re/GAMEPLAY.md` §2.1): `up` Up, `down` Down,
`left` Left, `right` Right, `camera_left` Q, `camera_right` W, `roll` E, `action` R, `look` A, `crouch` S, `jump` D,
`kick` F, `select` Tab, `start` Return.

Game side (`apps/bbtime/main.cpp`, owned by gameplay): `bbtime::loadSettings()` before parsing flags, then flags
override. Verified 2026-10-18: with a launcher-written `settings.ini` (`width/height = 800x600`, `lit_root`/`tb_root`
= wizard copies) `bbtime --game lit --hidden --screenshot` loaded 167 LiT / 58 TB levels from the copies and rendered
at 800×600 logical (1120×840 at 1.4× scale); `--width 640 --height 480` overrode it (896×672).

## Tabs

* **Games** — background `checkInstall` of the configured roots on start; "Import original games…" wizard:
  1. pick a folder per game (disc mount, install dir or extracted ISO; the one with `Datas/`);
  2. validation: detected game, matched executable + version (`bugs.exe` "LiT 1.6", `bblit2.exe` "TB EN ISO"),
     executable SHA-256, file count/size, BZE level count, decode failures. A game is usable only if the
     detected game matches the slot, an executable matched, `Datas/` exists and nothing failed to decode;
  3. use in place (read-only) or copy `Datas/` to `$XDG_DATA_HOME/bbtime/<game>/` with a progress bar
     (`<dst>.partial` then rename; cancel removes the partial copy). Finish writes `lit_root`/`tb_root`.
* **Graphics & audio** — renderer, fidelity, resolution (+ presets), fullscreen, vsync, FOV, MSAA,
  music/effects volume.
* **Controls** — editable key table, restore defaults; connected gamepads via SDL3 (Qt 6 has no gamepad module).
* **Saves** — back up now, restore selected (the current saves are backed up first as `…-before-restore`).
* **Logs** — game logs, newest first; live while a game started from the launcher runs.
* **Updates** — signed update check/install/rollback (`docs/UPDATES.md`).
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
