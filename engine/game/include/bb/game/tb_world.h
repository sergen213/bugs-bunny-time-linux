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

#include <algorithm>
#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
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
    int8_t sx = 0, sy = 0;  // +0xc / +0xd (right +, up +; digital 0x7f / -0x80)
};
// One tick of 0x461100 on the PC report (0x40e0c0 keyboard = type 0x41, 0x40e710 joystick = type 0x73): raw = the
// report bits; held = raw with □ ○ △ ✕ L1 L2 R1 R2 replaced by their remap words `remap` (GD+0x20.. of the pad record,
// 8 × u16 in that order; 0x460d80 fills them from preset GD+0x95 / +0x96 at every level load), not while paused
// (S[9]) on the digital branch; pressed = held & ~(prevHeld & edgeMask) (chunk 0x3a tag 0x3c @12). Stick: digital
// d-pad ±0x7f / −0x80; joystick (in.analog) = tbJoyAxis bytes, < 0x10 / > 200 set the d-pad bits (digital branch),
// else 0x460f80: dead zone 0x4d..0xb1, trunc((b − 0x4d) · 128 / 78) left / up, (b − 0xb2) · 128 / 78 right / down.
void tbPadTick(TbPad& p, const PadInput& in, uint16_t edgeMask, std::span<const uint8_t> remap, bool paused);
// 0x40e710 joystick axis byte from PadInput's 0..255 axis: DirectInput range −127..128 with dead zone 2000 (0x40ecd0;
// mapped as Wine's dinput joystick_map_axis does, the oracle's runtime), + 0x80 clamped to 0xff.
uint8_t tbJoyAxis(uint8_t axis);

struct TbActor {
    uint16_t id = 0;                         // tag 0x27 (+0x34): 2 Bugs, 3 Taz, template id for spawned entities
    uint16_t behaviour = 0;                  // tag 0x13 (+0x31) class: 14 = character 0x453de0, 16 director, 0x21 held item
    uint32_t uid = 0;                        // entity handle: 0 Bugs, 1 Taz, others >= 2
    uint32_t listPos = 0;                    // position in the entity list 0x4d4fd0 (file order; spawns appended)
    int slot = -1;                           // its pool slot (0x4d2704 index; -1: none / freed)
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
    // Rig swaps 0x469320 (V 0x80020000, distance LOD 0x45eee0): +0x98 asset links (tag 0x0f of the first chunk of its
    // template chain that has any), the model / rig lists +0x170 / +0x174 the swaps read (empty = links), the drawn
    // objects of the last swap (nullptr = model's), the LOD level +0x120 and its distances +0x121 / +0x122 (×256;
    // tag 0x39 @8 / @10).
    std::vector<uint16_t> links, modelList, rigList;
    const ActorModel* mesh = nullptr;
    uint8_t lod = 0, lod1 = 0, lod2 = 0;
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
    int16_t depth = 0;                       // +0x1a2: feet below the water surface (set by a water zone, f24 0x40)
    int16_t turnRate = 0;                    // +0x192
    uint16_t wander = 0;                     // +0x18e: ticks to the next random heading (flagsA 0x4000000)
    int16_t hover = 0, hoverSpan = 0;        // +0x194 hover height (flagsA 0x20000000; flagsC 0x10 pitch), +0x196
    int16_t hoverPhase = 0;                  // +0x198 hover bob phase
    int32_t groundPoly = -1;                 // +0x108
    int32_t groundSurf = -1;                 // +0x144: surface of the last floor contact (0x443f40; T 6), kept
    std::vector<std::array<int16_t, 6>> boxes;  // tag 0x61 collision box records (+0x78)
    int32_t boxIdx = 0;                      // +0x123 selected record (action 82)
    bool boxInstant = false;                 // +0x78 header 2: the next morph step jumps (S 82 flag 2)
    bool boxMorph = false;                   // +0x78 header 1: 0x450440 morphs +0x60.. toward record +0x123
    std::array<int16_t, 6> box{};            // +0x60.. actor box: min x, max y (feet), min z, max x, min y (top), max z
    uint16_t radius = 0;                     // +0x4a: distance of the farthest box corner (0x450440), 0 = no contacts
    int16_t inflate = 0;                     // +0x3e (tag 0x37 @0): box x/z inflation
    int16_t weight = 0;                      // +0x44 (tag 0x37 @2; W 0x87)
    int16_t f38 = 0, f3a = 0, f40 = 0;       // tag 0x37 @4 / @6 / @8 -> +0x38 / +0x3a / +0x40 (mover 0x443f40)
    int32_t attachPart = -1;                 // +0x84: part of the last TOD packet 0xa00 (attach point), -1 = none
    V3 attachPos;                            // +0x7c: world origin of that part (attack probe point)
    bool animLast = false;                   // +0x14e & 0x2000 of 0x469770: the last frame's hold expired this tick,
                                             // kept through the loop-restart tick (trace G; AnimState's flag is a tick late)
    int32_t parent = -1;                     // +0x90: uid of the entity this one is attached to
    int32_t parentPart = -1;                 // +0xe8: the parent's part it hangs on (-1 = parent root)
    // Its own coordinate record (+0xa0 built by 0x44e690 from +0xe4 -> +0x12c) while attached: position +0x13c and
    // yaw +0x136 in the parent's frame (+0x134 / +0x138 stay 0). Riders get them from the ride block of 0x453de0;
    // carried entities hang at 0. World position = parent record · local (+0xfc, rebuilt after each move).
    V3 local;
    int16_t localYaw = 0;
    bool rideRec = false;                    // +0x104 set (riders with flags10 0x20000): no +0xe8, +0xfc moves itself
    int16_t mass = 0;                        // +0x42: own weight (tag 0x37 @2) plus its riders' (0x44d910 / 0x44d870)
    int16_t turnDelta = 0;                   // +0x12a (flags0 8): the tick's turn of its record chain (heading of the
                                             // start-of-tick forward in the end frame); riders on +0x104 turn with it
    // Class 18 / 21 / 40 (0x44f7b0): +0x178 tag 0x1d (amplitude / bounds; classes other than 0/5/8/14/0x1e), +0x17a
    // phase / tilt accumulator, +0x184.. centre of the swing.
    int16_t arg = 0, phase = 0;
    V3 home;
    // Sprite sequences (+0x148.., 0x465dc0 classes 2 and flags0 0x200000, 0x453b40 class 4): last step, step, flags
    // (0x2000 the last step shown, 0x1000 looped, 0x100 ended, 0x10 held, the start mode's bits), hold reload /
    // countdown, the flipbook asset (tag 0x3e steps, chosen by the state's anim id = asset class), the record asset
    // (the first class-0x17 link), the record shown last (-1 none). texLink: tag 0x0b link id (+0x168), -1 none.
    struct SpriteSeq {
        uint16_t last = 0, frame = 0, flags = 0;
        uint8_t reload = 0, countdown = 0;
        int32_t asset = -1, records = -1, shown = -1;
    } spr;
    int32_t texLink = -1;
    // View list 0x45eee0: tag 0x39 @0 +0x6e flags (0x20 never listed, 0x40 always, 2 range test, 4/8 frustum, 0x10
    // kept while out of view), @2 +0x76 range, @4 +0x118 activation block; +0x50 sleep after a range cull.
    uint16_t view = 0, range = 0, actBlock = 0;
    int32_t sleep = 0;
    uint8_t zone = 0;                        // +0x32 world zone (tag 0x1f; spawns take the spawner's, children their
                                             // parent's every parent tick; the focus FindZone 0x460520 in 0x460630)
    bool inst = false;                       // +0x9c model instance made (classes 9 / 39 run their first tick once)
    bool active = false;                     // listed (ticked) this tick
    bool far = false;                        // unlisted only by the +0x76 range (incl. its sleep): Enhanced still draws it
    bool collidable = false;                 // in the contact list 0x4d2734 this tick (flags0 & 0x2000100 == 0x100)
    bool modelInst = false;                    // +0x9c model instance: built by its first anim tick (0x469210), freed by a release
    uint16_t gdBit = 0;                      // tag 0x65 (+0x48): GAME_DATA +500 bit; flags10 & 4: removed when set
    int16_t maxHp = 1;                       // 0x4d5ebc (flags1 0x200 boss bar S[1])
    int16_t hp = 0;                          // +0x3c health (tag 0x1e; players GD+0xa5/+0xa6)
    int16_t dmgIn = 0;                       // +0x66 damage pending
    int16_t hitTr = -1;                      // +0x5e transition the attacker's state asks for (-1 none)
    int32_t contact = -1;                    // +0x54 uid of the last touching / hit entity
    int32_t link = -1;                       // +0x1ac uid (V 0x100 target, selector 0xd)
    std::array<uint32_t, 3> trig{};          // +0x20/+0x24/+0x28 bits raised this tick (program triggers & +0x14..)
    uint16_t reg = 0;                        // +0x126 entity register: the u16 S[0x1fe] (0x4d269e) while its logic runs
    int16_t fxHit = 0;                       // +0x128 effect slot whose particle touched it (0x447ee0; R 0x48)
    bool pathSlot = false;                   // +0x11c holds a CameraTrackPath slot (TbWorld::pathAlloc)
    // +0x33 draw-order bias (tag 0x38 @0 + 0x10, S 127; 0x465710 / 0x464b20 add it to the ordering-table depth, the
    // shadow + 0x80), +0x36 shadow entity id (tag 0x45, U 17; 0 = none: 0x465130 lists it for 0x464480), +0x1a0 pole
    // bits (flagsA 0x200 climb; R 0x42). +0x124 / +0x125 shade and its target (0x464090 sets the target 0x10 and
    // +0x28 0x1000 on a floor surface with flag 0x80 for flags10 0x200; the draw 0x465130 steps the shade 2 toward it
    // per frame and draws the entity at (32 - shade) / 32).
    int8_t drawBias = 0;
    uint16_t shadowId = 0;
    uint16_t climb = 0;
    uint8_t shade = 0, shadeTarget = 0;
    // Class 31 overlay text (template 0x18, S30 0x4680b0, behaviour 0x450d00): script +0x17a (0x8000 = drawn by
    // the spawner this tick), display time +0x178 (0 = until removed), priority +0x42 (> 0: deferred, lowest wins).
    uint16_t textScript = 0;
    int16_t textTime = 0, textPrio = 0;
    const TbStateRec* rec() const { return state >= 0 ? &states[size_t(state)] : nullptr; }
    int padIndex() const { return (flags1 >> 11) & 1; }  // flags1 0x800 -> pad record 1
    bool drawn() const { return !(f20 & 0x2000) && !(rec() && (rec()->flagsC & 0x20000)); }  // 0x464480 skip
};

struct TbCamera {  // view 0 block B = 0x4d5120 (0x437800)
    V3 body, look, eye;                      // B+0x2c spring body P, B+0x10 look-at L, B+4 eye
    V3 prevBody, prevLook, prevEye;          // B+0x8c / +0x7c / +0x6c: restored at the start of every follow tick
    std::array<int32_t, 3> vel{};            // B+0xc0 body velocity (Q12)
    V3 orbit;                                // B+0xd0 / +0xd8: the L2/R2 orbit step for the next tick (x, z)
    int32_t dist = 1200;                     // 0x4d9ae0 spring rest length D (B+0x118 = 1200)
    // B+0x118..0x148 ('_' sub-ops; 0x437440 defaults): rest length, wall radius / push, aim and look heights, vertical
    // band, sphere radius, push / reset / guide speeds, velocity damping (Q12).
    int32_t rest = 1200, wallR = 500, wallPush = 20, aimH = 400, band = 50, lookH = 300, radius = 300;
    int32_t pushSpeed = 30, resetSpeed = 10, guideSpeed = 15, damp = 400;
    uint32_t flags = 0;                      // B+0xe0 follow flags (4 reset, 0x10 pushed, 0x20 guide, 0x80 single
                                             // focus, 0x100 two-player, 0x1000 at rest, 0x2000 check line of sight)
    int32_t blend = 0x1000;                  // 0x4d9acc
    V3 saved;                                // 0x4d9ae8: target when the players split
    int32_t acc = 0;                         // B+0xe4 L2/R2 accumulator
    int32_t pathId = 0;                      // B+0xbc camera path node's id (+8), 0 = none (T 13)
    int32_t ticks = 0;                       // B+0xe8: follow ticks since the last body contact / airborne focus
    V3 guideN, guidePt, n2;                  // B+0xec guide normal (Q12), B+0xf4 its polygon centre; scratch normal
    int32_t guidePoly = -1;                  // B+0x114
    int32_t splitTicks = 0;                  // 0x4d9ac8
    int lastFocus = -1;                      // 0x4d9ac4
    bool init = false;
};

class TbWorld {
public:
    // Throws FormatError without a chunk-0x35 collision mesh or without both characters. scene supplies the actor
    // models (addTbActors) for animation and the collision box (TOD packet 0x900); may lack them (no animation).
    // Entities without a scene model get their own (mesh, rig, anims) from their tag-0x0f links. gd: GAME_DATA
    // carried from the previous level (the reload 0x4014c0 keeps the global block); none = new game (3 lives, 6 health,
    // GD+0x93 = `language`, the registry language 0x639b48). GD+0x93 (> 6 = 0) picks the language section (0 EN → id 6,
    // 1 → 5, 2..6 → 7..11) and entry of the 7 × {off, size} speech (0x4b / 0x4c) and text (0x48) tags.
    static TbWorld load(const BzeFile& bze, const Scene& scene, const std::array<uint8_t, 0x300>* gd = nullptr,
                        uint8_t language = 0);
    // Models the world built itself from level data (entities without a scene actor, spawn templates), each once:
    // the mod kit exports them (bb::mods::exportModels extra). Mods already applied (bb::mods::applyToModel).
    std::vector<const ActorModel*> builtModels() const;
    // Level table 0x4cd998 (96 entries) index -> file stem ("ARCA01"); "" when out of range.
    static std::string_view levelName(int index);

    void tick(const PadInput& p1, const PadInput& p2 = {});
    void setTwoPlayer(bool on);              // GD+0xA4 0x10 (menu "2 Players" 0x4370c0)
    bool twoPlayer() const { return GD[0xa4] & 0x10; }  // GD+0xa4: 0x10 two players, 4 Taz focus, 8 / 0x20 R1 / L1 held
    // 0x4d236c (index into actors: 0 Bugs, 1 Taz): set by the view-list pass 0x45eee0 when it lists the player whose
    // id GD+0xa4 & 4 names (Taz 3 / Bugs 2), so an R1 switch (after the pass) reaches it on the next tick's pass.
    int focus() const { return focusPtr_; }
    bool callFriend(bool force = false);     // 0x45eb10; true if the partner was placed
    TbActor* spawn(uint16_t tmpl, V3 pos, int16_t yaw);  // 0x460b00 template entity (nullptr if not loaded)
    // Enters state `id` of actors[actor] like a transition target (anim restarts next tick; stateTicks kept).
    // For replays that start from a captured state. False if the actor has no such state.
    bool setState(int actor, uint16_t id);
    bool setState(TbActor& a, uint16_t id);
    TbActor* entity(int32_t uid);            // any entity by uid (nullptr if gone)
    const TbActor* entity(int32_t uid) const { return const_cast<TbWorld*>(this)->entity(uid); }
    TbActor* byListPos(uint32_t pos);        // a level entity by its position in the entity list (file order)
    const std::vector<TbActor*>& list() { return order(); }  // every entity in list (tick) order
    // 0x407080 for entity a (emitter uid + 1): mode 1 start, 2 release.
    void playSound(const TbActor& a, uint16_t id, uint16_t mode = 1) { sfx.request(int32_t(a.uid) + 1, id, mode, rng); }
    // 0x447ee0 (from the mover 0x4450c0 for flags10 & 0x4000): a particle of a slot with flags 0x400 within its size
    // + r of p raises +0x28 0x4000 (program trigger) and records the slot in +0x128.
    void fxTouch(TbActor& a, const V3& p, int32_t r);
    // Water surface y (PS1 y down) above entity a while it is in a water zone (f24 0x40), else none: the zone top
    // the depth +0x1a2 was measured from. f24 0x20 = shallow (depth < 200).
    std::optional<int32_t> waterSurface(const TbActor& a) const {
        return a.f24 & 0x40 ? std::optional<int32_t>(a.pos.y - a.depth) : std::nullopt;
    }
    // Surface rectangles of the water zones (flags +0xc 0x1000): the 4 corners (PS1) of [0, extX] x [0, extZ] taken
    // back through the zone rotation 0x448780 tests in, at the surface y = origin + height (the depth's zero).
    std::vector<std::array<V3, 4>> waterZoneTops() const;

    TbCollision col;
    std::array<TbActor, 2> actors;           // [0] Bugs (id 2), [1] Taz (id 3)
    std::list<TbActor> entities;             // every other entity: level instances, then V 0x10 spawns
    // Entity pool 0x4d2704 (0x1b0-byte slots, +4 bit 0 = free): the slot count (tag 0x3d u16 @6 + 5, 0x4d270e)
    // and which slots are taken. The view entity (0x460a40), the world entity + 15 (0x462030) take 0..16, then the
    // loader one per chunk 0x07 / 0x08 / 0x0a / 0x63 in file order (templates keep theirs); 0x460b00 takes the
    // lowest free slot below the count, the destroy 0x45ecd0 frees it.
    int poolCap = 1 << 30;
    std::vector<bool> slotUsed;
    int poolUsed() const { return int(std::ranges::count(slotUsed, true)); }
    int poolFirstFree() const { return int(std::ranges::find(slotUsed, false) - slotUsed.begin()); }
    std::array<TbPad, 2> pads;
    uint16_t edgeMask = 0;                   // 0x4d26c0
    uint16_t cooldown = 50;                  // 0x4ceb08 (50 at level start in the runtime trace; counts to 45 after a call)
    uint8_t airTicks = 0;                    // 0x4d5eb6
    int camFocus = 0;                        // 0x4d5028: 0x4d236c latched before 0x45eee0 (the camera's focus)
    int focusPtr_ = 0;                       // 0x4d236c (focus())
    TbCamera cam;
    int32_t camTarget = -1;                  // B+0x44 (0x4d5164): director uid a class-16 tick hands the camera (0x450bc0)
    int32_t loadLookH = 0;                   // B+0x134 at the level-load placement: 0 on the session's first level, then 300
    bool loadAtOrigin = false;               // 0x4d236c at the load placement is a template (pos 0, yaw 0), not a player
    std::array<uint8_t, 0x400> S{};          // logic byte variables 0x4d24a0
    std::array<uint8_t, 0x300> GD{};         // GAME_DATA image: +0xa0 lives, +0xa2 down flags, +0xa5/+0xa6 health,
                                             // +0xac/+0xad ability bits
    std::array<int16_t, 4> checkpoint{};     // 0x4d26e0..ec respawn point x y z yaw (V 2/8/0x2000)
    int16_t invuln = 0;                      // 0x4d271c: S 53 (0x431a40) sets 30, nothing ever lowers it (carry it over levels)
    // Camera shake 0x439650 (end of every 0x439700 tick; S 41 sets it, S 49 by the distance of the look point): look x
    // += sin(phase) · amp >> 11 (off), phase += speed (B+0x2b4 = X · 11), amp −3 per tick once phase >= 0x800.
    struct CamShake { int16_t speed = 0, amp = 0, phase = 0, off = 0; } camShake;
    bool quitToMenu = false;                 // S 119 (0x432cc0): state 0x4d03d0 = 11, the game leaves for the front end
    int32_t saveSlot = -1;                   // 0x48840c: the save slot the front end chose (-1 none; R 0x52)
    // Draw distances 0x4d5954 / 0x4d237a (chunk 0x3a tag 0x3c @0 / @10, S 109 / S 117): the world's near slice and
    // the entities' cap (0x465710 / 0x465af0, the far slice 0x4d5980 = Scene::tbFarClip), the render context +0x1a.
    uint16_t nearClip = 0, viewDist2 = 0;
    bool lookMode() const { return lookMode_; }  // 0x4d4fd4 & 2: the L2 + R2 look view (0x439700) this tick
    std::array<int16_t, 2> lookStick() const { return {lookUp_, lookTurn_}; }  // 0x4d9a98 / 0x4d9a9a
    // One logic item run on entity a: a condition's test (cur = true) or an action's effect; the result. game.tb's data
    // scan feeds it every 'R' / 'S' / 'U' item of the 58 levels and asserts `unsupported` stays empty.
    uint32_t runItem(TbActor& a, TbLogicItem& it);
    // Cheat codes, process-wide in the exe (carry them over levels): 0x4d097c keys of "RULETHEEARTH" typed, 0x4d095c
    // armed, 0x4d9a68 last raw P1 word, the 11 pad sequences' progress (table 0x4ce8e8).
    struct Cheats { int typed = 0; bool on = false; int16_t last = 0; std::array<uint8_t, 11> progress{}; } cheats;
    void cheatKeys(const std::array<bool, 256>& dikHeld);  // 0x40fbc0, every input poll (DirectInput key states)
    std::array<uint8_t, 2> flash{};          // 0x4d2058/9 Bugs/Taz hit flash (+0x28 0x200 while < 0x3c)
    std::array<uint32_t, 2> deathEvent{};    // 0x4d2720 / 0x4d5948 death transition overrides (V 0x80002000)
    std::array<uint32_t, 2> deathEvent2{};   // 0x4d5944 / 0x4d273c (V 0x80001000)
    bool restart = false;                    // 0x4d593a: level reload requested (loads GD+0 = nextLevel, else the same level)
    int32_t nextLevel = -1;                  // GD+0 written by V 0x1000 / game over (level index; 5 = RETRY), -1 = unchanged
    int32_t lastSpawn = -1;                  // 0x4d5eb8 uid (V 0x200000; V 0x80000001 teleports it)
    // '_' op 0x15 view block B+0x3c flags, B+0x48 eye point (flag 0x400), B+0x58 look point (0x800); 0x439700 places
    // the camera at them (0x20 eases by 1/4; one-shot 0x10000 / 8 drop the eye / look point after one tick).
    struct CamScript { uint32_t flags = 0; V3 eye, look; } camScript;
    // CRT rand 0x46afe0 (seed 0x4cf26c, srand(1) once at start-up 0x401000): logic 'W' chances, action 7/8, spawn
    // yaw. The original shares it with every entity, sound and effect of the level; replays set seed per tick.
    MsvcRand rng;
    int32_t stickMag = 0;                    // 0x4d5ec0: smoothed stick magnitude of the stick steering branches
    // Sound voices (sfx.h, TB rules): the mixer pass 0x42ae60 runs at the top of each tick, before the entities
    // (requests start one tick later); emitter = entity uid + 1. Oracle replays set sfx.device = false.
    Sfx sfx;
    // Effects drawn this tick (tb_fx.cpp, in the exe's draw order): particle slots 0x453350 (sprites / gouraud lines)
    // and 0x452e80 (flat quads on a water surface), the weather streaks 0x451370. PS1 world units / axes.
    struct FxDraw {
        enum Kind : uint8_t { Sprite, Ground, Line } kind = Sprite;
        // Sprite: view-plane quad at pos (PS1 screen offsets of 0x453350 converted to world units: corner k =
        // pos + size · (right · cx[k] + down · cy[k]) with (cx, cy) = (0.625c, 1.6s), (-s, c), (s, -c), (-0.625c,
        // -1.6s), c / s = cos / sin of angle; uv (0,0) (1,0) (0,1) (1,1)). Ground: quad[] corners in that uv order.
        // Line: pos -> to, then to moves (dx, dy) PS1 screen units (x / y in the 512 x 240 space) at its depth.
        V3 pos, to;
        std::array<V3, 4> quad{};
        int16_t size = 0, angle = 0, dx = 0, dy = 0;
        uint16_t asset = 0;                  // Scene::sprites id (class-0x17 sprite sequence)
        uint8_t frame = 0;
        std::array<uint8_t, 3> c0{}, c1{};   // Line colours at pos / to
    };
    std::vector<FxDraw> fx;
    // Blob shadows of this tick (0x464480 over the draw list, at most 40): the shadow entity's role-12 model, placed by
    // 0x464090 at the ground below (PS1 units / axes), rotated (x, y, z) onto the floor normal, x / z scaled by
    // scale / 4096. The app adds 0x464480's view-space shift (5 down, 9 away) and draws it as a model.
    struct Shadow { const ActorModel* model = nullptr; V3 pos; std::array<int16_t, 3> rot{}; int32_t scale = 0x1000; uint32_t uid = 0; };
    std::vector<Shadow> shadows;
    // ---- tb_fx.cpp: particle emitters 0x4d6ae0 (20 × 0x9c B, defaults 0x451e40 at level load; fields S 118 ->
    // 0x452060), the shared pool of 120 particles (0x4d5fa0, 0x18 B, free list 0x4d5ee0) and the weather 0x451370.
    struct FxPart {                          // 0x18-byte particle
        std::array<int16_t, 3> pos{}, vel{}; // +0 (slot-local with flags 0x2000), +8 velocity (pos += vel >> 5)
        uint8_t frame = 0;                   // +6 sprite record
        int8_t age = 0;                      // +7 (< 0: still delayed)
        int16_t size = 0, bright = 0;        // +0xe, +0x14
        uint8_t hold = 0, cycle = 0;         // +0x16 / +0x17 animation counters
    };
    struct FxSlot {                          // offsets in the 0x9c record; S 118 field number in brackets
        std::array<int16_t, 3> accel{};      // +0x00 [0x10] added to the velocity every tick
        uint16_t flags = 0x40;               // +0x06 [10]
        std::array<int16_t, 3> offset{}, off{};  // +0x10 [0x14] emitter offset, +0x18 as rotated at the last emission
        std::array<uint8_t, 3> rgb1{100, 100, 255}, rgb2{0, 0, 0x32};  // +0x20 [1] / +0x24 [2] line colours
        // Keyed curves (angle [0xd] +0x28, size [0xe] +0x34, brightness [0xf] +0x40): values at age 0 / key age
        // (+0x32 / +0x3e / +0x4a) / life, and the per-tick rates before / after the key.
        struct Curve { std::array<int16_t, 3> v{}; int8_t at = 1; int16_t r0 = 0, r1 = 0; };
        Curve ang, size{{100, 100, 100}}, bright{{0x830, 0x830, 0x830}};
        int16_t speed = 600, drag = 100, spread = 500;  // +0x4c [5], +0x4e [0xc], +0x50 [6]
        uint8_t count = 10, delay = 10, life = 40, f55 = 0;  // +0x52 [4], +0x53 [8], +0x54 [3], +0x55 [0x15]
        int16_t sizeSpread = 0;              // +0x56 [0x11]
        int32_t owner = -1;                  // +0x60 [7] entity uid owning the slot (0x451e10)
        int32_t emitter = -1;                // +0x64 entity of the last emission (0x200 velocity, 0x4000 surface)
        int32_t coordUid = -1, coordPart = -1;  // +0x68 [9] coordinate: that entity's part (-1 = its root)
        int32_t sprite = -1;                 // +0x8c [0xb] Scene::sprites id, -1 none
        uint8_t frames = 0, hold = 1;        // +0x90 (sprite record 0 byte 2), +0x91
        int16_t f92 = 100;                   // +0x92 [0x13]
        uint8_t frame = 0, sub = 0xff;       // +0x98 [0x16] record of new particles, +0x99 [0x17] slot on the surface
        int16_t surface = 0;                 // +0x9a water surface y (emitter y - depth)
        std::vector<FxPart> parts;           // list +0x58..+0x5c, oldest first
    };
    std::array<FxSlot, 20> fx_{};           // public with fxTick for oracle replays (game.tb fx)
    size_t fxFree_ = 120;                    // particles left on the free list 0x4d5ee0
    void fxTick();                           // 0x451370 weather, 0x453350 particles (tick() runs it after the entities)
    int music = 0;                           // logic action 126 value n (Track(n-1).xa), first setter in the level
    int musicTrack = 0;                      // 0x4cca38: what PlayMusic streams (follows `music` unless paused)
    bool musicPaused = false;                // S[9] at the 0x4014c0 head: the stream stopped in place (0x429180)
    // Speech XA 0x42a030 (every tick after the sound mix): S[0x22] 8 (S62 X > 1: clip X - 0x66) -> 2 -> 4 playing -> 0,
    // 1 = held silent (S62 1); a state with mode 0x80 (0x469770) asks for clip anim + 0x9a itself (2, or again once 0
    // while it lasts). The music stays stopped while S[0x22] != 0 (0x4291e0) and starts over afterwards. With audio the
    // app plays Datas/SPEECH/<GD+0x93 as %.2d>/Speech<clip>.xa (speechAudio) and reports its end (speechEnded); without
    // (or no such file) the exe times every clip from table 0x481678: 6000 / 44100 s.
    uint8_t speechClip = 0;                  // 0x63b73c
    bool speechAudio = false, speechEnded = false;
    bool speechPlaying() const { return S[0x22] == 4 && !speechTimer_; }
    int dt = 0;                              // 0x4d26c4: 0 on the first tick after load, then 2
    uint32_t ticks = 0;
    // View list frustum (0x45eee0): half-angle tangents of the render view. The exe's planes span tan 0x8a0/0xd7a
    // (0.64) across and ~0.48 vertically (51.3°); the app sets its wider Enhanced FOV here so edge entities stay listed.
    double viewTanX = 0.64, viewTanY = 0.48;
    // Debug free camera {eye, look} (app F1 / --camera): the view list and the portal walk cull from it instead of the
    // game camera, so what the free camera sees is listed and drawn.
    std::optional<std::array<V3, 2>> freeView;
    // HUD key names (text op 0x14): per player the keys of □ ○ △ ✕ L1 L2 R1 R2 select start ← → ↑ ↓ F4, lower-cased
    // GetKeyNameText. Defaults = the config.pc defaults 0x415fd0; the app writes its live maps (key capture).
    std::array<std::array<std::string, 15>, 2> keyNames{
        {{"r", "q", "w", "e", "a", "d", "s", "f", "tab", "esc", "left", "right", "up", "down", "f4"},
         {"[", "i", "o", "p", "l", ";", "k", "'", "ctrl", "right shift", "keypad 4", "keypad 6", "keypad 8", "keypad 5", "f4"}}};
    int lastCallFriend = 0;                  // 1 placed, -1 refused/failed, 0 not requested (this tick)
    std::set<std::string> unsupported;       // logic conditions/actions met but not interpreted
    // Texture-link alias table 0x4d038c: link id -> texture word (sprite record +10) last written by a link entity
    // (0x465c80: class 2 / flags0 0x20 sprite entities). Absent = not written (the renderer keeps its clock).
    std::map<uint16_t, uint16_t> textureLinks;
    // World zones (chunk 0x20, tag 0x21 id = slot of 0x4d5040, 0..49): the zone model's octree root box (FindZone
    // 0x460520: ±5) and its portals (model +0xc extra block, runtime +0x44: u32 count, 0x24-byte records, byte @3 =
    // zone seen through it, s16 x/y/z ×5 @4). Visible-zone table 0x4d56a0 (view 0): entry 0 = the camera zone, the
    // others the zones the render's portal walk 0x465710 / 0x406840 found, with their clamped screen rectangles
    // (640×480 viewport, y up); zone -1 = free. tick() rebuilds it at the render point (portalWalk); the next tick's
    // view list reads it (view flag 1). A fresh level starts with the BSS table (16 × zone 0).
    struct VisZone { int16_t zone = 0; int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
    std::array<VisZone, 16> visZones{};
    uint8_t camZone = 0;                     // world entity 0x4d5924 +0x32 (0x460630: FindZone of the camera body)
    int findZone(const V3& p) const;         // 0x460520: first zone slot whose root box ±5 holds p, -1 = none
    void portalWalk();                       // 0x465710 for the world entity (class 7): the table from cam.eye/look

    // ---- tb_hud.cpp: screen fade, overlay text, HUD (draw data in the LiT HUD / overlay conventions) ----
    // Fade 0x464e90 (from black) / 0x464ec0 (to black), advanced by 0x464ef0 once per rendered tick (tick() ends
    // with it): S51 X=0 fadeIn(1, 0x100), X=1 fadeOut(2, 0x100) (holds black); S52 flash fadeIn(0x41, 0x400). The
    // level change (0x401910) is fadeOut(2, 0x100) + 16 world ticks, after the load (0x4018a0) fadeIn(1, 200).
    Fade fade;
    int8_t bars = 0;                         // GD+0x9f letterbox as drawn this tick (0x464c30): |bars|·h/120 px top and bottom
    void fadeIn(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0, fade.dark = 0x1000; }
    void fadeOut(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0x1000, fade.dark = 0; }
    void fadeTick();
    // 0x4014c0 on the frame after a reload request (0x4d593a 1): unpause (S[9], S[0x22], latch 0x4d02d0 = 0, so the
    // fade's 16 world ticks run unpaused; oracle w13_quit_trace.csv), then 0x401910's fadeOut(2, 0x100) + one step.
    void reloadStart() { S[9] = S[0x22] = 0, pauseLatch_ = false, fadeOut(2, 0x100), fadeTick(); }
    // 0x4463e0 (tb_mover.cpp, on the mover's bodies): the nearest of the world polygon hit (col.query) and, with query flag 1,
    // the shapes of the listed entities matching `masks` (flags0 / flags1 / flags10 any of [0..2], none of [3..5]) on
    // the ray s -> s + d. nullopt = free; nullptr = a polygon; else the entity whose shape takes it. A hit shortens d
    // to the hit (the exe writes it back into the query). excludeSelf = false: no self (0x450650); `uncollide` lose
    // flags0 0x100 for the query.
    std::optional<TbActor*> rayHit(TbActor& self, const V3& s, V3& d, int32_t radius, uint32_t q,
                                   const std::array<uint32_t, 6>& masks = {0, 0, 8, 0, 0, 0}, bool excludeSelf = true,
                                   std::initializer_list<const TbActor*> uncollide = {}, std::array<int16_t, 3>* n = nullptr);
    std::array<uint8_t, 4> fadeOverlay() const;  // RGB to multiply the frame by (fade / S 50 flash); alpha 0 = none
    // Overlay text (0x466d50 over the level's language block, chunk 0x48): glyph sprites 0x4d5af4 and box rects
    // 0x4d5af8 {x, y, w, h}, 512x240 space centred; both lists restart at the top of each tick (0x45ee90).
    std::vector<OverlayGlyph> glyphs;
    std::vector<std::array<int16_t, 4>> textBoxes;
    // HUD (class 6 entity 0x463170): 15 template models (0x4d5ca0, table 0x4ced18; x/y = view translation = screen
    // offset, z depth, as LiT's 0x408910) and 32 HUD-sheet sprites (0x4d5d00, POLY_FT4 of Scene::hudSheet frames in
    // the 512x240 space: x, y top-left, w x h texels from u, v).
    struct HudSprite { uint8_t frame = 0; bool shown = false; int16_t x = 0, y = 0, w = 0, h = 0, u = 0, v = 0; };
    std::array<HudItem, 15> hud{};
    std::array<int16_t, 2> panelPos(size_t i) const { return panels_[i].cur; }  // text panel i origin (0x4d59a0)
    std::array<HudSprite, 32> hudSprites{};
    uint32_t textStatus = 0;                 // 0x4d9b40: status of the last S30 / R 0x22 text (0x4680b0)

private:
    int16_t lookYaw_ = 0;                    // 0x4d5972: the look view's yaw (stale afterwards: the called partner's)
    int16_t lookPitch_ = 0;                  // 0x4d5970: the look view's pitch (look-mode spawns)
    int16_t lookUp_ = 0, lookTurn_ = 0;      // 0x4d9a98 / 0x4d9a9a: stick pitch (follow: -200) / yaw offset
    uint8_t lookLock_ = 0;                   // 0x4d2714: 1 (S 86, in no PC level) forbids the look view; 3 after one
    bool lookMode_ = false;                  // 0x4d4fd4 & 2
    int8_t speedMul_ = 0;                    // 0x4d26b8 (BSS 0; S 88): S-variable state speeds (0x2000) × this
    void cameraFollow();                     // 0x439700 without the shake
    bool lookView(TbActor& P);               // 0x439700's look branch; false = follow (leaving the look view first)
    void hideCarried(TbActor& a);            // look view: +0x24 |= 4 when the focus carries a (any depth)
    std::map<uint16_t, TbActor> templates_;  // spawnable templates (chunk 0x08) referenced by V 0x10 actions
    std::map<uint16_t, TbBlock> globalBlocks_;  // blocks >= 10000 (global list 0x4d5984)
    // Animation-asset logic (chunk 0x22 'M' blocks, file order), run by the player 0x469770 on frame steps: the
    // footstep / jump / spin sounds ('`' items) live here. Keyed by the TOD each model parsed for that asset.
    std::map<const Tod*, std::vector<TbBlock>> animLogic_;
    // Sprite-sequence assets (chunk 0x22 by asset id): class, tag-0x3f records (@+6 flags, @+10 texture word, @+0xc
    // w, @+0xe h), tag-0x3e flipbook steps {record, ticks}, and the asset's 'M' blocks (+0x14, run on each step).
    struct SpriteAsset {
        uint16_t role = 0;
        std::vector<std::array<uint16_t, 4>> recs;
        std::vector<std::array<uint8_t, 2>> steps;
        std::vector<TbBlock> logic;
    };
    std::map<uint16_t, SpriteAsset> spriteAssets_;
    uint32_t spriteSeq(TbActor& a, uint16_t anim, uint16_t mode);  // 0x465dc0 (0x200 running, 0x100 ended, 0x400 none)
    void spriteShow(TbActor& a, uint16_t rec);  // 0x465b80 / 0x465c80: record rec drawn, or written to its link
    void linkTick(TbActor& a);               // class 2 0x4599e0
    void spriteTick(TbActor& a);             // class 4 0x453b40
    void hungTick(TbActor& a);               // class 33 0x459b10
    void projTick(TbActor& a);               // class 30 0x44d1f0 (template 151, the look-view shot)
    struct FxSprite { int32_t asset = -1; uint8_t frames = 0; };
    std::map<uint16_t, FxSprite> fxSprites_; // entity id -> its first class-0x17 sprite link (0x451d70)
    struct Drop { std::array<int16_t, 3> p{}; bool fresh = true; std::array<int16_t, 2> prev{}, cur{}; };
    struct Weather {                         // 0x4d7710.. (defaults 0x4511b0 at level load)
        uint32_t flags = 0;                  // 0x4d7734: 1 set up, 2 on, 4 fading out
        uint16_t count = 0x78, target = 0x78, max = 0x78;  // 0x4d7714 / 0x4d7730 / 0x4d7732
        std::array<int16_t, 3> wind{2, 5, 2};  // 0x4d7738..
        int16_t clamp = 7;                   // 0x4d7716 streak length limit (screen units)
        V3 lastLook;                         // 0x4d7720..
        std::vector<Drop> drops;
    } weather_;
    void fxField(TbActor& a, uint16_t x, uint16_t y);  // S 118 -> 0x452060
    void fxEmit(TbActor& a, uint32_t p1, uint16_t slot);  // V 0x80200000 -> 0x452ab0
    void fxClear(int16_t slot);              // S 138 -> 0x4529d0
    void fxWeather(uint16_t x, uint16_t y);  // S 110 -> 0x432bb0
    FxPart* fxAlloc(size_t slot);            // 0x4528f0
    // 0x405ab0: world matrix of entity a's part (its root for -1; the slot coordinate +0x68, '^' 0x2000 spawns).
    Mat coordMat(const TbActor* a, int32_t part) const;
    bool hungRoot(const TbActor& a, Mat& M) const;  // a hung entity's root coordinate through its parent's chain
    void fxRates(FxSlot& s, int which) const;  // the keyed-curve rates (0 angle, 1 size, 2 brightness, -1 all)
    // Rig swaps 0x469320: (model asset role 13 + k, rig asset role 1 + k) -> objects / meshes / slots of the model
    // and the rig's TOD (frame 0 rebinds the parts' objects), built at load for every pair a swap can ask for.
    std::map<std::pair<uint16_t, uint16_t>, std::shared_ptr<const ActorModel>> rigMeshes_;
    std::map<uint16_t, uint16_t> assetRole_;   // chunk 0x22 asset id -> role / class (tag 0x27 @0)
    std::map<uint16_t, std::vector<uint16_t>> linksById_;  // first chunk with that entity id -> its links (0x4d26d0)
    std::map<uint16_t, std::shared_ptr<const ActorModel>> shadowModels_;  // entity id -> first role-12 link's model
    void shadowPass();                       // 0x465130 shade steps + 0x464480 / 0x464090 (tb_shadow.cpp)
    bool shadowPlace(TbActor& a, Shadow& s); // 0x464090
    // 0x469320(a, model source, rig source, k): lists default to a's current +0x170 / +0x174.
    void rigSwap(TbActor& a, const std::vector<uint16_t>* models, const std::vector<uint16_t>* rigs, uint8_t k);
    void lodTick(TbActor& a, uint32_t dist);  // 0x45eee0 view flag 0x400: LOD k by camera distance (hysteresis 100)
    void push(TbActor& a, TbActor& c, std::array<int32_t, 3>& disp, const std::array<int32_t, 3>& target, bool probed);  // 0x453de0 push block
    void ride(TbActor& a, TbActor* floor, std::array<int32_t, 3>& disp);  // 0x453de0 ride block (the mover's floor entity)
    Mat recordMat(const TbActor& a) const;      // 0x44e5d0: Rx·Rz (flags10 0x100000: Rz·Rx), Ry, scaled by +0xec, at +0xfc
    V3 hangPos(const TbActor& a, const TbActor& par, int16_t* yaw = nullptr) const;  // the +0xe8 chain on a.local
    // Trigger zones (chunk 0x09 aux items, list 0x4d2760, GAMEPLAY_TB.md §10): corner origin (+0x10), rotation (+0x18;
    // the test uses 0x1000 - each angle), x / z extents (+0x20 / +0x24) and the height below the origin (+0x22, < 0),
    // flags +0x8 (1: unrotated) / +0xc (0x1000 water); blocks keyed by entity id (10 any player, 8 Bugs, 9 Taz), in the
    // list order of +0x28 (pushed at the head: reverse file order).
    struct Zone {
        V3 origin;
        std::array<int16_t, 3> rot{};
        int16_t extX = 0, height = 0, extZ = 0;
        uint32_t flags8 = 0, flagsC = 0;
        std::vector<std::pair<uint16_t, TbBlock>> blocks;
    };
    std::vector<Zone> zones_;
    struct ZonePortal { int16_t zone = 0; std::array<V3, 5> pt{}; };
    struct ZoneModel { bool present = false; std::array<int16_t, 6> box{}; std::vector<ZonePortal> portals; };
    std::array<ZoneModel, 50> zoneModels_{};
    uint32_t zoneTick(TbActor& a);           // 0x44f550 (box test 0x448780): the zones' blocks for this entity
    std::vector<TbActor*> list_;             // entity list 0x4d4fd0 order: players at their file position
    V3 prevEye_;                             // 0x4d2750: camera position of the previous list build
    const TbWorld* listOwner_ = nullptr;     // list_ is rebuilt when stale (spawn/erase) or the world was moved
    uint32_t nextUid_ = 2, nextListPos_ = 0;
    TbActor* regOwner_ = nullptr;            // entity whose register +0x126 S[0x1fe] holds (runItems)
    std::vector<TbActor*>& order();
    void viewList();                         // 0x45eee0 list build: activation (+0x6e/+0x76/+0x118/+0x50)
    void cheatPad();                         // 0x43bce0 pad cheat sequences (0x45eee0 tail)
    void classTick(TbActor& a);              // dispatch table 0x4cea60 by +0x31
    bool hitIntake(TbActor& a);              // 0x453de0 head: pending hit -> health, hurt / death event
    // 0x453de0 contact loop (+0x54, 0x40/0x80, pole grab); true = no floor
    bool contacts(TbActor& a, const std::vector<TbActor*>& touched, std::array<int32_t, 3>& disp);
    void attackHits(TbActor& a, const std::vector<TbActor*>& touched);  // 0x453de0 attack probes 0x447fd0
    std::vector<TbActor*> probe(TbActor& a, const V3& p, uint32_t maskIn, size_t max);  // 0x447fd0
    static V3 aheadPoint(const TbActor& a);             // 100 up, 100 past the box front (0x453de0 probes)
    TbActor* pickAhead(TbActor& a, const V3& p);       // 0x44e7d0: the carriable entity at p, if in clear sight
    bool dropClamp(TbActor& parent, TbActor& child);    // 0x450650: a dropped child kept out of walls
    void releaseNested(TbActor& c, TbActor& thrower);   // 0x44e6d0: c's own children let go first
    void carryRelease(TbActor& a);                      // 0x453de0 release block (flagsA 0x40 / flagsB 0x40000)
    void carryGrab(TbActor& a);                         // 0x453de0 grab block (flagsA 0x20)
    void playerDown(TbActor& a);             // 0x44ea50 dead player: partner takes over / lives / checkpoint
    void runNear(TbActor& a, uint32_t block, uint32_t radius);  // V 8 / V 0x80000010: block on flags0 0x8000 ents
    void teleport(TbActor& t, V3 p);          // V 4/1/0x100000 placement (clears ground, detaches)
    void actorStep(TbActor& a);              // class 14 0x453de0
    // 0x453de0 steering block (tb_steer.cpp): turns the entity and builds its local move s (x side, y up-, z fwd).
    // false = 0x453de0 returns here (flagsB 0x100 without a CameraTrackPath slot).
    bool steer(TbActor& a, const TbStateRec*& r, bool par, std::array<int32_t, 3>& s);
    void platformTick(TbActor& a, uint32_t mode);  // 0x44f7b0 (classes 18 mode 1, 21 mode 0x10, 40 mode 0x1000)
    void blockTick(TbActor& a);              // class 5 0x4516e0 pushable block
    Mat chainRot(const TbActor& a) const;    // rotation of a's record chain (0x404a50 products, root last)
    bool dispatch(TbActor& a, uint32_t ev);  // 0x44d990; false when no transition applied
    void camCap(TbActor& a, const TbStateRec& r);  // flagsB 0x40 in the flagsC 0x80 branch
    TbBlock* findBlock(TbActor& a, uint16_t id);
    void boxStep(TbActor& a);                // 0x450440(a, 1) box morph toward record +0x123
    uint32_t runBlock(TbActor& a, uint16_t block);
    uint32_t runItems(TbActor& a, TbBlock& b);
    uint32_t runProgram(TbActor& a, uint32_t c20, uint32_t c24 = 0, uint32_t c28 = 0);  // 0x45ea90 on +0x2c, then clear
    bool cond(TbActor& a, TbLogicItem& it, bool cur);
    bool condW(TbActor& a, const TbLogicItem& it);
    uint32_t action(TbActor& a, const TbLogicItem& it);
    uint32_t actionV(TbActor& a, const TbLogicItem& it);
    TbActor* lookup(uint32_t sel, TbActor& self);  // 0x45a120
    bool inZone(const TbActor& a, const Zone& z) const;  // 0x448780 test (zone frame, height band)
    void attach(TbActor& parent, TbActor& child, bool keepWorld = false);  // V 0x40000 (keepWorld: V 0x840000)
    void link(TbActor& parent, TbActor& child);        // 0x44d910 (+0x90, mass +0x42 up the chain)
    void detach(TbActor& child);                        // 0x44d870
    V3 partWorld(const TbActor& a, int32_t part) const;  // 0x44e560 chain: world origin of a part (-1 = root)
    V3 partLocal(const TbActor& a, int32_t part, V3 v = {}) const;  // a point of the part in the root coordinate (s16 chain)
    V3 chainPoint(const TbActor& a, int32_t part, V3 v, int depth = 0) const;  // 0x44e560: on up to the world
    template <class F> void forEachEntity(F&& f) {  // over a copy: callbacks may spawn (the list is then rebuilt)
        const std::vector<TbActor*> all = order();
        for (TbActor* a : all) f(*a);
    }
    // Mover 0x4450c0 (tb_mover.cpp, tb_sweep.cpp): the exe's collision code on bodies of the entities. State
    // record r (none for class 5), attached displacement `follow` (request +0x78). Returns the displacement (request
    // +0x50, not applied) and the entity stood on (+0x10); appends the actor contacts (+0x14, count +0x60).
    struct MoveOut { std::array<int32_t, 3> disp{}; TbActor* floor = nullptr; };
    MoveOut move(TbActor& a, const std::array<int32_t, 3>& target, std::vector<TbActor*>& touched, const TbStateRec* r,
                 const V3& follow = {});
    void moverStage(TbActor& a);             // the mover's state for this world + the entity bodies (move / rayHit / camera)
    bool cameraSweep(TbActor& stage, const V3& s, V3& m, int32_t r, bool look);  // 0x440360; m -> travelled delta
    void moverTail(TbActor& a, const V3& pos0, const std::array<int16_t, 3>& ang0, const std::array<int16_t, 6>& box0,
                   bool hungStale = true);  // a class tail: 0x44e5d0 + shape cache
    void moverForget(TbActor& a);            // 0x45ed.. removal: the shape cache record back to the free list
    int takeSlot();                          // 0x4608c0 / 0x460b00: the lowest free pool slot below poolCap, -1 if none
    void freeSlot(TbActor& a) { if (a.slot >= 0) slotUsed[size_t(a.slot)] = false, a.slot = -1; }
    int16_t shapeRecs_ = 0;                  // 0x4d7758: actor shape cache records (chunk 0x3a tag 0x3d @0x1c)
    // CameraTrackPath pool (0x4d26f0, 0x4d205a = tag 0x3d @0x1e slots; 5 in ARR01 / AZR01 / TRR01, else 0):
    // the slots left. 0x43bc00 takes one for steering flagsB 0x100 and U 20; removal (0x45ecd0 -> 0x43bc80) frees it.
    int pathFree_ = 0;
    bool pathAlloc(TbActor& a);              // +0x11c set or a slot taken; false = none left
    int16_t viewCap_ = INT16_MAX, collCap_ = INT16_MAX;  // 0x4d5978 / 0x4d26c8: view / collision list sizes (tag 0x3d @0xc / @0x20)
    uint64_t moverGen_ = 0;                  // mover state owner check (a moved / copied world rebuilds its bodies)
    void switchTick();
    void pauseTick();                        // 0x4014c0 head: Start -> S[9] = 1 + pause menu template 0x1d
    void speechTick();                       // 0x42a030
    bool speechAnim(uint8_t clip);           // 0x469770 mode 0x80: request `clip`; true = started (0x200)
    void cameraTick();
    void cameraPlace(const TbActor& t);      // 0x437180
    void cameraInit(const TbActor& t);       // 0x439570
    void cameraReset(const TbActor& t);      // 0x437260
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
    // Text panels 0x4d59a0 (20 slide records = op 0xe text origins) with states 0x4d5bc0 (0x20 idle, 0x80 start record
    // 0x4d5bc1 of table 0x4cf078, 0x40 sliding), set by S75 / tested by R 0x3b, stepped by 0x4667a0 (in 0x45eee0).
    std::array<Slide, 20> panels_{};
    Slide page_{};                           // 0x4d5960: pause-screen page scroll (text op 0xf, era models)
    void hudPage();                          // 0x461db0
    std::array<std::array<uint8_t, 2>, 20> panelState_{};
    void panelTick();                        // 0x4667a0
    std::vector<std::array<int16_t, 2>> hudFrames_;  // HUD sheet frame sizes (w, h)
    std::vector<uint8_t> textBlock_;         // language block 0x4d5b88 (GD+0x93 = 0: English): u16 offsets + scripts
    struct TextJob { uint32_t uid; uint16_t script; };
    std::vector<TextJob> textQueue_;         // 0x4d5b20: deferred scripts of the lowest priority (0x4681c0)
    int16_t textQueuePrio_ = 32000;          // 0x4d5b28
    uint8_t blink_ = 0;                      // 0x4cf188: palette 9 = white for 16 runs, yellow for 14
    bool pauseLatch_ = false;                // 0x4d02d0: Start must be released after a resume
    bool speechTimer_ = true;                // 0x4cca44 == 0: the clip is timed, not streamed
    int16_t speechLast_ = -1;                // 0x4cca40: last clip started
    uint32_t speechStart_ = 0;               // tick of the timed clip's start (QueryPerformanceCounter 0x4d1f80)
    uint8_t musicVol_ = 0;                   // 0x63af80: GD+0x98 at the load
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
