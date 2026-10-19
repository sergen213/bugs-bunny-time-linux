// LiT gameplay simulation at the original 30 Hz tick (GAMEPLAY.md): tick order pad -> camera (0x4318f0, follows
// the previous tick's player position) -> actor activation and behaviours (0x447d40: player first among the
// ordinary actors) -> level change request. Integer PS1 units/axes (+Y down), angles 4.12. Headless and
// deterministic. Logic VM: lit_logic.cpp (LOGIC.md); actor behaviours: lit_actors.cpp (GAMEPLAY.md §7).
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/game/heightfield.h"
#include "bb/game/litmath.h"
#include "bb/game/pad.h"
#include "bb/scene.h"

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

// Actor script data (LOGIC.md §2): tag 0x30 step (payload id@0 anim@2 mode@4 group@8 flagsA@12 flagsB@16
// w@20..29 = node +0x14..+0x1c), tag 0x31 item, tag 0x32 state (5 step ids, 0xfff0 ends).
struct ActorStep {
    uint16_t id = 0, anim = 0;
    int16_t mode = 0;
    uint16_t group = 0;
    uint32_t flagsA = 0, flagsB = 0;
    std::array<int16_t, 5> w{};  // +0x14 speed, +0x16 damage / target id, +0x18 arc step, +0x1a arc height, +0x1c knock-back state
};
struct ActorItem {
    uint16_t group = 0, next = 0;
    uint32_t mask14 = 0, mask18 = 0;
    LogicItem item;
    uint32_t flags = 0;
    int32_t p1 = 0, p2 = 0;
};
// One entity chunk 0x07 / 0x0a (live) or 0x08 (spawn template), tag map GAMEPLAY.md §7.1.
struct ActorDef {
    uint8_t chunk = 0;
    uint16_t type = 0, entityId = 0;
    V3 pos;
    std::array<int16_t, 3> rot{};
    std::array<int32_t, 3> scale{4096, 4096, 4096};
    uint32_t flags8 = 0, flagsC = 0;                // tag 0x16 (+0x08 / +0x0c)
    uint16_t cls = 0;                               // tag 0x15 (+0x2a, 2 = collision list)
    int16_t reg = 0;                                // tag 0x1e (+0x1e: hit points / register)
    int16_t param = 0;                              // tag 0x1d (+0x17c; type 14 turn shift +0x196)
    std::array<int16_t, 6> box{};                   // tags 0x1b/0x1c/0x42: minX, maxY, minZ, maxX, minY (top), maxZ
    int16_t inflate = 0;                            // tag 0x38 (+0x20)
    uint16_t act3c = 0, act3e = 0;                  // tag 0x3a activation flags / radius
    int16_t zone = -1;                              // tag 0x1f
    int32_t model = -1;                             // Scene::actorModels index (TOD actors), -1 none
    uint16_t idleAnim = 0;                          // ActorInstance::anim: looped by behaviours without script animation
    std::vector<PlayerState> states;
    std::vector<ActorStep> steps;
    std::vector<ActorItem> items;
};

// Fields MoveActor 0x437390 and the arc 0x43a250 act on; shared by Bugs and every actor (offsets of the 0x1f4-B actor).
struct Mover {
    V3 pos;                               // +0xe4
    int16_t yaw = 0;                      // +0xde
    uint32_t flags14 = 0, flags18 = 0;    // +0x14 / +0x18
    int16_t jumpH = 0, jumpStep = 0, phase = 0x800, vy = 0;  // +0x104..+0x10a
    int32_t groundBase = 0;               // +0x100
    int16_t hoff = 0;                     // +0x10c; ground y = groundBase + hoff
    int16_t zone = -1;                    // +0x26
    uint16_t timer = 0;                   // +0x22 ticks
    int16_t reg = 0;                      // +0x1e
    uint16_t entityId = 0;                // +0x1c (tag 0x27)
};

struct Player : Mover {
    bool initialised = false;  // actor+0x78 (part list) built: the exe initialises on the first behaviour call
    int16_t lean = 0;          // +0xe0
    uint16_t state = 1, seq = 0;   // +0x17c, +0x17e
    uint16_t anim = 0;             // +0x158 current animation id (0 = restart the move's animation)
    int32_t lastMove = 0;          // +0x1b4 (OR of this tick's world delta)
    uint8_t frameSkip = 0;         // +0x149
    int32_t platform = -1;         // +0x70 actor ridden (LitWorld::actors index)
    int32_t carrier = -1;          // actor carrying Bugs (its +0x6c, item flag 0x20000000): Bugs follows it
    uint16_t contact = 0;          // +0x34 entity id of the last actor touched / hit
    AnimState animState;
};

struct LitActor : Mover {
    int32_t def = -1;              // LitWorld::defs index
    uint16_t type = 0;             // +0x2c
    uint32_t flags8 = 0, flagsC = 0;
    std::array<int16_t, 3> rot{};  // +0xdc (yaw kept in Mover::yaw)
    std::array<int32_t, 3> scale{4096, 4096, 4096};
    std::array<int16_t, 6> box{};
    int16_t inflate = 0;
    uint16_t act3c = 0, act3e = 0;  // +0x3c activation flags (A32/A64) / +0x3e radius
    int16_t param = 0;             // +0x17c (platforms) / +0x196 (type 14)
    int16_t state = -1;            // +0x180: ActorDef::states index, -1 = not started
    uint16_t seq = 0;              // +0x17c (type 14 / 2): step index within the state
    int16_t step = -1;             // +0x184: ActorDef::steps index
    uint16_t anim = 0;             // +0x158 playing animation id (0 = restart)
    uint16_t contact = 0;          // +0x34 entity id of the last contact
    int16_t damage = 0;            // +0x38 pending damage
    int16_t wander = 0, wanderDir = 0;  // +0x18e / +0x190
    int16_t p17e = 0;              // platform phase / offset +0x17e
    int32_t lo = 0, hi = 0;        // platform lift limits +0x184 / +0x194
    V3 base;                       // platform rest position +0x190..
    int32_t rider = -1;            // +0x6c (player riding = -2)
    bool visible = true;           // +4 bit 2 (cleared when culled)
    bool active = false;           // linked into the active list this tick
    bool dead = false;             // flags14 0x100 handled: freed
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
    V3 point, lookPoint;               // 0x4b3984.. eye point (flag 0x400) / 0x4b3994.. target point (0x800): type-36 directors
    int32_t fixedDist = 0;             // 0x4b39a4 scripted eye distance from the target (flag 0x1000)
};

class LitWorld {
public:
    // Throws FormatError if the level has no player entity or no chunk 0x36 heightfield. scene (optional,
    // buildLitScene of the same BZE) supplies the TOD actor models: without it animations never end.
    static LitWorld load(const BzeFile& bze, const Scene* scene = nullptr);
    // Exe level table 0x4ad380 (16-B entries, BZE path): level id -> file stem ("L01B"); empty if out of range.
    static std::string levelName(int id);

    void tick(const PadInput& in);
    // (Re)starts Bugs at spawn with new-game globals; load() calls it. Tests fill hf/states/moves/rules then call it.
    void start(V3 spawn, int16_t yaw, int16_t zone = -1);
    // The load-time actor update of 0x430f80 (one 0x447d40 call before the first frame, GAMEPLAY.md §1.2): initialises
    // the player behaviour, which resets the camera to the spawn (0x431160). Call after start() and any GAME_DATA
    // restore (the start director reads G), before the first tick().
    void loadUpdate() { cameraTick(); actorsTick(); }
    const ActorModel* playerModel() const { return model_; }

    Heightfield hf;
    std::vector<PlayerState> states;
    std::vector<PlayerMove> moves;
    std::vector<PlayerRule> rules;
    std::vector<Trigger> triggers;
    std::vector<ActorDef> defs;      // every entity chunk (live + templates); actors index into it
    std::vector<LitActor> actors;    // live actors (dead ones stay with dead = true so indices are stable)
    Player p;
    Camera cam;
    Pad pad;
    std::array<uint8_t, 256> G{}, S{};
    uint32_t ticks = 0;
    int nextLevel = -1;              // level change request (GD+0 / DAT_004b39bc = 1), LitWorld::levelName id
    std::vector<uint16_t> sfx;       // section-3 sound-bank blob indices started this tick (sound table, LOGIC §3)
    std::set<std::string> unsupported;  // logic conditions/actions/flags met but not interpreted (status/overlay)

    int health() const { return G[1]; }
    int carrots() const { return G[3]; }
    int clocks() const { return G[5]; }
    int goldenCarrots() const { return G[7] | G[0xfc] << 8; }
    int32_t groundY() const { return p.groundBase + p.hoff; }
    const V3& checkpoint() const { return checkpoint_; }  // 0x4b2460 respawn point

    // Logic VM (LOGIC.md §4/§5). self = the actor running the item (nullptr = Bugs).
    bool cond(const LogicItem& it, LitActor* self);
    void act(const LogicItem& it, LitActor* self);
    // Spawns template entity `entityId` (0x448d40) at pos/yaw; returns the actors index or -1.
    int spawn(uint16_t entityId, const V3& pos, int16_t yaw);

private:
    const ActorModel* model_ = nullptr;
    const Scene* scene_ = nullptr;
    V3 spawn_;
    int16_t spawnYaw_ = 0;
    V3 levelEye_;  // chunk 0x3b tag 0x2b
    std::vector<std::array<int32_t, 2>> sounds_;  // sound id -> {blob index or -1, flags} (tags 0x04 + section-4 program table)

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
    int32_t camHold_ = 0;            // 0x4b3dac (A41)
    MsvcRand rand_;

    void initPlayer();
    void playerTick();
    void playerContacts(V3& w);
    void playerAttacks(const PlayerMove& mv);
    void moveActor(Mover& m, V3& delta, Ground& r, int16_t boxTop);
    void arcTick(Mover& m);
    void cameraReset();
    void cameraSnap(const V3& at);
    void cameraTick();
    void zoneTriggers(Mover& m, LitActor* self);
    void kill(int16_t deathState);
    void respawn();
    void forceState(uint16_t state);  // 0x4377a0
    bool damagePlayer(LitActor& a, const ActorStep* st);  // 0x437d90
    void playSound(uint32_t id);
    const PlayerState* findState(uint16_t id) const;
    const PlayerMove* findMove(uint16_t id) const;
    int16_t boxTop() const;

    // lit_actors.cpp
    LitActor instantiate(size_t def) const;
    void actorsTick();
    void behaviour(size_t i);
    void type14(size_t i);
    void type2(size_t i);
    void platform(size_t i, uint32_t mode);
    void director(size_t i);
    void cameraDirector(size_t i);
    bool runItem(size_t i, const ActorItem& it, bool gated);  // item gates + condition; true = fires
    int findActor(uint16_t entityId) const;
    void startScript(LitActor& a, uint16_t stateId);
    uint32_t animate(LitActor& a, uint16_t anim, uint32_t mode);
};

} // namespace bb::game
