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
#include "bb/game/sfx.h"
#include "bb/scene.h"

#include <array>
#include <cstdint>
#include <optional>
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
// Frame events of one animation asset (chunk 0x22 tags 0x28 / 0x0d, run by 0x4393e0 on the frame boundary).
struct AnimFx {
    struct Sound { uint8_t id = 0, frame = 0, reload = 0, count = 0; };  // tag 0x28 {sound, frame, repeat reload}
    struct Event { uint16_t id = 0; uint8_t f2 = 0, f3 = 0; uint16_t frame = 0; LogicItem item; };  // tag 0x0d (12 B)
    uint16_t anim = 0;  // asset role = animation id
    std::vector<Sound> sounds;
    std::vector<Event> events;
};

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
    uint16_t radius = 0;                            // tag 0x1c/0x42 4th u16: +0x2e (+0x30 = radius²)
    int16_t inflate = 0;                            // tag 0x38 (+0x20)
    uint16_t act3c = 0, act3e = 0;                  // tag 0x3a activation flags / radius
    int16_t zone = -1;                              // tag 0x1f
    uint16_t shadow = 0;                            // tag 0x46 (+0x3a): shadow template entity id, 0 = none
    int32_t model = -1;                             // Scene::actorModels index (TOD actors), -1 none
    uint16_t idleAnim = 0;                          // ActorInstance::anim: looped by behaviours without script animation
    uint16_t sprite = 0, spriteFrames = 0;          // first listed class-8 asset (Scene::sprites) and its record count
    int16_t spriteW = 0, spriteH = 0;               // record 0 texel size: type-4 box / billboard size (2w x 2h)
    std::vector<PlayerState> states;
    std::vector<ActorStep> steps;
    std::vector<ActorItem> items;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> flipbooks;  // listed tag-0x3f assets: anim id, {record, ticks}...
    std::vector<AnimFx> animFx;                     // listed animation assets with frame events (copies: counters per def)
};

// 0x44e400 packet 10 (+0x58 attach point) / 0x4393e0 child at the hand: part i's origin (+ local) walked up its
// coordinate chain in the exe's integers — per level RotMatrix 0x407a50 + ScaleMatrix 0x408060 of the part's rot /
// scale, ApplyMatrix 0x408a60 on the s16 vector, + its translation; the actor root (rot, scale, pos) last. nullopt for
// a part index out of range.
std::optional<V3> partOriginQ12(const AnimState& st, size_t i, const V3& pos, const std::array<int16_t, 3>& rot,
                                const std::array<int32_t, 3>& scale, V3 local = {});

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
    int16_t carrierPart = -1;      // the carrier's hand part (+0x68) as of the attach, offset `local` (+0x1a4..) under it; -1 root
    uint16_t contact = 0;          // +0x34 entity id of the last actor touched / hit
    // Hanging on a type-8 rope (0x43a310 grab): +0x70 = the rope, +0xc0 -> local coordinate +0x18c whose super is the
    // rope's root +0x7c; world position = rope matrix · local trans (+0x1a4..+0x1ac), local yaw +0x19e.
    int32_t rope = -1;
    V3 local;
    int16_t localYaw = 0;
    AnimState animState;
    V3 hand;                   // +0x58 attach point (TOD packet 10 part origin, world); valid when handSet
    bool handSet = false;
};

struct LitActor : Mover {
    int32_t def = -1;              // LitWorld::defs index
    uint16_t type = 0;             // +0x2c
    uint32_t flags8 = 0, flagsC = 0;
    std::array<int16_t, 3> rot{};  // +0xdc (yaw kept in Mover::yaw)
    std::array<int32_t, 3> scale{4096, 4096, 4096};
    std::array<int16_t, 6> box{};
    uint16_t radius = 0;           // +0x2e (tag 0x1c, then TOD packet 9)
    uint32_t radius2 = 0;          // +0x30 pre-test radius² (tag: radius², packet 9: (inflate + radius)²)
    int16_t inflate = 0;
    uint16_t act3c = 0, act3e = 0;  // +0x3c activation flags (A32/A64) / +0x3e radius
    int16_t param = 0;             // +0x17c (platforms; type 32 overlay: wait ticks) / +0x196 (type 14)
    uint16_t script = 0;           // type 32 +0x17e: overlay script index, | 0x8000 until its first behaviour call
    int32_t resume = -1;           // type 32 +0x180: continuation offset into LitWorld::overlayText, -1 none
    int16_t state = -1;            // +0x180: ActorDef::states index, -1 = not started
    uint16_t seq = 0;              // +0x17c (type 14 / 2): step index within the state
    int16_t step = -1;             // +0x184: ActorDef::steps index
    uint16_t anim = 0;             // +0x158 playing animation id (0 = restart)
    uint16_t contact = 0;          // +0x34 entity id of the last contact
    int16_t damage = 0;            // +0x38 pending damage
    int16_t wander = 0, wanderDir = 0;  // +0x18e / +0x190
    int16_t p17e = 0;              // platform phase / offset +0x17e
    int16_t hops = 0;              // +0x194 type 14: bounce count of a flagsA 0x8000000 hop / hover drift direction
    int16_t drift = 0;             // +0x192 type 14: hover drift ticks (flagsA 0x20000000) / slope speed (0x40000)
    int16_t pushX = 0, pushZ = 0;  // +0x40 / +0x44 type 5: Bugs' push move this tick (0x43a880 writes, 0x445a60 uses)
    bool boxInit = false;          // type 5 first call done (box rotated into the world frame)
    int32_t lo = 0, hi = 0;        // platform lift limits +0x184 / +0x194
    V3 base;                       // platform rest position +0x190..
    int32_t rider = -1;            // +0x6c actor carried (item 0x20000000 without 0x80; player = -2)
    // Attached coordinate (+0xc4 super, +0x70 parent): parent actors index (kParentBugs = Bugs), at the parent's hand
    // part as of the attach (+0xc4 = the parent's +0x68 then: a later packet 10 on another part does not move it;
    // runtime: LS01 time machine 620 stays on actor 89's part 0 while its anim 1 jitters part 1) or root (+0x7c, -1);
    // local translation +0x134.. and local yaw +0x12e under the parent's frame.
    int32_t parent = -1;
    int16_t parentPart = -1;
    V3 local;
    int16_t localYaw = 0;
    int16_t matYaw = 0;            // local yaw in the coordinate matrix +0x80: rebuilt after the world yaw is read (one tick behind)
    bool visible = true;           // +4 bit 2 (cleared when culled)
    bool far = false;              // hidden only by the +0x3e distance (+0x3c & 0x42): Enhanced still draws it
    bool active = false;           // linked into the active list this tick
    bool dead = false;             // flags14 0x100 handled: freed
    V3 hand;                       // +0x58 attach point (TOD packet 10 part origin, world); valid when handSet (+0x68)
    bool handSet = false;
    int16_t handPart = -1;         // the last packet-10 part: the coordinate attached children hang from (+0xc4 = +0x68)
    AnimState animState;
    // type 8 rope (0x43efc0): +0x140 swing phase (degrees, table 0x4addc8), +0x144 amplitude, +0x14e segment length,
    // +0x14c swing side (sound), +0x13c grabbed part (parts index, -1), root pitch +0xdc (kept out of rot[0], which
    // stays the draw rotation) and root matrix +0x80 (RotMatrix Y·X·Z, TransMatrix +0xe4). Parts: animState.parts,
    // part 0 = the dropped root node (draws the swing pitch), 1.. the chain top to bottom.
    int32_t ropePhase = 0, ropeAmp = 0;
    int16_t ropeSeg = 0, ropeSide = 0, ropeGrab = -1, ropePitch = 0;
    Mat ropeM;
};

// Matrix of a rope node as the rope writes it each tick (0x407a50 with rotation (rx, 0, 0): exactly RotX).
inline Mat ropePartMatrix(const TodPart& p) {
    Mat m;
    rotMatrixXYZ(m, p.rot[0], 0, 0);
    return m;
}

// One overlay text glyph of this tick's 2-D sprite list (0x4b373c, 8 B {font sprite, s16 x, s8 y, u8 palette}),
// emitted by the overlay interpreter 0x44d100 and drawn by 0x44cfa0: a 16x16 font cell with its top-left at (x, y)
// in a 512x240 screen space whose origin is the screen centre. Palette: 0 white, 1 yellow, 2 green, 3 blue, 4 red,
// 5-12 button icons (untinted).
struct OverlayGlyph {
    uint8_t ch = 0;     // script byte
    uint8_t glyph = 0;  // 1-based font glyph (table 0x4ae3a8[ch]): Scene::overlayFont[glyph - 1]
    int16_t x = 0;
    int16_t y = 0;      // TB's s16 (paged text boxes lay out whole pages before their shift); LiT stays in s8
    uint8_t pal = 0;
};

// One HUD element (slot table 0x4b23a0, 47 actor copies linked in 0x4b2164, drawn by 0x44b370): the first object of
// template entity `tmpl`'s model (Scene::templateModels) under the part matrix (rot / scaleX), placed in view space
// at depth z with its projected origin offset by (x, y) in the 512x240 screen space (origin at the centre; 0x408910:
// pixel x = W/512 * (400 * vx / depth + x), y = H/384 * 400 * vy / depth + H/240 * y, PS1 axes, depth = z + vz).
struct HudItem {
    uint16_t tmpl = 0;             // +0x1c template entity id
    bool shown = false;            // +4 bit 2 (drawn)
    int16_t state = 0, hold = 0;   // +0x1e: 0 fly out once hold is 0, 1 fly in, 2 hide; +0x22 hold ticks
    int16_t x = 0, y = 0;          // part +0xa8 / +0xac
    int32_t z = 0;                 // part +0xb0: 1200 in place, past 8000 hidden
    std::array<int16_t, 3> rot{};  // part matrix: slot 0xb spins (RotMatrixY), stopwatch hands 0x11/0x12 (RotMatrixZ)
    int32_t scaleX = 0x1000;       // slot 0xc: boss energy bar (ScaleMatrix x)
};

// Screen fade 0x44be90 / 0x44bec0, advanced once per tick by 0x44bef0 (A69 colour, A70 fade, A71 flash).
struct Fade {
    uint8_t mode = 0;              // 0x4b3232: 1 from black, 2 to black, | 0x40 colour flash (then 2 back), 4 / 8 restart
    int16_t speed = 0;             // 0x4b3236 per tick
    int16_t level = 0;             // 0x4b2388 progress
    int16_t dark = 0;              // 0x4b3738: 0x1000 = black (or full flash colour)
    std::array<uint8_t, 3> rgb{};  // 0x4b2190 / 0x4b236b / 0x4b236a flash colour (A69)
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
    // restore (the start director reads G), before the first tick(). It stands for counters 1 and 3 (tick t = counter
    // 2t + 3): the HUD actor shows every counter in the load pass and runs again in the counter-3 tick (Wine: the
    // elements fly out from counter 121 = after the 60-tick hold counted from counter 1). Then 0x41d900 opens the
    // zone's track (Wine: it plays over the rest of the loading screen until the fade in stops it).
    void loadUpdate() { cameraTick(); actorsTick(); cameraNodes(); hudTick(); hudTick(); musicTick(); }
    const ActorModel* playerModel() const { return model_; }
    // Drawing an actor attached at a parent's part (0x44c170 / GsGetLws: the child coordinate's super is that part's):
    // the part's world matrix (rotation and scale through the parent's part chain and root, PS1 axes, row major, 1.0 =
    // 4096) to put in front of the child's own RotMatrix(rot[0], matYaw, rot[2]) · Scale. nullopt when not attached
    // at a part of an actor (root / Bugs' hand: the yaw-only frame of followParent). A zero-scale part hides the child
    // (LS01 clock-gate digits under the parents' anim 270, RUNTIME_REFERENCE §28).
    std::optional<std::array<float, 9>> parentFrame(const LitActor& a) const;

    Heightfield hf;
    std::vector<PlayerState> states;
    std::vector<PlayerMove> moves;
    std::vector<PlayerRule> rules;
    std::vector<Trigger> triggers;
    std::vector<ActorDef> defs;      // every entity chunk (live + templates); actors index into it
    std::vector<LitActor> actors;    // live actors (dead ones stay with dead = true so indices are stable)
    Player p;
    Camera cam;
    int16_t camZone = -1;            // 0x4b39b4 +0x26: FindZone of the eye (0x4318f0), kept while the eye is in none
    // Visible-zone table 0x4b3460 of the last rendered frame (its unflagged zone ids; app: Renderer::zoneTable). Actor
    // flag +0x3c & 1 needs its zone listed. nullopt = not rendered (tests): every zone after the first tick.
    std::optional<std::vector<int16_t>> zoneTable;
    Pad pad;
    std::array<uint8_t, 256> G{}, S{};
    uint32_t ticks = 0;
    // Actor on-screen test (+0x3c & 0x20, 0x447d40): projected half-width at H = 400. The exe's 0x100 = the 4:3 PS1
    // reference; the app widens it to its render FOV (wide Enhanced FOV / Hor+) so actors at the screen edges stay drawn.
    double viewHalfX = 0x100;
    int nextLevel = -1;              // level change request (GD+0 / DAT_004b39bc = 1), LitWorld::levelName id
    // Sound voices (sfx.h): requests during the tick, one mixer pass after the behaviours (0x41edb0); the app drains
    // sfx.cmds. Replays of the audio-less Wine oracle set sfx.device = false (runtime: its 24 voices never free, all
    // busy by counter 1739, so chance sounds stop consuming rand).
    Sfx sfx;
    std::set<std::string> unsupported;  // logic conditions/actions/flags met but not interpreted (status/overlay)
    MsvcRand rand_;                  // exe rand() 0x4ae610 (tests sync it with the original's seed)
    int level = -1;                  // GD+0 current level id (LitWorld::levelName), set by the caller (HUD hides in 3)
    std::array<HudItem, 47> hud;     // HUD elements (lit_hud.cpp)
    Fade fade;
    // 0x44be90 (from black) / 0x44bec0 (to black); fadeTick is the per-tick 0x44bef0 that tick() ends with.
    void fadeIn(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0, fade.dark = 0x1000; }
    void fadeOut(uint8_t mode, int16_t speed) { fade.mode = mode, fade.speed = speed, fade.level = 0x1000, fade.dark = 0; }
    void fadeTick();
    // Full-screen RGBA to alpha-blend over the frame: the fade's brightness multiply ((0x1000 - dark) >> 4) / 255 as a
    // black veil, or the flash quad (0x40b980: colour rgb · f, alpha f, f = (0x1000 - dark) / 4096); alpha 0 = none.
    std::array<uint8_t, 4> fadeOverlay() const;
    // Exe level table +4 (0x430e60): loading-screen level id shown while `target` loads, 0 = none; LS01 (3) by world
    // G[0x25]. holdsLoading: target ids with the 3000 ms minimum (0x4065e0).
    static int loadingScreen(int target, uint8_t world);
    static bool holdsLoading(int target);
    // Title front end (title.bze logic, GAMEPLAY.md §8): Savedata.dat slot mask 0x4b28e4 (bit slot-1; A93 icons, A100
    // cursor), copied with the summaries into S[0x2b..0x40] by litFrontEnd (lit_save.h); A128 "Quit game" sets
    // quitRequest (the exe's 0x45f2b0 = 0x14 "Do you really want to quit?" dialog; the app returns to its menu).
    uint32_t saveMask = 0;
    bool quitRequest = false;
    // XA music (formats/AUDIO_XA.md "Music selection"): section-1 chunk 0x05 tag 0x06 {zone, track, track2} (0x41d8b0,
    // max 40). musicTick = the per-tick effect of 0x41d900 (entry of the player's zone +0x26, picked when the zone
    // changes), 0x41edb0 (a stopped stream reopens the entry's track2) and 0x41d630's silence while a fade 0x4b3232 or
    // speech S[0x17] runs. music: ..\track\Track<music>.xa to stream now (0 = silence; a change reopens the file),
    // musicNext: the track that follows when it ends (= music: loop); musicPaused: 0x41d5c0 (help / pause, resumed).
    std::vector<std::array<uint8_t, 3>> musicZones;
    uint8_t music = 0, musicNext = 0;
    bool musicPaused = false;
    // Pause menu (level logic, every playable level): item table 0x4b3e80 {link item, link value, value, count} (21 x
    // 16 B; C92/C93, A102-A105, A109/A110), page lock 0x4b3fc0 (A120/A121: F1 / Esc ignored, pause kept), config
    // volumes +0x30 / +0x34 (0x9ca850 / 0x9ca854, A122-A127; defaults 100).
    std::array<std::array<int32_t, 4>, 21> menu{};
    uint8_t menuLock = 0;
    int32_t volume30 = 100, volume34 = 100;
    void musicTick();
    // Overlay font tables (lit_overlay.cpp): 0x4ae3a8 byte -> 1-based glyph (Scene::overlayFont), 0x4ae4a8 pen advance.
    static uint8_t fontGlyph(uint8_t c);
    static uint8_t fontAdvance(uint8_t c);

    int health() const { return G[1]; }
    std::pair<int32_t, int32_t> rate() const { return {rateAcc_, rateY_}; }  // 0x4b3b8c / 0x4b3c44 (tests)
    // Rotation of Bugs' parent coordinate for drawing: the ridden platform's (rx, yaw, rz), else zero (draw with
    // local yaw p.yaw − parent yaw under it).
    std::array<int16_t, 3> playerParentRot() const {
        if (p.platform < 0) return {};
        const LitActor& a = actors[size_t(p.platform)];
        return {a.rot[0], a.yaw, a.rot[2]};
    }
    void behaviourForTest(size_t i) { behaviour(i); }  // one behaviour call of actor i (tests)
    int carrots() const { return G[3]; }
    int clocks() const { return G[5]; }
    int goldenCarrots() const { return G[7] | G[0xfc] << 8; }
    int32_t groundY() const { return p.groundBase + p.hoff; }
    const V3& checkpoint() const { return checkpoint_; }  // 0x4b2460 respawn point

    // Ground shadow of one drawn actor (0x44bb50): Scene::templateModels[entity] posed at Trans(pos) * RotMatrix(rot) *
    // Scale(scale, 0x1000, scale), PS1 axes.
    struct Shadow { uint16_t entity = 0; V3 pos; std::array<int16_t, 3> rot{}; int32_t scale = 0x1000; };
    // This tick's shadows (0x44c4b0 registration, max 40): Bugs unless first person, then drawn actors with tag 0x46.
    std::vector<Shadow> shadows() const;
    int32_t shadowMaxY = 0x7fff;     // 0x4b2378: shadow y clamp (chunk 0x3b tag 0x3c s16, A88, A91)

    // Logic VM (LOGIC.md §4/§5). self = the actor running the item (nullptr = Bugs).
    bool cond(const LogicItem& it, LitActor* self);
    void act(const LogicItem& it, LitActor* self);
    // Spawns template entity `entityId` (0x448d40) at pos/yaw; returns the actors index or -1.
    int spawn(uint16_t entityId, const V3& pos, int16_t yaw);

    // Overlay text scripts (chunk 0x49 tag 0x4a, one block per language in section 3; English loaded): u16 offset
    // table, NUL-terminated '>'-command text (GAMEPLAY.md §7.6). Empty = level without overlays.
    std::vector<uint8_t> overlayText;
    std::vector<OverlayGlyph> glyphs;  // this tick's overlay text (0x4b373c, cleared at the start of actorsTick)
    // >D / >J key names (config.pc maps, original defaults): keyNames[n] = keyboard key of >D n (config kbdMap entries
    // 11 kick □, 9 crouch ○, 8 look △, 10 jump ✕, 6 roll L1, 5 camera R2, 7 action R1, 4 camera L2, named as the exe's
    // DIK table); joyButtons[n] = joyMap byte of >J n ("BUTTON%i"); joystick = config+0x38 use-joystick (C98, >DJ).
    std::array<std::string, 8> keyNames{"F", "S", "A", "D", "E", "W", "R", "Q"};
    std::array<uint8_t, 8> joyButtons{3, 1, 0, 2, 4, 6, 5, 7};
    bool joystick = false;
    // 0x44e310: spawns a type-32 overlay actor (template 68) running script `script` (resume: continue `a`'s paused
    // script instead); returns the script's status nibble (>P digits; 4 for an unpaused >W0), 0 if nothing ran.
    uint32_t overlay(uint16_t script, LitActor* resume = nullptr);

private:
    uint8_t keys_ = 0;               // 0x4b18d0 extra keys this tick: bit 1 Esc (C96)
    void cameraNodes();              // 0x40a1f0 / 0x40a530: a TOD camera node (part flags 0x40) sets the view
    const ActorModel* model_ = nullptr;
    const Scene* scene_ = nullptr;
    V3 spawn_;
    int16_t spawnYaw_ = 0;
    V3 levelEye_;  // chunk 0x3b tag 0x2b
    // Emitter handle (sfx.h): Bugs 1, actor i -> i + 2.
    int32_t emitter(const LitActor* a) const { return a ? int32_t(a - actors.data()) + 2 : 1; }
    uint8_t loopSounds_ = 0;  // 0x4b39d0: 8 glide sound on, 0x10 dig sound on
    std::vector<AnimFx> playerFx_;   // the player's animation sound / logic events

    // Player-behaviour globals (§3).
    int32_t vsX_ = 0, vsZ_ = 0;    // 0x4b3c10 / 0x4b3c18 smoothed ground velocity
    int32_t momX_ = 0, momZ_ = 0;  // 0x4b3c30 / 0x4b3c38 momentum / slope term
    int32_t accelK_ = 0x800;       // 0x4b3b90
    int16_t turnErr_ = -1;         // 0x4b3c20
    int16_t camYawRef_ = 0;        // 0x4b39fe
    int32_t paceAcc_ = 0;          // 0x4b3bc0
    int32_t rateAcc_ = 0, rateY_ = 0;  // 0x4b3b8c / 0x4b3c44: flagsB 0x2000000 animation-rate accumulator, last root y
    V3 safe_;                      // 0x4b3780
    int32_t invuln_ = 0;           // 0x4b3160
    V3 checkpoint_;                // 0x4b2460
    int32_t checkpointYaw_ = 0;    // 0x4b246c
    int32_t defaultDeathState_ = 0;  // 0x4b2370 (zone trigger 0x400000)
    int32_t deathStateA92_ = 0;      // 0x4b39d8 (A92)
    int32_t camHold_ = 0;            // 0x4b3dac (A41)
    uint16_t playerShadow_ = 0;      // player +0x3a (tag 0x46)
    // 0x43a880 float in buoyant actors (flags8 0x900000, flags14 0x400): 0x4b3b88 bob amplitude, 0x4b3c40 entry
    // plunge speed, 0x4b3bc8 bob phase, 0x4b3c04 plunge / ramp phase.
    int32_t swimAmp_ = 0, swimDrop_ = 0, swimPh_ = 0, swimRamp_ = 0;
    void swim(int contact, V3& w);
    // Ropes (type 8): 0x43a310 grab test (rope index, Bugs' local height out), 0x43a7b0 climb limit (0 inside, 1 past
    // the bottom, 2 at the top), world position of Bugs' local coordinate, unlink (+0xc4 / +0x70, flags14 4; the
    // callers set the yaw as their exe paths do).
    int ropeGrab(int32_t& ly);
    int ropeClimb(int32_t v);
    V3 ropeWorld() const;
    V3 platformWorld() const;                    // riding: platform matrix · p.local
    // 0x434230: any box corner / centre of the rider m (rotation rot, box) over platform pl's box.
    bool overPlatform(const LitActor& pl, const Mover& m, std::array<int16_t, 3> rot, const std::array<int16_t, 6>& box) const;
    void dropRope();
    void rope(size_t i);  // lit_actors.cpp: behaviour 0x43efc0
    int16_t playerInflate_ = 0;      // player +0x20 (tag 0x38) box inflation
    bool handHitsPlayer(const V3& h) const;
    // 0x433500's face push of mover m (inflation, radius, box) against actor o; top = o's box top − m's y in o's frame.
    bool boxPush(const Mover& m, int32_t mInflate, int32_t mRadius, const std::array<int16_t, 6>& mBox, const LitActor& o,
                 V3& w, int32_t* top = nullptr) const;
    // 0x433500 for mover s (actor si or Bugs = kParentBugs) against the collision actors of `mask`: merged push in w
    // (x/z), w.y = the contact's box top − s's y; the contact or -1.
    int contactPush(const Mover& s, int32_t si, int32_t inflate, int32_t radius, const std::array<int16_t, 6>& box,
                    uint32_t mask, V3& w);
    bool linkedToBugs(size_t i, bool fromBugs) const;  // 0x433500 skip: parent / carried links between Bugs and actor i
    std::vector<uint32_t> order_;    // live list 0x4b3740 order (actors indices; spawns appended, dead removed)
    std::vector<size_t> collide_;    // collision list 0x4b39ac order (rebuilt by actorsTick; spawns appended, dead removed)
    std::vector<size_t> spawned_;    // spawned during this actorsTick: run after the active list (0x448d40)

    uint32_t todTick(const ActorModel& m, AnimState& st, uint16_t anim, uint32_t mode, std::vector<AnimFx>& fx, LitActor* self);
    void initPlayer();
    void playerTick();
    int playerContacts(V3& w, uint32_t moveFlagsA, int16_t lz);  // 0x433500 contact actor, -1 none
    void playerAttacks(const PlayerMove& mv);
    void moveActor(Mover& m, V3& delta, Ground& r, int16_t boxTop);
    void arcTick(Mover& m);
    void cameraReset();
    void cameraSnap(const V3& at);
    void cameraTick();
    void cameraFollow(Mover& target, bool type14);  // 0x4318f0 body past the target checks
    void cameraShake();                             // 0x4312c0
    int32_t camTarget_ = -1;                        // 0x4b39b8 camera target (A85: actors index for one tick, -1 Bugs)
    int32_t focus_ = -1;                            // 0x4b3950 camera focus actor (type 14 tail; consumed by 0x4318f0)
    int16_t shakeSpeed_ = 0, shakeAmp_ = 0, shakePhase_ = 0;  // 0x4b3946 / 0x4b3948 / 0x4b394a (A59 / A68)
    void zoneTriggers(Mover& m, LitActor* self);
    void kill(int16_t deathState);
    void respawn();
    void forceState(uint16_t state);  // 0x4377a0
    bool damagePlayer(LitActor& a, const ActorStep* st);  // 0x437d90
    void playSound(const LitActor* self, uint32_t id, uint16_t mode = 1);  // 0x41deb0 (self nullptr = Bugs)
    void bugsLoopSounds(uint16_t move);                                    // 0x437ab0: glide / dig loops
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
    void pushable(size_t i);
    void projectile(size_t i);
    void director(size_t i);
    void cameraDirector(size_t i);
    void spriteEffect(size_t i);  // behaviour 4 (0x4452a0)
    bool runItem(size_t i, const ActorItem& it, bool gated);  // item gates + condition; true = fires
    int findActor(uint16_t entityId) const;
    void startScript(LitActor& a, uint16_t stateId);
    // Attached actors (0x440120 coordinate chain): parent frame (origin, yaw), attach / follow / detach.
    static constexpr int32_t kParentBugs = -2;
    V3 frameWorld(int32_t parent, int16_t part, const V3& local, int16_t* yaw = nullptr) const;  // a point of the parent coordinate (part -1: root)
    int16_t attachPart(int32_t parent, bool hand) const;  // the parent's hand part (+0x68) to hang on now, -1 = root
    int16_t coordYaw(const LitActor& c) const;  // heading of c's coordinate matrix walked from its current local rot
    void attach(LitActor& c, int32_t parent, bool hand);
    void followParent(LitActor& a, const V3& move);
    void detach(LitActor& a);
    uint32_t animate(LitActor& a, uint16_t anim, uint32_t mode);

    // lit_overlay.cpp
    uint32_t overlayStatus_ = 0;     // 0x4b3fc4: status of the last C55/A49/A51/A52 overlay (C56)
    uint32_t overlayRun(LitActor& a, size_t pc);  // 0x44d100: interpret from overlayText[pc]; raw status bits
    void overlayActor(size_t i);                  // type 32 behaviour 0x4470e0
    uint8_t speech_ = 0;             // 0x4b322e playing speech id (A81)
    uint32_t speechStart_ = 0, speechMs_ = 0;  // 0x4b1c68 start (tick) / 0x4b1c60 length (ms)
    void speechTick();               // 0x41e2b0 status S[0x17] (lit_world.cpp)
    uint8_t musicTrack_ = 0;         // 0x4b39d4 the zone entry's track (0 at load / outside every entry's zone)
    int musicEntry_ = -1;            // 0x54efc4 current entry (index into musicZones)
    int32_t musicZone_ = 0xffff;     // 0x4b3b76 zone the entry was picked for (0xffff at level init)
    bool helpRequest_ = false;       // 0x4b1a2c F1 help request (consumed by tick step 7)
    bool pauseRequest_ = false;      // 0x4b1908 Esc pause request (consumed by tick step 7)

    // lit_hud.cpp: HUD actor 0x44acc0 (type 6). Shown values start at 0xff / -1 (0x4474b0).
    struct HudState {
        uint8_t carrots = 0xff, golden = 0xff, clocks = 0xff;  // 0x4b3a9f / 0x4b39fc / 0x4b316c
        int8_t health = -1;                                    // 0x4b3730
        std::array<uint16_t, 4> timers{};                      // 0x4b39be / 0x4b3648 / 0x4b3168 / 0x4b3654
        uint8_t boss = 0;                                      // 0x4b322a shown boss energy (S[1] target)
        int16_t spin = 0;                                      // 0x4b3b78
    } hudState_;
    void hudTick();
    void hudShow(size_t slot, uint16_t tmpl, size_t pos, bool instant);  // 0x449c00
    bool hudCounter(size_t p, uint16_t value, bool instant);            // 0x44a9a0
    void hudAnim(size_t p, int32_t speed);                              // 0x44ab70
    void hudHealth(size_t p, bool instant);                             // 0x44a710
    void hudTimer(size_t p, int32_t t, bool hide);                      // 0x44a800
    void hudBoss(size_t p);                                             // 0x44a580
    void hudTotals();                                                   // 0x449d80 pause-screen totals
};

} // namespace bb::game
