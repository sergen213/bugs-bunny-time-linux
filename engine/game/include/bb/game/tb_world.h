// Time Busters gameplay at the original 30 Hz tick (GAMEPLAY_TB.md; § numbers there): two type-14 characters
// (Bugs id 2, Taz id 3) driven by their state records / transitions / logic blocks, the collision mover, R1 switch,
// L1 call friend, 1- and 2-player pads and the spring camera. Integer PS1 units/axes (+Y down), angles 4.12.
// Headless and deterministic. Only the player characters are simulated (no NPCs, items, level logic).
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/game/pad.h"
#include "bb/game/tb_collision.h"
#include "bb/scene.h"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bb::game {

// Tag 0x2f (48 B) state record, §3.1.
struct TbStateRec {
    uint16_t id = 0, anim = 0, mode = 0, logicA = 0, logicB = 0;
    uint32_t flagsA = 0, flagsB = 0, flagsC = 0;
    int16_t fwd = 0, grip = 0, dmg = 0, vert = 0, target = 0, side = 0, cap = 0, hitTrans = 0, ownHurt = 0;
};
struct TbTransition { uint16_t event = 0; std::array<uint16_t, 5> seq{}; };  // tag 0x31
struct TbLogicItem {
    uint8_t tag = 0;                // 0x4e..0x60 ('N'..'`')
    std::array<uint8_t, 20> d{};    // payload (size per tag, tb_level.cpp kLogic)
    int32_t skip = -1;              // statement markers: item index the encoder 0x433900 makes the skip land on
};

// Pad record GD+8+pad*0x28 (GAMEPLAY.md §2.3).
struct TbPad {
    uint16_t pressed = 0, held = 0, raw = 0, prevHeld = 0, prevRaw = 0;
    int8_t sx = 0, sy = 0;  // +0xc / +0xd (right +, up +)
};
// One tick of the TB pad layer: held/raw from the report, pressed = held & ~(prev & edgeMask) (chunk 0x3a tag 0x3c
// @12), stick: digital ±0x7f; analog dead zone 0x4d..0xb1 scaled 128/78 (formula Inferred).
void tbPadTick(TbPad& p, const PadInput& in, uint16_t edgeMask);

struct TbActor {
    uint16_t id = 0;                         // tag 0x27: 2 Bugs, 3 Taz
    uint32_t flags0 = 0, flags1 = 0, flags10 = 0;  // tag 0x16 words -> +0x08, +0x0c (pad bits 0x400/0x800), +0x10
    std::vector<TbStateRec> states;
    std::vector<TbTransition> trans;
    std::map<uint16_t, std::vector<TbLogicItem>> blocks;
    const ActorModel* model = nullptr;
    AnimState anim;
    uint16_t curAnim = 0;                    // +0x14c (0 = restart the state's anim)
    int32_t paceAcc = 0;                     // +0x19e
    V3 pos, spawn;                           // +0xfc
    int16_t pitch = 0, yaw = 0, roll = 0, spawnYaw = 0;  // +0xf4 / +0xf6 / +0xf8
    std::array<int32_t, 3> vel{};            // +0x70 velocity x16
    uint32_t f20 = 0, f24 = 0, f28 = 0;      // runtime flags
    int32_t state = -1, tr = -1;             // indices into states / trans (+0x17c / +0x178)
    uint16_t seq = 0;                        // +0x188
    uint32_t stateTicks = 0;
    int16_t turn = 0;                        // +0x18c
    int32_t groundPoly = -1;                 // +0x108
    std::vector<std::array<int16_t, 6>> boxes;  // tag 0x61 collision box records (+0x78)
    int32_t boxIdx = 0;                      // +0x123 selected record (action 82)
    bool boxInstant = false;
    std::array<int16_t, 6> box{};            // +0x60.. actor box: min x, max y (feet), min z, max x, min y (top), max z
    bool started = false;
    const TbStateRec* rec() const { return state >= 0 ? &states[size_t(state)] : nullptr; }
    int padIndex() const { return (flags1 >> 11) & 1; }  // flags1 0x800 -> pad record 1
};

struct TbCamera {
    V3 body, look, eye;                      // B+0x2c spring body P, B+0x10 look-at L, B+4 eye
    std::array<int32_t, 3> vel{};            // B+0xc0 body velocity (Q12)
    int32_t dist = 1200;                     // DAT_004d9ae0 spring rest length D (B+0x118)
    uint32_t flags = 0;                      // B+0xe0 follow flags (0x80 single focus, 0x100 two-player)
    int32_t blend = 0x1000;                  // 0x4d9acc
    V3 saved;
    int32_t acc = 0;                         // B+0xe4 L2/R2 accumulator
    int32_t splitTicks = 0;
    int lastFocus = -1;
    bool init = false;
};

class TbWorld {
public:
    // Throws FormatError without a chunk-0x35 collision mesh or without both characters. scene supplies the actor
    // models (addTbActors) for animation and the collision box (TOD packet 0x900); may lack them (no animation).
    static TbWorld load(const BzeFile& bze, const Scene& scene);

    void tick(const PadInput& p1, const PadInput& p2 = {});
    void setTwoPlayer(bool on);              // GD+0xA4 0x10 (menu "2 Players" 0x4370c0)
    bool twoPlayer() const { return A4 & 0x10; }
    int focus() const { return (A4 & 4) ? 1 : 0; }  // index into actors: 0 Bugs, 1 Taz
    bool callFriend(bool force = false);     // 0x45eb10; true if the partner was placed

    TbCollision col;
    std::array<TbActor, 2> actors;           // [0] Bugs (id 2), [1] Taz (id 3)
    std::array<TbPad, 2> pads;
    uint16_t edgeMask = 0;                   // 0x4d26c0
    uint8_t A4 = 0;                          // GD+0xA4
    uint16_t cooldown = 50;                  // 0x4ceb08 (50 at level start in the runtime trace; counts to 45 after a call)
    uint8_t airTicks = 0;                    // 0x4d5eb6
    int camFocus = 0;                        // DAT_004d5028: focus latched before the switch (camera one tick late)
    TbCamera cam;
    std::array<uint8_t, 0x400> S{};          // logic byte variables 0x4d24a0
    std::array<uint8_t, 0x300> GD{};         // GAME_DATA image (ability bits GD+0xac/+0xad)
    int music = 0;                           // logic action 126 value n (Track(n-1).xa), first setter in the level
    int dt = 0;                              // DAT_004d26c4: 0 on the first tick after load, then 2
    uint32_t ticks = 0;
    int lastCallFriend = 0;                  // 1 placed, -1 refused/failed, 0 not requested (this tick)
    std::set<std::string> unsupported;       // logic conditions/actions met but not interpreted

private:
    int16_t lookYaw_ = 0;                    // DAT_004d5972 (stale look-mode yaw given to the called partner)
    void actorTick(TbActor& a);
    void dispatch(TbActor& a, uint32_t ev);
    uint32_t runBlock(TbActor& a, uint16_t block);
    bool cond(TbActor& a, const TbLogicItem& it, bool cur);
    void action(TbActor& a, const TbLogicItem& it);
    void move(TbActor& a, const std::array<int32_t, 3>& target);
    void switchTick();
    void cameraTick();
    void cameraSnap(const TbActor& t);
    int16_t camYaw() const;
    const TbPad& padOf(const TbActor& a, int override = -1) const { return pads[size_t(override >= 0 ? override : a.padIndex())]; }
};

} // namespace bb::game
