// settings.ini: parse with malformed lines, format -> parse round trip, atomic save/load, missing file.
#include "settings.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <print>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
    std::vector<std::string> w;
    const bbtime::Settings s = bbtime::parseSettings(
        "# comment\n"
        "; also comment\n"
        "  width = 1920  \r\n"
        "height=1080\n"
        "this line is junk\n"            // 5: no '='
        "= value\n"                      // 6: empty key
        "fullscreen = maybe\n"           // 7: bad bool
        "renderer = vk\n"
        "fidelity = ultra\n"             // 9: not allowed
        "fov = 72.5\n"
        "msaa = 3\n"                     // 11: not 0/2/4/8
        "vsync = off\n"
        "music_volume = 101\n"           // 13: out of range
        "sfx_volume = 40\n"
        "width = 12x\n"                  // 15: trailing junk -> keeps 1920
        "lit_root = /games/Bugs Bunny/lit\n"
        "bind.jump = Space,D\n"
        "bind.fly = X\n"                 // 18: unknown action
        "future_key = kept as is = really\n"
        "shadows = map\n"
        "reflections = off\n"
        "upscaling = fsr_balanced\n"
        "frame_generation = on\n"
        "mods = hd-textures, big-head ,,taz-swap\n"
        "update_url = https://example.invalid/manifest.json",  // no trailing newline
        &w);
    assert(w.size() == 8);
    assert(w[0].starts_with("line 5:") && w[1].starts_with("line 6:") && w[7].starts_with("line 18:"));
    for (const auto& x : w) std::println("warning {}", x);
    assert(s.width == 1920 && s.height == 1080 && !s.fullscreen && s.renderer == "vk" && s.fidelity == "enhanced");
    assert(s.fov == 72.5f && s.msaa == 4 && !s.vsync && s.musicVolume == 100 && s.sfxVolume == 40 && s.shadows == "map");
    assert(!s.reflections && s.upscaling == "fsr_balanced" && s.frameGeneration);
    assert(s.litRoot == "/games/Bugs Bunny/lit" && s.tbRoot.empty());
    assert(s.bindings.at("jump") == "Space,D" && s.bindings.at("kick") == "F" && !s.bindings.contains("fly"));
    assert(s.unknown.at("future_key") == "kept as is = really");
    assert(s.updateUrl == "https://example.invalid/manifest.json");
    assert((s.mods == std::vector<std::string>{"hd-textures", "big-head", "taz-swap"}));

    // Round trip: everything formatSettings writes parses back identically and warning-free.
    std::vector<std::string> w2;
    const std::string text = bbtime::formatSettings(s);
    const bbtime::Settings r = bbtime::parseSettings(text, &w2);
    assert(w2.empty() && bbtime::formatSettings(r) == text);
    assert(r.width == s.width && r.fov == s.fov && r.litRoot == s.litRoot && r.bindings == s.bindings &&
           r.unknown == s.unknown && r.vsync == s.vsync && r.renderer == s.renderer && r.mods == s.mods);

    // Defaults round-trip too, and a missing file yields defaults.
    assert(bbtime::formatSettings(bbtime::parseSettings(bbtime::formatSettings({}))) == bbtime::formatSettings({}));
    char tmpl[] = "/tmp/bbtime-settings-XXXXXX";
    const fs::path dir = mkdtemp(tmpl);
    const fs::path file = dir / "sub/settings.ini";
    assert(bbtime::formatSettings(bbtime::loadSettings(file)) == bbtime::formatSettings({}));
    assert(bbtime::saveSettings(s, file).empty());
    assert(bbtime::formatSettings(bbtime::loadSettings(file)) == text);
    assert(std::distance(fs::directory_iterator(file.parent_path()), fs::directory_iterator()) == 1); // no temp left

    // XDG paths: absolute env wins, relative is ignored per spec.
    setenv("XDG_CONFIG_HOME", "/cfg", 1);
    assert(bbtime::settingsPath() == "/cfg/bbtime/settings.ini");
    setenv("XDG_CONFIG_HOME", "rel", 1);
    setenv("HOME", "/home/x", 1);
    assert(bbtime::settingsPath() == "/home/x/.config/bbtime/settings.ini");
    unsetenv("XDG_DATA_HOME");
    assert(bbtime::saveDir() == "/home/x/.local/share/bbtime/saves");

    fs::remove_all(dir);
    std::println("settings: all checks passed");
    return 0;
}
