#include "settings.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace bbtime {
namespace {

fs::path xdg(const char* var, const char* fallback) {
    if (const char* v = std::getenv(var); v && *v == '/') return fs::path(v) / "bbtime"; // spec: relative = ignore
    const char* home = std::getenv("HOME");
    return fs::path(home && *home ? home : ".") / fallback / "bbtime";
}

std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == s.npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r") - b + 1);
}

template <class T> bool number(std::string_view v, T& out, T lo, T hi) {
    T x{};
    const auto r = std::from_chars(v.data(), v.data() + v.size(), x);
    if (r.ec != std::errc{} || r.ptr != v.data() + v.size() || x < lo || x > hi) return false;
    out = x;
    return true;
}

bool boolean(std::string_view v, bool& out) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") return out = true, true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return out = false, true;
    return false;
}

bool oneOf(std::string_view v, std::string& out, std::initializer_list<std::string_view> allowed) {
    if (std::ranges::find(allowed, v) == allowed.end()) return false;
    out = v;
    return true;
}

} // namespace

Settings::Settings() {
    bindings = {{"up", "Up"},     {"down", "Down"},  {"left", "Left"},         {"right", "Right"},
                {"camera_left", "Q"}, {"camera_right", "W"}, {"roll", "E"}, {"action", "R"},
                {"look", "A"},    {"crouch", "S"},   {"jump", "D"},            {"kick", "F"},
                {"select", "Tab"}, {"start", "Return"}};
}

fs::path configDir() { return xdg("XDG_CONFIG_HOME", ".config"); }
fs::path dataDir() { return xdg("XDG_DATA_HOME", ".local/share"); }
fs::path stateDir() { return xdg("XDG_STATE_HOME", ".local/state"); }

Settings parseSettings(std::string_view text, std::vector<std::string>* warnings) {
    Settings s;
    auto warn = [&](int line, std::string msg) {
        if (warnings) warnings->push_back(std::format("line {}: {}", line, msg));
    };
    int lineNo = 0;
    for (size_t pos = 0; pos <= text.size();) {
        size_t end = text.find('\n', pos);
        if (end == text.npos) end = text.size();
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNo;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == line.npos) { warn(lineNo, "expected key = value"); continue; }
        const std::string key(trim(line.substr(0, eq)));
        const std::string_view v = trim(line.substr(eq + 1));
        if (key.empty()) { warn(lineNo, "empty key"); continue; }
        bool ok = true;
        if (key == "lit_root") s.litRoot = v;
        else if (key == "tb_root") s.tbRoot = v;
        else if (key == "renderer") ok = oneOf(v, s.renderer, {"gl", "vk"});
        else if (key == "fidelity") ok = oneOf(v, s.fidelity, {"original", "enhanced"});
        else if (key == "width") ok = number(v, s.width, 320, 16384);
        else if (key == "height") ok = number(v, s.height, 200, 16384);
        else if (key == "fullscreen") ok = boolean(v, s.fullscreen);
        else if (key == "fov") ok = number(v, s.fov, 30.0f, 120.0f);
        else if (key == "msaa") ok = number(v, s.msaa, 0, 8) && (s.msaa & (s.msaa - 1)) == 0 && s.msaa != 1;
        else if (key == "vsync") ok = boolean(v, s.vsync);
        else if (key == "music_volume") ok = number(v, s.musicVolume, 0, 100);
        else if (key == "sfx_volume") ok = number(v, s.sfxVolume, 0, 100);
        else if (key == "update_url") s.updateUrl = v;
        else if (key.starts_with("bind.")) {
            const std::string action = key.substr(5);
            ok = std::ranges::find(kActions, action) != std::end(kActions) && !v.empty();
            if (ok) s.bindings[action] = v;
        } else s.unknown[key] = v;
        if (!ok) {
            if (key == "msaa") s.msaa = Settings{}.msaa; // number() may have stored a rejected value
            warn(lineNo, std::format("bad value for {}: '{}'", key, v));
        }
    }
    return s;
}

std::string formatSettings(const Settings& s) {
    std::string o = "# bbtime settings (written by bbtime-launcher; see docs/LAUNCHER.md). CLI flags override.\n";
    auto kv = [&](std::string_view k, const auto& v) { o += std::format("{} = {}\n", k, v); };
    kv("lit_root", s.litRoot.string());
    kv("tb_root", s.tbRoot.string());
    kv("renderer", s.renderer);
    kv("fidelity", s.fidelity);
    kv("width", s.width);
    kv("height", s.height);
    kv("fullscreen", s.fullscreen ? "true" : "false");
    kv("fov", s.fov);
    kv("msaa", s.msaa);
    kv("vsync", s.vsync ? "true" : "false");
    kv("music_volume", s.musicVolume);
    kv("sfx_volume", s.sfxVolume);
    kv("update_url", s.updateUrl);
    for (const auto& [a, k] : s.bindings) kv("bind." + a, k);
    for (const auto& [k, v] : s.unknown) kv(k, v);
    return o;
}

Settings loadSettings(const fs::path& file, std::vector<std::string>* warnings) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseSettings(ss.str(), warnings);
}

std::string saveSettings(const Settings& s, const fs::path& file) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = file.string() + std::format(".tmp{}", getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        const std::string text = formatSettings(s);
        if (!f.write(text.data(), std::streamsize(text.size())) || !f.flush())
            return std::format("cannot write {}", tmp.string());
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return std::format("cannot replace {}", file.string());
    }
    return {};
}

} // namespace bbtime
