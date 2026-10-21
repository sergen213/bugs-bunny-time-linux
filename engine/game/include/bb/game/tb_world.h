// Time Busters gameplay at the original 30 Hz tick (GAMEPLAY_TB.md; § numbers there): every placed entity of the
// level (chunks 0x07/0x0a/0x63, templates resolved as 0x433620) in the original list order, ticked when the view
// list 0x45eee0 activates it, by class (14 character 0x453de0 for Bugs, Taz, NPCs, enemies and items; 16/35 logic
// directors), with the logic VM, the collision mover, actor contacts, attack probes, damage/health/lives/death/
// respawn, R1 switch, L1 call friend, 1- and 2-player pads and the spring camera. Integer PS1 units/axes (+Y down),
// angles 4.12. Headless and deterministic.
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/game/litmath.h"
#include "bb/game/pad.h"
#include "bb/game/tb_collision.h"
#include "bb/scene.h"

#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
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
    uint8_t counter = 0;            // '`': repeat counter the VM (op 0x16) writes back into the compiled block
};
using TbBlock = std::vector<TbLogicItem>;

// Pad record GD+8+pad*0x28 (GAMEPLAY.md §2.3).
struct TbPad {
    uint16_t pressed = 0, held = 0, raw = 0, prevHeld = 0, prevRaw = 0;
    int8_t sx = 0, sy = 0;  // +0xc / +0xd (right +, up +)
};
// One tick of the TB pad layer: held/raw from the report, pressed = held & ~(prev & edgeMask) (chunk 0x3a tag 0x3c
// @12), stick: digital ±0x7f; analog dead zone 0x4d..0xb1 scaled 128/78 (formula Inferred).
void tbPadTick(TbPad& p, const PadInput& in, uint16_t edgeMask);

struct TbActor {
    uint16_t id = 0;                         // tag 0x27 (+0x34): 2 Bugs, 3 Taz, template id for spawned entities
    uint16_t behaviour = 0;                  // tag 0x13 (+0x31) class: 14 = character 0x453de0, 16 director, 0x21 held item
    uint32_t uid = 0;                        // entity handle: 0 Bugs, 1 Taz, others >= 2
    uint32_t listPos = 0;                    // position in the entity list 0x4d4fd0 (file order; spawns appended)
    uint32_t flags0 = 0, flags1 = 0, flags10 = 0;  // tag 0x16 words -> +0x08, +0x0c (pad bits 0x400/0x800), +0x10
    uint16_t progBlock = 0;                  // tag 0x16 word 3 -> +0x2c entity program (logic block id)
    std::array<uint32_t, 3> progMask{};      // tag 0x16 words 4..6 -> +0x14/+0x18/+0x1c (+0x20/+0x24/+0x28 triggers)
    std::vector<TbStateRec> states;
    std::vector<TbTransition> trans;
    std::map<uint16_t, TbBlock> blocks;      // blocks < 10000 (entity list +0x114)
    uint16_t firstBlock = 0;                 // head of +0x114 (own blocks first, then the template's): director logic
    const ActorModel* model = nullptr;
    std::shared_ptr<const ActorModel> ownModel;  // spawned entities: the template's model, rig and anims
    AnimState anim;
    uint16_t curAnim = 0;                    // +0x14c (0 = restart the state's anim)
    int32_t paceAcc = 0;                     // +0x19e
    V3 pos, spawn;                           // +0xfc
    int16_t pitch = 0, yaw = 0, roll = 0, spawnYaw = 0;  // +0xf4 / +0xf6 / +0xf8
    std::array<int16_t, 3> scale{4096, 4096, 4096};       // tag 0x12 (+0xec, drawing only)
    std::array<int32_t, 3> vel{};            // +0x70 velocity x16
    uint32_t f20 = 0, f24 = 0, f28 = 0;      // runtime flags
    int32_t state = -1, tr = -1;             // indices into states / trans (+0x17c / +0x178)
    uint16_t seq = 0;                        // +0x188
    uint16_t stateTicks = 0;                 // +0x82: +1 after every tick, reset only by logic action 16 (not by
                                             // state changes; runtime trace local/wine/tbplay/G_idle_rng.csv)
    int16_t turn = 0;                        // +0x18c
    int16_t turnRate = 0;                    // +0x192
    int32_t groundPoly = -1;                 // +0x108
    std::vector<std::array<int16_t, 6>> boxes;  // tag 0x61 collision box records (+0x78)
    int32_t boxIdx = 0;                      // +0x123 selected record (action 82)
    bool boxInstant = false;
    std::array<int16_t, 6> box{};            // +0x60.. actor box: min x, max y (feet), min z, max x, min y (top), max z
    uint16_t radius = 0;                     // +0x4a: distance of the farthest box corner (0x450440), 0 = no contacts
    int16_t inflate = 0;                     // +0x3e (tag 0x37 @0): box x/z inflation
    int16_t weight = 0;                      // +0x42 (tag 0x37 @2; W 0x87)
    int32_t attachPart = -1;                 // +0x84: part of the last TOD packet 0xa00 (attach point), -1 = none
    V3 attachPos;                            // +0x7c: world origin of that part (attack probe point)
    bool animLast = false;                   // +0x14e & 0x2000 of 0x469770: the last frame's hold expired this tick,
                                             // kept through the loop-restart tick (trace G; AnimState's flag is a tick late)
    int32_t parent = -1;                     // +0x90: uid of the entity this one is attached to
    int32_t parentPart = -1;                 // +0xe8: the parent's part it hangs on (-1 = parent root)
    bool started = false;
    // View list 0x45eee0: tag 0x39 @0 +0x6e flags (0x20 never listed, 0x40 always, 2 range test, 4/8 frustum, 0x10
    // kept while out of view), @2 +0x76 range, @4 +0x118 activation block; +0x50 sleep after a range cull.
    uint16_t view = 0, range = 0, actBlock = 0;
    int32_t sleep = 0;
    bool active = false;                     // listed (ticked) this tick
    bool collidable = false;                 // in the contact list 0x4d2734 this tick (flags0 & 0x2000100 == 0x100)
    uint16_t gdBit = 0;                      // tag 0x65 (+0x48): GAME_DATA +500 bit; flags10 & 4: removed when set
    int16_t maxHp = 1;                       // DAT_004d5ebc (flags1 0x200 boss bar S[1])
    int16_t hp = 0;                          // +0x3c health (tag 0x1e; players GD+0xa5/+0xa6)
    int16_t dmgIn = 0;                       // +0x66 damage pending
    int16_t hitTr = -1;                      // +0x5e transition the attacker's state asks for (-1 none)
    int32_t contact = -1;                    // +0x54 uid of the last touching / hit entity
    int32_t link = -1;                       // +0x1ac uid (V 0x100 target, selector 0xd)
    std::array<uint32_t, 3> trig{};          // +0x20/+0x24/+0x28 bits raised this tick (program triggers & +0x14..)
    const TbStateRec* rec() const { return state >= 0 ? &states[size_t(state)] : nullptr; }
    int padIndex() const { return (flags1 >> 11) & 1; }  // flags1 0x800 -> pad record 1
    bool drawn() const { return !(f20 & 0x2000) && !(rec() && (rec()->flagsC & 0x20000)); }  // 0x464480 skip
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
    // Enters state `id` of actors[actor] like a transition target (anim restarts next tick; stateTicks kept).
    // For replays that start from a captured state. False if the actor has no such state.
    bool setState(int actor, uint16_t id);
    bool setState(TbActor& a, uint16_t id);
    TbActor* entity(int32_t uid);            // any entity by uid (nullptr if gone)
    TbActor* byListPos(uint32_t pos);        // a level entity by its position in the entity list (file order)
    const std::vector<TbActor*>& list() { return order(); }  // every entity in list (tick) order

    TbCollision col;
    std::array<TbActor, 2> actors;           // [0] Bugs (id 2), [1] Taz (id 3)
    std::list<TbActor> entities;             // every other entity: level instances, then V 0x10 spawns
    std::array<TbPad, 2> pads;
    uint16_t edgeMask = 0;                   // 0x4d26c0
    uint8_t A4 = 0;                          // GD+0xA4
    uint16_t cooldown = 50;                  // 0x4ceb08 (50 at level start in the runtime trace; counts to 45 after a call)
    uint8_t airTicks = 0;                    // 0x4d5eb6
    int camFocus = 0;                        // DAT_004d5028: focus latched before the switch (camera one tick late)
    TbCamera cam;
    std::array<uint8_t, 0x400> S{};          // logic byte variables 0x4d24a0
    std::array<uint8_t, 0x300> GD{};         // GAME_DATA image: +0xa0 lives, +0xa2 down flags, +0xa5/+0xa6 health,
                                             // +0xac/+0xad ability bits
    std::array<int16_t, 4> checkpoint{};     // 0x4d26e0..ec respawn point x y z yaw (V 2/8/0x2000)
    int16_t invuln = 0;                      // 0x4d271c ticks without damage after action 0x431a40
    std::array<uint8_t, 2> flash{};          // 0x4d2058/9 Bugs/Taz hit flash (+0x28 0x200 while < 0x3c)
    std::array<uint32_t, 2> deathEvent{};    // 0x4d2720 / 0x4d5948 death transition overrides (V 0x80002000)
    std::array<uint32_t, 2> deathEvent2{};   // 0x4d5944 / 0x4d273c (V 0x80001000)
    bool restart = false;                    // 0x4d593a: level reload requested (game over: GD+0 = 5 RETRY)
    int32_t nextLevel = -1;                  // GD+0 written by V 0x1000 (level index; 5 = RETRY), -1 = unchanged
    int32_t lastSpawn = -1;                  // 0x4d5eb8 uid (V 0x200000; V 0x80000001 teleports it)
    struct CamScript { uint32_t flags = 0; V3 look, eye; } camScript;  // '_' op 0x15 (B+0x3c, +0x48, +0x58)
    // CRT rand 0x46afe0 (seed 0x4cf26c, srand(1) once at start-up 0x401000): logic 'W' chances, action 7/8, spawn
    // yaw. The original shares it with every entity, sound and effect of the level; replays set seed per tick.
    MsvcRand rng;
    std::vector<uint16_t> sounds;            // sound requests this tick (0x407080 ids; not played here)
    std::vector<uint16_t> effects;           // '^' effect-template spawns this tick (0x45cc40; not simulated)
    int music = 0;                           // logic action 126 value n (Track(n-1).xa), first setter in the level
    int dt = 0;                              // DAT_004d26c4: 0 on the first tick after load, then 2
    uint32_t ticks = 0;
    int lastCallFriend = 0;                  // 1 placed, -1 refused/failed, 0 not requested (this tick)
    std::set<std::string> unsupported;       // logic conditions/actions met but not interpreted

private:
    int16_t lookYaw_ = 0;                    // DAT_004d5972 (stale look-mode yaw given to the called partner)
    std::map<uint16_t, TbActor> templates_;  // spawnable templates (chunk 0x08) referenced by V 0x10 actions
    std::map<uint16_t, TbBlock> globalBlocks_;  // blocks >= 10000 (global list 0x4d5984)
    std::vector<TbActor*> list_;             // entity list 0x4d4fd0 order: players at their file position
    V3 prevEye_;                             // DAT_004d2750: camera position of the previous list build
    const TbWorld* listOwner_ = nullptr;     // list_ is rebuilt when stale (spawn/erase) or the world was moved
    uint32_t nextUid_ = 2, nextListPos_ = 0;
    std::vector<TbActor*>& order();
    void viewList();                         // 0x45eee0 list build: activation (+0x6e/+0x76/+0x118/+0x50)
    void classTick(TbActor& a);              // dispatch table 0x4cea60 by +0x31
    bool hitIntake(TbActor& a);              // 0x453de0 head: pending hit -> health, hurt / death event
    void contacts(TbActor& a, const std::vector<TbActor*>& touched);  // 0x453de0 contact loop (+0x54, 0x40/0x80)
    void attackHits(TbActor& a, const std::vector<TbActor*>& touched);  // 0x453de0 attack probes 0x447fd0
    std::vector<TbActor*> probe(TbActor& a, const V3& p, uint32_t maskIn, size_t max);  // 0x447fd0
    void playerDown(TbActor& a);             // 0x44ea50 dead player: partner takes over / lives / checkpoint
    void runNear(TbActor& a, uint32_t block, uint32_t radius);  // V 8 / V 0x80000010: block on flags0 0x8000 ents
    void teleport(TbActor& t, V3 p);          // V 4/1/0x100000 placement (clears ground, detaches)
    void actorStep(TbActor& a);              // class 14 0x453de0
    bool dispatch(TbActor& a, uint32_t ev);  // 0x44d990; false when no transition applied
    TbBlock* findBlock(TbActor& a, uint16_t id);
    uint32_t runBlock(TbActor& a, uint16_t block);
    uint32_t runItems(TbActor& a, TbBlock& b);
    uint32_t runProgram(TbActor& a, uint32_t c20, uint32_t c24 = 0, uint32_t c28 = 0);  // 0x45ea90 on +0x2c, then clear
    bool cond(TbActor& a, TbLogicItem& it, bool cur);
    bool condW(TbActor& a, const TbLogicItem& it);
    uint32_t action(TbActor& a, const TbLogicItem& it);
    uint32_t actionV(TbActor& a, const TbLogicItem& it);
    TbActor* lookup(uint32_t sel, TbActor& self);  // 0x45a120
    TbActor* spawn(uint16_t tmpl, V3 pos, int16_t yaw);  // 0x460b00
    void attach(TbActor& parent, TbActor& child);      // V 0x40000 / 0x44d910
    void detach(TbActor& child);                        // 0x44d870
    V3 partWorld(const TbActor& a, int32_t part) const;  // 0x44e560 chain: world origin of a part (-1 = root)
    template <class F> void forEachEntity(F&& f) {  // over a copy: callbacks may spawn (the list is then rebuilt)
        const std::vector<TbActor*> all = order();
        for (TbActor* a : all) f(*a);
    }
    // Mover 0x4450c0; appends the entities the swept shape touched (actor contacts 0x448ce0, shape overlap I).
    void move(TbActor& a, const std::array<int32_t, 3>& target, std::vector<TbActor*>& touched);
    void switchTick();
    void cameraTick();
    void cameraSnap(const TbActor& t);
    int16_t camYaw() const;
    const TbPad& padOf(const TbActor& a, int override = -1) const { return pads[size_t(override >= 0 ? override : a.padIndex())]; }
};

} // namespace bb::game
