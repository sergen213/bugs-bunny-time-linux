// Time Busters gameplay at the original 30 Hz tick (GAMEPLAY_TB.md; § numbers there): every placed entity of the
// level (chunks 0x07/0x0a/0x63, templates resolved as 0x433620) in the original list order, ticked when the view
// list 0x45eee0 activates it, by class (14 character 0x453de0 for Bugs, Taz, NPCs, enemies and items; 16/35 logic
// directors), with the logic VM, the collision mover, actor contacts, attack probes, damage/health/lives/death/
// respawn, R1 switch, L1 call friend, 1- and 2-player pads and the spring camera. Integer PS1 units/axes (+Y down),
// angles 4.12. Headless and deterministic.
#pragma once
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/game/lit_world.h"  // HudItem, Fade, OverlayGlyph: the HUD / fade / overlay-text drawing is shared
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
#include <string_view>
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
    uint32_t chunk = 0;                      // section-1 offset of its instance chunk (0 = spawned)
    uint32_t flags0 = 0, flags1 = 0, flags10 = 0;  // tag 0x16 words -> +0x08, +0x0c (pad bits 0x400/0x800), +0x10
    uint16_t progBlock = 0;                  // tag 0x16 word 3 -> +0x2c entity program (logic block id)
    std::array<uint32_t, 3> progMask{};      // tag 0x16 words 4..6 -> +0x14/+0x18/+0x1c (+0x20/+0x24/+0x28 triggers)
    std::vector<TbStateRec> states;
    std::vector<TbTransition> trans;
    std::map<uint16_t, TbBlock> blocks;      // blocks < 10000 (entity list +0x114)
    int32_t firstBlock = -1;                 // head of +0x114 (own blocks first, then the template's; block 0 is valid)
    const ActorModel* model = nullptr;
    std::shared_ptr<const ActorModel> ownModel;  // spawned entities: the template's model, rig and anims
    AnimState anim;
    uint16_t curAnim = 0;                    // +0x14c (0 = restart the state's anim)
    int32_t paceAcc = 0;                     // +0x19e
    V3 pos, spawn;                           // +0xfc
    int16_t pitch = 0, yaw = 0, roll = 0, spawnYaw = 0;  // +0xf4 / +0xf6 / +0xf8
    std::array<int16_t, 3> scale{4096, 4096, 4096};       // tag 0x12 (+0xec, drawing only)
    std::array<int32_t, 3> vel{};            // +0x70 velocity x16
    std::array<int16_t, 3> carry{};          // +0x58 displacement added to the mover's target once (then cleared)
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
    uint16_t reg = 0;                        // +0x126 entity register: the u16 S[0x1fe] (0x4d269e) while its logic runs
    // Class 31 overlay text (template 0x18, S30 0x4680b0, behaviour 0x450d00): script +0x17a (0x8000 = drawn by
    // the spawner this tick), display time +0x178 (0 = until removed), priority +0x42 (> 0: deferred, lowest wins).
    uint16_t textScript = 0;
    int16_t textTime = 0, textPrio = 0;
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
    // Entities without a scene model get their own (mesh, rig, anims) from their tag-0x0f links. gd: GAME_DATA
    // carried from the previous level (the reload 0x4014c0 keeps the global block); none = new game (3 lives, 6 health).
    static TbWorld load(const BzeFile& bze, const Scene& scene, const std::array<uint8_t, 0x300>* gd = nullptr);
    // Level table 0x4cd998 (96 entries) index -> file stem ("ARCA01"); "" when out of range.
    static std::string_view levelName(int index);

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
    // 0x407080 for entity a (emitter uid + 1): mode 1 start, 2 release.
    void playSound(const TbActor& a, uint16_t id, uint16_t mode = 1) { sfx.request(int32_t(a.uid) + 1, id, mode, rng); }

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
    bool restart = false;                    // 0x4d593a: level reload requested (loads GD+0 = nextLevel, else the same level)
    int32_t nextLevel = -1;                  // GD+0 written by V 0x1000 / game over (level index; 5 = RETRY), -1 = unchanged
    int32_t lastSpawn = -1;                  // 0x4d5eb8 uid (V 0x200000; V 0x80000001 teleports it)
    struct CamScript { uint32_t flags = 0; V3 look, eye; } camScript;  // '_' op 0x15 (B+0x3c, +0x48, +0x58)
    // CRT rand 0x46afe0 (seed 0x4cf26c, srand(1) once at start-up 0x401000): logic 'W' chances, action 7/8, spawn
    // yaw. The original shares it with every entity, sound and effect of the level; replays set seed per tick.
    MsvcRand rng;
    // Sound voices (sfx.h, TB rules): the mixer pass 0x42ae60 runs at the top of each tick, before the entities
    // (requests start one tick later); emitter = entity uid + 1. Oracle replays set sfx.device = false.
    Sfx sfx;
    std::vector<uint16_t> effects;           // '^' effect-template spawns this tick (0x45cc40; not simulated)
    int music = 0;                           // logic action 126 value n (Track(n-1).xa), first setter in the level
    int dt = 0;                              // DAT_004d26c4: 0 on the first tick after load, then 2
    uint32_t ticks = 0;
    int lastCallFriend = 0;                  // 1 placed, -1 refused/failed, 0 not requested (this tick)
    std::set<std::string> unsupported;       // logic conditions/actions met but not interpreted

    // ---- tb_hud.cpp: screen fade, overlay text, HUD (draw data in the LiT HUD / overlay conventions) ----
    // Fade 0x464e90 (from black) / 0x464ec0 (to black), advanced by 0x464ef0 once per rendered tick (tick() ends
    // with it): S51 X=0 fadeIn(1, 0x100), X=1 fadeOut(2, 0x100) (holds black); S52 flash fadeIn(0x41, 0x400). The
    // level change (0x401910) is fadeOut(2, 0x100) + 16 world ticks, after the load (0x4018a0) fadeIn(1, 200).
    Fade fade;
    int8_t bars = 0;                         // GD+0x9f letterbox as drawn this tick (0x464c30): |bars|·h/120 px top and bottom
    void fadeIn(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0, fade.dark = 0x1000; }
    void fadeOut(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0x1000, fade.dark = 0; }
    void fadeTick();
    std::array<uint8_t, 4> fadeOverlay() const;  // RGBA to blend over the frame (as LitWorld::fadeOverlay)
    // Overlay text (0x466d50 over the level's language block, chunk 0x48): glyph sprites 0x4d5af4 and box rects
    // 0x4d5af8 {x, y, w, h}, 512x240 space centred; both lists restart at the top of each tick (0x45ee90).
    std::vector<OverlayGlyph> glyphs;
    std::vector<std::array<int16_t, 4>> textBoxes;
    // HUD (class 6 entity 0x463170): 15 template models (0x4d5ca0, table 0x4ced18; x/y = view translation = screen
    // offset, z depth, as LiT's 0x408910) and 32 HUD-sheet sprites (0x4d5d00, POLY_FT4 of Scene::hudSheet frames in
    // the 512x240 space: x, y top-left, w x h texels from u, v).
    struct HudSprite { uint8_t frame = 0; bool shown = false; int16_t x = 0, y = 0, w = 0, h = 0, u = 0, v = 0; };
    std::array<HudItem, 15> hud{};
    std::array<HudSprite, 32> hudSprites{};
    uint32_t textStatus = 0;                 // 0x4d9b40: status of the last S30 / R 0x22 text (0x4680b0)

private:
    int16_t lookYaw_ = 0;                    // DAT_004d5972 (stale look-mode yaw given to the called partner)
    std::map<uint16_t, TbActor> templates_;  // spawnable templates (chunk 0x08) referenced by V 0x10 actions
    std::map<uint16_t, TbBlock> globalBlocks_;  // blocks >= 10000 (global list 0x4d5984)
    // Animation-asset logic (chunk 0x22 'M' blocks, file order), run by the player 0x469770 on frame steps: the
    // footstep / jump / spin sounds ('`' items) live here. Keyed by the TOD each model parsed for that asset.
    std::map<const Tod*, std::vector<TbBlock>> animLogic_;
    // Particle emitter slots 0x4d6ae0 (20 × 0x9c B, defaults of 0x451e40 at level load), only the fields that gate
    // rand() draws: count +0x52, flags +6, owner entity +0x60 (uid, -1 none).
    struct FxSlot { uint8_t count = 10; uint16_t flags = 0x40; int32_t owner = -1; };
    std::array<FxSlot, 20> fx_{};
    std::vector<TbActor*> list_;             // entity list 0x4d4fd0 order: players at their file position
    V3 prevEye_;                             // DAT_004d2750: camera position of the previous list build
    const TbWorld* listOwner_ = nullptr;     // list_ is rebuilt when stale (spawn/erase) or the world was moved
    uint32_t nextUid_ = 2, nextListPos_ = 0;
    TbActor* regOwner_ = nullptr;            // entity whose register +0x126 S[0x1fe] holds (runItems)
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
    void pauseTick();                        // 0x4014c0 head: Start -> S[9] = 1 + pause menu template 0x1d
    void cameraTick();
    void cameraSnap(const TbActor& t);
    int16_t camYaw() const;
    const TbPad& padOf(const TbActor& a, int override = -1) const { return pads[size_t(override >= 0 ? override : a.padIndex())]; }

    // tb_hud.cpp internals
    struct Slide {  // 16-byte slide record 0x4d5da0 + i*16 (stepped by 0x461840)
        std::array<int16_t, 2> cur{};        // +0 x, +2 y (offset of the element group)
        int16_t in = 0, out = 0;             // +4 / +6 targets of the moving axis (flags 1 x, 2 y)
        uint16_t flags = 0;                  // 4 moving in, 8 out, 0x200 on screen, 0x10/0x20 start reversed, 0x40/0x80 overshoot
        int8_t maxv = 0, vel = 0, acc = 0, back = 0, decel = 0;  // +0xa .. +0xe
    };
    struct HealthRec {  // 16-byte bar record 0x4ced78 + i*16 (0x462820)
        uint8_t bar = 0, panel = 0;          // sprite indices: clipped bar, its panel
        int16_t shown = -1;                  // displayed value (scaled), -1 = not yet shown
        uint16_t timer = 0;
        uint8_t flags = 0, dx = 0, dy = 0, max = 1, scale = 0, frame = 0;  // flags 1 right-aligned, 2 changed, 4 laid out
        int8_t link = -1;                    // +0xc: the other bar of the same panel (its flag 2 shows this one)
    };
    struct HudState {
        bool init = false;                   // 0x4d5ce8
        uint8_t cea = 0;                     // 0x4d5cea: shown S[4] mode bits (1 show all briefly, 2 panel)
        std::array<Slide, 11> slide{};
        std::array<HealthRec, 5> health{};   // Bugs, Taz, S[6] bar, Bugs S[0x6d], Taz S[0x6e]
        std::array<std::array<int16_t, 2>, 32> pos{};  // 0x4cebc0 sprite positions
        std::array<uint16_t, 15> timer{};    // model entity +0x82: on-screen countdown
        std::array<std::array<int16_t, 3>, 10> ang{}, spd{};  // 0x4d5e60 angles, 0x4cedc8 speeds per tick
        int16_t lives = -1;                  // 0x4d5d84 (s8)
        int32_t slot1 = 0xffff, slot2 = 0xffff;  // 0x4d5c88 / 0x4d5c80
        uint8_t focusId = 0xff;              // 0x4d5d80
        uint8_t callTicks = 0, callBusy = 0; // 0x4d5d82 / 0x4d5e54 call-friend icon frame switch
        uint8_t s7 = 0;                      // 0x4d5eb0
        int16_t iconX = -160, iconY = -104;  // 0x4cec28 / 0x4cec2a
        uint16_t watchTicks = 0, watchPrev = 0;  // 0x4d203e / 0x4d203c stopwatch
    } hud_;
    std::vector<std::array<int16_t, 2>> hudFrames_;  // HUD sheet frame sizes (w, h)
    std::vector<uint8_t> textBlock_;         // language block 0x4d5b88 (GD+0x93 = 0: English): u16 offsets + scripts
    struct TextJob { uint32_t uid; uint16_t script; };
    std::vector<TextJob> textQueue_;         // 0x4d5b20: deferred scripts of the lowest priority (0x4681c0)
    int16_t textQueuePrio_ = 32000;          // 0x4d5b28
    uint8_t blink_ = 0;                      // 0x4cf188: palette 9 = white for 16 runs, yellow for 14
    bool camNodeDone_ = false;
    bool pauseLatch_ = false;                // 0x4d02d0: Start must be released after a resume
    void hudSetup();                         // 0x462430
    void hudTick();                          // 0x463170
    bool slideStep(Slide& s);                // 0x461840
    void hudSetSprite(size_t i, uint8_t frame, const std::array<int16_t, 4>* clip = nullptr);  // 0x461a00
    void hudMove(size_t i, int16_t x, int16_t y);  // the prim move blocks: top-left to (x, y)
    void hudDigits(uint32_t value, size_t desc, const Slide& at);  // 0x461b20
    void hudCounter(Slide& s, size_t slot, size_t desc, uint32_t value, bool changed);  // 0x462d70
    void hudHealth(size_t h, Slide& s, int32_t value);  // 0x462820
    uint32_t textRun(TbActor* e, uint16_t script, int16_t prio);  // 0x466d50
    uint32_t textSpawn(uint16_t script, int16_t prio);  // 0x4680b0 with a new class-31 entity
    void textTick(TbActor& a);               // class 31 behaviour 0x450d00
    void textFlush();                        // 0x4681c0: the deferred scripts of the lowest priority, then reset
    void cameraNodes();                      // 0x405b50: a model camera node (TOD part flag 0x40) drives the view
};

} // namespace bb::game
