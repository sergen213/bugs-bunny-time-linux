// bbtime: collection front end (game select -> level list -> level viewer) for
// Bugs Bunny: Lost in Time and Bugs Bunny & Taz: Time Busters.
#include "bb/render/bmp.h"
#include "bb/render/renderer.h"
#include "bb/render_vk/renderer.h"
#include "fixed_step.h"
#include "level_hook.h"
#include "music.h"
#include "play.h"
#include "settings.h"
#include "tb_front.h"

#include "bb/actor.h"
#include "bb/audio.h"
#include "bb/game/lit_save.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <span>
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
    bool noHud = false;  // --no-hud: no text HUD / diagnostics overlay (clean screenshots for compares)
    fs::path screenshot;
    int frames = 30;
    std::optional<std::array<float, 5>> camera; // x y z yaw pitch (engine space, radians)
    bool vulkan = false, vkValidation = false;
    fs::path roots[2];  // per-game install roots (contain Datas/): [0] lit, [1] tb
    bool vsync = true;
    int msaa = 4;
    float fov = 60.0f;
    std::map<std::string, std::string> bindings;  // action -> SDL key names (settings.ini, launcher)
    std::vector<bb::game::PadInput> autoplay;     // --autoplay: one pad input per tick, lockstep rendering
    std::vector<bb::game::PadInput> autoplay2;    // --autoplay2: TB player 2 (with --players 2)
    int players = 1;                              // --players: TB 1-player (switching) or 2-player co-op
    int slot = -1;                                // --slot: LiT save slot 1..6 (saveDir()/lit/Savegame<N>.dat)
    fs::path importSaves;                         // --import-saves: original Datas/bin (or install root) to import
    bool menu = false;                            // --menu: start in --game's original front end
};

fs::path defaultDataRoot() {
    if (const char* e = std::getenv("BBTIME_DATA"); e && *e) return e;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return fs::path(x) / "bbtime/original";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local/share/bbtime/original";
}

constexpr const char* kUsage =
    "usage: bbtime [--data DIR] [--game lit|tb] [--level NAME] [--fidelity original|enhanced]\n"
    "              [--width N] [--height N] [--fullscreen] [--hidden] [--mute] [--no-hud]\n"
    "              [--screenshot FILE.png|FILE.ppm] [--frames N] [--camera X,Y,Z,YAW,PITCH]\n"
    "              [--renderer gl|vk] [--vk-validation] [--autoplay KEYS*TICKS,...]\n"
    "              [--players 1|2] [--autoplay2 KEYS*TICKS,...]  (TB co-op; autoplay2 = player 2)\n"
    "              [--slot 1-6] [--import-saves DIR]  (LiT saves in $XDG_DATA_HOME/bbtime/saves/lit)\n"
    "              [--menu]  (with --game, no --level: start in that game's original front end)\n"
    "DIR contains lit/ and tb/ (default: $BBTIME_DATA, else $XDG_DATA_HOME/bbtime/original)";

std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    a.data = defaultDataRoot();
    // Precedence: built-in defaults < settings.ini (launcher, docs/LAUNCHER.md) < command line.
    std::vector<std::string> warn;
    const bbtime::Settings cfg = bbtime::loadSettings(bbtime::settingsPath(), &warn);
    for (const auto& w : warn) std::println(stderr, "bbtime: {}: {}", bbtime::settingsPath().string(), w);
    a.fidelity = cfg.fidelity == "original" ? Fidelity::Original : Fidelity::Enhanced;
    a.width = cfg.width;
    a.height = cfg.height;
    a.fullscreen = cfg.fullscreen;
    a.vsync = cfg.vsync;
    a.msaa = cfg.msaa;
    a.fov = cfg.fov;
    a.vulkan = cfg.renderer == "vk";
    a.bindings = cfg.bindings;
    bool dataExplicit = std::getenv("BBTIME_DATA") && *std::getenv("BBTIME_DATA");
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
        if (f == "--data") { auto v = value(); ok = bool(v); if (v) a.data = *v; dataExplicit = true; }
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
        else if (f == "--no-hud") a.noHud = true;
        else if (f == "--renderer") {
            auto v = value();
            ok = v && (*v == "gl" || *v == "vk");
            if (ok) a.vulkan = *v == "vk";
        }
        else if (f == "--vk-validation") a.vkValidation = true;
        else if (f == "--autoplay" || f == "--autoplay2") {
            auto v = value();
            auto script = v ? bbtime::parseAutoplay(*v) : std::nullopt;
            ok = bool(script);
            if (ok) (f == "--autoplay" ? a.autoplay : a.autoplay2) = std::move(*script);
        }
        else if (f == "--players") {
            auto v = value();
            ok = v && (*v == "1" || *v == "2");
            if (ok) a.players = *v == "2" ? 2 : 1;
        }
        else if (f == "--slot") ok = number(a.slot) && a.slot <= 6;
        else if (f == "--import-saves") { auto v = value(); ok = bool(v); if (v) a.importSaves = *v; }
        else if (f == "--menu") a.menu = true;
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
    // --data / $BBTIME_DATA > settings lit_root/tb_root > $XDG_DATA_HOME/bbtime/original/<lit|tb>.
    a.roots[0] = !dataExplicit && !cfg.litRoot.empty() ? cfg.litRoot : a.data / "lit";
    a.roots[1] = !dataExplicit && !cfg.tbRoot.empty() ? cfg.tbRoot : a.data / "tb";
    if (!a.level.empty() && !a.game) {
        std::println(stderr, "bbtime: --level needs --game");
        return std::nullopt;
    }
    if (a.slot > 0 && a.game != GameId::LostInTime) {
        std::println(stderr, "bbtime: --slot needs --game lit");
        return std::nullopt;
    }
    if (a.menu && (!a.game || !a.level.empty())) {
        std::println(stderr, "bbtime: --menu needs --game and no --level");
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
    const char* key;   // short id (lit / tb)
    const char* title;
    const char* shortName;
    fs::path root{}, backdropPath{};
    std::vector<std::string> levels{}; // BZE stems found on disk
    uint32_t backdrop = 0;
    int bw = 0, bh = 0;
    std::string status{};
};

template <class R> // bb::render::Renderer (GL) or bb::render_vk::Renderer
void loadGame(Game& g, const fs::path& root, R& r) {
    g.root = root;
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

enum class Screen { Main, Options, Game, Level, TbFront };
enum class Action { Up, Down, Left, Right, Accept, Back };

template <class R> // bb::render::Renderer (GL) or bb::render_vk::Renderer: same interface shape
struct App {
    Args args;
    R r;
    bb::render::Settings s;
    Game games[2] = {{.id = GameId::LostInTime, .key = "lit", .title = "Bugs Bunny: Lost in Time", .shortName = "LiT"},
                     {.id = GameId::TimeBusters, .key = "tb", .title = "Bugs Bunny & Taz: Time Busters", .shortName = "TB"}};
    Screen screen = Screen::Main;
    int mainSel = 0, optSel = 0, gameIdx = 0, levelSel = 0;
    bool running = true, overlay = true, fullscreen = false, vsync = true;
    bool helpKey = false;                 // LiT F1 pressed since the last tick (PadInput::help)
    bb::Scene scene;
    size_t sceneTris = 0;
    float moveSpeed = 8.0f; // units per second, scaled to the scene in enterLevel()
    bb::render::Camera cam, prevCam;
    SDL_Gamepad* pad = nullptr;
    SDL_Gamepad* pad2 = nullptr;          // TB player 2
    double frameTime = 1.0 / 60.0;
    float alpha = 0.0f;
    uint32_t simTick = 0;                 // 30 Hz ticks since start (texture animation clock)
    std::vector<bb::AnimState> actorAnim; // per Scene::actors (player excluded in play mode)
    std::vector<bb::Mesh> actorMeshes;    // posed every frame
    std::optional<bbtime::Play> play;     // LiT play mode (bb_game simulation)
    std::optional<bbtime::TbPlay> tbPlay; // TB play mode (bb_game TbWorld: Bugs + Taz)
    int players = 1;                      // TB 1 or 2 players (Options, --players)
    bbtime::KeyMap tbKeys[2] = {bbtime::tbKeyMap(0), bbtime::tbKeyMap(1)};
    size_t autoplay2At = 0;
    std::optional<std::array<uint8_t, 0x300>> tbCarry;  // TB GAME_DATA for the next enterLevel (reload 0x4014c0 keeps it)
    int tbLevel = -1;                     // current TB level-table index (GD+0), -1 = not in the table
    std::string playStatus;               // why a LiT level stayed in viewer mode
    // Original SFX mixer commands (bb::game::Sfx) -> OpenAL. The config SFX volume of the exes' formulas follows the
    // device's SFX volume (settings.ini sfx_volume / the TB Sound page): LiT 0..100, TB 0..127.
    void pumpSfx(bb::game::Sfx& s) {
        bb::AudioDevice* dev = args.mute ? nullptr : bbtime::audioDevice();
        if (dev)
            for (const bb::SfxCmd& c : s.cmds) dev->sfx(c);
        s.cmds.clear();
        s.volume = int32_t(std::lround((dev ? dev->sfxVolume() : 1.0f) * (s.tb ? 127 : 100)));
    }
    bbtime::KeyMap keys;
    size_t autoplayAt = 0;
    // LiT GAME_DATA image of the active slot (--slot; Savegame<N>.dat or a new game): seeds G[] when a level is
    // entered without carried globals, and keeps GD+0x08..+0x3f and the checksum chain for the next save.
    std::optional<bb::game::LitSaveGame> litSave;
    int litLevel = 0;                     // current LiT level id (GD+0), for saves
    // LiT level change (GAMEPLAY §1.2): 0x44bec0 fade out while the world keeps ticking, then the loading screen
    // (the loading BZE's TIM) while the target loads (+ the 3000 ms minimum of LitWorld::holdsLoading), then two
    // unrendered ticks and the 0x44be90 fade in (litEnter; also on the first entry, like the exe's /b boot path).
    struct LitLoading { int next = -1; std::array<uint8_t, 256> G{}; uint32_t image = 0; uint64_t startMs = 0; bool loaded = false; };
    std::optional<LitLoading> litLoading;
    bool litFadingOut = false, litEnter = false;
    bool escKey = false;                  // LiT Esc pressed since the last tick (PadInput::esc, title C96)
    bool frontEnd = false;                // session started from a game's original front end: leaving returns to Main
    std::vector<uint32_t> fontImages;     // per Scene::overlayFont entry (0 = missing), built by drawOverlayText
    Fidelity fontFidelity = Fidelity::Original;
    std::map<std::array<int16_t, 5>, uint32_t> hudImages;  // TB HUD sprite crops {frame, u, v, w, h} -> image
    bool tbFadingOut = false;             // TB level change: 0x401910 fade-out running before the load
    std::optional<bbtime::TbFront> tbFront;  // TB original front end (tb_front.cpp), kept for the session
    uint32_t tbFrontImage = 0;            // its 640x480 canvas
    uint32_t tbAutoPrev = 0;              // --autoplay buttons (+ 0x10000 Esc) of the previous front-end tick

    Game& game() { return games[gameIdx]; }

    // globals: GAME_DATA carried over a level change (LiT G persists across levels, GAMEPLAY §1.2).
    void enterLevel(const std::string& level, const std::array<uint8_t, 256>* globals = nullptr) {
        bb::BzeFile bze;
        scene = bb::loadLevelScene(game().id, game().root, level, &bze);
        freeFont();
        actorAnim.assign(scene.actors.size(), {});
        play.reset();
        tbPlay.reset();
        playStatus.clear();
        if (bb::AudioDevice* dev = args.mute ? nullptr : bbtime::audioDevice()) dev->stopSfx();  // the level's voices
        if (game().id == GameId::LostInTime && !scene.placeholder) {
            try {
                bb::game::LitWorld w = bb::game::LitWorld::load(bze, &scene);
                // Debug: BBTIME_LIT_START="x,y,z" (PS1 units) places Bugs there in the first LiT level entered
                // (smoke screenshots of far-away spots such as level exits); later levels start normally.
                static bool litStartUsed = false;
                if (const char* s = std::getenv("BBTIME_LIT_START"); s && !litStartUsed) {
                    litStartUsed = true;
                    bb::game::V3 at{};
                    if (std::sscanf(s, "%d,%d,%d", &at.x, &at.y, &at.z) == 3) w.start(at, 0, -1);
                }
                if (globals) w.G = *globals;
                else if (litSave) std::ranges::copy(litSave->G(), w.G.begin());
                litLevel = 0;
                for (int id = 0; !bb::game::LitWorld::levelName(id).empty(); ++id)
                    if (iequals(bb::game::LitWorld::levelName(id), level)) { litLevel = id; break; }
                w.level = litLevel;
                // >D overlay key names: the first key bound to each config kbdMap action, in the exe's DIK spelling
                // for the default keys (letters as is, arrows / Tab / Return as KEY_*).
                static constexpr const char* kKeyActions[8] = {"kick", "crouch", "look", "jump", "roll", "camera_right", "action", "camera_left"};
                for (size_t n = 0; n < 8; ++n)
                    if (auto b = args.bindings.find(kKeyActions[n]); b != args.bindings.end() && !b->second.empty()) {
                        std::string k = b->second.substr(0, b->second.find(','));
                        std::ranges::transform(k, k.begin(), [](unsigned char c) { return char(std::toupper(c)); });
                        static const std::map<std::string, std::string> kDik = {
                            {"UP", "KEY_UPARROW"}, {"DOWN", "KEY_DOWNARROW"}, {"LEFT", "KEY_LEFTARROW"},
                            {"RIGHT", "KEY_RIGHTARROW"}, {"TAB", "KEY_TAB"}, {"RETURN", "KEY_ENTER"}, {"SPACE", "KEY_SPACE"}};
                        w.keyNames[n] = k.size() == 1 ? k : kDik.contains(k) ? kDik.at(k) : "KEY_" + k;
                    }
                w.joystick = pad != nullptr;  // config use-joystick: on with a connected gamepad (">DJ" adds "/Button<n>")
                w.loadUpdate();  // as 0x430f80: player + camera initialised before the first rendered frame
                play.emplace(std::move(w));
                litEnter = true, litFadingOut = false;
            } catch (const std::exception& e) {
                playStatus = std::format("viewer only: {}", e.what());
            }
        }
        if (game().id == GameId::TimeBusters && !scene.placeholder) {
            try {
                tbPlay.emplace(bb::game::TbWorld::load(bze, scene, tbCarry ? &*tbCarry : nullptr));
                tbPlay->world.setTwoPlayer(players == 2);
                // Debug (first TB level entered; smokes compared with the oracle's pokes): BBTIME_TB_START="x,y,z,yaw"
                // (PS1 units, 4.12) places Bugs (+0xfc/+0xf6); BBTIME_TB_GD="a0=0,a2=2,a5=1" writes GAME_DATA bytes
                // (hex offset = value: lives, down flags, health) before the first tick.
                static bool tbStartUsed = false;
                if (!tbStartUsed) {
                    tbStartUsed = true;
                    bb::game::TbWorld& w = tbPlay->world;
                    int x = 0, y = 0, z = 0, yaw = 0;
                    if (const char* s = std::getenv("BBTIME_TB_START"); s && std::sscanf(s, "%d,%d,%d,%d", &x, &y, &z, &yaw) >= 3)
                        w.actors[0].pos = {x, y, z}, w.actors[0].yaw = int16_t(yaw);
                    unsigned off = 0, val = 0;
                    int n = 0;
                    for (const char* s = std::getenv("BBTIME_TB_GD"); s && std::sscanf(s, "%x=%u%n", &off, &val, &n) == 2; s += n + (s[n] == ','))
                        if (off < w.GD.size()) w.GD[off] = uint8_t(val);
                    w.actors[0].hp = w.GD[0xa5], w.actors[1].hp = w.GD[0xa6];
                }
                // Rigid props the simulation draws itself (an entity with its own mesh model) leave the static scene,
                // so collected / removed ones disappear; mesh names "prop<n>@<chunk offset>/..." (buildTbScene).
                std::set<uint32_t> live;
                for (bb::game::TbActor* e : tbPlay->world.list())
                    if (e->chunk && e->model && !e->model->meshes.empty()) live.insert(e->chunk);
                std::erase_if(scene.meshes, [&](const bb::Mesh& m) {
                    unsigned off = 0;
                    return std::sscanf(m.name.c_str(), "prop%*u@%x", &off) == 1 && live.contains(off);
                });
            } catch (const std::exception& e) {
                playStatus = std::format("viewer only: {}", e.what());
            }
            tbCarry.reset();
            tbLevel = -1;
            for (int i = 0; !bb::game::TbWorld::levelName(i).empty(); ++i)
                if (iequals(bb::game::TbWorld::levelName(i), level)) { tbLevel = i; break; }
            tbFadingOut = false;
            if (tbPlay) {  // load 0x4018a0: GD+0 = the level, then the fade in from black at speed 200
                if (tbLevel >= 0) tbPlay->world.GD[0] = uint8_t(tbLevel);
                tbPlay->world.fadeIn(1, 200);
            }
        }
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
        if (play) cam = play->camera(1.0f, false);
        if (tbPlay) cam = tbPlay->camera(1.0f, false);
        prevCam = cam;
        screen = Screen::Level;
        SDL_SetWindowRelativeMouseMode(r.window(), !play && !tbPlay);
    }

    bool freeCamera() const { return play ? play->freeFly : tbPlay ? tbPlay->freeFly : true; }

    void leaveLevel() {
        SDL_SetWindowRelativeMouseMode(r.window(), false);
        screen = frontEnd ? Screen::Main : Screen::Game;
        frontEnd = false;
        if (litLoading) r.destroyImage(litLoading->image), litLoading.reset();
        litFadingOut = false;
    }

    // A game entry of the collection menu (Accept, or --menu): that game's original front end. Its own Quit (and
    // Esc on the LiT title page / Backspace) returns to Screen::Main; Right / X opens the debug level list instead.
    void openFrontEnd(int gi) {
        gameIdx = mainSel = gi;
        levelSel = 0;
        if (game().id == GameId::LostInTime) {
            // TITLE (level 0) in play mode with its own logic (GAMEPLAY.md §8): no slot yet (0x407190 sets G).
            litSave.reset();
            enterLevel("TITLE");
            frontEnd = true;
            overlay = false;  // the original screen, diagnostics on F3
        } else {  // GDI title menu 0x41c0f0 over menu.men (tb_front.cpp)
            if (!tbFront) tbFront.emplace();
            if (const std::string err = tbFront->open(game().root, bbtime::saveDir() / "tb", tbKeys, args.mute); !err.empty()) {
                std::println(stderr, "bbtime: TB front end: {}", err);
                screen = Screen::Game;
                return;
            }
            screen = Screen::TbFront;
            overlay = false;  // the original screen, diagnostics on F3
        }
    }

    // TB front-end result: 1 / 2 Players boot CCINTRO like the exe's state 5 after the menu (R: CCINTRO -> GRANNY),
    // a load starts the record's level with the file's GAME_DATA, Quit returns to the collection menu.
    void tbFrontAction(const bbtime::TbFrontAction& a) {
        if (a.kind == bbtime::TbFrontAction::None) return;
        if (a.kind == bbtime::TbFrontAction::Quit) { screen = Screen::Main; return; }
        players = a.players;
        if (a.kind == bbtime::TbFrontAction::Load) tbCarry = a.gd;
        else tbCarry.reset();
        std::string name(a.kind == bbtime::TbFrontAction::Load ? bb::game::TbWorld::levelName(a.level) : "CCINTRO");
        for (const std::string& l : game().levels)
            if (iequals(l, name)) name = l;
        enterLevel(name);
        frontEnd = true;
    }

    // Window point -> 640x480 canvas pixel of the 4:3 viewport the front end is drawn in (-1 outside).
    void tbMouse(float x, float y) {
        int ww = 1, wh = 1;
        SDL_GetWindowSize(r.window(), &ww, &wh);
        const auto v = bb::render::fitAspect(r.width(), r.height(), 4.0f / 3.0f);
        const float px = x * float(r.width()) / float(ww) - float(v.x), py = y * float(r.height()) / float(wh) - float(v.y);
        const bool in = px >= 0 && py >= 0 && px < float(v.w) && py < float(v.h);
        tbFront->mouse(in ? int(px * 640.0f / float(v.w)) : -1, in ? int(py * 480.0f / float(v.h)) : -1);
    }

    // Fade-out over: the loading screen of the target (LitWorld::loadingScreen; "<name>_<language G[0x10]>.bze", plain
    // LOADING.bze for id 1) with the PC texel curve in Original Fidelity, as the TIM upload does.
    void startLitLoading(int next) {
        LitLoading l{.next = next, .G = play->world.G, .startMs = SDL_GetTicks()};
        if (const int id = bb::game::LitWorld::loadingScreen(next, l.G[0x25])) {
            const std::string stem = id == 1 ? "LOADING" : std::format("{}_{}", bb::game::LitWorld::levelName(id), l.G[0x10]);
            bb::Texture t = bb::loadLitLoadingImage(game().root, stem);
            if (const float gamma = s.texGamma(scene.pcTexGamma); gamma != 1.0f)
                for (size_t i = 0; i < t.rgba.size(); ++i)
                    if (i % 4 != 3) t.rgba[i] = uint8_t(std::floor(256.0f * std::pow(float(t.rgba[i]) / 256.0f, gamma)));
            l.image = r.createImage(t);
        }
        litLoading = l;
    }

    // S[0x0a] request from level logic, as 0x407190 each tick. 6 = save (the "Do you want to save your game?" yes,
    // item S[18] == 1): 0x4074f0 writes Savegame<G[0x9f]>.dat + Savedata.dat, S[8] = 0x0a ok / 0x14 failed (failure
    // also S[0x1b] = 7). 4/5 (new game / load from the title menu) and the title's Savedata read: litFrontEnd.
    void litRequest(uint8_t& req) {
        if (req == 6) {
            bb::game::LitSaveGame g = litSave.value_or(bb::game::LitSaveGame::newGame(0));
            g.setLevel(litLevel);
            std::ranges::copy(play->world.G, g.G().begin());
            const std::string err = bb::game::writeLitSave(bbtime::saveDir() / "lit", g);
            play->world.S[8] = err.empty() ? 0x0a : 0x14;
            if (err.empty()) litSave = g;
            else play->world.S[0x1b] = 7;
            std::println("bbtime: save slot {} level {}: {}", g.slot(), litLevel, err.empty() ? "ok" : err);
        }
        req = 0;
    }

    void menu(Action a) {
        const int step = a == Action::Up || a == Action::Left ? -1 : 1;
        switch (screen) {
        case Screen::Main:
            if (a == Action::Up || a == Action::Down) mainSel = (mainSel + step + 4) % 4;
            else if (a == Action::Back) running = false;
            else if (a == Action::Right && mainSel < 2) { gameIdx = mainSel; levelSel = 0; screen = Screen::Game; }
            else if (a == Action::Accept) {
                if (mainSel < 2) openFrontEnd(mainSel);
                else if (mainSel == 2) screen = Screen::Options;
                else running = false;
            }
            break;
        case Screen::Options:
            if (a == Action::Up || a == Action::Down) optSel = (optSel + step + 8) % 8;
            else if (a == Action::Back || (a == Action::Accept && optSel == 7)) screen = Screen::Main;
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
        case Screen::TbFront:
            // Keyboard / pad Accept: nav(0, 0) focuses the first item when nothing is hovered (the mouse-only original
            // starts that way) and is a no-op otherwise.
            if (a == Action::Accept) tbFrontAction(tbFront->click()), tbFront->nav(0, 0);
            else if (a == Action::Back) tbFrontAction(tbFront->back());
            else tbFront->nav(a == Action::Left ? -1 : a == Action::Right ? 1 : 0, a == Action::Up ? -1 : a == Action::Down ? 1 : 0);
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
        case 6: players = 3 - players; break;  // applies when the next TB level starts
        }
    }

    void event(const SDL_Event& e) {
        switch (e.type) {
        case SDL_EVENT_QUIT: running = false; break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!pad) pad = SDL_OpenGamepad(e.gdevice.which);
            else if (!pad2) pad2 = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; }
            if (pad2 && SDL_GetGamepadID(pad2) == e.gdevice.which) { SDL_CloseGamepad(pad2); pad2 = nullptr; }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:  // the TB front end is mouse-driven (0x42e480: WM_LBUTTONDOWN -> 0x41bf90)
            if (screen == Screen::TbFront && e.button.button == SDL_BUTTON_LEFT) {
                tbMouse(e.button.x, e.button.y);
                tbFrontAction(tbFront->click());
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (screen == Screen::TbFront) tbMouse(e.motion.x, e.motion.y);  // WM_MOUSEMOVE -> 0x41bc40
            if (screen == Screen::Level && freeCamera()) {
                cam.yaw -= e.motion.xrel * 0.0025f;
                cam.pitch = std::clamp(cam.pitch - e.motion.yrel * 0.0025f, -1.55f, 1.55f);
                prevCam.yaw = cam.yaw;
                prevCam.pitch = cam.pitch;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (screen == Screen::Level && (play || tbPlay)) { // every other button is a game button (play.cpp)
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) menu(Action::Back);
                break;
            }
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: if (screen != Screen::Level) menu(Action::Up); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: if (screen != Screen::Level) menu(Action::Down); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (screen != Screen::Level) menu(Action::Left); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (screen != Screen::Level) menu(Action::Right); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: menu(Action::Accept); break;
            case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: menu(Action::Back); break;
            case SDL_GAMEPAD_BUTTON_NORTH: overlay = !overlay; break;
            case SDL_GAMEPAD_BUTTON_WEST: if (screen == Screen::Main) menu(Action::Right); break;  // level list
            default: break;
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            const SDL_Keycode k = e.key.key;
            if (screen == Screen::TbFront && tbFront->capturing()) { tbFront->captureKey(e.key.scancode); break; }
            if (k == SDLK_F3) overlay = !overlay;
            else if (k == SDLK_F11) { fullscreen = !fullscreen; r.setFullscreen(fullscreen); }
            // LiT title (level 0) past its logo page: Esc is the game's C96 (back to the slot menu).
            else if (k == SDLK_ESCAPE && screen == Screen::Level && play && litLevel == 0 && play->world.S[0x1b]) escKey = true;
            else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) menu(Action::Back);
            else if (screen == Screen::Level) {
                if (k == SDLK_F2) s.fidelity = s.fidelity == Fidelity::Enhanced ? Fidelity::Original : Fidelity::Enhanced;
                else if (k == SDLK_F4) s.fog = !s.fog;
                else if (k == SDLK_F1 && play) helpKey = true;  // the original's F1: help screen (LitWorld::tick)
                else if (k == SDLK_F5 && play) { // debug free-fly camera
                    play->freeFly = !play->freeFly;
                    if (play->freeFly) prevCam = cam = play->camera(alpha, s.fidelity == Fidelity::Enhanced);
                    SDL_SetWindowRelativeMouseMode(r.window(), play->freeFly);
                } else if (k == SDLK_F1 && tbPlay) {
                    tbPlay->freeFly = !tbPlay->freeFly;
                    if (tbPlay->freeFly) prevCam = cam = tbPlay->camera(alpha, s.fidelity == Fidelity::Enhanced);
                    SDL_SetWindowRelativeMouseMode(r.window(), tbPlay->freeFly);
                }
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

    // One simulation tick at kTickHz (30 Hz, GAMEPLAY §1.1): actor animation, LiT play mode, free-fly camera.
    void tick(float dt) {
        prevCam = cam;
        r.setTextureTick(++simTick);
        if (screen == Screen::TbFront && autoplayAt < args.autoplay.size()) {  // --autoplay smokes: button edges
            const bb::game::PadInput in = args.autoplay[autoplayAt++];
            const uint32_t held = in.buttons | (in.esc ? 0x10000u : 0u), down = held & ~tbAutoPrev;
            tbAutoPrev = held;
            using namespace bb::game;
            if (down & PadUp) menu(Action::Up);
            if (down & PadDown) menu(Action::Down);
            if (down & PadLeft) menu(Action::Left);
            if (down & PadRight) menu(Action::Right);
            if (down & (PadCross | PadStart)) menu(Action::Accept);
            if (down & 0x10000u) menu(Action::Back);
            return;
        }
        if (screen != Screen::Level) return;
        for (size_t i = 0; i < scene.actors.size(); ++i) {
            const bb::ActorInstance& a = scene.actors[i];
            if (a.chunk == 8 || (play && a.player) || tbPlay) continue;  // templates are not placed; play mode simulates
            bb::animTick(scene.actorModels[a.model], actorAnim[i], a.anim, actorAnim[i].parts.empty() ? bb::AnimLoop : 0);
        }
        if (play || litLoading) {
            auto input = [&] {
                bb::game::PadInput in;
                if (autoplayAt < args.autoplay.size()) in = args.autoplay[autoplayAt++];
                else if (!play->freeFly) in = bbtime::readPad(keys, pad);
                in.help = in.help || std::exchange(helpKey, false);
                in.esc = in.esc || std::exchange(escKey, false);
                return in;
            };
            if (litLoading) {  // the loading screen stays the presented frame while the target loads
                if (!litLoading->loaded) {
                    litLoading->loaded = true;
                    const auto globals = litLoading->G;  // GAME_DATA persists across levels
                    const std::string name = bb::game::LitWorld::levelName(litLoading->next);
                    std::printf("level change -> %d %s (app tick %u; the loading screen is the frame before)\n",
                                litLoading->next, name.c_str(), simTick);
                    try { enterLevel(name, &globals); } catch (const std::exception& e) { play.reset(); playStatus = e.what(); }
                    return;
                }
                if (bb::game::LitWorld::holdsLoading(litLoading->next) && SDL_GetTicks() - litLoading->startMs < 3000) return;
                r.destroyImage(litLoading->image);
                litLoading.reset();
                if (!play) return;
            }
            if (litEnter) {  // 0x4065e0 after the load: the second unrendered tick (counter 5; loadUpdate stood for the
                litEnter = false;  // load pass and counter 3), update 1 of the fade in, then tick + draw (counter 7)
                play->tick(input());
                play->world.fadeIn(1, 0x100);
                play->world.fadeTick();
            }
            play->tick(input());
            pumpSfx(play->world.sfx);
            // 0x407190: title requests 4/5 + the Savedata read (a game started: its GAME_DATA image), then save (6).
            if (auto g = bb::game::litFrontEnd(play->world, bbtime::saveDir() / "lit")) {
                litSave = *g;
                std::println("bbtime: title {} slot {}: next level {}", play->world.S[0x1b] == 4 ? "load failed," : "start",
                             g->slot(), play->world.G[0x0e]);
            }
            if (uint8_t& req = play->world.S[0x0a]; req) litRequest(req);
            if (play->world.quitRequest) {  // A128 "Quit game": the collection menu instead of the exe's quit dialog
                frontEnd = true;
                leaveLevel();
                return;
            }
            if (const int next = play->world.nextLevel; next >= 0) {  // level change request: fade out first
                if (!litFadingOut) litFadingOut = true, play->world.fadeOut(2, 0x100), play->world.fadeTick();
                else if (!play->world.fade.mode) litFadingOut = false, startLitLoading(next);  // never drawn bright
                return;
            }
            if (!play->freeFly) return;
        }
        if (tbPlay) {
            bb::game::PadInput p1, p2;
            if (autoplayAt < args.autoplay.size()) p1 = args.autoplay[autoplayAt++];
            else if (!tbPlay->freeFly) p1 = bbtime::readPad(tbKeys[0], pad);
            if (autoplay2At < args.autoplay2.size()) p2 = args.autoplay2[autoplay2At++];
            else if (!tbPlay->freeFly) p2 = bbtime::readPad(tbKeys[1], pad2);
            tbPlay->tick(p1, p2);
            pumpSfx(tbPlay->world.sfx);
            // Reload 0x4014c0 (level GD+0, GAME_DATA kept) after 0x401910's fade-out: fadeOut(2, 0x100) on the request
            // tick, the world keeps ticking, and the load follows the tick that reaches level 0 (16 frames; trace
            // local/wine/tbnpc/hud/fade_trace.csv). enterLevel then fades in at speed 200.
            if (bb::game::TbWorld& w = tbPlay->world; w.restart) {
                if (!tbFadingOut) {
                    tbFadingOut = true;
                    w.fadeOut(2, 0x100);
                    w.fadeTick();
                }
                if (w.fade.level > 0) return;
                const int next = w.nextLevel >= 0 ? w.nextLevel : tbLevel;
                const std::string name(bb::game::TbWorld::levelName(next));
                std::printf("level change -> %d %s\n", next, name.c_str());
                tbCarry = w.GD;
                try { enterLevel(name); } catch (const std::exception& e) { tbPlay.reset(); playStatus = e.what(); }
                return;
            }
            if (!tbPlay->freeFly) return;
        }
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
        const int sc = uiScale(), lh = (R::kGlyph + 4) * sc;
        size_t widest = 0;
        for (const auto& it : items) widest = std::max(widest, it.size());
        const float w = float((widest + 4) * size_t(R::kGlyph * sc));
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
        // LiT TITLE: its 3-D menu panel sits under 4:3 screen text, so it keeps the original framing in both modes.
        s.frame43 = screen == Screen::Level && play && litLevel == 0;
        r.beginFrame(s);
        const int sc = uiScale(), g = R::kGlyph;
        const float W = float(r.width()), H = float(r.height());
        const std::string_view hint = screen == Screen::Main
            ? "Up/Down: move  Enter/A: play (original menu)  Right/X: level list  Esc/B: back  F3: diagnostics"
            : "Up/Down: move  Enter/A: select  Esc/B: back  F3: diagnostics  F11: fullscreen";
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
                  std::format("Vertical FOV:   {:.0f} deg (Hor+, Enhanced; Original 51.28)", s.vfovDeg),
                  std::format("4:3 pillarbox:  {} (Original mode)", onoff(s.pillarbox)),
                  std::format("MSAA:           {} (x{}, Enhanced mode)", onoff(s.msaa), r.msaaSamples()),
                  std::format("VSync:          {}", onoff(vsync)),
                  std::format("Fullscreen:     {}", onoff(fullscreen)),
                  std::format("TB players:     {} ({})", players, players == 2 ? "co-op: P2 keypad / 2nd gamepad" : "R1 switch, L1 call friend"),
                  "Back"},
                 optSel, 0, 8);
            break;
        }
        case Screen::TbFront: {  // the 640x480 GDI page stretched over the 4:3 client, like the other 2-D screens
            if (const bb::Texture* t = tbFront->frame()) {
                r.destroyImage(tbFrontImage);
                tbFrontImage = r.createImage(*t);
            }
            const auto v = bb::render::fitAspect(r.width(), r.height(), 4.0f / 3.0f);
            r.drawImage(tbFrontImage, float(v.x), float(v.y), float(v.w), float(v.h), s.fidelity == Fidelity::Enhanced);
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
            if (litLoading || litEnter) {  // loading screen (0x423bd0: the TIM stretched over the 4:3 client) / not drawn
                if (litLoading && litLoading->image) {
                    const auto v = bb::render::fitAspect(r.width(), r.height(), 4.0f / 3.0f);
                    r.drawImage(litLoading->image, float(v.x), float(v.y), float(v.w), float(v.h), s.fidelity == Fidelity::Enhanced);
                }
                break;
            }
            // Original Fidelity: no interpolation (the exe renders the tick's state); Enhanced: lerp between ticks.
            const bool lerp = s.fidelity == Fidelity::Enhanced;
            bb::render::Camera c = cam;
            if (!freeCamera()) {
                c = play ? play->camera(alpha, lerp) : tbPlay->camera(alpha, lerp);
            } else {
                c.pos = glm::mix(prevCam.pos, cam.pos, alpha);
                c.yaw = glm::mix(prevCam.yaw, cam.yaw, alpha);
                c.pitch = glm::mix(prevCam.pitch, cam.pitch, alpha);
            }
            poseActors(lerp, c);
            r.drawScene(c, s);
            if (play || tbPlay) {
                drawOverlayText(play ? play->world.glyphs : tbPlay->world.glyphs);  // game text: drawn with --no-hud too
                if (tbPlay) drawTbSprites();  // the HUD sheet sprites (game HUD)
                if (const auto f = play ? play->world.fadeOverlay() : tbPlay->world.fadeOverlay(); f[3])
                    r.drawRect(0, 0, W, H, f.data());  // fade (LiT also the A71 flash)
            }
            if (play && !args.noHud) drawHud();
            if (tbPlay && !args.noHud) drawTbHud();
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
        if (screen != Screen::Level && screen != Screen::TbFront)
            r.drawText(float(8 * sc), H - float(g * sc) - float(36 * sc), sc, hint, kGrey);
        if (overlay) drawOverlay();
    }

    // Live actors (not spawn templates); in play mode Bugs at the simulated position and collected pickups hidden.
    void poseActors(bool lerp, const bb::render::Camera& c) {
        const glm::vec3 eye = c.pos;
        actorMeshes.clear();
        for (size_t i = 0; i < scene.actors.size(); ++i) {
            const bb::ActorInstance& a = scene.actors[i];
            if (a.chunk == 8) continue;
            if (tbPlay && a.player) {
                const size_t k = a.entityId == 2 ? 0 : 1;
                const auto v = tbPlay->view(alpha, lerp);
                bb::poseActor(scene.actorModels[a.model], tbPlay->world.actors[k].anim, {v.pos[k].x, v.pos[k].y, v.pos[k].z},
                              {0, v.yaw[k], v.roll[k]}, a.scale, actorMeshes);
                continue;
            }
            if (play && a.player) {
                const auto v = play->view(alpha, lerp);
                bb::poseActor(scene.actorModels[a.model], play->world.p.animState, {v.player.x, v.player.y, v.player.z},
                              {0, v.yaw, v.lean}, a.scale, actorMeshes);
                continue;
            }
            if (play || tbPlay) continue;  // the simulation's actors are drawn below
            bb::poseActor(scene.actorModels[a.model], actorAnim[i], a.pos, a.rot, a.scale, actorMeshes);
        }
        if (tbPlay)  // listed entities (view list 0x45eee0; the shadow pass 0x464480 draws from it too, I), not removed
            for (const bb::game::TbActor& e : tbPlay->world.entities)
                if (e.active && e.drawn() && e.model && !e.anim.parts.empty())
                    bb::poseActor(*e.model, e.anim, {e.pos.x, e.pos.y, e.pos.z}, {e.pitch, e.yaw, e.roll},
                                  {e.scale[0], e.scale[1], e.scale[2]}, actorMeshes);
        if (play)
            for (const bb::game::LitActor& a : play->world.actors) {
                const int32_t m = play->world.defs[size_t(a.def)].model;
                if (a.dead || !a.visible || m < 0 || a.animState.parts.empty()) continue;
                bb::poseActor(scene.actorModels[size_t(m)], a.animState, {a.pos.x, a.pos.y, a.pos.z},
                              {a.rot[0], a.yaw, a.rot[2]}, a.scale, actorMeshes);
            }
        if (play) {  // ground shadows (0x44bb50): the shadow entity's model on one identity part
            // The PC exe orders polygons by ordering-table bucket (the shadow +9 deeper in view space), not per pixel;
            // with a depth buffer the quad would sink into the visual floor (which differs from the collision heights
            // it is placed on), so its vertices are pulled kShadowPull units toward the eye (Inferred value; ponytail:
            // a per-mesh polygon offset in both backends if thin geometry ever shows it through).
            constexpr float kShadowPull = 24.0f;
            bb::AnimState rigid;
            rigid.parts.push_back({.flags = 4, .object = 0, .parent = -1});
            for (const bb::game::LitWorld::Shadow& s : play->world.shadows()) {
                const size_t first = actorMeshes.size();
                bb::poseActor(scene.actorModels[scene.templateModels.at(s.entity)], rigid, {s.pos.x, s.pos.y, s.pos.z}, s.rot,
                              {s.scale, 0x1000, s.scale}, actorMeshes);
                for (size_t k = first; k < actorMeshes.size(); ++k)
                    for (bb::Vertex& v : actorMeshes[k].vertices) {
                        const glm::vec3 p{v.pos[0], v.pos[1], v.pos[2]}, q = p + kShadowPull * glm::normalize(eye - p);
                        v.pos[0] = q.x, v.pos[1] = q.y, v.pos[2] = q.z;
                    }
            }
        }
        if (play) poseHud(c, play->world.hud);
        if (tbPlay) poseHud(c, tbPlay->world.hud);
        r.setActorMeshes(actorMeshes);
    }

    // HUD (LiT 0x44b370 over LitWorld::hud, TB 0x463ef0 over TbWorld::hud): each shown element's template model on one
    // identity part under its rotation / scale, at screen offset (x, y) of the 512x240 space and view depth z through
    // the exe's projection (0x408910, focal 400 in 384-line units). Each vertex is rebuilt on this frame's camera ray
    // at depth·k (k = 2·zNear/1000): the same pixels at any FOV, in front of the world (the exe forces the HUD's depth
    // to the near plane).
    void poseHud(const bb::render::Camera& c, std::span<const bb::game::HudItem> items) {
        const glm::mat4 toWorld = glm::inverse(c.view());
        const float f = 192.0f / std::tan(s.vfovRad() * 0.5f), k = s.zNear * 2.0f / 1000.0f;
        bb::AnimState rigid;
        rigid.parts.push_back({.flags = 4, .object = 0, .parent = -1});
        for (const bb::game::HudItem& h : items) {
            const auto m = scene.templateModels.find(h.tmpl);
            if (!h.shown || m == scene.templateModels.end()) continue;
            const size_t first = actorMeshes.size();
            bb::poseActor(scene.actorModels[m->second], rigid, {0, 0, 0}, h.rot, {h.scaleX, 0x1000, 0x1000}, actorMeshes);
            for (size_t i = first; i < actorMeshes.size(); ++i)
                for (bb::Vertex& v : actorMeshes[i].vertices) {
                    const float d = std::max(float(h.z) - v.pos[2], 1.0f);  // PS1 z = -engine z
                    const float sx = 400.0f * v.pos[0] / d + float(h.x), sy = -400.0f * v.pos[1] / d + float(h.y) * 1.6f;
                    const float dd = d * k;
                    const glm::vec4 w = toWorld * glm::vec4(sx * dd / f, -sy * dd / f, -dd, 1.0f);
                    v.pos[0] = w.x, v.pos[1] = w.y, v.pos[2] = w.z;
                }
        }
    }

    // Debug notes (the game HUD is LitWorld::hud, posed by poseHud).
    void drawHud() {
        const int sc = uiScale();
        const float x = float(8 * sc), y = float(r.height()) - float(14 * sc);
        if (!play->world.playerModel()) // no decoded actor model for Bugs: say so instead of drawing nothing silently
            r.drawText(x, y, sc, "PLACEHOLDER: Bugs model not decoded", kRed);
        if (play->freeFly) r.drawText(x, y - float(12 * sc), sc, "F5 FREE-FLY (debug camera)", kRed);
    }

    // Overlay text (LitWorld::glyphs, sprite list drawn by 0x44cfa0 / GL 0x40b130): each glyph's font TIM
    // (Scene::overlayFont) at its pen position, w x h in the exe's 512x240 2-D space (origin at the centre) on the
    // centred 4:3 view, texels times the palette's channel mask (0 white, 1 yellow, 2 green, 3 blue, 4 red, 5+
    // untinted). Original Fidelity applies the PC texel curve (Scene::pcTexGamma) like the 3-D textures.
    void drawOverlayText(const std::vector<bb::game::OverlayGlyph>& glyphs) {
        static constexpr uint8_t kPal[5][4] = {
            {255, 255, 255, 255}, {255, 255, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 0, 0, 255}};
        if (fontImages.size() != scene.overlayFont.size() || fontFidelity != s.fidelity) {
            freeFont();
            fontFidelity = s.fidelity;
            const float gamma = s.texGamma(scene.pcTexGamma);
            for (const int32_t t : scene.overlayFont) {
                if (t < 0) { fontImages.push_back(0); continue; }
                bb::Texture tx = scene.textures[size_t(t)];
                if (gamma != 1.0f)
                    for (size_t i = 0; i < tx.rgba.size(); ++i)
                        if (i % 4 != 3) tx.rgba[i] = uint8_t(std::floor(256.0f * std::pow(float(tx.rgba[i]) / 256.0f, gamma)));
                fontImages.push_back(r.createImage(tx));
            }
        }
        // 0x408100: position scale 0x4efb7c = view width / 512 (x), 0x4efb80 = height / 240 (y); sprite size scale
        // 0x4efb84 steps with the width scale: >= 2 -> 2, >= 1.5 -> 1.5, >= 1 -> 1, else 0.85 (640x480: 16x16 px).
        const float W = float(r.width()), H = float(r.height()), sx = H / 384.0f, sy = H / 240.0f;
        // Enhanced: glyphs follow the position scale so text stays legible and evenly spaced above ~768 px height.
        const float ss = s.fidelity == Fidelity::Enhanced ? sx : sx >= 2.0f ? 2.0f : sx >= 1.5f ? 1.5f : sx >= 1.0f ? 1.0f : 0.85f;
        for (const bb::game::OverlayGlyph& g : glyphs) {
            if (!g.glyph || g.glyph > fontImages.size() || !fontImages[g.glyph - 1u]) continue;
            const bb::Texture& tx = scene.textures[size_t(scene.overlayFont[g.glyph - 1u])];
            r.drawImage(fontImages[g.glyph - 1u], W * 0.5f + std::trunc(float(g.x) * sx), H * 0.5f + std::trunc(float(g.y) * sy),
                        float(tx.width) * ss, float(tx.height) * ss, s.fidelity == Fidelity::Enhanced,
                        g.pal < 5 ? kPal[g.pal] : kWhite);
        }
    }
    void freeFont() {
        for (const uint32_t i : fontImages) r.destroyImage(i);
        fontImages.clear();
        for (const auto& [key, i] : hudImages) r.destroyImage(i);
        hudImages.clear();
    }

    // TB HUD sprites (TbWorld::hudSprites, POLY_FT4s of the Scene::hudSheet frames): top-left (x, y) in the 512x240
    // space placed like the overlay text, w x h from texel (u, v) (the health bar crop) stretched by the space's scale
    // (H/384 x H/240 on the centred 4:3 view). Crops are cached per {frame, u, v, w, h}; Original Fidelity applies the
    // PC texel curve. Call after drawOverlayText (it frees the caches on a fidelity change).
    void drawTbSprites() {
        const float W = float(r.width()), H = float(r.height()), sx = H / 384.0f, sy = H / 240.0f;
        const float gamma = s.texGamma(scene.pcTexGamma);
        for (const auto& sp : tbPlay->world.hudSprites) {
            if (!sp.shown || sp.frame >= scene.hudSheet.size() || scene.hudSheet[sp.frame] < 0) continue;
            const int cw = sp.w + 1, ch = sp.h + 1;  // the prim spans texels u..u+w
            auto [it, fresh] = hudImages.try_emplace({sp.frame, sp.u, sp.v, sp.w, sp.h}, 0u);
            if (fresh) {
                const bb::Texture& src = scene.textures[size_t(scene.hudSheet[sp.frame])];
                bb::Texture t;
                t.name = src.name;
                t.width = uint32_t(std::max(0, std::min<int>(cw, int(src.width) - sp.u)));
                t.height = uint32_t(std::max(0, std::min<int>(ch, int(src.height) - sp.v)));
                t.rgba.resize(size_t(t.width) * t.height * 4);
                for (uint32_t y = 0; y < t.height; ++y)
                    std::copy_n(&src.rgba[((size_t(sp.v) + y) * src.width + size_t(sp.u)) * 4], size_t(t.width) * 4, &t.rgba[size_t(y) * t.width * 4]);
                if (gamma != 1.0f)
                    for (size_t i = 0; i < t.rgba.size(); ++i)
                        if (i % 4 != 3) t.rgba[i] = uint8_t(std::floor(256.0f * std::pow(float(t.rgba[i]) / 256.0f, gamma)));
                it->second = t.width && t.height ? r.createImage(t) : 0;
            }
            if (it->second)
                r.drawImage(it->second, W * 0.5f + std::trunc(float(sp.x) * sx), H * 0.5f + std::trunc(float(sp.y) * sy),
                            float(cw) * sx, float(ch) * sy, s.fidelity == Fidelity::Enhanced);
        }
    }

    // TB debug notes (the game HUD is TbWorld::hud / hudSprites).
    void drawTbHud() {
        const int sc = uiScale();
        const float x = float(8 * sc), y = float(r.height()) - float(14 * sc);
        const bb::game::TbWorld& w = tbPlay->world;
        if (!w.actors[0].model || !w.actors[1].model) r.drawText(x, y, sc, "PLACEHOLDER: actor model missing", kRed);
        if (tbPlay->freeFly) r.drawText(x, y - float(12 * sc), sc, "F1 FREE-FLY (debug camera)", kRed);
    }

    void drawOverlay() {
        const int sc = std::max(1, (r.height() + 300) / 600);
        const bool enh = s.fidelity == Fidelity::Enhanced;
        std::string text = std::format(
            "{} | {}\n{}\n{:.1f} FPS  {:.2f} ms  {} draws  {}x{}\n"
            "mode {}  {}  MSAA {}  FOV {:.0f} {}  vsync {}\ntick {:.0f} Hz (GAMEPLAY 1.1)  alpha {:.2f}",
            r.glVendor(), r.glRenderer(), r.glVersion(), 1.0 / frameTime, frameTime * 1000.0, r.drawCalls(), r.width(),
            r.height(), enh ? "Enhanced" : "Original", enh ? std::format("trilinear aniso x{:.0f}", r.maxAnisotropy())
                                                          : scene.game == bb::GameId::LostInTime ? "linear" : "nearest",
            enh && s.msaa && r.msaaSamples() ? std::format("x{}", r.msaaSamples()) : "off", glm::degrees(s.vfovRad()),
            (!enh && s.pillarbox) || s.frame43 ? "4:3 pillarbox" : "Hor+", vsync ? "on" : "off", bb::kTickHz, alpha);
        if constexpr (requires { r.diagnostics(); }) text += std::format("\n{}", r.diagnostics());
        if (screen == Screen::Level)
            text += std::format("\n{} {}  meshes {}  tris {}  textures {}  fog {}\npos {:.1f} {:.1f} {:.1f}  {}",
                                game().shortName, scene.level, scene.meshes.size(), sceneTris, scene.textures.size(),
                                s.fog ? "on" : "off", cam.pos.x, cam.pos.y, cam.pos.z,
                                scene.placeholder ? "PLACEHOLDER SCENE" : "decoded data");
        if (screen == Screen::Level && play) {
            const bb::game::LitWorld& w = play->world;
            text += std::format("\nPLAY  tick {}  state {} seq {}  anim {}  flags14 {:08x}\nbugs {} {} {}  yaw {}  ground {}  zone {}"
                                "\ncam eye {} {} {}  yaw {}  F1 help  F5 free-fly  F2 fidelity  F4 fog",
                                w.ticks, w.p.state, w.p.seq, w.p.anim, w.p.flags14, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.p.yaw,
                                w.groundY(), w.p.zone, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z, w.cam.yaw);
            if (!w.unsupported.empty()) {
                text += "\nnot interpreted:";
                for (const std::string& u : w.unsupported) text += " " + u;
            }
        }
        if (screen == Screen::Level && tbPlay) {
            const bb::game::TbWorld& w = tbPlay->world;
            text += std::format("\nTB PLAY  tick {}  {}  A4 {:02x}  focus {}  cam {}  cooldown {}  music n={}", w.ticks,
                                w.twoPlayer() ? "2P" : "1P", w.A4, w.focus(), w.camFocus, w.cooldown, w.music);
            for (const bb::game::TbActor& a : w.actors)
                text += std::format("\n{} state {} seq {} anim {}  pos {} {} {}  yaw {}  f20 {:08x}  vel {} {} {}",
                                    a.id == 2 ? "bugs" : "taz ", a.rec() ? a.rec()->id : 0, a.seq, a.curAnim, a.pos.x, a.pos.y,
                                    a.pos.z, a.yaw, a.f20, a.vel[0] >> 4, a.vel[1] >> 4, a.vel[2] >> 4);
            text += std::format("\ncam eye {} {} {}  look {} {} {}  D {}  F1 free-fly  F2 fidelity  F4 fog", w.cam.eye.x,
                                w.cam.eye.y, w.cam.eye.z, w.cam.look.x, w.cam.look.y, w.cam.look.z, w.cam.dist);
            if (!w.unsupported.empty()) {
                text += "\nnot interpreted:";
                for (const std::string& u : w.unsupported) text += " " + u;
            }
        }
        if (screen == Screen::Level && !playStatus.empty()) text += "\n" + playStatus;
        if (screen == Screen::Level)
            for (const std::string& n : scene.notes) text += "\n" + n;
        int lines = 1, cols = 0, cur = 0;
        for (char ch : text) {
            if (ch == '\n') { ++lines; cur = 0; } else cols = std::max(cols, ++cur);
        }
        const int g = R::kGlyph;
        r.drawRect(0, 0, float((cols + 2) * g * sc), float(lines * (g + 2) * sc + 8 * sc), kPanel);
        r.drawText(float(g * sc), float(4 * sc), sc, text, kWhite);
    }
};


template <class R>
int run(const std::optional<Args>& args) {
    {
        App<R> app;
        app.args = *args;
        app.s.fidelity = args->fidelity;
        app.overlay = !args->noHud;
        app.fullscreen = args->fullscreen;
        bb::render::WindowConfig wc;
        wc.width = args->width;
        wc.height = args->height;
        wc.fullscreen = args->fullscreen;
        wc.hidden = args->hidden;
        wc.msaaSamples = args->msaa;
        app.s.vfovDeg = args->fov;
        app.s.msaa = args->msaa > 0;
        app.vsync = args->vsync;
        app.players = args->players;
        {
            std::vector<std::string> warn;
            app.keys = bbtime::makeKeyMap(args->bindings, &warn);
            for (const auto& w : warn) std::println(stderr, "bbtime: {}", w);
        }
        std::string err;
        if constexpr (requires { app.r.init(wc, true); }) err = app.r.init(wc, args->vkValidation);
        else err = app.r.init(wc);
        if (!err.empty()) {
            std::println(stderr, "bbtime: {}", err);
            return 1;
        }
        app.r.setVsync(app.vsync);
        std::println("bbtime: {} | {} | {} | data {}", app.r.glVendor(), app.r.glRenderer(), app.r.glVersion(),
                     args->data.string());
        for (int gi = 0; gi < 2; ++gi) {
            Game& g = app.games[gi];
            loadGame(g, args->roots[gi], app.r);
            std::println("bbtime: {}: {}{}", g.shortName, g.status,
                         g.backdrop ? " | backdrop " + g.backdropPath.string() : std::string(" | no backdrop"));
        }
        if (!args->importSaves.empty()) {  // original Datas/bin, or an install root containing it
            fs::path from = findPath(args->importSaves, {"Datas", "bin"});
            if (from.empty()) from = args->importSaves;
            std::println("bbtime: import LiT saves from {}: {}", from.string(),
                         bb::game::importLitSaves(from, bbtime::saveDir() / "lit"));
        }
        std::string level = args->level;
        if (args->slot > 0) {  // as the title menu: load the slot (0x407190 case 5) or start a new game in it (case 4)
            const fs::path file = bb::game::litSaveGamePath(bbtime::saveDir() / "lit", args->slot);
            std::error_code ec;
            try {
                app.litSave = fs::exists(file, ec) ? bb::game::parseLitSaveGame(bb::readFile(file), args->slot)
                                                   : bb::game::LitSaveGame::newGame(args->slot);
            } catch (const std::exception& e) {
                std::println(stderr, "bbtime: {} (a .bak copy may hold the previous save)", e.what());
                return 1;
            }
            app.litSave->G()[0x36] = 0;  // load clears GD+0x76 (camera/cutscene lock)
            const auto G = app.litSave->G();
            std::println("bbtime: slot {} {}: level {} health {}/{} carrots {} golden {} clocks {}", args->slot,
                         fs::exists(file, ec) ? file.string() : "new game", app.litSave->startLevel(), G[1], G[2],
                         G[3], G[7] | G[0xfc] << 8, G[5]);
            if (level.empty()) level = bb::game::LitWorld::levelName(app.litSave->startLevel());
        }
        if (args->game) {
            app.gameIdx = *args->game == GameId::LostInTime ? 0 : 1;
            app.mainSel = app.gameIdx;
            app.screen = Screen::Game;
            if (args->menu) app.openFrontEnd(app.gameIdx);
            if (!level.empty()) {
                auto& lv = app.game().levels;
                if (auto it = std::ranges::find_if(lv, [&](const std::string& l) { return iequals(l, level); }); it != lv.end()) {
                    app.levelSel = int(it - lv.begin());
                    app.enterLevel(*it);
                } else {
                    std::println(stderr, "bbtime: level '{}' not found on disk; showing placeholder anyway", level);
                    app.enterLevel(level);
                }
            }
            if (args->camera) {
                const auto& c = *args->camera;
                app.cam.pos = {c[0], c[1], c[2]};
                app.cam.yaw = c[3];
                app.cam.pitch = c[4];
                app.prevCam = app.cam;
                if (app.play) app.play->freeFly = true; // explicit camera = debug free-fly view of play mode
                if (app.tbPlay) app.tbPlay->freeFly = true;
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
            if ((app.autoplayAt < args->autoplay.size() || app.autoplay2At < args->autoplay2.size()) &&
                (app.screen == Screen::Level || app.screen == Screen::TbFront)) { // lockstep: 1 tick per frame
                app.tick(float(step.dt));
                app.alpha = 1.0f;
            } else {
                for (int n = step.advance(dt); n > 0; --n) app.tick(float(step.dt));
                app.alpha = step.alpha();
            }
            if (app.screen == Screen::Main && app.mainSel < 2) musicIdx = app.mainSel; // highlighted game
            else if (app.screen == Screen::Game) musicIdx = app.gameIdx;
            fs::path track;  // menu: highlighted game's track; TB level: logic action 126 (GAMEPLAY_TB.md §6)
            if (!args->mute) {
                // The TB front end never calls PlayMusic (GAMEPLAY_TB §6.3): silent in Original Fidelity; Enhanced keeps
                // the collection menu's TB track.
                if (app.screen == Screen::TbFront) {
                    if (app.s.fidelity == Fidelity::Enhanced) track = bbtime::menuTrack(GameId::TimeBusters, args->roots[1]);
                } else if (app.screen != Screen::Level) track = bbtime::menuTrack(app.games[musicIdx].id, args->roots[musicIdx]);
                else if (app.tbPlay) track = bbtime::tbLevelTrack(args->roots[1], app.tbPlay->world.music);
                else if (app.play && app.litLevel == 0) track = bbtime::menuTrack(GameId::LostInTime, args->roots[0]);  // TITLE: TRACK25
            }
            bbtime::playMusic(track);
            app.draw();
            if (!args->screenshot.empty() && ++frame >= args->frames) {
                const bool ok = app.r.screenshot(args->screenshot);
                std::println("bbtime: screenshot {} {}", args->screenshot.string(), ok ? "written" : "FAILED");
                app.running = false;
            }
            app.r.present();
        }
        if (app.pad) SDL_CloseGamepad(app.pad);
        if (app.pad2) SDL_CloseGamepad(app.pad2);
        for (Game& g : app.games) app.r.destroyImage(g.backdrop);
        app.r.destroyImage(app.tbFrontImage);
        app.freeFont();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    auto args = parseArgs(argc, argv);
    if (!args) return 2;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::println(stderr, "SDL_Init: {}", SDL_GetError());
        return 1;
    }
    const int rc = args->vulkan ? run<bb::render_vk::Renderer>(args) : run<bb::render::Renderer>(args);
    SDL_Quit();
    return rc;
}
