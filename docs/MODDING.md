# Modding (textures, models, animations, sounds, music)

Status: **Implemented**, **Tested** (`core.mods` ctest; export → edit → mod round trip on LiT L01A and TB ARCA01 in
both renderers, screenshots in `local/shots/w9mod/`). Mods change only what you see and hear; with no mod enabled
the game is byte-for-byte the unmodded one (verified pixel-identical, Vulkan and OpenGL).

A mod is a folder of replacement files. You get the originals to edit from the **exporter**, which writes them with
the same names the game looks up, so the workflow is always: export → edit in GIMP / Blender / Audacity → copy the
files you changed into a mod folder → enable the mod in the launcher.

Exported originals are your own copies of the game data: keep them in your folder, don't redistribute them.
Share a mod only with files you made.

## Where mods live

```
$XDG_DATA_HOME/bbtime/mods/            (default ~/.local/share/bbtime/mods/)
  my-mod/
    mod.json
    textures/<game>/<level>/<texture id>.png
    textures/<game>/common/<texture id>.png
    models/<game>/<model id>.gltf              (+ its .bin / images, or a .glb)
    models/<game>/<level>/<asset id>.gltf
    models/<game>/<level>/world.gltf
    animations/<game>/<model id>/<anim id>.gltf
    sounds/<game>/<level>/<index>.wav
    sounds/<game>/<level>_speech/<index>.wav
    music/<game>/<track>.ogg | .wav
```

`<game>` is `lit` (Lost in Time) or `tb` (Time Busters). Folder and file names are case-insensitive. Other files
(readme, licence, glTF `.bin` and images next to their `.gltf`) are ignored.

`mod.json`:

```json
{
  "id": "my-mod",
  "name": "My mod",
  "version": "1.0",
  "author": "me",
  "description": "What it changes",
  "game": "lit",
  "swap":  { "tb:taz": "tb:bugs" },
  "anims": { "lit:bugs:72": 900 }
}
```

`game` is `lit`, `tb` or `both` (only informational: files of the other game are flagged by validation). The folder
name is the id the settings use. `swap` and `anims` are optional (see below).

## Enabling and load order

The launcher's **Mods** page lists installed mods: tick to enable, ▲ / ▼ to order. Enabled mods load top to bottom
and **the later one wins** when two mods provide the same file; the page lists those conflicts. The order is saved
as `mods = a,b,c` in `settings.ini`. The game also takes `--mods a,b,c` (replaces the list) and `--no-mods`.
Every applied override is logged once (`bbtime: mod override <key> <- <file>`, in the launcher's Logs page) and the
F3 overlay shows `mods: N active, M overrides`.

Other buttons: **New mod…** (creates the folder, `mod.json` and the empty sub-folders), **Open folder**, **Install
from .zip…** (an archive with `mod.json` at its root or in one top folder), **Remove**, **Export assets…**.

The **3-D model viewer** tab (next to the details) previews a mod's model and animation glTF files and, with **Show
originals**, the original models of a level (exported once with `bbtime-modkit` into
`$XDG_CACHE_HOME/bbtime/viewer/`; originals the selected mod replaces are marked), so a replacement and the model it
replaces can be compared. It shows the static rest / bind pose with the base colour texture (animations and skins are
not played); drag to orbit, wheel to zoom, double-click to reset.

**Replace model…** there does the copying for you: select the original (or the mod's file), pick your edited
`.gltf` / `.glb`, and the launcher copies it into the selected mod at the path the game looks up for that model
(the same relative path as in the list), plus the `.bin` / images its relative URIs name at the same relative place
(an edited export's `../../textures/…` images land in the mod's `textures/` and so also replace that level texture,
as when copied by hand). Files that don't parse are refused; missing part / world node names, absolute or
out-of-mod URIs (not copied) and overwrites are listed before anything is written. **Remove replacement** deletes
the mod's file and the side files no other model of the mod uses.

## Exporting the originals

Launcher: Mods → **Export assets…** (game, level, what, output folder; default
`~/.local/share/bbtime/modkit-export`). Command line (`bbtime-modkit` ships next to the launcher):

```
bbtime-modkit export --game lit --level L01A --what all --out ~/bbmod-export
bbtime-modkit export --game tb  --level ARCA01 --what models --out ~/bbmod-export
bbtime-modkit export --game lit --level L01A --what music --out ~/bbmod-export   # every Datas/track/*.xa as WAV
bbtime-modkit validate ~/.local/share/bbtime/mods/my-mod
bbtime-modkit new my-mod --name "My mod" --author me --game lit
```

`--what`: `textures`, `models` (also writes the textures the glTF files reference), `sounds`, `music`, `all`
(= textures + models + sounds; music only on request, it is large and not per level). Game folders come from the
launcher settings (`lit_root` / `tb_root`), `--root DIR` overrides. The export tree has exactly the mod layout:
copy the files you edit, at the same relative path, into your mod.

`models/<game>/<level>-ids.txt` lists every model of the level: level asset id, model id, `scene` or `simulation`
(Time Busters pickups and props the game builds from the level data while it plays — mod files and swaps work the
same for both), objects, parts and its animation ids.

## Ids

* **Texture id**: the TIM id inside the level (`textures/lit/l01a/15.png`). Ids are per level; `common/<id>.png`
  applies to that id in every level of the game — only useful for ids that mean the same texture everywhere.
* **Model id**: `bugs` / `taz` for the playable characters, otherwise `m` + an 8-digit hash of the model's
  geometry, vertex colours and UVs (e.g. `m7ca434e6`). The hash is the same in every level that uses the same
  model, so one file replaces it everywhere; look-alikes that differ only in colour (TB's gold and blue coins) get
  different ids. `models/<game>/<level>/<asset id>.gltf` replaces one level's model by its asset id
  (the `-ids.txt` first column) and wins over the global file.
* **Anim id**: the animation's role id (what the level scripts request, stable across levels; e.g. LiT Bugs' idle
  is 72). Listed in `-ids.txt`.
* **Sound index**: position in the level's sound bank (0-based) — the exported WAV names.
* **Music track**: file stem of the original track (`track5`, `track14`, …).

## Textures (GIMP or any PNG editor)

Edit the exported PNG and save as PNG (8-bit RGBA; JPEG is accepted only inside glTF models). **Any size works**:
an upscaled texture pack simply uses bigger PNGs, sampled in place of the original (UVs are normalized). Alpha
follows the original rule: alpha 0 = transparent (the TIM colour key); faces that were opaque switch to alpha-test
when the new texture has transparency. Alpha-tested faces show partial alpha through the renderer's alpha test (no soft edges); faces that were
blended keep their original blend mode. Limits:

* HUD, font and 2-D sprite textures are drawn at their texel size: replacements are resampled (nearest) to the
  original size.
* Time Busters' brightness setting is applied to mod textures too. Exports are written at brightness 1.
* Animated textures: replace each frame's id.
* In Original Fidelity the PC texel curve (LiT) still applies, as for the originals.

## Models (Blender)

Import the exported `.gltf` (File → Import → glTF 2.0). You get an empty `bbtime` (it turns the PS1 axes upright
and scales 1 unit = 1 mm) holding a hierarchy of objects **`part1`, `part2`, …** — the model's rigid parts in the
order the game's animation data addresses them — each with its mesh **`object<N>`** (the original's TMD object).
Materials are named `m<k>_tex<id>` and use the exported PNGs.

Edit the meshes (shape, vertex count, UVs, materials, new textures) and export as glTF 2.0 (`.gltf` + `.bin` or
`.glb`) into `models/<game>/<model id>.gltf` of your mod. Rules:

* **Keep the part objects and their names.** The game moves each part with the original animations; a mesh is
  shown by the part it sits under (any depth, its own transform is baked in), or by its mesh name `object<N>`.
  Meshes that are not under a part are skipped with a note.
* Parts you leave out keep their original mesh. A part's mesh replaced in the file replaces all of that object.
* Vertices that stay where they were keep the original's seam behaviour (the exporter and importer weld them to
  the original skinning slots); moved or new vertices follow their own part rigidly. Seams between parts can open
  slightly when you move vertices that sat on a seam.
* Materials: `m<k>_tex<id>` with its original image keeps the original texture (and its texture animation /
  texture-mod); any other image (PNG / JPEG, file or embedded) becomes a new texture. Base colour factor tints the
  vertices; `MASK` → alpha test, `BLEND` → alpha blend; double-sided is kept. Vertex colours (`COLOR_0`) are used
  when present: 0.5 grey ≈ the PS1's neutral 128 (1.0 = twice as bright).
* Collision and gameplay boxes always stay the original's (they come from the level data and the clips' box
  packets, not from the mesh).
* Validation warns above 20 000 triangles per model and when the size is far off (originals are PS1 units: a
  character is roughly 100–2000 units tall; keep the `bbtime` empty's scale).

Rigid one-part models (HUD icons, shadows) export without parts: their meshes `object<N>` sit directly under
`bbtime`.

### Swapping a whole model (no Blender)

`"swap": { "tb:taz": "tb:bugs" }` in `mod.json` draws the first model with the second's skeleton, meshes and clips
(clips the target lacks play its first clip, so actions waiting for a clip end still finish); the original's
gameplay boxes are kept. The target can be any model id (`bugs`, `taz`, `m…`, `<level>/<asset>`) **present in the
same level**, or the name of a model file in an enabled mod (`"lit:bugs": "my-robot"` →
`models/lit/my-robot.gltf`, which then needs the original's part names). Model files for the swapped model still
apply on top.

### World meshes

`models/<game>/<level>/world.gltf` (exported per level) holds the static level meshes as nodes `<index>#<name>` in
engine space. Edit or replace them; a node without a mesh hides that mesh. Collision is the level's own data and
does not change.

## Animations (Blender)

`animations/<game>/<model id>/<anim id>.gltf` is the model plus one clip (glTF animation on the part nodes:
translation / rotation / scale, keyed on every original frame, 30 fps). In Blender set the scene to 30 fps before
importing, edit the part objects' keys (Dope Sheet / Graph Editor), and export glTF with animations into the same
path of your mod.

* **Replace a clip**: same anim id. The game samples your keys at the original frame rate of that clip (frame
  length = the original's); the clip's length follows your last key. Channels you don't animate, and the original's
  non-pose data (gameplay boxes, attach points, mesh switches) are kept frame by frame.
* **Add a clip**: a new anim id (e.g. `900.gltf`). The game only plays ids its level scripts request, so use the
  new clip through `mod.json` `"anims": { "lit:bugs:72": 900 }`: every request for clip 72 of model `bugs` plays
  clip 900. This keeps the original file untouched and lets several clips be switched by editing one line.
* Interpolation LINEAR, STEP and CUBICSPLINE are read; rotations become the game's Euler angles exactly.
* Parts: keys on `part<N>` nodes only. A clip cannot add parts (see Limits); key a part's scale to 0 to hide it for
  as long as the clip wants.
* Time Busters' packed clips are converted to the editable form on import; the game plays both.

## Sounds and music (Audacity)

* `sounds/<game>/<level>/<index>.wav` replaces bank entry `<index>` of that level (`<level>_speech/` the speech
  bank). WAV: PCM 8/16/24/32-bit or 32-bit float, mono or stereo, any rate. The game keeps its own pitch rules (LiT
  plays a sound at its program's note relative to the file's rate, so keep the exported rate for the same pitch).
  Looping sounds: a `smpl` loop (the exporter writes the original loop; Audacity keeps it only with a plug-in) sets
  the loop, otherwise the whole replacement loops.
* `music/<game>/<track>.ogg` or `.wav` replaces `Datas/track/<Track>.xa` (Ogg Vorbis via stb_vorbis, decoded when the
  track starts). Mono or stereo, any rate.

## Validation

The Mods page validates the selected mod (`bbtime-modkit validate` prints the same): broken `mod.json`, unknown
paths, non-numeric ids, bad PNG / WAV, texture ids that the level does not have (needs the game installed), glTF that
cannot be read, models / clips without `part<N>` nodes, missing part numbers, too many triangles, odd scale, world
files without `<index>#` nodes, swap keys without a game prefix, files for the other game.

## Engine notes

`bb::mods::ModSet` (`engine/core/include/bb/mods.h`) resolves a key (lower-case relative path without extension)
against the enabled mods in order. `bb::mods::apply` runs once per decoded level at the asset layer
(`apps/bbtime/level_hook.cpp` `loadLevelScene`): textures, actor models, swaps, clips and world meshes are replaced
in the `bb::Scene` that both renderers upload and the simulations read their actor models from. Sound banks are
patched after the world loads (`applySoundMods`), music by path (`modMusic`). Gameplay code is not touched.
Glue: `engine/core/src/mods.cpp` (resolver, PNG, validation), `mods_gltf.cpp` (glTF reader / writer, TOD ↔ glTF),
`engine/audio` `loadPcm` / `writeWav`, `tools/modkit.cpp`. Vendored: `third_party/stb` (stb_image, stb_image_write,
stb_vorbis; public domain / MIT).

Time Busters entities without a scene actor (pickups, props spawned from templates) get their model from the
simulation (`TbWorld` builds it from the level data); `bb::mods::applyToModel` applies model files, swaps and clips to
it as it is built, and `apply()` registers the images of every mod model with the level beforehand so those models
can use new textures.

Limits: a clip cannot add parts (the skeleton's part list is fixed; the game's own clips create helper parts only
through packets the importer keeps). To give a character extra geometry, put the new mesh under an existing part
in the model file — it then moves with that part. A clip can hide a part: key its scale to 0 (the game skips parts
whose matrix is zero, as the original). Animation retargeting is by part number only.
