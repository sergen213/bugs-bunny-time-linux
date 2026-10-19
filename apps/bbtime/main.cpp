// bbtime: collection front end (game select -> level list -> level viewer) for
// Bugs Bunny: Lost in Time and Bugs Bunny & Taz: Time Busters.
#include "bb/render/bmp.h"
#include "bb/render/renderer.h"
#include "fixed_step.h"
#include "level_hook.h"
#include "music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using bb::GameId;
using bb::render::Fidelity;

namespace {

constexpr uint8_t kWhite[4] = {255, 255, 255, 255};
constexpr uint8_t kGrey[4] = {170, 170, 170, 255};
constexpr uint8_t kYellow[4] = {255, 220, 60, 255};
constexpr uint8_t kRed[4] = {255, 60, 60, 255};
constexpr uint8_t kPanel[4] = {0, 0, 0, 170};
constexpr uint8_t kHighlight[4] = {40, 90, 200, 200};

struct Args {
    fs::path data;
    std::optional<GameId> game;
    std::string level;
    Fidelity fidelity = Fidelity::Enhanced;
    int width = 1280, height = 720;
    bool fullscreen = false, hidden = false, mute = false;
    fs::path screenshot;
    int frames = 30;
    std::optional<std::array<float, 5>> camera; // x y z yaw pitch (engine space, radians)
};

fs::path defaultDataRoot() {
    if (const char* e = std::getenv("BBTIME_DATA"); e && *e) return e;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return fs::path(x) / "bbtime/original";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local/share/bbtime/original";
}

constexpr const char* kUsage =
    "usage: bbtime [--data DIR] [--game lit|tb] [--level NAME] [--fidelity original|enhanced]\n"
    "              [--width N] [--height N] [--fullscreen] [--hidden] [--mute]\n"
    "              [--screenshot FILE.png|FILE.ppm] [--frames N] [--camera X,Y,Z,YAW,PITCH]\n"
    "DIR contains lit/ and tb/ (default: $BBTIME_DATA, else $XDG_DATA_HOME/bbtime/original)";

std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    a.data = defaultDataRoot();
    for (int i = 1; i < argc; ++i) {
        const std::string_view f = argv[i];
        auto value = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return argv[++i];
        };
        auto number = [&](int& out) {
            auto v = value();
            return v && std::from_chars(v->data(), v->data() + v->size(), out).ec == std::errc{} && out > 0;
        };
        bool ok = true;
        if (f == "--data") { auto v = value(); ok = bool(v); if (v) a.data = *v; }
        else if (f == "--game") {
            auto v = value();
            ok = v && (*v == "lit" || *v == "tb");
            if (ok) a.game = *v == "lit" ? GameId::LostInTime : GameId::TimeBusters;
        }
        else if (f == "--level") { auto v = value(); ok = bool(v); if (v) a.level = *v; }
        else if (f == "--fidelity") {
            auto v = value();
            ok = v && (*v == "original" || *v == "enhanced");
            if (ok) a.fidelity = *v == "original" ? Fidelity::Original : Fidelity::Enhanced;
        }
        else if (f == "--width") ok = number(a.width);
        else if (f == "--height") ok = number(a.height);
        else if (f == "--frames") ok = number(a.frames);
        else if (f == "--fullscreen") a.fullscreen = true;
        else if (f == "--hidden") a.hidden = true;
        else if (f == "--mute") a.mute = true;
        else if (f == "--screenshot") { auto v = value(); ok = bool(v); if (v) a.screenshot = *v; }
        else if (f == "--camera") {
            auto v = value();
            std::array<float, 5> c{};
            const char *p = v ? v->data() : nullptr, *end = v ? v->data() + v->size() : nullptr;
            for (size_t k = 0; ok && k < c.size(); ++k) {
                auto r = p ? std::from_chars(p, end, c[k]) : std::from_chars_result{nullptr, std::errc::invalid_argument};
                ok = r.ec == std::errc{} && (k + 1 == c.size() ? r.ptr == end : r.ptr < end && *r.ptr == ',');
                p = r.ptr + 1;
            }
            if (ok) a.camera = c;
        }
        else if (f == "--help" || f == "-h") { std::println("{}", kUsage); std::exit(0); }
        else ok = false;
        if (!ok) {
            std::println(stderr, "bbtime: bad or incomplete argument '{}'\n{}", f, kUsage);
            return std::nullopt;
        }
    }
    if (!a.level.empty() && !a.game) {
        std::println(stderr, "bbtime: --level needs --game");
        return std::nullopt;
    }
    return a;
}

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

// The install trees mix case (LiT Datas/bze, TB Datas/BZE): resolve each component case-insensitively.
fs::path findPath(fs::path base, std::initializer_list<std::string_view> parts) {
    for (std::string_view part : parts) {
        std::error_code ec;
        fs::path next;
        for (const auto& e : fs::directory_iterator(base, ec))
            if (iequals(e.path().filename().string(), part)) { next = e.path(); break; }
        if (next.empty()) return {};
        base = next;
    }
    return base;
}

struct Game {
    GameId id;
    const char* key;   // subdirectory of the data root
    const char* title;
    const char* shortName;
    fs::path root{}, backdropPath{};
    std::vector<std::string> levels{}; // BZE stems found on disk
    uint32_t backdrop = 0;
    int bw = 0, bh = 0;
    std::string status{};
};

void loadGame(Game& g, const fs::path& dataRoot, bb::render::Renderer& r) {
    g.root = dataRoot / g.key;
    // Title-screen backdrops (verified by eye): LiT Datas/ScrnBgEn.bmp 800x600x8, TB Datas/data/english.bmp 640x480x24.
    g.backdropPath = g.id == GameId::LostInTime ? findPath(g.root, {"Datas", "ScrnBgEn.bmp"})
                                                : findPath(g.root, {"Datas", "data", "english.bmp"});
    const fs::path bze = findPath(g.root, {"Datas", "bze"});
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(bze, ec))
        if (iequals(e.path().extension().string(), ".bze")) g.levels.push_back(e.path().stem().string());
    std::ranges::sort(g.levels);
    g.status = bze.empty() ? std::format("no Datas/bze under {}", g.root.string())
                           : std::format("{} levels in {}", g.levels.size(), bze.string());
    if (g.backdropPath.empty()) return;
    if (auto t = bb::render::loadBmp(g.backdropPath)) {
        g.backdrop = r.createImage(*t);
        g.bw = int(t->width);
        g.bh = int(t->height);
    } else {
        g.status += " | " + t.error();
    }
}

enum class Screen { Main, Options, Game, Level };
enum class Action { Up, Down, Left, Right, Accept, Back };

struct App {
    Args args;
    bb::render::Renderer r;
    bb::render::Settings s;
    Game games[2] = {{.id = GameId::LostInTime, .key = "lit", .title = "Bugs Bunny: Lost in Time", .shortName = "LiT"},
                     {.id = GameId::TimeBusters, .key = "tb", .title = "Bugs Bunny & Taz: Time Busters", .shortName = "TB"}};
    Screen screen = Screen::Main;
    int mainSel = 0, optSel = 0, gameIdx = 0, levelSel = 0;
    bool running = true, overlay = true, fullscreen = false, vsync = true;
    bb::Scene scene;
    size_t sceneTris = 0;
    float moveSpeed = 8.0f; // units per second, scaled to the scene in enterLevel()
    bb::render::Camera cam, prevCam;
    SDL_Gamepad* pad = nullptr;
    double frameTime = 1.0 / 60.0;
    float alpha = 0.0f;

    Game& game() { return games[gameIdx]; }

    void enterLevel(const std::string& level) {
        scene = bb::loadLevelScene(game().id, args.data, level);
        r.uploadScene(scene);
        sceneTris = 0;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const bb::Mesh& m : scene.meshes) {
            sceneTris += m.indices.size() / 3;
            for (const bb::Vertex& v : m.vertices) {
                lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
                hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            }
        }
        cam = {};
        if (lo.x > hi.x) lo = hi = glm::vec3(0.0f);
        const glm::vec3 centre = (lo + hi) * 0.5f;
        const float radius = std::max(glm::length(hi - lo) * 0.5f, 1.0f);
        const auto inside = [&](const std::array<float, 3>& p) {
            return glm::all(glm::greaterThanEqual(glm::vec3(p[0], p[1], p[2]), lo)) &&
                   glm::all(glm::lessThanEqual(glm::vec3(p[0], p[1], p[2]), hi));
        };
        float fogEnd = radius * 2.5f;
        if (scene.spawn && inside(*scene.spawn)) { // behind and above the player start, facing the player's direction
            cam.yaw = scene.spawnYaw;
            cam.pitch = -0.1f;
            const glm::vec3 p(scene.spawn->at(0), scene.spawn->at(1), scene.spawn->at(2));
            // ponytail: fixed offset (LiT units, Bugs is a few hundred tall); no collision, may clip into walls.
            cam.pos = p + glm::vec3(0.0f, 300.0f, 0.0f) - glm::vec3(cam.forward().x, 0.0f, cam.forward().z) * 300.0f;
        } else if (hi.z - lo.z < radius * 1e-3f) { // flat wall facing +Z (texture gallery): no fog
            cam.pos = centre + glm::vec3(0.0f, 0.0f, radius * 1.1f);
            fogEnd = radius * 8.0f;
        } else {
            cam.pos = centre + glm::vec3(0.0f, radius * 0.25f, radius * 0.75f);
            cam.pitch = -0.3f;
        }
        // Draw distance: the original fades over the last 25 % of D = min(0xFFFF, b + (9*fogDist >> 1) * u_level)
        // (RESEARCH.md §8.5); the per-level fogDist/u_level source is Research Required, so D scales with the level
        // extent here (LiT levels span ~10k-40k units -> near ~10, far = D).
        s.fogEnd = std::min(fogEnd, 65535.0f);
        s.fogStart = s.fogEnd * 0.75f;
        s.zNear = std::max(radius * 5e-4f, 0.05f);
        s.zFar = s.fogEnd * 1.05f;
        moveSpeed = std::max(8.0f, radius * 0.05f);
        prevCam = cam;
        screen = Screen::Level;
        SDL_SetWindowRelativeMouseMode(r.window(), true);
    }

    void leaveLevel() {
        SDL_SetWindowRelativeMouseMode(r.window(), false);
        screen = Screen::Game;
    }

    void menu(Action a) {
        const int step = a == Action::Up || a == Action::Left ? -1 : 1;
        switch (screen) {
        case Screen::Main:
            if (a == Action::Up || a == Action::Down) mainSel = (mainSel + step + 4) % 4;
            else if (a == Action::Back) running = false;
            else if (a == Action::Accept) {
                if (mainSel < 2) { gameIdx = mainSel; levelSel = 0; screen = Screen::Game; }
                else if (mainSel == 2) screen = Screen::Options;
                else running = false;
            }
            break;
        case Screen::Options:
            if (a == Action::Up || a == Action::Down) optSel = (optSel + step + 7) % 7;
            else if (a == Action::Back || (a == Action::Accept && optSel == 6)) screen = Screen::Main;
            else if (a != Action::Back) changeOption(a == Action::Left ? -1 : 1);
            break;
        case Screen::Game: {
            const int n = int(game().levels.size());
            if (a == Action::Back) screen = Screen::Main;
            else if (n == 0) break;
            else if (a == Action::Up || a == Action::Down) levelSel = (levelSel + step + n) % n;
            else if (a == Action::Left || a == Action::Right) levelSel = std::clamp(levelSel + step * 10, 0, n - 1);
            else if (a == Action::Accept) enterLevel(game().levels[size_t(levelSel)]);
            break;
        }
        case Screen::Level:
            if (a == Action::Back) leaveLevel();
            break;
        }
    }

    void changeOption(int dir) {
        switch (optSel) {
        case 0: s.fidelity = s.fidelity == Fidelity::Enhanced ? Fidelity::Original : Fidelity::Enhanced; break;
        case 1: s.vfovDeg = std::clamp(s.vfovDeg + 5.0f * float(dir), 30.0f, 120.0f); break;
        case 2: s.pillarbox = !s.pillarbox; break;
        case 3: s.msaa = !s.msaa; break;
        case 4: vsync = !vsync; r.setVsync(vsync); break;
        case 5: fullscreen = !fullscreen; r.setFullscreen(fullscreen); break;
        }
    }

    void event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (screen == Screen::Level) {
                cam.yaw -= e.motion.xrel * 0.0025f;
                cam.pitch = std::clamp(cam.pitch - e.motion.yrel * 0.0025f, -1.55f, 1.55f);
                prevCam.yaw = cam.yaw;
                prevCam.pitch = cam.pitch;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: if (screen != Screen::Level) menu(Action::Up); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: if (screen != Screen::Level) menu(Action::Down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (screen != Screen::Level) menu(Action::Left); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (screen != Screen::Level) menu(Action::Right); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: menu(Action::Accept); break;
            case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: menu(Action::Back); break;
            case SDL_GAMEPAD_BUTTON_NORTH: overlay = !overlay; break;
            default: break;
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode k = e.key.key;
            if (k == SDLK_F3) overlay = !overlay;
            else if (k == SDLK_F11) { fullscreen = !fullscreen; r.setFullscreen(fullscreen); }
            else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) menu(Action::Back);
            else if (screen == Screen::Level) {
                if (k == SDLK_F) s.fog = !s.fog;
                else if (k == SDLK_F1) s.fidelity = s.fidelity == Fidelity::Enhanced ? Fidelity::Original : Fidelity::Enhanced;
            }
            else if (k == SDLK_UP || k == SDLK_W) menu(Action::Up);
            else if (k == SDLK_DOWN || k == SDLK_S) menu(Action::Down);
            else if (k == SDLK_LEFT || k == SDLK_A) menu(Action::Left);
            else if (k == SDLK_RIGHT || k == SDLK_D) menu(Action::Right);
            else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_KP_ENTER) menu(Action::Accept);
            break;
        }
        default: break;
        }
    }

    // One simulation tick at kTickHz: free-fly camera.
    void tick(float dt) {
        prevCam = cam;
        if (screen != Screen::Level) return;
        const bool* key = SDL_GetKeyboardState(nullptr);
        glm::vec3 move(0.0f); // x = right, y = up, z = forward
        move.z += float(key[SDL_SCANCODE_W]) - float(key[SDL_SCANCODE_S]);
        move.x += float(key[SDL_SCANCODE_D]) - float(key[SDL_SCANCODE_A]);
        move.y += float(key[SDL_SCANCODE_SPACE] || key[SDL_SCANCODE_E]) - float(key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_Q]);
        float speed = key[SDL_SCANCODE_LSHIFT] ? moveSpeed * 4.0f : moveSpeed;
        if (pad) {
            auto axis = [&](SDL_GamepadAxis a) {
                const float v = float(SDL_GetGamepadAxis(pad, a)) / 32767.0f;
                return std::abs(v) < 0.15f ? 0.0f : v; // dead zone
            };
            move.x += axis(SDL_GAMEPAD_AXIS_LEFTX);
            move.z -= axis(SDL_GAMEPAD_AXIS_LEFTY);
            move.y += axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) - axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            cam.yaw -= axis(SDL_GAMEPAD_AXIS_RIGHTX) * 2.5f * dt;
            cam.pitch = std::clamp(cam.pitch - axis(SDL_GAMEPAD_AXIS_RIGHTY) * 2.0f * dt, -1.55f, 1.55f);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) speed *= 4.0f;
        }
        cam.pos += (cam.forward() * move.z + cam.right() * move.x + glm::vec3(0, 1, 0) * move.y) * speed * dt;
    }

    int uiScale() const { return std::max(1, r.height() / 360); }

    void backdrop(const Game& g, uint8_t dim) {
        if (!g.backdrop) return;
        const auto v = bb::render::fitAspect(r.width(), r.height(), float(g.bw) / float(g.bh));
        const uint8_t tint[4] = {dim, dim, dim, 255};
        r.drawImage(g.backdrop, float(v.x), float(v.y), float(v.w), float(v.h), s.fidelity == Fidelity::Enhanced, tint);
    }

    // Vertical list in a panel; returns nothing, highlights `sel`.
    void list(float x, float y, const std::vector<std::string>& items, int sel, int first, int rows) {
        const int sc = uiScale(), lh = (bb::render::Renderer::kGlyph + 4) * sc;
        size_t widest = 0;
        for (const auto& it : items) widest = std::max(widest, it.size());
        const float w = float((widest + 4) * size_t(bb::render::Renderer::kGlyph * sc));
        const int shown = std::min(rows, int(items.size()) - first);
        r.drawRect(x, y, w, float(shown * lh + 2 * sc), kPanel);
        for (int i = 0; i < shown; ++i) {
            const int idx = first + i;
            const float ly = y + float(i * lh + sc);
            if (idx == sel) r.drawRect(x, ly, w, float(lh), kHighlight);
            r.drawText(x + float(16 * sc), ly + float(2 * sc), sc, items[size_t(idx)], idx == sel ? kYellow : kWhite);
        }
    }

    void draw() {
        r.beginFrame(s);
        const int sc = uiScale(), g = bb::render::Renderer::kGlyph;
        const float W = float(r.width()), H = float(r.height());
        const std::string_view hint = "Up/Down: move  Enter/A: select  Esc/B: back  F3: diagnostics  F11: fullscreen";
        switch (screen) {
        case Screen::Main: {
            backdrop(games[mainSel < 2 ? mainSel : 0], 200);
            r.drawText(float(16 * sc), H * 0.45f - float(24 * sc), sc * 2, "BBTIME COLLECTION", kYellow);
            list(float(16 * sc), H * 0.45f, {games[0].title, games[1].title, "Options", "Quit"}, mainSel, 0, 4);
            break;
        }
        case Screen::Options: {
            backdrop(games[0], 90);
            auto onoff = [](bool b) { return b ? "On" : "Off"; };
            r.drawText(float(16 * sc), H * 0.3f - float(24 * sc), sc * 2, "OPTIONS", kYellow);
            list(float(16 * sc), H * 0.3f,
                 {std::format("Renderer mode:  {}", s.fidelity == Fidelity::Enhanced ? "Enhanced" : "Original Fidelity"),
                  std::format("Vertical FOV:   {:.0f} deg (Hor+)", s.vfovDeg),
                  std::format("4:3 pillarbox:  {} (Original mode)", onoff(s.pillarbox)),
                  std::format("MSAA:           {} (x{}, Enhanced mode)", onoff(s.msaa), r.msaaSamples()),
                  std::format("VSync:          {}", onoff(vsync)),
                  std::format("Fullscreen:     {}", onoff(fullscreen)), "Back"},
                 optSel, 0, 7);
            break;
        }
        case Screen::Game: {
            Game& gm = game();
            backdrop(gm, 255);
            const int rows = std::max(1, int(H * 0.6f) / ((g + 4) * sc));
            const int n = int(gm.levels.size());
            const int first = std::clamp(levelSel - rows / 2, 0, std::max(0, n - rows));
            r.drawRect(0, H - float(26 * sc), W, float(26 * sc), kPanel);
            r.drawText(float(8 * sc), H - float(22 * sc), sc, gm.title, kYellow);
            r.drawText(float(8 * sc), H - float(11 * sc), sc, gm.status, n ? kGrey : kRed);
            if (n) list(W - float(16 * g * sc) - float(16 * sc), H * 0.15f, gm.levels, levelSel, first, rows);
            break;
        }
        case Screen::Level: {
            bb::render::Camera c = cam;
            c.pos = glm::mix(prevCam.pos, cam.pos, alpha);
            c.yaw = glm::mix(prevCam.yaw, cam.yaw, alpha);
            c.pitch = glm::mix(prevCam.pitch, cam.pitch, alpha);
            r.drawScene(c, s);
            if (scene.placeholder) {
                const std::string_view msg =
                    scene.notes.empty() ? std::string_view("PLACEHOLDER - not original data") : std::string_view(scene.notes.front());
                const int bs = std::clamp(int(W) / int(msg.size() * size_t(g) + 4), 1, sc * 2); // fits narrow windows
                const float tw = float(msg.size() * size_t(g * bs));
                r.drawRect((W - tw) * 0.5f - float(4 * bs), H - float(14 * bs), tw + float(8 * bs), float(12 * bs), kPanel);
                r.drawText((W - tw) * 0.5f, H - float(12 * bs), bs, msg, kRed);
            }
            break;
        }
        }
        if (screen != Screen::Level) r.drawText(float(8 * sc), H - float(g * sc) - float(36 * sc), sc, hint, kGrey);
        if (overlay) drawOverlay();
    }

    void drawOverlay() {
        const int sc = std::max(1, (r.height() + 300) / 600);
        const bool enh = s.fidelity == Fidelity::Enhanced;
        std::string text = std::format(
            "GL {} | {}\nGL {}\n{:.1f} FPS  {:.2f} ms  {} draws  {}x{}\n"
            "mode {}  {}  MSAA {}  FOV {:.0f} {}  vsync {}\ntick {:.0f} Hz (UNVERIFIED)  alpha {:.2f}",
            r.glVendor(), r.glRenderer(), r.glVersion(), 1.0 / frameTime, frameTime * 1000.0, r.drawCalls(), r.width(),
            r.height(), enh ? "Enhanced" : "Original", enh ? std::format("trilinear aniso x{:.0f}", r.maxAnisotropy()) : "nearest",
            enh && s.msaa && r.msaaSamples() ? std::format("x{}", r.msaaSamples()) : "off", s.vfovDeg,
            !enh && s.pillarbox ? "4:3 pillarbox" : "Hor+", vsync ? "on" : "off", bb::kTickHz, alpha);
        if (screen == Screen::Level)
            text += std::format("\n{} {}  meshes {}  tris {}  textures {}  fog {}\npos {:.1f} {:.1f} {:.1f}  {}",
                                game().shortName, scene.level, scene.meshes.size(), sceneTris, scene.textures.size(),
                                s.fog ? "on" : "off", cam.pos.x, cam.pos.y, cam.pos.z,
                                scene.placeholder ? "PLACEHOLDER SCENE" : "decoded data");
        if (screen == Screen::Level)
            for (const std::string& n : scene.notes) text += "\n" + n;
        int lines = 1, cols = 0, cur = 0;
        for (char ch : text) {
            if (ch == '\n') { ++lines; cur = 0; } else cols = std::max(cols, ++cur);
        }
        const int g = bb::render::Renderer::kGlyph;
        r.drawRect(0, 0, float((cols + 2) * g * sc), float(lines * (g + 2) * sc + 8 * sc), kPanel);
        r.drawText(float(g * sc), float(4 * sc), sc, text, kWhite);
    }
};

} // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (!args) return 2;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::println(stderr, "SDL_Init: {}", SDL_GetError());
        return 1;
    }
    {
        App app;
        app.args = *args;
        app.s.fidelity = args->fidelity;
        app.fullscreen = args->fullscreen;
        bb::render::WindowConfig wc;
        wc.width = args->width;
        wc.height = args->height;
        wc.fullscreen = args->fullscreen;
        wc.hidden = args->hidden;
        if (std::string err = app.r.init(wc); !err.empty()) {
            std::println(stderr, "bbtime: {}", err);
            SDL_Quit();
            return 1;
        }
        app.r.setVsync(app.vsync);
        std::println("bbtime: {} | {} | {} | data {}", app.r.glVendor(), app.r.glRenderer(), app.r.glVersion(),
                     args->data.string());
        for (Game& g : app.games) {
            loadGame(g, args->data, app.r);
            std::println("bbtime: {}: {}{}", g.shortName, g.status,
                         g.backdrop ? " | backdrop " + g.backdropPath.string() : std::string(" | no backdrop"));
        }
        if (args->game) {
            app.gameIdx = *args->game == GameId::LostInTime ? 0 : 1;
            app.mainSel = app.gameIdx;
            app.screen = Screen::Game;
            if (!args->level.empty()) {
                auto& lv = app.game().levels;
                if (auto it = std::ranges::find_if(lv, [&](const std::string& l) { return iequals(l, args->level); }); it != lv.end()) {
                    app.levelSel = int(it - lv.begin());
                    app.enterLevel(*it);
                } else {
                    std::println(stderr, "bbtime: level '{}' not found on disk; showing placeholder anyway", args->level);
                    app.enterLevel(args->level);
                }
            }
            if (args->camera) {
                const auto& c = *args->camera;
                app.cam.pos = {c[0], c[1], c[2]};
                app.cam.yaw = c[3];
                app.cam.pitch = c[4];
                app.prevCam = app.cam;
            }
        }

        bb::FixedStep step;
        uint64_t last = SDL_GetTicksNS();
        int frame = 0, musicIdx = app.gameIdx;
        while (app.running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) app.event(e);
            const uint64_t now = SDL_GetTicksNS();
            const double dt = double(now - last) * 1e-9;
            last = now;
            app.frameTime += (dt - app.frameTime) * 0.05;
            for (int n = step.advance(dt); n > 0; --n) app.tick(float(step.dt));
            app.alpha = step.alpha();
            if (app.screen == Screen::Main && app.mainSel < 2) musicIdx = app.mainSel; // highlighted game
            else if (app.screen == Screen::Game) musicIdx = app.gameIdx;
            bbtime::menuMusic(args->data, app.screen == Screen::Level || args->mute
                                              ? std::nullopt : std::optional(app.games[musicIdx].id));
            app.draw();
            if (!args->screenshot.empty() && ++frame >= args->frames) {
                const bool ok = app.r.screenshot(args->screenshot);
                std::println("bbtime: screenshot {} {}", args->screenshot.string(), ok ? "written" : "FAILED");
                app.running = false;
            }
            app.r.present();
        }
        if (app.pad) SDL_CloseGamepad(app.pad);
        for (Game& g : app.games) app.r.destroyImage(g.backdrop);
    }
    SDL_Quit();
    return 0;
}
