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
| Level script (section 1) | Tested (167/167 parse) | Research Required |
| TIM textures | Implemented (python verified visually) | Implemented (python, 58/58) |
| World geometry | Implemented (python verified visually) | Research Required |
| Missing texture ids (33/79 levels) | Research Required | — |
| Animation (0x25 blocks) | Research Required | Research Required |
| Collision | Research Required | Research Required |
| Player controller / camera / entities | Research Required | Research Required |
| Bugs/Taz switching, co-op | — | Research Required |
| `.xa` IMA ADPCM audio | Implemented (python) | Implemented (python) |
| Simulation tick | 30 Hz observed statically | 30 Hz observed statically |
| GL renderer, Hor+, fidelity modes | Tested (placeholder scenes) | Tested (placeholder scenes) |
| Collection menu + real title art | Tested | Tested |
| Save / config | config.pc decoded (Research), saves untested | Research Required |
| Vulkan / RT / launcher / updater | Designed | Designed |

## Known caveats

- The TB disc image is a third-party repack; `bblit2.exe` should be compared with the copy inside the original InstallShield cabs before treating addresses as canonical.
- Original renderer: CPU-projected screen-space vertices via GL 1.1, affine texturing (LiT). Our GL path is perspective-correct; Original Fidelity mode approximates filtering and aspect, not the affine warp.
- Original FOV is fixed ≈65.2°×51.3° and stretches on non-4:3; we use Hor+.
