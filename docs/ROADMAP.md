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

## Wave 9

Collection menu navigation by layout, launcher restyled with the games' menu art, updater using the GitHub release
feed by default and restarting into a newer launcher, Vulkan real-time shadows, ray-traced AO and reflections with
smooth normals, FSR 3.1.4 upscaling and frame generation (Enhanced only). PS1 comparison was shelved after the tooling stage: `tools/psx/` (headless PCSX-Redux runner, pad plans, RAM
traces, level redirect) and the PS1↔PC data findings stay documented in RUNTIME_REFERENCE.md §1.1.

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
| Vulkan renderer | Tested (validation-clean, GL/VK PSNR 66–76 dB; Original Fidelity pixel-identical to the wave-8 build with the shadow code in) | Tested |
| Real-time sun shadows (Vulkan, Enhanced only) | Tested (world + actors; visual check L01A; validation-clean L01A/L02A1 in every mode, CC3A rt_ao; LiT blob shadows kept in Original Fidelity and on the GL backend) | Tested (visual check ARCA01; validation-clean ARCA01/TRCA01 in every mode, VICA01 rt_ao; sky domes excluded) |
| Ray tracing | Tested: ray-query sun shadows + ray-traced AO + one-bounce reflections (water L02A1 / L03A, static scene BLAS per level, actor BLAS + TLAS rebuilt per frame, alpha-tested casters resolved by texture), smooth crease-aware normals for shading / AO / reflections, RADV RX 7900 XTX; shadow-map / no-reflection fallback without ray queries (`BBTIME_VK_NO_RT=1`, ctest `render_vk.init_no_rt`) | Tested (same path; ice VICASG01, marble ARCA01, water VIQ01 / TRCA01) |
| FSR 3.1.4 upscaling + frame generation (Vulkan, Enhanced only) | Tested (third_party/fidelityfx-sdk as in etiyami; motion vectors for camera + posed actors; L01A walking capture: one sharp in-between Bugs, no cross-fade; validation-clean in every mode; `BBTIME_VK_NO_FSR=1` fallback) | Tested (ARCA01, VICASG01) |
| Qt 6 launcher + import wizard | Tested (desktop smoke, both installs) | Tested |
| Signed updater (Ed25519 + SHA-256, rollback) | Tested (local HTTPS + real GitHub release check); default feed = this repo's latest release | — |

## Enhanced sun shadows and ray tracing (Vulkan)

Neither original has a dynamic light (LiT draws blob shadows, TB none), so this is an Enhanced-only addition of the
Vulkan backend; Original Fidelity, the GL backend and the 4:3 3-D menus (`frame43`) draw exactly as before.

- **Modes** (`shadows` in settings.ini, `--shadows off|map|rt|rt_ao`, the collection Options row "Shadows", the
  launcher's "Shadows (Vulkan)"; default `rt_ao`): `map` = 4 cascaded 2048² shadow maps (every GPU); `rt` = ray-query
  shadows; `rt_ao` = plus ray-traced ambient occlusion (6 cosine rays, 210 units, closest-hit falloff). `rt`/`rt_ao`
  fall back to `map` without `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (or with `BBTIME_VK_NO_RT=1`); the
  diagnostics overlay (F3) shows the probe and the mode drawn.
- **Sun**: 55° high, behind the level's opening camera (its yaw at the first frame after the level upload) and 35°
  to its right, so the opening view is front-lit and shadows fall away from the camera. Shadowed surfaces keep 55 % of
  their baked vertex colour; the shading only darkens (faces turned from the sun count as shadowed, soft terminator).
- **Casters / receivers**: opaque and alpha-tested world meshes and world actors cast (alpha-tested ones through
  their texture, also in the BLAS); translucent surfaces only receive; additive/subtractive glows, TB sky domes
  (`sky…` meshes), LiT's sky dome actor (behaviour 9) and HUD models do neither. With sun shadows on, the app does not
  draw LiT's blob shadows (kept in Original Fidelity and on GL).
- **Acceleration structures**: one static BLAS per level (one geometry per caster mesh, fast-trace), the posed actors'
  BLAS and the two-instance TLAS rebuilt every frame (fast-build) from the streamed actor vertices. Rays skip hits
  closer than 9 units: the PS1 levels stack decal polygons a few units above the ground.
- **Smooth normals**: per-vertex normals at load (scene) and per frame (posed actors, only when a lit / FSR pipeline
  runs): faces meeting at a welded position (0.25-unit grid) are averaged weighted by their corner angle when within
  55° of each other, so rounded models (Bugs, Taz, the L01A dinosaur, rocks) shade smoothly while box edges stay hard.
  They drive N·L, the AO hemisphere and reflections; ray origins and the "faces away from the sun" test keep the
  geometric face normal (no self-shadowing). `BBTIME_VK_FLAT_NORMALS=1` restores face normals for comparisons.
- **Reflections** (`reflections` in settings.ini, `--reflections on|off`, Options row, launcher; default on; off
  without ray queries): one closest-hit ray per pixel, Schlick-style Fresnel, the hit textured, vertex-coloured and
  sun-shadowed; misses (sky) leave the surface. Neither game has material data, so surfaces are picked from texture +
  blend mode + vertex colour (renderer.cpp `reflectiveMaterials`; dump with `BBTIME_VK_MATERIALS=1`), only on
  up-facing meshes > 4 character units² in area: **water** = translucent / additive / animated and blue-dominant
  (strength 0.6, roughness 0.03: LiT L02A1, L03A, TB VIQ01, TRCA01, AZQ01), **ice** = opaque, strongly blue, bright,
  low-contrast (0.35 / 0.05: TB VICASG01 rink), **polished floor** = opaque, bright, unsaturated, low-contrast (0.25 /
  0.08: TB ARCA01 marble square, LiT LB02). Roughness jitters the ray only with FSR on (its history averages it);
  without FSR the AO / reflection ray sets are fixed per pixel (no grain).
- **Stylised interactive water** (`water = enhanced|cartoon|original` in settings.ini, `--water enhanced|cartoon|original`, Options row
  "Water", launcher; default enhanced; Enhanced + Vulkan only, `water=original` and Original Fidelity draw byte-identical
  to before). Ported from the Joan of Arc port (scene.frag `shadeWater`, `bb/render/water_waves.h`), distances rescaled
  ×3.75 (Bugs ~300 units vs Joan ~80). Water = the reflection classifier's water meshes plus flat dark-blue opaque
  planes (L03A sea, L02A1 river floor); a translucent layer > 200 units over such a plane makes it the bed (L02A1),
  20–200 over it the layer is sheen the stylised sea replaces (L03A 880). A fill plane at the main water height covers
  open water the level draws as sky dome / holes (to the horizon over a sea, over the water bounds over a bed). The
  opaque + alpha-tested scene is copied (colour, depth; MSAA: depth resolved sample 0) and the water drawn opaque over
  it: absorption teal→deep blue over the refracted bed, shoreline foam, two scrolling layers of a procedural tiling
  normal map (no game data), Fresnel reflection (ray-traced with reflections on, else screen-space over the copy, else
  the sky seen at the top of the screen), GGX sun glints with the frame's sun shadow; FSR reactive 0.4. Waves: a damped
  2-D wave equation (`WaveField`, 256² cells of 15 units following the camera, fixed 1/60 s substeps, allocation-free)
  walled by the shores (`WaterSurfaces`: ground within 600 units over the surface) and by wading bodies; the app feeds
  players and drawn actors (`Renderer::setWaterBodies`, read-only: no gameplay effect): wakes while moving, a bob while
  standing, a splash when falling through the surface. Heights uploaded as R16F per frame. Shots:
  `local/shots/w11/water/` (side_*.png; side_fall_L02A1.png = Bugs falling into the L02A1 river). Implemented,
  visual port (no original counterpart). `cartoon`: the same water toon-shaded (crisp crest lines, hard glints, solid
  foam with a white shore rim, colour posterised into 5 luminance bands with soft steps).
- **Frame time** at 1920×1080 (X11, MSAA 4×, vsync off, RX 7900 XTX RADV, `--frames 400`, ms per frame as the
  overlay FPS): LiT L01A off 0.12 / map 0.18 / rt 0.46 / rt_ao 1.9–2.2; TB ARCA01 off 0.08 / map 0.16 / rt 0.58 /
  rt_ao 2.4–2.7; forced fallback (rt_ao, `BBTIME_VK_NO_RT=1`) 0.18 / 0.16. Screenshots: `local/shots/w9vk/`.

## FSR 3.1.4 upscaling and frame generation (Vulkan)

AMD FidelityFX SDK v1.1.4 subset vendored in `third_party/fidelityfx-sdk` exactly as in the ETI Yami port
(`/mnt/1st4TB/etiyami`: same files, licence, local patches and `yami/` glue; `third_party/CMakeLists.txt` compiles
only the FP32 / wave32 / render-resolution-unjittered-motion-vector permutations with glslangValidator (else glslc
`-O -g`, which keeps the resource names the SDK binds by), and `tools/embed_fsr_shaders.py` generates the blob accessor). `engine/render_vk/src/fsr.{h,cpp}` holds the
contexts; the renderer owns every image.

- **Settings** (Enhanced, not the 4:3 3-D menus): `upscaling = off|fsr_quality|fsr_balanced|fsr_performance|native_aa`
  (render scale 1/1.5, 1/1.7, 1/2, 1; RCAS sharpening 0.3 when scaling), `frame_generation = off|on`; flags
  `--upscaling`, `--frame-generation`; Options rows "Upscaling" / "Frame generation"; launcher Graphics page. FSR
  replaces MSAA. Frame generation alone runs FSR as native AA (interpolation needs its dilated depth / motion).
  Unsupported devices (formatless storage, quad + arithmetic subgroups, storage formats, swapchain transfer-dst,
  independent blend) or `BBTIME_VK_NO_FSR=1` keep MSAA and native rendering.
- **Inputs**: the scene renders at the render size with FSR's jitter into colour + R16G16 motion vectors + depth. Motion
  = previous minus current unjittered clip position in UV units: the static scene uses the previous frame's
  view-projection, posed actors their previous frame's vertices when the actor mesh layout is unchanged (else
  camera-only motion); translucent surfaces write no motion. History resets on level load and after any frame drawn
  without FSR. The upscaled frame is blitted to the swapchain; the 2-D layer is drawn on top at display resolution.
- **Reactive / transparency mask** (R8, a third attachment blended with MAX, passed as FSR's `reactive` and
  `transparencyAndComposition` and as frame interpolation's shading-change mask, local SDK patch 7): additive /
  subtractive glows by their strength (max channel × alpha × 1.5), alpha-blended surfaces (water, veils) by their
  coverage, LiT type-4 sprites (`sprite` actor meshes) and animated textures by their alpha, reflective surfaces by
  half their reflection strength; capped at 0.9. These change without matching motion vectors and trailed before.
  Measured (LiT walking capture, frame 140 of `U*100,UL*40`, 1920×1080, FSR Quality, mean absolute error against the
  native MSAA frame over the pixels the mask changes): L02A1 (a dust sprite) 41.6 → 37.1 on 4 392 px, L03A (water)
  25.4 → 24.2 on 4 649 px; L01A's view has no masked surface (no change). `BBTIME_VK_NO_REACTIVE=1` drops the mask
  for comparisons. Screenshots: `local/shots/w9vk3/compare_reactive_*` (no mask | mask | native).
- **Frame generation**: optical flow + frame interpolation (FSR 3.1.4) between the previous and this frame's upscaled
  scene. While it runs the frame renders into per-slot images (real frame; the generated one gets the frame's 2-D
  layer too) and a present thread — etiyami's pattern — owns the swapchain: it copies the generated frame into an
  acquired image and presents it, then the real frame half a rendered-frame interval later (interval smoothed over the
  frames' hand-off times; skipped when the next frame is already waiting), with or without vsync. The render thread
  waits only for its slot's copies; queue access is serialised by a mutex (one queue: the copies queue behind the next
  frame's GPU work). Present intervals (`BBTIME_VK_PRESENT_LOG=1`, TB ARCA01 1920×1080 FSR Quality, 900 free-running
  frames, X11, longest 1 % dropped):

  | | interval mean | stddev | cv | p5 / p50 / p95 |
  |---|---|---|---|---|
  | vsync off, FG off | 1.43–1.55 ms | 0.09–0.20 | 0.06–0.13 | 1.32 / 1.47 / 1.58 |
  | vsync off, FG on, before (back to back) | 0.99 ms | 1.01 | 1.01 | 0.002 / 0.008 / 2.12 |
  | vsync off, FG on, paced | 1.21 ms | 0.31 | 0.26 | 0.83 / 1.21 / 1.75 |
  | vsync on (FIFO 60 Hz), FG on, before | 16.9 ms | 17.1 | 1.01 | 0.002 / 0.006 / 34.7 |
  | vsync on (FIFO 60 Hz), FG on, paced | 17.05 ms | 0.14 | 0.01 | 16.88 / 17.05 / 17.13 |

  `BBTIME_VK_SHOT_GENERATED=1` makes `--screenshot` also write the generated frame (`<stem>-generated.png`).
- **Frame time** (ms per rendered frame, X11, vsync off, `--frames 400`, rt_ao + reflections; with frame generation
  two frames are shown per rendered frame — the overlay prints both FPS):

  | | upscaling off (MSAA 4×) | FSR Quality | FSR Quality + FG | FSR Performance | FSR Performance + FG | Native AA + FG |
  |---|---|---|---|---|---|---|
  | LiT L01A 1920×1080 | 1.86 | 2.04 | 2.17 | 1.34 | 2.15 | 3.11 |
  | TB ARCA01 1920×1080 | 1.78 | 1.62 | 2.18 | 1.33 | 1.96 | 3.10 |
  | LiT L01A 3840×2160 | 6.03 | 4.68 | 6.52 | 3.37 | 4.75 | 10.24 |
  | TB ARCA01 3840×2160 | 4.48 | 3.71 | 6.28 | 2.68 | 4.86 | 9.64 |

  Screenshots (quality comparison, overlay FPS, walking capture real / generated / real / cross-fade):
  `local/shots/w9vk2/`.

## Known caveats

- The TB disc image is a third-party repack; `bblit2.exe` should be compared with the copy inside the original InstallShield cabs before treating addresses as canonical.
- Original renderer: CPU-projected screen-space vertices via GL 1.1, affine texturing (LiT; TB's shipped GL path is perspective-correct, `0x639dc8 = 1` at runtime — RUNTIME_REFERENCE.md §5). Our GL path is perspective-correct; Original Fidelity mode approximates filtering and aspect, not the affine warp.
- Original FOV is fixed ≈65.2°×51.3° (vertical **51.28°** = 2·atan(240 / (1.25·400)), Verified at runtime — RUNTIME_REFERENCE.md §5, §7) and stretches on non-4:3. Original Fidelity uses vertical FOV 51.28°; Enhanced keeps Hor+.
