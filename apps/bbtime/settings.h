// User settings shared by the game (bbtime) and the launcher (bbtime-launcher). No Qt, no SDL.
// File: $XDG_CONFIG_HOME/bbtime/settings.ini (default ~/.config/bbtime/settings.ini), one `key = value` per line,
// `#`/`;` comments. Unknown keys are kept and written back; malformed lines are skipped with a warning.
// Precedence in the game: built-in defaults < settings.ini < command-line flags. Contract: docs/LAUNCHER.md.
#pragma once
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bbtime {

// Logical actions bindable in settings (`bind.<action> = Key[,Key...]`, SDL key names). Defaults = LiT config.pc
// keyboard map (docs/re/GAMEPLAY.md §2.1).
inline constexpr std::string_view kActions[] = {
    "up", "down", "left", "right", "camera_left", "camera_right", "roll", "action",
    "look", "crouch", "jump", "kick", "select", "start",
};

struct Settings {
    // Install roots: the directory that contains Datas/. Empty = <data root>/lit|tb (bbtime --data / $BBTIME_DATA).
    std::filesystem::path litRoot, tbRoot;
    std::string renderer = "gl";       // gl | vk
    std::string fidelity = "enhanced"; // original | enhanced
    int width = 1280, height = 720;
    bool fullscreen = false;
    float fov = 60.0f;  // vertical degrees at 4:3, Hor+ wider (bb::render::Settings::vfovDeg)
    int msaa = 4;       // 0, 2, 4, 8
    bool vsync = true;
    // Vulkan renderer, Enhanced fidelity only: dynamic sun shadows (bb::render::Shadows). off | map (shadow map) | rt
    // (ray-query shadows) | rt_ao (+ ray-traced ambient occlusion). rt/rt_ao fall back to map without ray queries.
    std::string shadows = "rt_ao";
    // Vulkan, Enhanced only: ray-traced reflections on water / ice / polished floors (needs ray queries, else off).
    bool reflections = true;
    // Vulkan, Enhanced only: water surfaces. enhanced (stylised water: refracted bed, shore foam, reflections, glints,
    // waves from the characters) | cartoon (the same, toon-shaded) | original (the games' own translucent layers).
    std::string water = "enhanced";
    // Vulkan, Enhanced only: FSR 3.1.4. upscaling: off | fsr_quality | fsr_balanced | fsr_performance | native_aa;
    // frame_generation (FSR frame interpolation, one generated frame per rendered frame; with upscaling off it runs
    // FSR as native AA).
    std::string upscaling = "off";
    bool frameGeneration = false;
    // Enhanced only (BBLIT-Remaster's options; Original Fidelity plays exactly as the exe). stick_camera: LiT gameplay
    // camera on the right stick (horizontal), through the L2 / R2 manual rotation, camera_speed 4.12 units per tick at
    // full tilt (L2 / R2 step 38), camera_invert flips it; jump_buffer: ticks a LiT jump pressed before landing is kept
    // (0 = off); hud_edges: in-level HUD counters move to the window's edges at wide aspects (menus stay centred).
    bool stickCamera = true;
    int cameraSpeed = 52;
    bool cameraInvert = false;
    int jumpBuffer = 4;
    bool hudEdges = true;
    // Enhanced look (BBLIT-Remaster's options): haze = ground haze strength % in L01A / L01B / L01D1 / L01D2 (exe
    // levels 4 / 5 / 7 / 8, Vulkan), 0 = off;
    // title_logo = % of its depth the TITLE logo is drawn at (keeps it in front of the machine's carrot; 100 = as the
    // exe); push_r = Bugs' visual radius against a pushable stone (it is drawn moved out of him; 0 = off).
    int haze = 100;
    int titleLogo = 85;
    int pushR = 50;
    // music_volume = LiT config.pc +0x34 (pause menu steps: 0, 50..100 by 5; below 50 the exe starts no music), played
    // at litMusicGain; sfx_volume = +0x30 (0..100 by 10, linear).
    int musicVolume = 100, sfxVolume = 100; // 0..100
    // LiT config.pc / GAME_DATA kept by bbtime (docs/re/GAMEPLAY.md §2.2, §8.1): lit_pad_preset = G[0x11], the pad
    // remap preset 0..3 applied at every level start (0x448fa0); lit_fog_distance = +0x1c (0 near, 1 normal, 2 far);
    // lit_renderer = +0x04 (2 3-D card, 0 / 1 without: kept and shown, bbtime always uses its renderer);
    // lit_language = +0x08 (0..5; the installer's choice, G[0x10]): the level text / speech banks, the speech file
    // speeches/Speech0N.xa and the loading screens <name>_N.bze (this disc ships 0, 1, 3);
    // lit_joy_buttons = joyMap +0x3c: the gamepad button of kick, crouch, look, jump, roll, camera right, action,
    // camera left (pause menu Controls / Joystick; buttons 0 △ 1 ○ 2 ✕ 3 □ 4 L1 5 R1 6 R2 7 L2 8 Back 9 Start ...).
    int litPadPreset = 0, litFogDistance = 2, litRenderer = 2, litLanguage = 0;
    std::array<int, 8> litJoyButtons{3, 1, 0, 2, 4, 6, 5, 7};
    // TB front end Video page (bblit2.exe config.pc, docs/re/TIME_BUSTERS.md §8.1): +0xe0 brightness (HSI intensity
    // factor 0.25..2 on TB textures), +0x04 renderer (2 3-D card, 1 software) and +0x18 detail level (0 low .. 2
    // high) are kept and shown; bbtime always draws with its own renderer at the high detail level.
    float tbBrightness = 1.0f;
    int tbRenderer = 2, tbDetails = 2;
    // TB registry `language` (0x414550 / 0x414770; 0 EN, 1 FR, 2 DE, 3 ES, 4 IT, 5 PT, 6 NL): the front end's language
    // and GD+0x93 of a new game (0x401ad0), which picks the level's language section, speech and text blocks.
    int tbLanguage = 0;
    std::map<std::string, std::string> bindings; // action -> comma-separated SDL key names
    std::string updateUrl;                       // launcher only: signed release manifest URL (https)
    // Enabled mods in load order (later wins), ids of $XDG_DATA_HOME/bbtime/mods/<id>/ (bb/mods.h, docs/MODDING.md).
    std::vector<std::string> mods;
    std::map<std::string, std::string> unknown;  // keys this build does not know, preserved verbatim

    Settings();
};

std::filesystem::path configDir(); // $XDG_CONFIG_HOME/bbtime
std::filesystem::path dataDir();   // $XDG_DATA_HOME/bbtime
std::filesystem::path stateDir();  // $XDG_STATE_HOME/bbtime (logs)
// LiT music gain of the config volume v (0x41d5f0: IDirectSoundBuffer::SetVolume((v - 100) * 100), hundredths of a
// dB: 100 = 0 dB, 50 = -50 dB, 0 = -100 dB, DSBVOLUME_MIN).
inline float litMusicGain(int v) { return std::pow(10.0f, float(v - 100) / 20.0f); }
inline std::filesystem::path settingsPath() { return configDir() / "settings.ini"; }
inline std::filesystem::path saveDir() { return dataDir() / "saves"; } // saves/<lit|tb>/

// Never throws; problems are appended to `warnings` as "line N: ...".
Settings parseSettings(std::string_view text, std::vector<std::string>* warnings = nullptr);
std::string formatSettings(const Settings& s);
// Missing file = defaults, no warning.
Settings loadSettings(const std::filesystem::path& file = settingsPath(), std::vector<std::string>* warnings = nullptr);
// Atomic (write temp + rename). Returns an error message, empty on success.
std::string saveSettings(const Settings& s, const std::filesystem::path& file = settingsPath());

} // namespace bbtime
