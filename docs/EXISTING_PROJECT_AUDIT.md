# Existing project audit (read-only, 2026-10-18)

Method: `findmnt -T`, `ls`, `git -C <p> branch --show-current`, `git status --porcelain | wc -l`, `git log --oneline -15`, LICENSE/README grep, `find_package`/`pkg_check_modules` grep, `wc -l`, targeted `grep` for Ed25519/SHA-256. Nothing was built or written in audited trees.

**License rule for bbtime:** only Pusu: Uyanış has a LICENSE file (GPL-3.0). Kabus 22 README states "no project-wide license is assigned". etiyami, IKV, CitroenC4Robot, SuperCan have no LICENSE file at the root. Code without a license cannot be copied as-is; reuse below is therefore **pattern-only** unless the owner (apparently the same user — [INFERENCE] from shared conventions/skills) explicitly licenses it to bbtime. Vendored third-party code (FidelityFX SDK, MIT) can be taken from upstream directly.

## 1. `/mnt/1st4TB/Kabus 22`
- Mount: `/mnt/1st4TB` on `/dev/sda1` (btrfs). Git: branch `main`, clean (0 porcelain lines). HEAD `9625182 release: 0.4.8`; recent work: FSR 3.1.4 frame generation (`3702d61 Merge branch 'fsr-framegen'`), RT perf, Vulkan validation fixes, seamless doors.
- Language/build: C++ + Python tools, CMake. Deps: CURL, LibArchive, nlohmann_json, OpenSSL, Vulkan, epoxy, freetype2, fontconfig, gtk+-3.0, **SDL2**, ffmpeg libs. third_party: FidelityFX-SDK, k22-ffx-posix, ODE 0.16.6 (BSD).
- License: none project-wide (README). `packaging/NOTICE.txt` covers bundled components (uruntime MIT, innoextract, squashfuse GPL/LGPL boundaries).
- Docs: `docs/` (REVERSE_ENGINEERING, ASSET_FORMATS, ENGINE_ARCHITECTURE, GRAPHICS_ROADMAP, RELEASE_PROCESS, INSTALLER, TEXTURE_RESTORATION, WIDESCREEN_IMPLEMENTATION, COOP, KNOWN_ISSUES, PROGRESS, `docs/reverse-engineering/*`).
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Vulkan raster + inline-RT | `native/render_vulkan.cpp`, `native/render_vulkan_raster.cpp`, `native/render_vulkan_rt.cpp` (321 lines), `native/vulkan_loader.cpp`, `native/shaders/rt_signals.comp`, `rt_filter.comp` | pattern-only |
  | FSR3 Vulkan glue | `native/fsr_vk.cpp` (243 lines), `tools/k22_fsr3_blobs.py`, `third_party/k22-ffx-posix` | pattern-only (FidelityFX itself MIT, take from upstream) |
  | GL renderer | `native/render_gl.cpp`, `native/render.cpp` | pattern-only (SDL2/epoxy based) |
  | Render self-check | `native/check_render.cpp`, `tools/check_frame_pacing.py` | pattern-only |
  | Texture upscale | `tools/texture-upscaler/upscale.py` + `test_upscale.py`, `native/texture_pack.cpp` | pattern-only |
  | Signed updater | `installer/update.cpp` (393 lines, OpenSSL SHA-256 + base64), `installer/updater.cpp`, `installer/check_update.cpp`, `installer/check_updater_e2e.py`, `tools/release/make_update_manifest.py`, `tools/release/sign_manifest.py` (Ed25519, writes `MANIFEST.sig` base64 64 bytes) | pattern-only — best-documented signed-manifest scheme |
  | Installer/launcher (GTK3) | `installer/{main,setup,launcher,launcher_settings,layout,media}.cpp` | not suitable (GTK3, bbtime uses Qt6) |
  | AppImage | `tools/package_kabus_appimage.py`, `packaging/` (uruntime) | pattern-only |
  | Virtual gamepad test | `tools/virtual-gamepad/vpad.py`, `run-live.sh` | pattern-only |
- Unfinished: README calls it "experimental prerelease, not certification of a complete campaign"; `docs/KNOWN_ISSUES.md`.

## 2. `/mnt/1st4TB/etiyami`
- Mount: `/dev/sda1` btrfs. Git: `main`, **dirty (36 porcelain lines)**. HEAD `ee07a4e docs: record public v1.1.1 installation and upgrade proof`; v1.1.0 shipped Vulkan rendering + character shadows; README notes an *UNRELEASED* FSR 3.1.4 follow-up replacing custom TAA.
- Language/build: C++ (`native/`), Python tools, CMake. Deps (`YAMI_DEPS`): **sdl3**, epoxy, ffmpeg libs, libxml-2.0, libcurl>=7.85, libarchive, json-c. third_party: `fidelityfx-sdk` (MIT, shipped as `licenses/fidelityfx/LICENSE.txt` per README).
- License: no root LICENSE file.
- Docs: `README.md`, `REVERSE_ENGINEERING.md`.
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Vulkan renderer + RT effects + FSR inputs | `native/vulkan_renderer.cpp`, `native/vulkan_effects.cpp` (1117 lines), `native/shaders/{effects_rt.comp,fsr_inputs.frag,vulkan_raster.*,vulkan_final.*}`, `tools/embed_fsr_shaders.py` | pattern-only; closest to bbtime (SDL3 + epoxy + Vulkan) |
  | GL renderer | `native/renderer.cpp`, `native/graphics.hpp` | pattern-only |
  | Renderer checks | `native/check_renderer.cpp`, `native/check_vulkan.cpp` | pattern-only |
  | Updater | `native/update.cpp` (`sha256:` digest check, TLS verify), `native/update_install.cpp`, `native/updater_main.cpp`, checks `check_update*.cpp` | pattern-only (no Ed25519 found by grep in `native/*.cpp`) |
  | Installer/uninstall/AppImage | `native/setup_install.cpp`, `native/uninstall_main.cpp`, `native/uninstall.sh`, `tools/package_appimage.py`, `tools/package_setup.py`, `tools/package_release.py` | pattern-only |
  | Asset extraction | `extract_game.py`, `prepare_reference.py` | not suitable (game-specific) |
- Unfinished: uncommitted changes (36); FSR follow-up unreleased.

## 3. `/mnt/1st4TB/Pusu: Uyanış`
- Mount: `/dev/sda1` btrfs. Git: `main`, **dirty (15)**. 6 commits, HEAD `c17913b docs: link verified rendering and loading bugfix release`; first `c9d25e1 feat: publish native Linux Pusu engine and installer source`. Public repo per README: github.com/sergen213/pusu-uyanis-linux.
- Language/build: C++ (`src/`), Python packaging, CMake. Deps: GLEW, OpenGL, Vulkan, OpenSSL, PNG, ZLIB, libarchive, libcurl, ffmpeg, gio, gtk+-3.0, json-c, **SDL2 + SDL2_image + SDL2_mixer**.
- License: **GPL-3.0** (`LICENSE`).
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Vulkan raster/RT/quality | `src/renderer_vulkan{,_raster,_rt,_quality}.cpp`, `src/renderer_vulkan_private.hpp`, `src/renderer_shared.cpp` | copy only if bbtime accepts GPL-3.0 (it would make bbtime GPL); otherwise pattern-only |
  | Updater | `src/updater.cpp` (curl with VERIFYPEER/VERIFYHOST) | pattern-only |
  | AppImage + license bundling | `packaging/build_appimage.py`, `packaging/license_bundle.py`, `packaging/check_packaging.py` | pattern-only (useful for license-closure checking) |
  | GPU probes | `.work/native-vulkan-proof/*` | not suitable (scratch) |
- Unfinished: dirty tree; large `.work/` scratch.

## 4. `/mnt/2nd4TB/IKV`
- Mount: `/mnt/2nd4TB` on `/dev/sdb1` (btrfs). Git: `main`, clean. HEAD `4101f39 IKV Linux 0.8.0 …`; history 0.2.0 (native client, community server, Qt launcher) → 0.4.0 (in-launcher updates, RT lighting) → 0.7.0 (Windows build, LAN).
- Language/build: C++ (`src/`), Python tools, CMake; Docker/compose for server. Deps: OpenSSL ≥3.2, Boost, Threads, Qt6 (launcher), SDL3 3.4.18 / Vulkan-Headers 1.4.363 pinned in AppImage Dockerfile (README). third_party: FidelityFX-SDK, ikv-ffx-posix, stb_vorbis.
- License: no LICENSE file.
- Docs: `docs/` (LAUNCHER_UPDATES, GRAPHICS_ROADMAP, ENGINE_ARCHITECTURE, ASSET_FORMATS, FUNCTION_MAP, REVERSE_ENGINEERING, network/*).
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Vulkan RT | `src/vulkan_rt.cpp` (1434 lines), `src/renderer_vk.cpp`, `shaders/vulkan/{scene_rt,scene_rt_guided,temporal,shadow}.*` | pattern-only |
  | FSR3 glue | `src/fsr_vk.cpp` (243 lines, same size as Kabus copy), `tools/ffx_fsr3_blobs.py` | pattern-only |
  | GL renderer | `src/renderer_gl.cpp`, `tests/render_math_test.cpp` | pattern-only |
  | Qt6 launcher + updates | `src/launcher_main.cpp`, `src/launcher_updates.cpp` (686 lines, OpenSSL SHA-256), `src/update_helper_main.cpp`, `src/launcher_skin.cpp`, `src/launcher_extraction.cpp`, tests `tests/update_version_test.cpp`, `tests/update_integration.py`, `tests/launcher_gui_smoke.cpp` | pattern-only — closest match to bbtime's Qt6 launcher + updater |
  | Release signing | `tools/sign_manifest.py` (Ed25519 PEM over exact manifest bytes) | pattern-only |
  | AppImage (Docker-pinned) | `tools/package_appimage.py`, `tools/appimage/Dockerfile.ubuntu24` | pattern-only |
  | Image write | `src/image_write.cpp` | pattern-only |
- Unfinished: `docs/PORTING_GAPS.md`, `docs/KNOWN_ISSUES.md`.

## 5. `/mnt/2nd4TB/CitroenC4Robot`
- Mount: `/dev/sdb1` btrfs. **Not a git repository** (`git` fatal).
- Language/build: C++ + Python, CMake. Deps: SDL3 ≥3.2, Qt6 ≥6.4 (Widgets, OpenGL, Concurrent) for `launcher/`, OpenSSL.
- License: no LICENSE file.
- Docs: `README.md`, `docs/`; verification ledgers under `work/verification/`.
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Ghidra headless script | `tools/ghidra/analyze.py` | pattern-only |
  | Asset import + test | `tools/import_assets.py`, `tests/test_import_assets.py` | pattern-only |
  | Texture/GPU image (incl. BC) | `src/assets/{Images,GpuImage,GpuImageBc,GpuImageRuntime}.cpp` | pattern-only |
  | Vulkan RT / pipeline cache | `src/rendering/vulkan/{raytracing.cpp (1315 lines),VulkanScene.cpp,VulkanDispatch.cpp,quality.cpp}`, `tests/test_vulkan_pipeline_cache.cpp` | pattern-only |
  | Quality settings | `src/rendering/quality/{GraphicSettings,ShadowCascades,OpenGLQuality}.cpp` | pattern-only |
  | SDL3 platform | `src/platform/runtime.cpp` | pattern-only |
  | Qt6 launcher | `launcher/src/{main,hardware,quality_controls,integration,assets}.cpp` | pattern-only |
  | Ed25519 updater | `updater/src/main.cpp` (OpenSSL `EVP_PKEY` ED25519 PEM, 64-byte sig), `updater/tests/safety.py` | pattern-only — most self-contained C++ Ed25519 verifier seen |
  | AppImage | `packaging/package_native.py`, `packaging/appimage/{acquire,test_appimage}.py` | pattern-only |
- Unfinished: README "current repair" notes; no VCS.

## 6. `/mnt/2nd4TB/SuperCan`
- Mount: `/dev/sdb1` btrfs. **Not a git repository**.
- Language/build: C++ + Python, CMake. Deps: SDL3 ≥3.4, OpenGL, OpenSSL (Crypto).
- License: no LICENSE file.
- Docs: `docs/` (ASSET_FORMATS, ENGINE_ARCHITECTURE, FUNCTION_MAP, REVERSE_ENGINEERING, INSTALLER_ARCHITECTURE, SOURCE_COMPARISON, GRAPHICS_ROADMAP, KNOWN_ISSUES, PROGRESS).
- Reusable:
  | Area | Paths | Recommendation |
  |---|---|---|
  | Ghidra headless tooling | `tools/ghidra/{inspect_containers,decode_application,compiled_bindings}.py`, `test_container_probe.py` | pattern-only |
  | Container extraction | `tools/fx9_extract.py` | not suitable (format-specific) |
  | Image decode | `src/assets/image.cpp`, `tests/model_image_test.cpp` | pattern-only |
  | D3D9→GLSL / D3D9 device emulation | `src/assets/d3d9_glsl.cpp`, `src/render/d3d9_device.cpp` | not suitable (LiT/TB use Glide/D3D-era fixed function; [INFERENCE]) |
  | Fixed-tick timing / input tests | `tests/timing_test.cpp`, `tests/input_test.cpp`, `tests/software_mixer_test.cpp` | pattern-only |
- Unfinished: `docs/KNOWN_ISSUES.md`, `docs/PROGRESS.md`.

## Reuse shortlist (ranked)
1. **Signed updater** — `/mnt/2nd4TB/CitroenC4Robot/updater/src/main.cpp` (C++ OpenSSL Ed25519 verify) + `/mnt/1st4TB/Kabus 22/tools/release/sign_manifest.py` & `make_update_manifest.py` (signing side). Pattern-only; libsodium/OpenSSL both installed.
2. **Qt6 launcher with in-launcher updates** — `/mnt/2nd4TB/IKV/src/launcher_main.cpp`, `launcher_updates.cpp`, `update_helper_main.cpp`; tests `tests/update_version_test.cpp`, `tests/launcher_gui_smoke.cpp`. Pattern-only.
3. **SDL3 + Vulkan renderer with RT effects** — `/mnt/1st4TB/etiyami/native/vulkan_renderer.cpp`, `vulkan_effects.cpp`, `native/shaders/effects_rt.comp`; RT alternatives `/mnt/2nd4TB/IKV/src/vulkan_rt.cpp`, `/mnt/2nd4TB/CitroenC4Robot/src/rendering/vulkan/raytracing.cpp`. Pattern-only.
4. **FSR 3.1.4 / frame-gen glue** — `/mnt/1st4TB/Kabus 22/native/fsr_vk.cpp`, `tools/k22_fsr3_blobs.py`; FidelityFX SDK itself is MIT: vendor from upstream, not from these trees.
5. **AppImage packaging** — `/mnt/2nd4TB/IKV/tools/package_appimage.py` + `tools/appimage/Dockerfile.ubuntu24` (pinned, uruntime); license-closure checks `/mnt/1st4TB/Pusu: Uyanış/packaging/license_bundle.py` (GPL-3; pattern-only).
6. **Ghidra headless scripts** — `/mnt/2nd4TB/CitroenC4Robot/tools/ghidra/analyze.py`, `/mnt/2nd4TB/SuperCan/tools/ghidra/inspect_containers.py`. Pattern-only.
7. **Controller test harness** — `/mnt/1st4TB/Kabus 22/tools/virtual-gamepad/vpad.py`. Pattern-only.
