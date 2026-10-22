// User settings shared by the game (bbtime) and the launcher (bbtime-launcher). No Qt, no SDL.
// File: $XDG_CONFIG_HOME/bbtime/settings.ini (default ~/.config/bbtime/settings.ini), one `key = value` per line,
// `#`/`;` comments. Unknown keys are kept and written back; malformed lines are skipped with a warning.
// Precedence in the game: built-in defaults < settings.ini < command-line flags. Contract: docs/LAUNCHER.md.
#pragma once
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
    // Vulkan, Enhanced only: FSR 3.1.4. upscaling: off | fsr_quality | fsr_balanced | fsr_performance | native_aa;
    // frame_generation (FSR frame interpolation, one generated frame per rendered frame; with upscaling off it runs
    // FSR as native AA).
    std::string upscaling = "off";
    bool frameGeneration = false;
    int musicVolume = 100, sfxVolume = 100; // 0..100
    // TB front end Video page (bblit2.exe config.pc, docs/re/TIME_BUSTERS.md §8.1): +0xe0 brightness (HSI intensity
    // factor 0.25..2 on TB textures), +0x04 renderer (2 3-D card, 1 software) and +0x18 detail level (0 low .. 2
    // high) are kept and shown; bbtime always draws with its own renderer at the high detail level.
    float tbBrightness = 1.0f;
    int tbRenderer = 2, tbDetails = 2;
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
