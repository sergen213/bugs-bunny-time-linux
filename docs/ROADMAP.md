# bbtime roadmap

One native engine for **Bugs Bunny: Lost in Time** (LiT, 1999) and **Bugs Bunny & Taz: Time Busters** (TB, 2000), shipped as one collection app with a game-select menu.

Why one engine: TB's exe is `bblit2.exe` and shares LiT's frame pacer, LZSS, BZE loader, screen setup and 72 byte-identical functions (`docs/re/ENGINE_COMPARISON.md`, ~95% confidence). The games are **not** data-compatible: TB renumbers level-script bytes, uses different logic command tables, stores BZE sections uncompressed, and uses an octree world format. So: shared engine/runtime, per-game data decoders and gameplay tables.

Status vocabulary: Research Required → Designed → Implemented → Tested → Verified Against Original Behavior.

## Milestones

1. **Environment and audit** — done (`ENVIRONMENT.md`, `EXISTING_PROJECT_AUDIT.md`).
2. **Existing-work investigation** — done (`RESEARCH.md`: BBLIT-Remaster, BugsDecomp, community fixes).
3. **Asset pipeline** — BZE container/LZSS/importer done for both games; LiT textures + world geometry decoded; TB geometry in progress.
4. **First native renderer** — SDL3 + GL 4.6 collection app done; real LiT level rendering in progress.
5. **Core gameplay** — next: 30 Hz fixed tick (verified constant), player controller, collision, camera, entities from `docs/re/GAMEPLAY.md`.
6. **Playable progression** — one LiT era end-to-end (L01 Wild West), then TB with Bugs/Taz switching and two-player keyboard co-op (TB README documents it; split-screen code exists but is unreachable in the shipped build).
7. **Vulkan + enhancements** — second backend sharing `bb::Scene`; optional RT shadows/reflections (RADV exposes `VK_KHR_ray_tracing_pipeline`/`ray_query`); frame interpolation over the 30 Hz tick.
8. **Launcher + release** — Qt 6 launcher (import wizard, validation, backend/graphics/input, save backup), Ed25519-signed update manifests (pattern from CitroenC4Robot/Kabus 22 per audit), AppImage.

## Status matrix

| Subsystem | LiT | TB |
|---|---|---|
| Install detection / manifest (`bbimport`) | Tested | Tested |
| BZE container + checksum | Tested (167/167) | Tested (58/58) |
| LZSS | Tested (946/946 sections) | n/a (stored raw; `/comp` path only) |
| Level script (section 1) | Tested (167/167 parse) | Tested (58/58 parse, C++ + python) |
| TIM textures | Tested (C++, 28,206 refs; 1 unsupported 24-bit image) | Tested (C++, 17,103 refs) |
| World geometry → renderer | Tested (79 levels, 342k tris; visual check only) | Tested (58 levels, octree trees A+B; tree C = far LOD, not drawn) |
| Rigid props | Implemented (placement rule inferred) | Implemented (placement rule inferred) |
| Missing texture ids | Verified Against Original Behavior (rule `formats/TEXTURE_GAP.md`; loader resolves 5,271/5,271 world faces; L04C3 links 9/11 and L02A1 link 16 frame timings match the original alias-table traces `csv/litworld_L04C3_links.csv`, `csv/litworld_L02A1_links.csv` — RUNTIME_REFERENCE.md §8, GAMEPLAY_STATUS.md) | Tested (C++: 1,558/1,558 world faces resolve, animated) |
| Animation (Sony TOD, rigid parts) | Tested (233,491 poses; matches python to 5e-4) | Tested (packed TOD via the shared player, `bb/tb_actor.h`; Bugs, Taz + other skinned actors animate) |
| Collision | Tested (heightfield, `bb_game`; trace = approximation, GAMEPLAY_STATUS.md) | Tested (mesh + octree queries bit-exact vs python; mover = approximation; box/grip/jump Verified at runtime) |
| Player controller / camera / entities | Tested: player + camera + collectibles playable on L01a (GAMEPLAY_STATUS.md); jump arc and camera parameters (−200/600/1100, target-y lerp) Verified Against Original Behavior (RUNTIME_REFERENCE.md §3); enemies/platforms not ported | Tested: Bugs + Taz playable on ARCA01 (state-record interpreter, jump/run/steer, spring camera); jump/gravity and camera parameters (constants, spring rest 1200, look-at T.y − 300, 2P framing) Verified Against Original Behavior, run Consistent (48.84 u/tick observed) (RUNTIME_REFERENCE.md §4); NPCs/items not ported |
| Bugs/Taz switching, co-op | — | Verified Against Original Behavior (R1 switch timing — Consistent in RUNTIME_REFERENCE.md §4; L1 call-friend spot + cooldown 45, RUNTIME_REFERENCE.md §4); 2-player co-op camera Tested (`--players 2`, framing fitted to a runtime trace) |
| `.xa` IMA ADPCM music/speech | Tested (C++, 35 files) + menu music | Tested (C++, 446 files); level tracks per logic action 126 Implemented (GAMEPLAY_TB.md §6); menu track = design choice (original menu silent) |
| In-level SFX | Tested parse (3,055 AIFF PCM) | Tested parse (2,302 AIFF-wrapped ADPCM) |
| Simulation tick | Verified Against Original Behavior (30 Hz, counter +2/tick, 30.005 ticks/s; RUNTIME_REFERENCE.md §2); Implemented (`bb_game`, render locked in Original Fidelity) | Verified Against Original Behavior (tick `0x4d2708` +2/tick, 29.99 ticks/s; RUNTIME_REFERENCE.md §2); Implemented (`bb_game` TbWorld) |
| GL renderer, Hor+, fidelity modes | Tested on real levels | Tested on real levels |
| Collection menu + real title art | Tested | Tested |
| Save / config | Saves **Verified** (format, writer bytes, load vs Wine-made saves; `formats/SAVE_LIT.md`); config.pc not used | Designed (layout in GAMEPLAY §8) — untested |
| Vulkan renderer | Tested (validation-clean, GL/VK PSNR 66–76 dB) | Tested |
| Ray tracing | Research Required (RADV support probed only) | Research Required |
| Qt 6 launcher + import wizard | Tested (desktop smoke, both installs) | Tested |
| Signed updater (Ed25519 + SHA-256, rollback) | Tested (local HTTPS); no release URL yet | — |

## Known caveats

- The TB disc image is a third-party repack; `bblit2.exe` should be compared with the copy inside the original InstallShield cabs before treating addresses as canonical.
- Original renderer: CPU-projected screen-space vertices via GL 1.1, affine texturing (LiT; TB's shipped GL path is perspective-correct, `0x639dc8 = 1` at runtime — RUNTIME_REFERENCE.md §5). Our GL path is perspective-correct; Original Fidelity mode approximates filtering and aspect, not the affine warp.
- Original FOV is fixed ≈65.2°×51.3° (vertical **51.28°** = 2·atan(240 / (1.25·400)), Verified at runtime — RUNTIME_REFERENCE.md §5, §7) and stretches on non-4:3. Original Fidelity uses vertical FOV 51.28°; Enhanced keeps Hor+.
