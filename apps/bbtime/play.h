// LiT play mode glue: SDL keyboard/gamepad -> PS1 pad report (GAMEPLAY.md §2.1), --autoplay scripts, and the
// render view of the bb_game simulation (Original Fidelity: no interpolation, like the exe; Enhanced: positions
// and camera interpolated between the last two ticks, simulation unchanged).
#pragma once
#include "bb/game/lit_world.h"
#include "bb/game/tb_world.h"
#include "bb/render/camera.h"

#include <SDL3/SDL.h>

#include <filesystem>
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
// joy (LiT, >J order kick crouch look jump roll camera-right action camera-left) = the gamepad button index (kJoyButtons
// in play.cpp, the DirectInput "BUTTON<n>" of config.pc joyMap +0x3c) driving that action; default = bbtime's layout.
struct KeyMap {
    std::vector<std::pair<SDL_Scancode, uint16_t>> keys;
    std::array<uint8_t, 8> joy{3, 1, 0, 2, 4, 6, 5, 7};
};
// DirectInput scan code (bugs.exe DIK table 0x45e030) <-> SDL scancode; 0 / SDL_SCANCODE_UNKNOWN when not listed.
uint8_t dikOf(SDL_Scancode sc);
SDL_Scancode scancodeOfDik(uint8_t dik);
std::array<bool, 256> dikHeld();  // DirectInput key states (TB cheat word 0x40fbc0)
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

// Enhanced render interpolation of a non-player actor / entity (BBLIT-Remaster's idea; simulation untouched): its state
// one tick earlier, drawn toward the current one (bb::interpolatePose for the pose). Roots that jump more than 400
// (x + z) or 1000 (y) in a tick (teleports, respawns) snap.
struct ActorPrev {
    bb::AnimState anim;
    bb::game::V3 pos;
    std::array<int16_t, 3> rot{};
};
// A root between the last two ticks; a move over `jump` (x + z, y over max(1000, jump)) is a teleport: shown at once.
// TB 400 (call friend, respawn); LiT kLitJump: the LS01 fly-in carries Bugs ~620 a tick, a level-start teleport 7000.
inline constexpr int32_t kLitJump = 1500;
bb::game::V3 lerpRoot(const bb::game::V3& prev, const bb::game::V3& cur, float a, int32_t jump = 400);
int16_t lerpAngle(int16_t prev, int16_t cur, float a);

struct Play {
    struct Snap { bb::game::V3 eye, target, player; int16_t yaw = 0, lean = 0; std::array<int16_t, 3> parent{}; };
    bb::game::LitWorld world;
    Snap prev, cur;
    bool freeFly = false;  // F1: debug free-fly camera (simulation keeps running)
    std::vector<ActorPrev> prevActors;  // per LitWorld::actors index, as of the tick before the last
    bb::AnimState prevPlayerAnim;
    std::vector<bb::game::LitWorld::Shadow> prevShadows;  // the last tick's shadows() (Enhanced: interpolated per owner)

    explicit Play(bb::game::LitWorld w) : world(std::move(w)) { prev = cur = snap(); }
    void tick(const bb::game::PadInput& in);
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
    std::map<const bb::game::TbActor*, ActorPrev> prevEntities;  // entities live in a std::list: stable addresses
    std::array<bb::AnimState, 2> prevAnim;  // Bugs, Taz
    std::vector<bb::game::TbWorld::Shadow> prevShadows;  // the last tick's shadows (Enhanced: interpolated per owner)

    // speech: the speech player exists (audio on, 0x4d1f8c); its files live under speechRoot (the TB install).
    // A clip about to start (S[0x22] 2) streams only if its file opens, else the exe times it (TbWorld::speechAudio).
    bool speech = false;
    std::filesystem::path speechRoot;

    explicit TbPlay(bb::game::TbWorld w) : world(std::move(w)) { prev = cur = snap(); }
    void tick(const bb::game::PadInput& p1, const bb::game::PadInput& p2);
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
