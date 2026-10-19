# Build/run environment (observed 2026-10-18)

All values from commands run on this host (`uname -r`, `/etc/os-release`, `lscpu`, `free -h`, `glxinfo -B`, `vulkaninfo`, `--version`, `pkg-config --modversion`, `pacman -Q`, `ls /dev/input/by-id`).

## Host
| Item | Value |
|---|---|
| OS / kernel | CachyOS, Linux 7.2.9-2-cachyos, x86_64 |
| Session | Wayland, KDE (`XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=KDE`) |
| CPU | AMD Ryzen 9 9950X3D 16-Core (32 threads) |
| RAM | 30 GiB |
| GPU | AMD Radeon RX 7900 XTX (navi31) |
| OpenGL | radeonsi/ACO, Mesa 26.2.4-arch3.1, core profile 4.6 |
| Vulkan | RADV NAVI31, apiVersion 1.4.354, Mesa 26.2.4-arch3.1 |
| Ray tracing | `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_query` all reported by `vulkaninfo` |

## Toolchain
| Tool | Version |
|---|---|
| GCC | 16.2.1 (20260810) |
| Clang | 23.1.3 |
| CMake | 4.4.4 |
| Ninja | 1.13.2 |
| glslc (shaderc) | 2026.4 |
| Ghidra | 12.1.2 (`/opt/ghidra`) |
| Java | OpenJDK 27 |
| Python | 3.14.8 |
| minisign | 0.12 |
| ffmpeg | n9.0.2 |

Project standard: C++26 (root CMakeLists); GCC 16 / Clang 23 / CMake 4.4 support it.

## Libraries
| Dependency | Status | Version |
|---|---|---|
| SDL3 | installed | 3.4.18 |
| SDL3_ttf | **installed** (pacman `sdl3_ttf` 3.2.2-3.2; earlier context said missing — it is present) | 3.2.2 |
| SDL3_image | missing (no pkg-config `sdl3-image`) | — |
| SDL3_mixer | missing (no pkg-config `sdl3-mixer`) | — |
| Qt6 (qt6-base) | installed | 6.12.0 |
| Vulkan loader/headers | installed | 1.4.363 (pkg-config `vulkan`) |
| vulkan-validation-layers | installed | 1.4.363.0 |
| glm | installed (header-only, no `.pc`; `/usr/include/glm/glm.hpp`) | 1.0.3 |
| OpenAL | installed | 1.25.2 |
| libsndfile | installed | 1.2.2 |
| libsodium | installed | 1.0.22 |
| OpenSSL | installed | 3.6.5 |
| libcurl | installed | 8.23.0 |
| GoogleTest | installed | 1.18.0 |
| Catch2 | missing (`pacman -Q catch2`: not found) | — |

## Input devices (`/dev/input/by-id`)
- Controller: `usb-8BitDo_Ultimate_Wireless_Controller_…-hidraw` (hidraw only; no `/dev/input/js*` nodes; SDL3 hidapi path expected to handle it — [INFERENCE], untested).
- Keychron C2 Pro 8K keyboard (exposes an `event-joystick` interface), Razer DeathAdder V3 mouse, Gigabyte ITE device.

## Packages
No package install is currently required. Optional, proposed only:
- `sudo pacman -S sdl3_image` — only if PNG/BMP loading beyond `SDL_LoadBMP` is needed.
- `sudo pacman -S sdl3_mixer` — only if OpenAL+libsndfile route is dropped.
- `sudo pacman -S catch2` — not needed (plain assert tests / gtest).
