// LiT gameplay simulation at the original 30 Hz tick (GAMEPLAY.md): tick order pad -> camera (0x4318f0, follows
// the previous tick's player position) -> player (behaviour type 1 0x43a880, state machine = level data) ->
// actor timer. Integer PS1 units/axes (+Y down), angles 4.12. Headless and deterministic.
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/game/heightfield.h"
#include "bb/game/litmath.h"
#include "bb/game/pad.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace bb::game {

struct LogicItem { uint8_t cond = 0, a = 0, b = 0, act = 0; uint16_t x = 0, y = 0; };  // LOGIC.md §2

// Player tables (§3.1): tag 0x32 state, 0x34 move, 0x35 transition rule (payload layouts per tools/re/player_states.py).
struct PlayerState { uint16_t id = 0; std::array<int16_t, 5> moves{}; };
struct PlayerMove {
    uint16_t id = 0, anim = 0;
    int16_t mode = 0;
    uint32_t flagsA = 0, flagsB = 0;
    uint16_t link = 0;
    int16_t speed = 0, jumpH = 0, jumpStep = 0;
};
struct PlayerRule {
    uint16_t group = 0, all = 0, any = 0, heldCmd = 0;
    int16_t next = -13;
    LogicItem item;
    uint16_t invert = 0;
    uint32_t heldMask = 0, parentMask = 0, flags14Mask = 0, flags18Mask = 0;
};

// Zone trigger volume (section-1 chunk 0x09, §4.4/§7.5) and its 0x33 entries.
struct TriggerEntry { uint32_t actor = 0, mask14 = 0; LogicItem item; uint32_t flags = 0; std::array<int32_t, 3> arg{}; };
struct Trigger {
    V3 pos;
    int16_t rotY = 0;
    int16_t sizeX = 0, minY = 0, sizeZ = 0;
    int16_t zone = -1;
    uint32_t flags = 0;
    std::vector<TriggerEntry> entries;
};

enum class PickupKind : uint8_t { Carrot, GoldenCarrot, Clock };
struct Pickup {
    PickupKind kind = PickupKind::Carrot;
    uint32_t scriptOffset = 0;  // ScriptItem::offset of the entity chunk (= ActorInstance::scriptOffset)
    V3 pos;
    int16_t rotY = 0;
    std::array<int16_t, 6> box{};  // tags 0x1b/0x1c: minX, maxY, minZ, maxX, minY(top), maxZ
    int16_t inflate = 0;           // tag 0x38
    std::vector<std::pair<uint16_t, uint16_t>> bits;  // A12 orgi (X, G index): collected flags
    int counter = -1;              // per-level counter G index (golden carrots / clocks)
    bool taken = false;
};

struct Player {
    bool initialised = false;  // actor+0x78 (part list) built: the exe initialises on the first behaviour call
    V3 pos;                     // +0xe4
    int16_t yaw = 0, lean = 0;  // +0xde, +0xe0
    uint32_t flags14 = 0, flags18 = 0;
    uint16_t state = 1, seq = 0;   // +0x17c, +0x17e
    uint16_t anim = 0;             // +0x158 current animation id (0 = restart the move's animation)
    int16_t jumpH = 0, jumpStep = 0, phase = 0x800, vy = 0;  // +0x104..+0x10a
    int32_t groundBase = 0;        // +0x100
    int16_t hoff = 0;              // +0x10c; ground y = groundBase + hoff
    int16_t zone = -1;             // +0x26
    uint16_t timer = 0;            // +0x22
    int32_t lastMove = 0;          // +0x1b4 (OR of this tick's world delta)
    uint8_t frameSkip = 0;         // +0x149
    AnimState animState;
};

struct Camera {
    V3 eye, target, cand;              // 0x4b38c0 / 0x4b38cc / 0x4b3928
    V3 eyeCopy, targetCopy, candCopy;  // 0x4b3954.. / 0x4b3964.. / 0x4b3974.. (unshaken copies)
    int16_t yaw = 0;                   // 0x4b3e1a
    int16_t cachedYaw = 0;             // 0x4b3e2c
    int32_t cachedDist = 1100;         // 0x4b3da8
    uint32_t flags = 0x40;             // 0x4b3940
    int16_t recentreTimer = 0;         // 0x4b3e28
    int16_t recentreRequest = 0;       // 0x4b3df0
    int16_t lastStep = 0;              // 0x4b3e10
    int32_t heightOffset = -200, eyeHeight = 600, distance = 1100;  // 0x4b215c / 0x4b2150 / 0x4b2154 (§5)
    V3 lastPlayer;                     // 0x4b3e00..08
};

class LitWorld {
public:
    // Throws FormatError if the level has no player entity or no chunk 0x36 heightfield. playerModel (optional,
    // from Scene::actorModels) drives the move animations; without it clips never end (0x400 "missing").
    static LitWorld load(const BzeFile& bze, const ActorModel* playerModel = nullptr);

    void tick(const PadInput& in);
    // (Re)starts at spawn with new-game globals; load() calls it. Tests fill hf/states/moves/rules then call it.
    void start(V3 spawn, int16_t yaw, int16_t zone = -1);
    void setPlayerModel(const ActorModel* m) { model_ = m; }
    const ActorModel* playerModel() const { return model_; }

    Heightfield hf;
    std::vector<PlayerState> states;
    std::vector<PlayerMove> moves;
    std::vector<PlayerRule> rules;
    std::vector<Trigger> triggers;
    std::vector<Pickup> pickups;
    Player p;
    Camera cam;
    Pad pad;
    std::array<uint8_t, 256> G{}, S{};
    uint32_t ticks = 0;
    std::set<std::string> unsupported;  // logic conditions/actions met but not interpreted (status/overlay)

    int health() const { return G[1]; }
    int carrots() const { return G[3]; }
    int clocks() const { return G[5]; }
    int goldenCarrots() const { return G[7] | G[0xfc] << 8; }
    int32_t groundY() const { return p.groundBase + p.hoff; }
    const V3& checkpoint() const { return checkpoint_; }  // 0x4b2460 respawn point

private:
    const ActorModel* model_ = nullptr;
    uint16_t entityId_ = 1;  // player tag 0x27
    V3 spawn_;
    int16_t spawnYaw_ = 0;
    V3 levelEye_;  // chunk 0x3b tag 0x2b

    // Player-behaviour globals (§3).
    int32_t vsX_ = 0, vsZ_ = 0;    // 0x4b3c10 / 0x4b3c18 smoothed ground velocity
    int32_t momX_ = 0, momZ_ = 0;  // 0x4b3c30 / 0x4b3c38 momentum / slope term
    int32_t accelK_ = 0x800;       // 0x4b3b90
    int16_t turnErr_ = -1;         // 0x4b3c20
    int16_t camYawRef_ = 0;        // 0x4b39fe
    int32_t paceAcc_ = 0;          // 0x4b3bc0
    V3 safe_;                      // 0x4b3780
    int32_t invuln_ = 0;           // 0x4b3160
    V3 checkpoint_;                // 0x4b2460
    int32_t checkpointYaw_ = 0;    // 0x4b246c
    int32_t defaultDeathState_ = 0;  // 0x4b2370 (zone trigger 0x400000)
    int32_t deathStateA92_ = 0;      // 0x4b39d8 (A92)
    MsvcRand rand_;

    void initPlayer();
    void playerTick();
    void moveActor(V3& delta, Ground& r);
    void arcTick();
    void cameraReset();
    void cameraSnap(const V3& at);
    void cameraTick();
    void zoneTriggers();
    void pickupTouch();
    void kill(int16_t deathState);
    void respawn();
    const PlayerState* findState(uint16_t id) const;
    const PlayerMove* findMove(uint16_t id) const;
    bool cond(const LogicItem& it);
    void act(const LogicItem& it);
    int16_t boxTop() const;
};

} // namespace bb::game
