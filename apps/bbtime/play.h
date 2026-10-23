// LiT play mode glue: SDL keyboard/gamepad -> PS1 pad report (GAMEPLAY.md §2.1), --autoplay scripts, and the
// render view of the bb_game simulation (Original Fidelity: no interpolation, like the exe; Enhanced: positions
// and camera interpolated between the last two ticks, simulation unchanged).
#pragma once
#include "bb/game/lit_world.h"
#include "bb/game/tb_world.h"
#include "bb/render/camera.h"

#include <SDL3/SDL.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bbtime {

// Scancode -> pad bit. Built from settings bindings (defaults = LiT config.pc map: arrows, Q/W camera L2/R2,
// E roll L1, R action R1, A look △, S crouch ○, D jump ✕, F kick □, Tab select, Return start) plus fixed
// aliases Space = jump ✕ and Left Ctrl = kick □.
struct KeyMap {
    std::vector<std::pair<SDL_Scancode, uint16_t>> keys;
};
KeyMap makeKeyMap(const std::map<std::string, std::string>& bindings, std::vector<std::string>* warnings);
bb::game::PadInput readPad(const KeyMap& map, SDL_Gamepad* pad);
// Time Busters keyboard tables: bblit2.exe config.pc defaults 0x415fd0 (GAMEPLAY.md §2.3). P1: arrows, A L1 (call
// friend), D/F L2/R2, S R1 (switch), W △, Q ○, E ✕, R □, Tab select (Esc stays the app's back key) + Space ✕ /
// LCtrl □ aliases; P2: keypad 8/5/6/4, L L1, ; ' L2/R2, K R1, O △, I ○, P ✕, [ □, RCtrl select, RShift start.
KeyMap tbKeyMap(int player);

// --autoplay "KEYS*TICKS,...": KEYS from U D L R (d-pad), X ✕, S □, C ○, T △, 1 L1, 2 L2, 3 R1, 4 R2, P Start
// (Enter), H LiT F1 key (help; give it one tick), E LiT Esc key, - none.
// e.g. "-*10,U*60,X*8". nullopt on a syntax error.
std::optional<std::vector<bb::game::PadInput>> parseAutoplay(std::string_view script);

glm::vec3 toEngine(const bb::game::V3& v);  // PS1 (x, y down, z) -> engine (x, -y, -z), scene.h convention

struct Play {
    struct Snap { bb::game::V3 eye, target, player; int16_t yaw = 0, lean = 0; std::array<int16_t, 3> parent{}; };
    bb::game::LitWorld world;
    Snap prev, cur;
    bool freeFly = false;  // F1: debug free-fly camera (simulation keeps running)

    explicit Play(bb::game::LitWorld w) : world(std::move(w)) { prev = cur = snap(); }
    void tick(const bb::game::PadInput& in) { prev = cur; world.tick(in); cur = snap(); }
    // a = render alpha in [0,1); interpolate = Enhanced mode.
    bb::render::Camera camera(float a, bool interpolate) const;
    Snap view(float a, bool interpolate) const;

private:
    Snap snap() const {
        const auto [at, yaw] = world.playerDraw();
        return {world.cam.eye, world.cam.target, at, yaw, world.p.lean, world.playerParentRot()};
    }
};

// TB play mode: view of the bb_game TbWorld (both characters, camera), interpolated like Play.
struct TbPlay {
    struct Snap { bb::game::V3 eye, look; std::array<bb::game::V3, 2> pos; std::array<int16_t, 2> yaw{}, roll{}; };
    bb::game::TbWorld world;
    Snap prev, cur;
    bool freeFly = false;

    explicit TbPlay(bb::game::TbWorld w) : world(std::move(w)) { prev = cur = snap(); }
    void tick(const bb::game::PadInput& p1, const bb::game::PadInput& p2) { prev = cur; world.tick(p1, p2); cur = snap(); }
    Snap view(float a, bool interpolate) const;
    bb::render::Camera camera(float a, bool interpolate) const;

private:
    Snap snap() const {
        const auto& b = world.actors[0];
        const auto& t = world.actors[1];
        return {world.cam.eye, world.cam.look, {b.pos, t.pos}, {b.yaw, t.yaw}, {b.roll, t.roll}};
    }
};

} // namespace bbtime
