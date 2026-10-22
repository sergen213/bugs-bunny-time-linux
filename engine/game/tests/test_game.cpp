// bb_game checks: pad press window, heightfield step/fall thresholds, jump arc (GAMEPLAY §3.3: apex 389 at
// tick 15), and a deterministic scripted replay on real L01a (BBTIME_DATA, skipped without it).
#include "bb/game/lit_world.h"
#include "bb/level.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

using namespace bb;
using namespace bb::game;

namespace {

// One zone (id 0, hscale 16) 16x16 cells over x,z [0,5120), y (-5000, 0]; cellmap[i] (row-major) picks tiles,
// tile t is filled with byte tileBytes[t]. Large enough that the camera's wall probes stay inside.
using CellMap = std::array<uint16_t, 256>;
std::vector<uint8_t> block(const CellMap& cellmap, const std::vector<int8_t>& tileBytes) {
    std::vector<uint8_t> b(4 + 0x44 + 512 + tileBytes.size() * 64);
    auto put = [&](size_t o, auto v) { std::memcpy(b.data() + o, &v, sizeof v); };
    put(0, uint32_t(1));
    put(4 + 0, uint16_t(0));
    put(4 + 2, uint16_t(16));
    put(4 + 4, uint16_t(16));
    put(4 + 6, uint16_t(16));
    put(4 + 8, uint16_t(5120));
    put(4 + 0xa, uint16_t(5120));
    put(4 + 0x14, int32_t(0));
    put(4 + 0x18, int32_t(-5000));
    put(4 + 0x1c, uint32_t(4 + 0x44));
    put(4 + 0x20, uint32_t(4 + 0x44 + 512));
    for (size_t i = 0; i < 256; ++i) put(4 + 0x44 + i * 2, cellmap[i]);
    for (size_t t = 0; t < tileBytes.size(); ++t) std::memset(b.data() + 4 + 0x44 + 512 + t * 64, uint8_t(tileBytes[t]), 64);
    return b;
}

// Minimal L01a-like tables: idle 1, walk/run 5, jump 7 (moves/rules copied from L01a, §3.5).
LitWorld synthetic(const std::vector<uint8_t>& blk, V3 spawn, int16_t yaw) {
    LitWorld w;
    w.hf = Heightfield::parse(blk);
    w.states = {{1, {4, -16, -16, -16, -16}}, {5, {34, 6, -16, -16, -16}}, {7, {10, 17, 8, -16, -16}}};
    w.moves = {{4, 35, 2, 0x4001, 0x1000, 4, 0, 0, 0},           {34, 11, 1, 0x4002, 0x0c000000, 34, 20, 0, 0},
               {6, 13, 1, 0x4002, 0x0c000000, 6, 25, 0, 0},      {10, 15, 2, 0x4192, 0x800, 10, 16, 39, 70},
               {17, 33, 1, 0x4182, 0, 17, 16, 0, 0},             {8, 17, 2, 0x4001, 0, 4, 0, 0, 0}};
    auto rule = [](uint16_t group, uint16_t all, uint16_t any, int16_t next, uint32_t m14) {
        PlayerRule r;
        r.group = group, r.all = all, r.any = any, r.next = next, r.flags14Mask = m14;
        return r;
    };
    w.rules = {rule(4, PadCross, 0, 7, 0x12), rule(4, 0, 0xf000, 5, 0x12)};
    for (uint16_t g : {34, 6}) {
        w.rules.push_back(rule(g, PadCross, 0xf000, 7, 0x12));
        w.rules.push_back(rule(g, 0, 0xf000, -13, 0x12));
        w.rules.push_back(rule(g, 0, 0, 1, 0));
    }
    w.start(spawn, yaw);
    return w;
}

void testPad() {
    Pad p;
    int pressedTicks = 0;
    for (int t = 0; t < 8; ++t) {
        padTick(p, {.buttons = PadCross | PadL2});
        pressedTicks += (p.pressed & PadCross) != 0;
        assert(p.pressed & PadL2);  // level-triggered
    }
    assert(pressedTicks == 3);  // §2.2: ✕ stays pressed for the first 3 ticks
    padTick(p, {});
    padTick(p, {.buttons = PadCross});
    assert(p.pressed & PadCross);  // re-armed after release
    padTick(p, {.buttons = PadUp});
    assert(p.sy == 0x7f && p.sx == 0);
    padTick(p, {.lx = 0x7f + 49, .ly = 0x80, .analog = true});
    assert(p.sx == 0 && !(p.held & PadRight));  // dead zone 50
    padTick(p, {.lx = 0x7f + 100, .ly = 0x80, .analog = true});
    assert(p.sx == 100 && (p.held & PadRight));
}

void testJumpArc() {
    LitWorld w = synthetic(block({}, {0}), {2600, 0, 2600}, 0);
    for (int t = 0; t < 3; ++t) w.tick({});
    assert(w.p.pos.y == 0 && (w.p.flags14 & 2));
    int apex = 0, apexTick = 0, landTick = 0;
    for (int t = 1; t <= 40; ++t) {
        w.tick({.buttons = PadCross});  // hold ✕ (no variable-height cut)
        if (-w.p.pos.y > apex) { apex = -w.p.pos.y; apexTick = t; }
        if (t > 2 && !landTick && w.p.pos.y == 0) landTick = t;
    }
    std::printf("jump: apex %d at tick %d, lands tick %d\n", apex, apexTick, landTick);
    // The rule fires on tick 1 (state 7 chosen after the physics); the arc starts on tick 2.
    assert(apex == 389 && apexTick - 1 == 15 && landTick - 1 == 26);  // §3.3 standard jump
}

void testSteps() {
    // Cell (1,0) (x 320..640, z 0..320): tile 1 = -6 (96 above: walkable), tile 2 = -7 (112: blocked).
    for (int8_t b : {int8_t(-6), int8_t(-7)}) {
        CellMap cm{};
        cm[8 * 16 + 9] = 1;  // cell x 2880..3200, z 2560..2880
        LitWorld w = synthetic(block(cm, {0, b}), {2600, 0, 2720}, 0x400);  // facing +X
        for (int t = 0; t < 40; ++t) {
            w.tick({.buttons = PadUp});
            if (std::getenv("BBTIME_GAME_DEBUG"))
                std::printf("  t%d pos %d %d %d yaw %d state %u eye %d %d %d tgt %d %d %d\n", t, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.p.yaw,
                            w.p.state, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z, w.cam.target.x, w.cam.target.y, w.cam.target.z);
        }
        std::printf("step %d: x %d y %d\n", b, w.p.pos.x, w.p.pos.y);
        if (b == -6) assert(w.p.pos.x > 3200 && w.p.pos.y == 0);  // climbed 96, crossed, went down again
        else assert(w.p.pos.x < 2880 && w.p.pos.x >= 2840 && w.p.pos.y == 0);  // stopped within one sub-cell
    }
}

void testFall() {
    // Dropped 240 above flat ground (> 200): free fall vy = 70 + 70·cos, 0,1,2,4,7,10,... (§3.3).
    // Dropped 192 (<= 200): airborne ground snap clamped to ±70 per tick (§3.3 / 0x436de0).
    // (Running off a ledge in state 5 is redirected to the coyote jump, state 7: §3.1.)
    for (int h : {240, 192}) {
        LitWorld w = synthetic(block({}, {0}), {2600, -h, 2600}, 0);
        std::vector<int> dys;
        int y = w.p.pos.y;
        for (int t = 0; t < 30 && w.p.pos.y != 0; ++t) {
            w.tick({});
            dys.push_back(w.p.pos.y - y);
            y = w.p.pos.y;
        }
        std::printf("drop %d:", h);
        for (int d : dys) std::printf(" %d", d);
        std::printf("\n");
        assert(w.p.pos.y == 0 && (w.p.flags14 & 2));
        if (h == 240) assert(dys.size() >= 5 && dys[0] == 0 && dys[1] == 1 && dys[2] == 2 && dys[3] == 4 && dys[4] == 7);
        else assert(dys.size() == 3 && dys[0] == 70 && dys[1] == 70 && dys[2] == 52);
    }
}

// Logic VM opcodes (LOGIC.md §4/§5) and the type-14 / director item dialects on synthetic data.
void testLogic() {
    LitWorld w = synthetic(block({}, {0}), {2600, 0, 2600}, 0);
    auto C = [&](uint8_t c, uint8_t a, uint8_t b) { return w.cond({c, a, b, 0, 0, 0}, nullptr); };
    auto A = [&](uint8_t act, uint16_t x, uint16_t y) { w.act({0, 0, 0, act, x, y}, nullptr); };
    w.S[9] = 0xfe;
    assert(C(1, 0xfe, 9) && !C(1, 0xfd, 9) && C(70, 0xfd, 9) && !C(3, 0xff, 9) && C(5, 0xfe, 9));  // signed/unsigned
    w.G[7] = 255;
    A(4, 0, 7);  // incg on index 7 carries into G[0xfc] (16-bit golden-carrot total)
    assert(w.goldenCarrots() == 256 && w.G[7] == 0 && w.G[0xfc] == 1);
    A(19, 255, 7);  // movgi does not special-case index 7
    assert(w.G[7] == 255 && w.G[0xfc] == 1);
    A(23, 5, 120);  // incgg
    assert(w.G[5] == 1 && w.G[120] == 1);
    w.G[1] = 4;
    A(48, 0, 3);  // gvcrt: health first
    A(48, 0, 3);
    A(48, 0, 3);
    assert(w.G[1] == 6 && w.G[3] == 1);
    w.G[0x0e] = 5;
    A(42, 0, 0);
    assert(w.nextLevel == 5);
    w.nextLevel = -1;
    w.G[30] = 0b101;
    A(60, 3, 30);  // one golden carrot per clear bit among the first 3
    assert(w.goldenCarrots() == 0x1ff + 1);
    w.p.timer = 61;
    assert(C(22, 60, 0) && C(25, 1, 0) && !C(26, 2, 0));
    w.S[40] = 3;
    A(29, 0, 40);  // countdown: > 1 s elapsed -> S[Y]--, timer reset
    assert(w.S[40] == 2 && w.p.timer == 0);
    w.p.flags14 = 0x10;
    w.p.flags18 = 4;
    assert(C(82, 4, 0) && C(82, 0, 0x10) && C(83, 1, 1) && !C(83, 4, 0));
    w.pad.raw = PadCross | PadLeft;
    assert(C(57, 0x84, 0) && !C(57, 0x01, 0) && C(80, 0x01, 0));

    // Type-14 pickup idiom: touched (flags14 0x8000) -> A12 sets the collected bit, item flag 0x2000000 kills it.
    ActorDef pick;
    pick.chunk = 7, pick.type = 14, pick.entityId = 900, pick.pos = {2600, 0, 3000};
    pick.states = {{1, {1, -16, -16, -16, -16}}};
    pick.steps = {{1, 0, 1, 5, 0x1, 0, {}}};
    pick.items = {{5, 0, 0x8000, 0, {0, 0, 0, 12, 1, 20}, 0x2000000, 0, 0}};
    // Director: C2 G[10] == 1 -> level change to P1.
    ActorDef dir;
    dir.chunk = 0x0a, dir.type = 16, dir.entityId = 901;
    dir.items = {{0, 0, 0, 0, {2, 1, 10, 0, 0, 0}, 0x80000, 7, 0}};
    w.defs = {pick, dir};
    w.start({2600, 0, 2600}, 0);
    w.tick({});
    assert(w.actors.size() == 2 && !w.actors[0].dead && w.G[20] == 0 && w.nextLevel == -1);
    w.actors[0].flags14 |= 0x8000;
    w.tick({});
    assert(w.actors[0].dead && (w.G[20] & 1));
    w.G[10] = 1;
    w.tick({});
    assert(w.nextLevel == 7);
    std::printf("logic VM + item dialects ok\n");
}

void testTables(const std::filesystem::path& data) {
    const auto exe = data / "lit/Datas/bin/bugs.exe";
    if (!std::filesystem::exists(exe)) return;
    const std::vector<uint8_t> d = readFile(exe);
    // PE: .rdata VA 0x45f3f0 (sin) / 0x4633f0 (cos) are at file offsets 0x5cff0 / 0x60ff0 (tools/re probe).
    for (int i = 0; i < 4096; ++i) {
        int32_t s, c;
        std::memcpy(&s, d.data() + 0x5cff0 + i * 4, 4);
        std::memcpy(&c, d.data() + 0x60ff0 + i * 4, 4);
        assert(rsin(i) == s && rcos(i) == c);
    }
    std::printf("sin/cos tables match bugs.exe\n");
}

// Scripted replay on L01a: the level-start walk-in (director -> template 246 carries Bugs in state 130 to actor 49,
// then state 1; original: state 130 from counter 5 to 71 = ticks ~2..35, WineRef trace), then run forward 60 ticks
// and jump; positions stay on the heightfield. Camera (RUNTIME_REFERENCE.md §3): the load update snaps it to Bugs,
// then the type-36 director's eye point (23610, -1000, 11550) is approached >> 2 (original rests at 23613 -1000
// 11550, target y = Bugs - 400) with flags 0x440.
std::vector<V3> replayL01a(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.loadUpdate();
    assert(w.p.initialised && w.cam.target == (V3{w.p.pos.x, w.p.pos.y - 400, w.p.pos.z}));  // never the origin
    std::vector<V3> trace;
    int t130 = -1, t1 = -1;
    for (int t = 0; t < 160; ++t) {
        PadInput in;
        if (t >= 50 && t < 110) in.buttons = PadUp;
        if (t >= 110 && t < 130) in.buttons = PadCross;
        w.tick(in);
        // Runtime (RUNTIME_REFERENCE §3, csv/litfid_walkin*.{csv,txt}): tick 1 attaches Bugs (state 130) at z 8830;
        // the carrier moves +25 from tick 2 and Bugs follows one tick behind; tick 34 releases him at the carrier's
        // z 9655 in state 1.
        // Tick t = original counter 2t + 3 (load update = the exe's load pass; RUNTIME_REFERENCE §3/§8).
        if (t >= 1 && t <= 33) {  // ticks 1..33 carried
            const int k = t;
            assert(w.p.state == 130 && w.p.carrier >= 0 && w.p.pos.z == 8830 + 25 * std::max(0, k - 2));
            assert(w.actors[size_t(w.p.carrier)].pos.z == 8830 + 25 * (k - 1));
        }
        if (t == 34) assert(w.p.state == 1 && w.p.carrier < 0 && w.p.pos.z == 9655);
        if (t == 31) assert(w.cam.eye == (V3{23613, -1000, 11550}) && w.cam.flags == 0x440 && std::abs(w.cam.target.y - (w.p.pos.y - 400)) <= 3);  // >> 2 lerp residue
        if (w.p.state == 130 && t130 < 0) t130 = t;
        if (w.p.state == 1 && t130 >= 0 && t1 < 0) t1 = t;
        trace.push_back(w.p.pos);
        if ((w.p.flags14 & 2) && !(w.p.flags14 & 1)) assert(w.p.pos.y - w.groundY() >= -1 && w.p.pos.y - w.groundY() <= 100);
        V3 q = w.p.pos;
        Ground g;
        g.zone = w.p.zone;
        w.hf.groundAt(q, g);
        assert(!(g.kind & KindBlocked));  // never inside a wall
    }
    std::printf("L01a walk-in: state 130 from tick %d, state 1 from tick %d, pos after %d %d %d\n", t130, t1, trace[45].x,
                trace[45].y, trace[45].z);
    std::printf("L01a after replay: pos %d %d %d state %u carrots %d health %d\n", w.p.pos.x, w.p.pos.y, w.p.pos.z,
                w.p.state, w.carrots(), w.health());
    assert(t130 >= 0 && t130 <= 5 && t1 >= 25 && t1 <= 45);
    const V3 a = trace[49], b = trace[109];
    assert(std::abs(b.x - a.x) + std::abs(b.z - a.z) > 800);  // runs (accel, camera-relative steering, actor boxes in the way)
    return trace;
}

// HUD actor 0x44acc0 (Wine, local/wine/compare/lithud/w6b/trace.csv): the load-time update shows every counter at
// z 1200 (carrots 00 = templates 486 / 472 / 472, golden 03 and clocks 03 = G[G[0xb6]] / G[G[0xb8]] = G[0], three
// full health carrots); after the 60-tick hold they fly out from counter 121 (tick 59): z 1700, 2325, 3106, 4082,
// 5302, 6827, then hidden. Fades 0x44bef0: the forced L01A -> L01B change (dark 0, 256 .. 3840 over 16 ticks, then
// mode 0), the fade in after the load (update 1 then dark 3840 .. 256) and the A71 flash 0x41 / 0x400 (dark 4096,
// 3072, 2048, 1024, 0 then mode 0x42: 0, 1024, 2048, 3072, then off) all match the trace tick for tick.
void testHud(const BzeFile& bze, const Scene& scene) {
    for (uint16_t t : {472, 473, 474, 475, 476, 477, 478, 479, 480, 481, 485, 486, 487, 489, 490, 491})
        assert(scene.templateModels.contains(t) && !scene.actorModels[scene.templateModels.at(t)].meshes.empty());
    LitWorld w = LitWorld::load(bze, &scene);
    w.level = 4;
    w.loadUpdate();
    const size_t slots[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 14, 15, 16};
    const uint16_t tmpl[] = {486, 472, 472, 485, 472, 475, 487, 472, 475, 489, 489, 489};
    for (size_t i = 0; i < std::size(slots); ++i)
        assert(w.hud[slots[i]].shown && w.hud[slots[i]].tmpl == tmpl[i] && w.hud[slots[i]].z == 1200);
    assert(w.hud[0].x == -228 && w.hud[0].y == -80 && w.hud[9].x == 228 && w.hud[9].y == 90);
    const int32_t out[] = {1700, 2325, 3106, 4082, 5302, 6827};
    for (int t = 1; t <= 65; ++t) {
        w.tick({});
        const int32_t z = w.hud[0].z;  // tick t = counter 2t + 3
        if (t < 59) assert(z == 1200);
        else if (t < 65) assert(z == out[t - 59] && w.hud[0].shown);
        else assert(!w.hud[0].shown);
    }
    w.fadeOut(2, 0x100);  // the level-change fade (and A70 X=1)
    for (int k = 0; k < 16; ++k) {
        w.fadeTick();
        assert(w.fade.mode == 2 && w.fade.dark == k * 256);
    }
    w.fadeTick();
    assert(w.fade.mode == 0 && w.fade.dark == 0 && w.fadeOverlay()[3] == 0);
    w.fadeIn(1, 0x100);  // after the load (and A70 X=0)
    for (int k = 0; k < 16; ++k) {
        w.fadeTick();
        assert(w.fade.mode == 1 && w.fade.dark == 4096 - k * 256);
    }
    w.fadeTick();
    assert(w.fade.mode == 0 && w.fade.dark == 0);
    w.act({.act = 69, .x = 255, .y = 2}, nullptr);
    w.act({.act = 71}, nullptr);
    const int16_t flash[] = {4096, 3072, 2048, 1024, 0, 0, 1024, 2048, 3072};
    for (int k = 0; k < 9; ++k) {
        w.fadeTick();
        assert(w.fade.dark == flash[k] && w.fade.mode == (k < 4 ? 0x41 : 0x42));
    }
    w.fadeTick();
    assert(w.fade.mode == 0);
    w.fadeIn(0x41, 0x400);
    for (int k = 0; k < 5; ++k) w.fadeTick();
    assert((w.fadeOverlay() == std::array<uint8_t, 4>{0, 0, 255, 255}));  // full A69 colour (0, 0, 255)
    assert(LitWorld::loadingScreen(5, 0) == 106 && LitWorld::levelName(106) == "L_SARUN" && !LitWorld::holdsLoading(5));
    assert(LitWorld::loadingScreen(3, 2) == 0x5f && LitWorld::holdsLoading(0x45));
}

// Bugs' ground shadow (0x44bb50): template entity 3 (tag 0x46) = one 200 x 200 subtractive quad textured 133; on flat
// ground it lies at the ground height with scale 0x1000 and Bugs' yaw; in the air it shrinks 4/4096 per unit of height.
void testShadow(const BzeFile& bze, const Scene& scene) {
    assert(scene.templateModels.contains(3));
    const ActorModel& sm = scene.actorModels[scene.templateModels.at(3)];
    assert(sm.meshes.size() == 1 && sm.meshes[0].mesh.blend == Blend::Subtract && sm.objects[0].size() == 4);
    LitWorld w = LitWorld::load(bze, &scene);
    w.loadUpdate();
    for (int t = 0; t < 60; ++t) w.tick({});
    auto bugs = [&] {
        const auto s = w.shadows();
        assert(!s.empty() && s[0].entity == 3);
        return s[0];
    };
    LitWorld::Shadow s = bugs();
    std::printf("L01a shadow: at %d %d %d rot %d %d %d scale %d (Bugs %d %d %d ground %d)\n", s.pos.x, s.pos.y, s.pos.z,
                s.rot[0], s.rot[1], s.rot[2], s.scale, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.groundY());
    assert(s.pos.y == w.groundY() && s.scale == 0x1000 && std::abs(s.pos.x - w.p.pos.x) < 100 && std::abs(s.pos.z - w.p.pos.z) < 100);
    PadInput jump;
    jump.buttons = PadCross;
    int minScale = 0x1000;
    for (int t = 0; t < 16; ++t) {
        w.tick(jump);
        minScale = std::min(minScale, bugs().scale);
    }
    std::printf("L01a shadow during the jump: min scale %d\n", minScale);
    assert(minScale < 0x1000 && minScale >= 0x800);
}

// Walk through the first L01a carrot (type 14, touched -> spawn fx template whose clip end runs A48 gvcrt): it
// is collected and counted (health 6/6 -> carrots +1).
void testCarrot(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    auto isCarrot = [&](const LitActor& a) {
        return a.type == 14 && std::ranges::any_of(w.defs[size_t(a.def)].items, [&](const ActorItem& it) {
            if (it.item.act != 12 || !(it.flags & 0x40100)) return false;
            return std::ranges::any_of(w.defs, [&](const ActorDef& t) {
                return t.chunk == 8 && t.entityId == uint16_t(it.p2) &&
                       std::ranges::any_of(t.items, [](const ActorItem& j) { return j.item.act == 48; });
            });
        });
    };
    const auto it = std::ranges::find_if(w.actors, isCarrot);
    assert(it != w.actors.end());
    const size_t idx = size_t(it - w.actors.begin());
    const int32_t def0 = it->def;
    const V3 at = it->pos;
    std::printf("L01a: %zu actors, first carrot #%zu at %d %d %d\n", w.actors.size(), idx, at.x, at.y, at.z);
    w.G[18] |= 0x80;  // as when coming back from L01B: the start director skips the walk-in (C31 G[18] & 0x80)
    w.start({at.x, at.y, at.z - 300}, 0, w.p.zone);
    for (int t = 0; t < 90; ++t) {
        w.tick({.buttons = uint16_t(t > 2 && t < 30 ? PadUp : 0)});
        if (std::getenv("BBTIME_GAME_DEBUG")) std::printf("  t%d pos %d %d %d state %u carrot act %d dead %d cls %u box %d %d %d %d %d %d\n", t, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.p.state, w.actors[idx].active, w.actors[idx].dead, w.defs[size_t(w.actors[idx].def)].cls, w.actors[idx].box[0], w.actors[idx].box[1], w.actors[idx].box[2], w.actors[idx].box[3], w.actors[idx].box[4], w.actors[idx].box[5]);
    }
    std::printf("carrots %d, unsupported:", w.carrots());
    for (const std::string& u : w.unsupported) std::printf(" %s", u.c_str());
    std::printf("\n");
    assert(w.carrots() >= 1 && (w.actors[idx].dead || w.actors[idx].def != def0));  // freed slots are reused by spawns
}

// L01a level exit (zone trigger flag 0x80000 -> level 5 = L01B, GAMEPLAY §1.2): Bugs placed on the ground at the
// trigger box centre requests the change deterministically.
int testExit(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    const auto tr = std::ranges::find_if(w.triggers, [](const Trigger& t) {
        return std::ranges::any_of(t.entries, [](const TriggerEntry& e) { return e.flags & 0x80000; });
    });
    assert(tr != w.triggers.end());
    const V3 c = rotY(tr->rotY, tr->sizeX / 2, 0, tr->sizeZ / 2);
    const V3 centre{tr->pos.x + c.x, tr->pos.y, tr->pos.z + c.z};
    V3 from{centre.x, centre.y - 300, centre.z};
    Ground g;
    g.zone = -1;
    w.hf.groundAt(from, g);
    from.y = g.groundY;
    const int16_t yaw = int16_t(tr->rotY);
    w.G[18] |= 0x80;  // skip the level-start walk-in (see testCarrot)
    w.start(from, yaw, -1);
    int t = 0;
    for (; t < 150 && w.nextLevel < 0; ++t) w.tick({});
    std::printf("L01a exit: from %d %d %d -> nextLevel %d (%s) after %d ticks\n", from.x, from.y, from.z, w.nextLevel,
                LitWorld::levelName(w.nextLevel).c_str(), t);
    assert(w.nextLevel == 5 && LitWorld::levelName(5) == "L01B");
    return t;
}

// L01A's pushable block (type 5, 0x445a60; tag 0x1d 0x89 = X only, keep on the floor) vs the original
// (RUNTIME_REFERENCE §10, csv/litpush_L01A.csv): Bugs poked to (24723, -640, 10774) yaw 1422 at counter 103, ↑ from
// 201, R1 added at 215. He touches it at 217 (state 24 push, flags14 0x20000 from the 150-ahead probe), pushes from
// 219 (flags14 0x4000) and the block takes his move one tick later (it updates before him), sliding X only while he
// follows; release at 301 leaves it at x 25610.
void testPushL01a(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    size_t bi = 0;
    while (bi < w.actors.size() && w.actors[bi].type != 5) ++bi;
    assert(bi < w.actors.size());
    auto block = [&] { return &w.actors[bi]; };  // by index: spawns may reallocate the vector
    assert(block()->pos == (V3{25215, -640, 10430}) && block()->param == 0x89);
    struct At { int cnt; uint32_t seed; V3 bugs; uint16_t state; uint32_t f14; int32_t blockX; };
    const At want[] = {{217, 0xe0486980, {24950, -640, 10575}, 24, 2, 25215},
                       {219, 0x89753ad5, {24974, -640, 10557}, 24, 0x4002, 25215},
                       {221, 0xd91dc2de, {24990, -640, 10562}, 24, 0x4002, 25239},
                       {241, 0xba8daeee, {25084, -640, 10568}, 24, 0x4002, 25340},
                       {271, 0x4e57a60f, {25219, -640, 10568}, 24, 0x4002, 25475},
                       {301, 0x8a287814, {25345, -640, 10571}, 1, 2, 25610},
                       {325, 0x3f0bc472, {25345, -640, 10568}, 1, 2, 25610}};
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        if (cnt == 103) w.p.pos = {24723, -640, 10774}, w.p.yaw = 1422;
        w.tick({.buttons = uint16_t(cnt >= 301 ? 0 : cnt >= 215 ? PadUp | PadR1 : cnt >= 201 ? PadUp : 0)});
        if (cnt != want[k].cnt) continue;
        const At& a = want[k++];
        assert(w.rand_.seed == a.seed && w.p.pos == a.bugs && w.p.state == a.state && w.p.flags14 == a.f14 &&
               block()->pos == (V3{a.blockX, -640, 10430}) && (block()->flags14 & 0x8000));
    }
    std::printf("L01A pushable block: push 217-301 matches the original (block x 25215 -> 25610)\n");
}

// L02A1's chain (type 8 at (13039, -5569, 23412), 0x43efc0) and Bugs on it vs the original (RUNTIME_REFERENCE §13,
// local/wine/compare/litrope/r3.csv, Proton-GE /b09): Bugs poked onto the jetty at counter 301 (the chain starts
// swinging once active), into the air at the chain at 601; falls (14), grabs it at 605 (31, local height 700), rides
// the swing, R1 681-685 pumps the amplitude 128 -> 137, ↑ 701-739 climbs 32/tick to the top limit (local 284), ↓
// 761-799 climbs down 31/tick, ✕ 821-841 jumps off (state 7, released at the jump start, rope yaw kept) and the
// descending arc re-grabs it at 843 (local 532). Every counter 603-999 of Bugs' position / yaw / state / flags14 /
// local height and the chain's pitch / phase / amplitude / radius² matched; the rows below are a selection.
void testRopeL02a1(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L02A1");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    size_t ri = 0;
    while (ri < w.actors.size() && w.actors[ri].type != 8) ++ri;
    assert(ri < w.actors.size());
    struct At { int cnt; V3 bugs; int16_t yaw; uint16_t state; uint32_t f14; int32_t local; int16_t pitch; int32_t phase, amp; uint32_t r2; };
    const At want[] = {{321, {13039, -3920, 22900}, 0, 1, 2, 0, -92, 320, 128, 31238},
                       {603, {13039, -4869, 23412}, 0, 14, 0x200, 0, 120, 164, 128, 52194},
                       {605, {13066, -4881, 23536}, 141, 31, 4, 700, 123, 168, 128, 63110},
                       {661, {13037, -4870, 23406}, 141, 31, 4, 700, -14, 280, 128, 15665},
                       {685, {13015, -4879, 23303}, 141, 31, 4, 700, -112, 328, 137, 53489},
                       {727, {13030, -5288, 23372}, 141, 32, 4, 284, -86, 52, 128, 53489},
                       {739, {13034, -5286, 23391}, 141, 32, 4, 284, -40, 76, 128, 30754},
                       {799, {13074, -4713, 23575}, 141, 33, 4, 873, 125, 196, 128, 68651},
                       {821, {13061, -4703, 23513}, 141, 7, 4, 873, 71, 240, 128, 37322},
                       {823, {13061, -4742, 23513}, 141, 7, 1, 873, 64, 244, 128, 45218},
                       {841, {13061, -5037, 23513}, 141, 7, 1, 873, -14, 280, 128, 17058},
                       {843, {13036, -5038, 23400}, 141, 31, 4, 532, -23, 284, 128, 15642},
                       {999, {13053, -5042, 23479}, 141, 31, 4, 532, 78, 236, 128, 45482}};
    const std::pair<int, uint16_t> pads[] = {{681, PadR1}, {687, 0}, {701, PadUp}, {741, 0}, {761, PadDown}, {801, 0}, {821, PadCross}, {843, 0}};
    uint16_t buttons = 0;
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        for (const auto& [c, b] : pads)
            if (c == cnt) buttons = b;
        if (cnt == 303) w.p.pos = {13039, -3920, 22900}, w.p.yaw = 0;
        if (cnt == 603) w.p.pos = {13039, -4869, 23412}, w.p.yaw = 0;
        w.tick({.buttons = buttons});
        if (cnt != want[k].cnt) continue;
        const At& a = want[k++];
        const LitActor& r = w.actors[ri];
        assert(w.p.pos == a.bugs && w.p.yaw == a.yaw && w.p.state == a.state && w.p.flags14 == a.f14 &&
               (a.f14 != 4 || (w.p.rope == int(ri) && w.p.local.y == a.local)) && r.ropePitch == a.pitch &&
               r.ropePhase == a.phase && r.ropeAmp == a.amp && r.ropeSeg == 332 && r.radius2 == a.r2);
    }
    std::printf("L02A1 chain: grab, swing, pump, climb, jump off and re-grab (counters 603-999) match the original\n");
}

// L01D1's float column (type 14 at (21040, -2000, 7280), flags8 0x101000, state 2 until S[45] & 1) vs the original
// (RUNTIME_REFERENCE §13, local/wine/compare/litswim/s2.csv): S[45] = 1 poked after counter 351 (state 1 -> 142, its
// TOD box radius 540..584, top -2000 then -1280), Bugs poked into it at 401: float state 25 (flags14 0x404), plunge
// 6/tick decaying over 16 ticks, lift to the column top with the random bob, ✕ 601-619 water jump (26: H80/60 at
// 629), re-entry from above at 665 (plunge 86). The oracle's other actors draw rand() differently from ours, so its
// seed is replayed before the four ticks where Bugs draws (entry 403 / 665, bob amplitude 497 / 579). Every counter
// 403-759 matched (one torn oracle row at 475); the rows below are a selection.
void testFloatL01d1(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L01D1");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    size_t gi = 0;
    while (gi < w.actors.size() && !(w.actors[gi].flags8 & 0x100000)) ++gi;
    assert(gi < w.actors.size() && w.actors[gi].pos == (V3{21040, -2000, 7280}));
    struct At { int cnt; int32_t y; uint16_t state; uint32_t f14; };
    const At want[] = {{403, -2094, 25, 0x404}, {435, -2045, 25, 0x404}, {439, -2053, 25, 0x404}, {507, -3154, 25, 0x404},
                       {581, -3269, 25, 0x404}, {601, -3209, 26, 0x404}, {629, -3305, 26, 1},     {647, -3881, 26, 9},
                       {665, -3185, 25, 0x404}, {695, -2362, 25, 0x404}, {759, -3200, 25, 0x404}};
    const std::pair<int, uint32_t> seeds[] = {{403, 0xf8fed105}, {497, 0xd94260cd}, {579, 0xe7a6acdd}, {665, 0x7f5c04de}};
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        if (cnt == 303) w.p.pos = {21040, -2000, 6600}, w.p.yaw = 0;
        if (cnt == 353) w.S[45] = 1;
        if (cnt == 403) w.p.pos = {21040, -2100, 7280}, w.p.yaw = 0;
        for (const auto& [c, s] : seeds)
            if (c == cnt) w.rand_.seed = s;
        w.tick({.buttons = uint16_t(cnt >= 601 && cnt <= 619 ? PadCross : 0)});
        const LitActor& g = w.actors[gi];
        if (cnt == 355) assert(w.defs[size_t(g.def)].states[size_t(g.state)].id == 142 && g.radius == 551 && g.box[4] == -2000);
        if (cnt != want[k].cnt) continue;
        const At& a = want[k++];
        assert(w.p.pos == (V3{21040, a.y, 7280}) && w.p.state == a.state && w.p.flags14 == a.f14);
    }
    std::printf("L01D1 float column: entry, plunge, lift, bob, water jump and re-entry (counters 403-759) match the original\n");
}

// L02A2 carrier bird 1335 vs the original (local/wine/compare/w9lit2/o_bird_L02A2.csv, w9lit3/cmp_bird_L02A2.txt,
// w9lit7/o_bird_L02A2.csv + h_bird_L02A2.txt): Bugs poked to (5166, -1410, 4330) and S[61] = 3 (spawner 327 lets the
// bird out) as the oracle's pokes, which land after the sampled tick (counters 201 / 211 there = before ticks 203 /
// 213 here). Falling beside the planks he is pushed by their merged 0x433500 push once the wall trace allows it
// (231), then the spawner's growing box pushes the standing Bugs (counters 291-331), exactly as the original. The
// spawner matches tick for tick, the bird spawns at 331 and hovers (flagsA 0x20000000: bob +0x18 / +0x1a, height
// closing toward actor 99), grabs Bugs by its anim-203 event (f2 4: Bugs state 47 at its hand part), carries him
// toward 99, which rides the hand of the path actor at (0, 0, 0) (spawn 0x81c0), and drops him (A33) in state 39:
// the bird exactly (counters 331-1441), Bugs within 31 (one-tick hand lag every 33 ticks).
void testBirdL02a2(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L02A2");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    struct At { int cnt; V3 bird; V3 bugs; uint16_t state; };
    const At want[] = {{331, {5171, -1420, 4305}, {5346, -1020, 4296}, 1},    {341, {5285, -1455, 4278}, {5315, -1329, 4279}, 47},
                       {651, {4608, -1837, 6357}, {4597, -1761, 6355}, 47},   {1011, {4786, -2866, 2424}, {4783, -2764, 2424}, 47},
                       {1381, {7160, -3358, 5835}, {7383, -3119, 5474}, 39}};
    auto near = [](const V3& a, const V3& b, int32_t tol) { return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.z - b.z) <= tol; };
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        if (cnt == 203) w.p.pos = {5166, -1410, 4330};
        if (cnt == 213) w.S[61] = 3;
        w.tick({});
        struct Pushed { int cnt; V3 at; };
        for (const Pushed& q : {Pushed{229, {5258, -1105, 4402}}, Pushed{231, {5203, -1035, 4356}}, Pushed{233, {5224, -1020, 4371}},
                                Pushed{303, {5233, -1020, 4366}}, Pushed{321, {5282, -1020, 4350}}, Pushed{331, {5346, -1020, 4296}}})
            if (cnt == q.cnt) assert(w.p.pos == q.at);
        if (cnt != want[k].cnt) continue;
        const At& a = want[k++];
        const LitActor* b = nullptr;
        for (const LitActor& x : w.actors)
            if (!x.dead && x.entityId == 1335) b = &x;
        assert(b && b->pos == a.bird && near(w.p.pos, a.bugs, 31) && w.p.state == a.state);
        assert((a.state == 47) == (w.p.carrier == int(b - w.actors.data())));
    }
    std::printf("L02A2 bird: spawn, hover, grab (anim event), carry at the hand toward actor 99 and drop match the original\n");
}

// L04A1 (/b29) vs the original (local/wine/compare/w9lit3/{o_L04A1.csv, L04A1_pads.txt, cmp_L04A1.py}): with the
// oracle's pad replayed (raw 0x80 pulses, same rand stream) and Bugs poked beside door 165 at 153, the doors (flagsA
// 1: no motion block, flags14 never 2 / 0x200) take Bugs' hits (hp −1 a tick), the thrower at (1589, -1400, 11595)
// turns toward Bugs and starts its throw (state 225, rand gate 32600) at 201, and projectile 100 appears at 255 in
// hop state 171 where the original puts it (its first move pushed out of the thrower's box, w9lit7/t_L04A1.txt),
// moving (−6, −9, −9) a tick like the original.
void testThrowerL04a1(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L04A1");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    auto find = [&](auto pred) -> const LitActor* {
        for (const LitActor& a : w.actors)
            if (!a.dead && pred(a)) return &a;
        return nullptr;
    };
    uint16_t raw = 0;
    V3 shot{};
    for (int t = 0; t < 130; ++t) {
        const int cnt = 2 * t + 3;
        if (cnt == 3 || cnt == 37 || cnt == 69 || cnt == 103 || cnt == 137) raw = 0x80;
        if (cnt == 11 || cnt == 47 || cnt == 81 || cnt == 115 || cnt == 147) raw = 0;
        if (cnt == 153) w.p.pos = {1400, -320, 11380}, w.p.yaw = 0;
        w.tick({.buttons = raw});
        const LitActor* door = find([](const LitActor& a) { return a.entityId == 165; });
        const LitActor* door2 = find([](const LitActor& a) { return a.entityId == 177; });
        const LitActor* thrower = find([](const LitActor& a) { return a.type == 14 && a.pos == V3{1589, -1400, 11595}; });
        const LitActor* shotA = find([](const LitActor& a) { return a.entityId == 100; });
        assert(door && door2 && thrower);
        const auto stateId = [&](const LitActor& a) { return a.state < 0 ? -1 : int(w.defs[size_t(a.def)].states[size_t(a.state)].id); };
        if (cnt == 157) assert(door->flags14 == 0x4008000 && door->reg == 0);
        if (cnt == 163) assert(door->flags14 == 0x8000 && door->reg == -3 && door2->flags14 == 0);
        if (cnt == 199) assert(stateId(*thrower) == 1 && thrower->yaw == 2398);
        if (cnt == 201) assert(stateId(*thrower) == 225 && thrower->yaw == 2405);
        assert((shotA != nullptr) == (cnt >= 255));
        if (cnt == 255) assert(stateId(*shotA) == 171 && shotA->yaw == 2405 && shotA->pos == V3(1424, -1491, 11606)), shot = shotA->pos;
        if (cnt == 257) assert(shotA->pos == (V3{shot.x - 6, shot.y - 9, shot.z - 9}));
    }
    std::printf("L04A1: doors, thrower and the projectile's hops match the original (counters 155-257)\n");
}

// l04e cart 109 vs the original (local/wine/compare/w9lit2/{o_cart_l04e.csv, cart_pads.txt, cmp_cart_l04e.txt}):
// Bugs rides it (item 0x20000000), steps with flagsB's sign are pad-steered (flagsA 0x100000: constant speed 116,
// at most ±0x1e a tick toward the stick heading), a move cut by a wall sets flags14 0x100000 (bump state 226 → 227
// aimed at actor 246). Replaying the oracle's pad (Right 87-135, Left+Up 159-219): tick-exact through counter 293,
// within 9 to 503 (x / z / heading; Bugs as well).
void testCartL04e(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L04E");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    const std::pair<int, uint16_t> pads[] = {{3, PadLeft}, {45, 0}, {87, PadRight}, {137, 0}, {157, PadLeft}, {159, PadLeft | PadUp}, {219, PadUp}, {221, 0}};
    struct At { int cnt; V3 cart; int16_t yaw; uint16_t state; V3 bugs; };
    const At want[] = {{93, {3495, -592, 12598}, -1928, 1, {3517, -627, 12713}},   {123, {2835, -592, 11293}, 1957, 227, {2835, -647, 11293}},
                       {233, {4198, -584, 6222}, -1996, 227, {4208, -682, 6338}}, {283, {3948, -544, 3322}, -1997, 1, {3958, -579, 3438}},
                       {493, {10148, -140, 8156}, -21, 1, {10152, -187, 8041}}};
    auto near = [](const V3& a, const V3& b, int32_t tol) { return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.z - b.z) <= tol; };
    uint16_t buttons = 0;
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        for (const auto& [c, b] : pads)
            if (c == cnt) buttons = b;
        w.tick({.buttons = buttons});
        if (cnt != want[k].cnt) continue;
        const At& a = want[k++];
        const LitActor* c = nullptr;
        for (const LitActor& x : w.actors)
            if (!x.dead && x.entityId == 109) c = &x;
        const int32_t tol = cnt <= 283 ? 0 : 9;
        assert(c && near(c->pos, a.cart, tol) && near(w.p.pos, a.bugs, tol) && w.defs[size_t(c->def)].states[size_t(c->state)].id == a.state);
        assert(std::abs(angleDiff(c->yaw, a.yaw)) <= tol && w.p.carrier == int(c - w.actors.data()));
    }
    // Lean (w9lit5/{o_lean_l04e.csv, lean_pads.txt, cmp_lean_l04e.txt}): the root part's rot y follows 1/32 of the
    // wanted turn while steered (part flag 0x400 set, the animation no longer poses it); exact to 115, then within 5
    // (our camera yaw differs slightly).
    LitWorld l = LitWorld::load(bze, &scene);
    l.sfx.device = false;
    l.loadUpdate();
    const std::pair<int, int32_t> lean[] = {{101, 0}, {103, -16}, {113, -76}, {115, -85}, {127, -132}};
    k = 0;
    for (int t = 0; k < std::size(lean); ++t) {
        const int cnt = 2 * t + 3;
        buttons = cnt < 55 ? PadRight : cnt < 103 ? 0 : cnt < 105 ? PadLeft : PadLeft | PadUp;
        l.tick({.buttons = buttons});
        if (cnt != lean[k].first) continue;
        const auto c = std::ranges::find_if(l.actors, [](const LitActor& x) { return !x.dead && x.entityId == 109; });
        const TodPart& root = c->animState.parts.at(0);
        assert(std::abs(root.rot[1] - lean[k].second) <= (cnt <= 115 ? 0 : 5) && ((root.flags & 0x400) != 0) == (cnt > 101));
        ++k;
    }
    std::printf("l04e cart: pad steering, wall bumps, the ride (counters 93-493) and the lean match the original\n");
}

// L01A camera shake (A59 X 100 Y 35 in the start director, 0x4312c0): the oracle's look-at point 0x4b38cc around
// counters 87-113 (local/wine/compare/w9lit/shake_L01A.csv): +sin·amp >> 11 in x, +cos·amp >> 12 in y, phase +1100,
// amplitude 35 decaying by 3 once the phase passes 0x800.
void testShakeL01a(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    // Oracle look-at x / y per counter 87..113 (z eases in 9644 -> 9655 unshaken); after n ticks counter = 2n + 1.
    constexpr int32_t kX[14] = {23700, 23700, 23769, 23737, 23673, 23703, 23741, 23705, 23680, 23707, 23716, 23697, 23695, 23700};
    constexpr int32_t kY[14] = {-1037, -1002, -1016, -1054, -1041, -1017, -1035, -1052, -1037, -1029, -1041, -1044, -1039, -1039};
    for (int n = 1; n <= 56; ++n) {
        w.tick({});
        if (n >= 43) assert(w.cam.target.x == kX[n - 43] && w.cam.target.y == kY[n - 43]);
    }
    assert(w.cam.target.z == 9655);
}

// L01A seesaw (type 18 at (20150, -2460, 7705), yaw 1024, box x ±960, +0x1e 60) vs the oracle
// (local/wine/compare/w9lit3/o_seesaw_L01A.csv, RUNTIME_REFERENCE §18): Bugs poked to local x 500 above it at
// counter 153 lands at 167, tilts it (roll += 60·lx/960, capped past 0x164), slides down (lx += 60·roll/960), is a
// child coordinate (world = platform matrix · local), leaves it at 245 (0x434230) and falls on with the kept phase.
void testSeesawL01a(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    size_t s = w.actors.size();  // index: spawns may grow the vector
    for (size_t i = 0; i < w.actors.size(); ++i)
        if (w.actors[i].type == 18 && w.actors[i].pos == V3{20150, -2460, 7705}) s = i;
    assert(s < w.actors.size());
    struct Row { int cnt; V3 pos; uint16_t state; uint32_t f14; int32_t lx; int16_t roll; bool riding; };
    constexpr Row want[] = {{165, {20150, -2742, 7205}, 14, 0x200, 0, 0, false},       {167, {20150, -2707, 7205}, 14, 0x14, 500, 0, true},
                            {169, {20150, -2683, 7192}, 1, 0x14, 501, 31, true},       {189, {20150, -2341, 7044}, 1, 0x14, 624, 363, true},
                            {191, {20150, -2329, 7026}, 1, 0x14, 646, 363, true},      {243, {20150, -2027, 6540}, 1, 0x14, 1218, 363, true},
                            {245, {20150, -2015, 6521}, 1, 2, 1240, 363, false},       {247, {20150, -1991, 6521}, 14, 0x200, 1240, 341, false},
                            {253, {20150, -1885, 6521}, 14, 0x200, 1240, 282, false},  {263, {20150, -1600, 6521}, 14, 2, 1240, 207, false},
                            {301, {20150, -1600, 6521}, 1, 2, 1240, 67, false}};
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        const int cnt = 2 * t + 3;
        if (cnt == 153) w.p.pos = {20150, -2780, 7205};
        w.tick({});
        if (cnt != want[k].cnt) continue;
        const Row& r = want[k++];
        assert(w.p.pos == r.pos && w.p.state == r.state && w.p.flags14 == r.f14 && w.actors[s].rot[2] == r.roll && (w.p.platform >= 0) == r.riding);
        assert(r.cnt <= 165 || w.p.local.x == r.lx);
    }
    std::printf("L01A seesaw: tilt, slide, fall-off match the original (counters 165-301)\n");
}

// Health 0 -> death state (HUD 0x44acc0), clip end -> respawn at the checkpoint with health 6 (§3.6). L01a's
// start trigger sets the checkpoint (flag 0x100000) to (23700, -640, 9655).
void testDeath(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    const V3 start = w.p.pos;
    for (int t = 0; t < 40; ++t) w.tick({.buttons = uint16_t(t > 5 ? PadUp : 0)});
    assert(w.p.pos != start);
    w.G[1] = 0;
    w.tick({});
    assert(w.p.flags14 & 0x1000000);
    int t = 0;
    while ((w.p.flags14 & 0x1000000) && t++ < 600) w.tick({.buttons = PadUp});  // no control while dead
    std::printf("death: respawned after %d ticks at %d %d %d health %d\n", t, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.health());
    assert(w.checkpoint() == (V3{23700, -640, 9655}));
    assert(!(w.p.flags14 & 0x1000000) && w.p.pos == w.checkpoint() && w.health() == 6);
}

// L03C1 intro actor (type 14 at (2000, -30000, 2000), step 488 anim 444 flagsB 0x2000000, A56 on entry) vs the
// original (local/wine/compare/w9lit4/o_L03C1.csv): the 0x4b3b8c accumulator, the last root y and the frame (skip
// 2 -> 3 -> 5) match every counter 3-307; the carrier 268 never hurts the Bugs it carries (0x433500 skips its
// carried actor: health 6, state 1, carried through counter 669, w9lit5/cmp_L03C1.txt).
// Plus the type-4 sprite effects: L02C4's placed torch flames advance a record every 2 ticks and loop.
void testRateL03c1(const std::filesystem::path& dir) {
    const BzeFile bze = loadBze(dir / "L03C1.bze");
    const Scene scene = buildLitScene(bze, "L03C1");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    struct At { int cnt, acc, y; uint16_t frame; };
    const At want[] = {{203, 64, -502, 201}, {255, 808, 450, 255}, {305, 2560, 2566, 351}, {307, 2560, 2677, 356}};
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        w.tick({});
        if (2 * t + 3 != want[k].cnt) continue;
        const auto a = std::ranges::find_if(w.actors, [](const LitActor& x) { return x.type == 14 && x.pos.y == -30000; });
        assert(a != w.actors.end() && w.rate() == std::pair(want[k].acc, want[k].y) && a->anim == 444 && a->animState.frame == want[k].frame);
        ++k;
    }
    for (int t = 153; 2 * t + 3 <= 669; ++t) {
        w.tick({});
        assert(w.health() == 6 && w.p.state == 1 && w.p.carrier >= 0 && w.actors[size_t(w.p.carrier)].entityId == 268);
    }
    const BzeFile b4 = loadBze(dir / "L02C4.bze");
    LitWorld s = LitWorld::load(b4, nullptr);
    s.sfx.device = false;
    LitActor* f = nullptr;
    for (LitActor& x : s.actors)
        if (x.type == 4) { f = &x; break; }
    assert(f && s.defs[size_t(f->def)].spriteFrames == 5 && s.defs[size_t(f->def)].spriteW == 32 && s.defs[size_t(f->def)].spriteH == 64);
    const size_t fi = size_t(f - s.actors.data());
    s.behaviourForTest(fi);
    std::vector<uint16_t> frames{s.actors[fi].animState.frame};
    for (int t = 0; t < 11; ++t) s.behaviourForTest(fi), frames.push_back(s.actors[fi].animState.frame);
    assert((frames == std::vector<uint16_t>{0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 0, 0}) && s.actors[fi].box[4] == -64 && !s.actors[fi].dead);
    std::printf("L03C1 animation rate (A56, flagsB 0x2000000) matches the original (counters 3-307); type-4 flipbook ok\n");
}

// Every LiT level with a player + heightfield loads (with its scene: actors animate) and survives 240 scripted
// ticks (crash/UB sweep; run under BBTIME_SANITIZE for bounds checks). Prints what the levels' logic hit but we skip.
void sweep(const std::filesystem::path& dir) {
    int loaded = 0, skipped = 0, actors = 0;
    std::set<std::string> unsupported;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(std::tolower(uint8_t(c)));
        if (ext != ".bze") continue;
        const BzeFile bze = loadBze(e.path());
        const Scene scene = buildLitScene(bze, e.path().stem().string());
        LitWorld w;
        try {
            w = LitWorld::load(bze, &scene);
        } catch (const FormatError&) {
            ++skipped;
            continue;
        }
        ++loaded;
        actors += int(w.actors.size());
        // 900 ticks (was 240, which missed late items such as L02A2 Bugs' anim-17 events naming the absent template
        // 508 at tick 379): the app smoke's `--autoplay 'U*200,L*150,UX*20'`, then run right kicking.
        for (int t = 0; t < 900; ++t)
            w.tick({.buttons = uint16_t(t < 200 ? PadUp : t < 350 ? PadLeft : t < 370 ? PadUp | PadCross : t < 500 ? 0 : PadRight | PadSquare)});
        unsupported.insert(w.unsupported.begin(), w.unsupported.end());
        if (std::getenv("SWEEP_LEVELS")) {  // per-level list of what is not interpreted (finding test levels)
            std::printf("  %s:", e.path().stem().string().c_str());
            for (const std::string& u : w.unsupported) std::printf(" %s;", u.c_str());
            std::printf("\n");
        }
    }
    std::printf("sweep: %d levels simulated (%d actors), %d without player/heightfield; not interpreted:", loaded, actors, skipped);
    for (const std::string& u : unsupported) std::printf(" %s", u.c_str());
    std::printf("\n");
    assert(loaded >= 79);  // GAMEPLAY §4.1: 79 LiT BZEs carry chunk 0x36
}

// L01A's first enemy vs the original (RUNTIME_REFERENCE §10, local/wine/csv/litenemy_L01A_{idle,fight}.csv): template
// 932 spawned by the zone-gated director at (24500, -630, 15110), wandering (rand), turning at ledges (0x437f40), the
// type-2 idle-fidget props driven by Bugs' anim event (G[176]) and every rand() call in between. Tick t = counter
// 2t + 3; the oracle ran without audio (its 24 sound channels never free). Values below are the original's.
const LitActor* enemy932(const LitWorld& w) {
    for (const LitActor& a : w.actors)
        if (!a.dead && a.entityId == 932) return &a;
    return nullptr;
}
void testEnemyL01a(const BzeFile& bze, const Scene& scene) {
    struct At { int cnt; uint32_t seed; V3 pos; int16_t yaw; uint16_t state; int16_t wander; };
    {
        LitWorld w = LitWorld::load(bze, &scene);
        w.sfx.device = false;
        w.loadUpdate();
        const At want[] = {{9, 0x0bb31550, {24500, -624, 15125}, 4096, 1, 491},
                           {431, 0x884dc7d5, {25270, -624, 16030}, 5006, 1, 400},
                           {433, 0x67d0db44, {25278, -624, 16018}, 6210, 1, 399},  // ledge: steer +682, A38 +512
                           {691, 0x21eba4a7, {24657, -624, 15191}, 6900, 361, 330},
                           {1251, 0x79c4cece, {23853, -624, 15173}, 7440, 361, 276}};
        size_t k = 0;
        for (int t = 0; k < std::size(want); ++t) {
            w.tick({});
            if (2 * t + 3 == 5) assert(!enemy932(w));  // spawned on the 3rd tick (zone table empty before drawing)
            if (2 * t + 3 != want[k].cnt) continue;
            const LitActor* e = enemy932(w);
            assert(e && w.rand_.seed == want[k].seed && e->pos == want[k].pos && e->yaw == want[k].yaw &&
                   w.defs[size_t(e->def)].states[size_t(e->state)].id == want[k].state && e->wander == want[k].wander);
            ++k;
        }
    }
    // Fight (csv/litenemy_L01A_fight2.csv, pads replayed by counter): teleported next to it (memsrv poke at counter
    // 103), three kicks (□) make it face (state 236), chase (177) and start its attack (201) exactly as the original.
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    const std::pair<int, uint16_t> pads[] = {{585, PadSquare}, {603, 0}, {641, PadSquare}, {657, 0}, {697, PadSquare}, {711, 0},
                                             {771, PadCross}, {793, 0}, {881, PadUp}, {921, 0}, {967, PadCross}, {999, 0},
                                             {1089, PadSquare}, {1103, 0}, {1143, PadSquare}, {1159, 0}};
    const At want[] = {{603, 0xb2deca27, {25157, -624, 15651}, 6460, 236, 374},
                       {659, 0xebdba1b2, {25157, -624, 15651}, -1771, 177, 374},
                       {725, 0x2f841e10, {24730, -624, 14727}, -1780, 201, 374},
                       {755, 0x6cde71e7, {24546, -624, 14307}, -1779, 201, 374},
                       {1009, 0x9d5c6830, {24546, -624, 14307}, -1906, 209, 374},
                       {1097, 0x5740b482, {24546, -624, 14307}, -1906, 192, 374}};
    auto all = [&](uint16_t id) {
        std::vector<const LitActor*> v;
        for (const LitActor& a : w.actors)
            if (!a.dead && a.entityId == id) v.push_back(&a);
        return v;
    };
    uint16_t buttons = 0;
    size_t k = 0;
    for (int t = 0; 2 * t + 3 <= 1185; ++t) {
        const int cnt = 2 * t + 3;
        for (const auto& [c, b] : pads)
            if (c == cnt) buttons = b;
        if (cnt == 103) w.p.pos = {24500, -624, 14200};
        w.tick({.buttons = buttons});
        // Its growing attack box (TOD packet 9) pushes Bugs out (0x433500; his box is the fixed ±70 / ±40), then the
        // hand (packet 10 attach point, 0x434840) hits: health 5, knock-back state 10 on counter 769, sliding -8 z
        // at 773 without a push. Walking into it (881-921) is deflected, never stopped.
        if (cnt == 761) assert(w.p.pos == (V3{24452, -624, 14093}) && w.health() == 6);
        if (cnt == 767) assert(w.health() == 6 && w.p.state == 1);
        if (cnt == 769) assert(w.p.pos == (V3{24447, -624, 14082}) && w.health() == 5 && w.p.state == 10);
        if (cnt == 773) assert(w.p.pos == (V3{24447, -624, 14069}) && w.rand_.seed == 0xeea36799);
        if (cnt == 929) assert(w.p.pos == (V3{24510, -624, 14154}));
        // Stomp (✕ at 967): no contact at 1007 (corner past its box), at 1009 it is (state 209), Bugs bounces (91).
        if (cnt == 1007) assert(w.p.pos == (V3{24503, -923, 14144}) && w.p.state == 7);
        if (cnt == 1009) assert(w.p.pos == (V3{24503, -881, 14144}) && w.p.state == 91);
        // Kick (□ at 1089): the attach point hits at 1097 (health 9999, state 192); two carrots 39 and the golden
        // carrot 542 spawn; it is flung (state 4) and freed at 1185, the hop count doubles the carrots' second hop,
        // 542 lands on 1143 and is collected (state 129) when its frame-11 box touches Bugs at 1185.
        if (cnt == 1097) assert(enemy932(w)->reg == 9999);
        if (cnt == 1101) assert(all(39).size() == 2 && all(542).size() == 1 && all(542)[0]->pos == (V3{24541, -674, 14292}));
        if (cnt == 1165) assert(all(39)[0]->pos == (V3{24580, -986, 13287}) && all(39)[1]->pos == (V3{23675, -1052, 13758}));
        if (cnt == 1175) assert(all(39)[0]->pos == (V3{24585, -624, 13137}) && w.rand_.seed == 0x77680553);
        if (cnt == 1183) {
            const LitActor& g = *all(542)[0];
            assert(enemy932(w) && enemy932(w)->pos == (V3{25306, -229, 17187}) &&
                   w.defs[size_t(g.def)].states[size_t(g.state)].id == 1 && g.pos == (V3{24436, -897, 13977}));
        }
        if (cnt == 1185) {
            const LitActor& g = *all(542)[0];
            assert(!enemy932(w) && w.defs[size_t(g.def)].states[size_t(g.state)].id == 129 && g.pos == w.p.pos &&
                   w.rand_.seed == 0x7f6311a3);
        }
        if (k == std::size(want) || cnt != want[k].cnt) continue;
        const LitActor* e = enemy932(w);
        assert(e && w.rand_.seed == want[k].seed && e->pos == want[k].pos && e->yaw == want[k].yaw &&
               w.defs[size_t(e->def)].states[size_t(e->state)].id == want[k].state && e->wander == want[k].wander);
        ++k;
    }
    assert(k == std::size(want));
    std::printf("L01A enemy 932: idle 1251 counters and the fight to its defeat (counter 1185) match the original\n");
}

// PROBE=1 [PROBE_PADS=file of "counter rawpad(hex)"] [PROBE_IDS=932,...]: one line per tick, same fields as
// local/litenemies/enemy_trace.py (counter = 2t + 3).
void probeEnemy(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    if (std::getenv("PROBE_EVENTS")) {  // anim logic events (tag 0x0d) per entity def: anim, frame, id, f2 / f3, item
        for (const ActorDef& d : w.defs)
            for (const AnimFx& fx : d.animFx)
                for (const AnimFx::Event& e : fx.events)
                    std::printf("E def %u type %u anim %u frame %u id %u f2 %x f3 %x C%u A%u(%u,%u)\n", d.entityId, d.type, fx.anim, e.frame, e.id, e.f2,
                                e.f3, e.item.cond, e.item.act, e.item.x, e.item.y);
        return;
    }
    if (std::getenv("PROBE_TRIGGERS")) {  // trigger volumes: pos / yaw / size / zone / flags, then each entry
        for (const Trigger& t : w.triggers) {
            std::printf("T %d,%d,%d rot %d size %d,%d,%d zone %d fl %x\n", t.pos.x, t.pos.y, t.pos.z, t.rotY, t.sizeX, t.minY, t.sizeZ, t.zone, t.flags);
            for (const TriggerEntry& e : t.entries)
                std::printf("  actor %u m14 %x C%u(%u,%u) A%u(%u,%u) fl %x arg %d,%d,%d\n", e.actor, e.mask14, e.item.cond, e.item.a, e.item.b,
                            e.item.act, e.item.x, e.item.y, e.flags, e.arg[0], e.arg[1], e.arg[2]);
        }
        return;
    }
    w.loadUpdate();
    std::map<int, uint16_t> pads;
    if (const char* f = std::getenv("PROBE_PADS")) {
        std::FILE* fp = std::fopen(f, "r");
        int c;
        unsigned v;
        while (fp && std::fscanf(fp, "%d %x", &c, &v) == 2) pads[c] = uint16_t(v);
        if (fp) std::fclose(fp);
    }
    std::set<int> ids{932};
    if (const char* s = std::getenv("PROBE_IDS")) {
        ids.clear();
        for (const char* q = s; *q; ++q) {
            ids.insert(std::atoi(q));
            while (*q && *q != ',') ++q;
            if (!*q) break;
        }
    }
    std::set<int> types;  // PROBE_TYPES=5,8,...: also every live actor of these behaviour types (PROBE_T2 = 2)
    if (const char* s = std::getenv("PROBE_TYPES"))
        for (const char* q = s; *q; ++q) {
            types.insert(std::atoi(q));
            while (*q && *q != ',') ++q;
            if (!*q) break;
        }
    if (std::getenv("PROBE_T2")) types.insert(2);
    auto traced = [&](const LitActor& a) { return !a.dead && (ids.contains(a.entityId) || types.contains(a.type)); };
    const int n = std::getenv("PROBE_TICKS") ? std::atoi(std::getenv("PROBE_TICKS")) : 400;
    uint16_t raw = 0;
    for (int t = 0; t < n; ++t) {
        const int cnt = 2 * t + 3;
        if (auto it = pads.upper_bound(cnt); it != pads.begin()) raw = std::prev(it)->second;
        if (const char* tp = std::getenv("PROBE_TELEPORT")) {  // counter,x,y,z[,yaw]: memsrv poke of Bugs' position
            int c = 0, yaw = 0;
            V3 to{};
            const int got = std::sscanf(tp, "%d,%d,%d,%d,%d", &c, &to.x, &to.y, &to.z, &yaw);
            if (got >= 4 && c == cnt) w.p.pos = to;
            if (got == 5 && c == cnt) w.p.yaw = int16_t(yaw);
        }
        if (const char* s = std::getenv("PROBE_POKE")) {  // "C:S3d=8;C:x,y,z[,yaw]" as BBTIME_LIT_POKE / poke_trace.py
            for (const char* q = s; q && *q; q = std::strchr(q, ';') ? std::strchr(q, ';') + 1 : nullptr) {
                int c = 0, yaw = 0;
                char arr = 0;
                unsigned idx = 0, val = 0;
                V3 to{};
                unsigned eid = 0;
                if (std::sscanf(q, "%d:A%u|%x", &c, &eid, &val) == 3 || std::sscanf(q, "%d:Y%u=%d", &c, &eid, &yaw) == 3 ||
                    std::sscanf(q, "%d:P%u=%d,%d,%d", &c, &eid, &to.x, &to.y, &to.z) == 5) {
                    for (LitActor& a : w.actors)  // poke_trace.py A<eid>|<hex> (flags14 |=) / Y<eid>=<yaw> / P<eid>=x,y,z
                        if (c == cnt && !a.dead && a.entityId == eid) {
                            const char k = q[std::strcspn(q, ":") + 1];
                            if (k == 'A') a.flags14 |= val;
                            else if (k == 'Y') a.yaw = int16_t(yaw);
                            else a.pos = to;
                            break;
                        }
                } else if (std::sscanf(q, "%d:%c%x=%u", &c, &arr, &idx, &val) == 4 && (arr == 'S' || arr == 'G')) {
                    if (c == cnt) (arr == 'S' ? w.S : w.G)[idx & 0xff] = uint8_t(val);
                } else if (const int got = std::sscanf(q, "%d:%d,%d,%d,%d", &c, &to.x, &to.y, &to.z, &yaw); got >= 4 && c == cnt) {
                    w.p.pos = to;
                    if (got == 5) w.p.yaw = int16_t(yaw);
                }
            }
        }
        w.tick({.buttons = raw});
        std::printf("%d %08x %x %d %d,%d,%d %d %u %x %d,%d,%d |", cnt, w.rand_.seed, raw, w.health(), w.p.pos.x, w.p.pos.y, w.p.pos.z,
                    w.p.yaw, w.p.state, w.p.flags14, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z);
        for (const LitActor& a : w.actors)
            if (traced(a)) {
                const ActorDef& d = w.defs[size_t(a.def)];
                std::printf(" %u:%d,%d,%d:%d:%x:%d/%u/%d:%d:%u:%x:%d:%u:%u/%u/%u", a.entityId, a.pos.x, a.pos.y, a.pos.z, a.yaw, a.flags14,
                            a.state >= 0 && size_t(a.state) < d.states.size() ? d.states[size_t(a.state)].id : -1, a.seq,
                            a.step >= 0 ? d.steps[size_t(a.step)].id : -1, a.reg,
                            a.timer, a.flags18, a.wander, a.type, a.anim, a.animState.frame, a.animState.countdown);
                if (a.handSet) std::printf(":h%d,%d,%d", a.hand.x, a.hand.y, a.hand.z);  // +0x58
                if (a.parent >= 0) std::printf(":p%u", w.actors[size_t(a.parent)].entityId);  // +0x70 parent
                if (!a.animState.parts.empty())  // root part rot y / flags (w9lit5 poke_trace r token)
                    std::printf(":r%d/%x", a.animState.parts[0].rot[1], a.animState.parts[0].flags);
            }
        if (std::getenv("PROBE_BOX")) {  // enemy_trace.py bbox layout (Bugs; rot = yaw only) + the traced actors' boxes
            auto box = [](const std::array<int16_t, 6>& b, int r, int inf, int yaw, int roll = 0) {
                std::printf(" #%d,%d,%d,%d,%d,%d/%d/%d/0,%d,%d", b[0], b[1], b[2], b[3], b[4], b[5], r, inf, yaw, roll);
            };
            box(w.p.animState.box, w.p.animState.radius, 0, w.p.yaw);
            for (const LitActor& a : w.actors)
                if (traced(a)) box(a.box, a.animState.radius, a.inflate, a.yaw, a.rot[2]);
            std::printf(" L%d,%d,%d P%d", w.p.local.x, w.p.local.y, w.p.local.z, w.p.platform);  // +0x1a4.. / +0x70
        }
        if (std::getenv("PROBE_RATE")) std::printf(" R%d/%d", w.rate().first, w.rate().second);  // 0x4b3b8c / 0x4b3c44
        std::printf("\n");
    }
}

} // namespace

// L03ACOM camera focus actor 398 (flagsC 0x10: 0x4b3950 every tick) vs the original (local/wine/compare/w9lit5/
// o_L03ACOM_poke.csv, oracle poke at 81 = before counter 83 here): Bugs poked 1500 to the side; the follow yaw turns
// 1/16 a tick toward the focus -> Bugs direction (distance >= 1500). Without the focus the eye is ~1500 off.
void testFocusL03acom(const std::filesystem::path& file) {
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L03ACOM");
    LitWorld w = LitWorld::load(bze, &scene);
    w.sfx.device = false;
    w.loadUpdate();
    struct At { int cnt; V3 eye; };
    const At want[] = {{73, {9566, -2501, 1471}}, {85, {10271, -2501, 1284}}, {97, {11061, -2500, 1211}},
                       {121, {11313, -2217, 1226}}, {181, {11247, -2110, 1722}}};
    size_t k = 0;
    for (int t = 0; k < std::size(want); ++t) {
        if (2 * t + 3 == 83) w.p.pos = {10940, -1824, 3200};
        w.tick({});
        if (2 * t + 3 != want[k].cnt) continue;
        assert(w.cam.eye == want[k].eye);
        ++k;
    }
    std::printf("L03ACOM camera focus actor 398 matches the original\n");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testPad();
    testJumpArc();
    testSteps();
    testFall();
    testLogic();
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) { std::printf("BBTIME_DATA unset: real-data checks skipped\n"); return 0; }
    const std::filesystem::path data = env;
    testTables(data);
    const auto file = data / "lit/Datas/bze/L01a.bze";
    if (!std::filesystem::exists(file)) { std::printf("no L01a.bze: skipped\n"); return 0; }
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "L01A");
    if (const char* lv = std::getenv("PROBE_LEVEL"); std::getenv("PROBE") && lv) {  // e.g. PROBE_LEVEL=Merlin
        std::string up = lv;
        for (char& c : up) c = char(std::toupper(uint8_t(c)));
        const BzeFile b = loadBze(file.parent_path() / (std::string(lv) + ".bze"));
        probeEnemy(b, buildLitScene(b, up));
        return 0;
    }
    if (std::getenv("PROBE")) { probeEnemy(bze, scene); return 0; }
    testCarrot(bze, scene);
    assert(testExit(bze, scene) == testExit(bze, scene));  // deterministic
    sweep(file.parent_path());
    assert(replayL01a(bze, scene) == replayL01a(bze, scene));  // deterministic
    testShadow(bze, scene);
    testHud(bze, scene);
    testEnemyL01a(bze, scene);
    testPushL01a(bze, scene);
    testRopeL02a1(file.parent_path() / "L02A1.bze");
    testFloatL01d1(file.parent_path() / "L01d1.bze");
    testBirdL02a2(file.parent_path() / "L02A2.bze");
    testThrowerL04a1(file.parent_path() / "L04A1.bze");
    testCartL04e(file.parent_path() / "l04e.bze");
    testDeath(bze, scene);
    testShakeL01a(bze, scene);
    testSeesawL01a(bze, scene);
    testRateL03c1(file.parent_path());
    testFocusL03acom(file.parent_path() / "L03ACOM.bze");
    {  // L04B3 actor 98 hops onto pillar 57 and rides it (actor contact 0x433500, flagsB/flags8 riders), turns there
       // through its local yaw (local/wine/compare/w9lit6/o_L04B3.csv: every counter 3-341 equal for 98 / 57 / 269);
       // its anim-233 event spawns ball 100 at its hand, whose first move (x, x >> 16, y) is pushed out of 98's box
       // (w9lit7/o_L04B3.csv, h_L04B3.txt: equal every counter 3-661); the ball poked back at 98 (oracle pokes at 601)
       // loses flags14 0x1000 at its anim loop wrap (611), 98 steps with its stale world yaw (749): w9lit6/
       // o_L04B3_attach.csv equal for 98 / 100 through 1079. w9lit8: the ball's death spawns flipbook explosion 8 at its
       // hand with the hand part's scale, which hurts and touches pillar 57 for 16 ticks; 98 rides pillar 53 from 1067
       // (o_back.csv: 8 / 53 / 57 / 98 / 269 equal every counter to 2325). Ball poked onto 98 (attach run): the hand
       // turns with 98's local yaw at once, the ball's attach-tick move is turned by 98's world yaw and MoveActor'd
       // (w9lit7/o_L04B3_attach.csv: 57 / 98 / 100 equal every counter to 1505). Bugs, knocked back by the explosion,
       // falls once the trace's one-sub-cell lookahead finds the open floor (1041 / 1871); the second ball thrown at him
       // falls into the floor trigger volume and is freed at 2067 (o_back.csv, Bugs equal every counter to 2325).
        const BzeFile b = loadBze(file.parent_path() / "L04B3.bze");
        const Scene sc = buildLitScene(b, "L04B3");
        for (const bool back : {true, false}) {
            LitWorld w = LitWorld::load(b, &sc);
            w.sfx.device = false;
            w.loadUpdate();
            auto find = [&](uint16_t id) { for (LitActor& a : w.actors) if (!a.dead && a.entityId == id) return &a; return (LitActor*)nullptr; };
            auto parentId = [&](const LitActor* a) { return a->parent >= 0 ? int(w.actors[size_t(a->parent)].entityId) : -1; };
            for (int t = 0; 2 * t + 3 <= (back ? 2067 : 1239); ++t) {
                const int cnt = 2 * t + 3;
                if (cnt == 603) {
                    LitActor* ball = find(100);
                    ball->pos = back ? V3{5244, -1104, 1812} : V3{3145, -1426, 5787}, ball->yaw = back ? 3738 : 3072, ball->flags14 |= 0x1000;
                }
                w.tick({});
                const LitActor* a = find(98);
                const LitActor* ball = find(100);
                const LitActor* pillar = find(57);
                if (cnt == 23) assert(a && a->pos == V3(2730, -1276, 5860) && a->parent == -1 && a->flags14 == 0x200);
                if (cnt == 25) assert(a->pos == V3(2786, -1276, 5803) && a->flags14 == 0x14 && parentId(a) == 57);
                if (cnt == 303) assert(a->yaw == 1536);
                if (cnt == 305) assert(a->yaw == 1670 && find(269)->yaw == 1670);
                if (cnt == 351) assert(a->hand == V3(2692, -1721, 5717) && ball && ball->pos == V3(2630, -1766, 5656));
                if (cnt == 353) assert(ball->pos == V3(2662, -1811, 5605));
                if (!back) {
                    if (cnt == 605) assert(a->yaw == 1670 && a->hand == V3(2848, -1381, 5756));
                    if (cnt == 607) assert(ball->pos == V3(2795, -1352, 5727) && ball->yaw == 1347 && parentId(ball) == 98);
                    if (cnt == 609) assert(a->yaw == 1676 && ball->pos == V3(2781, -1352, 5718));
                    if (cnt == 1039) assert(w.p.pos == V3(5750, -1440, 1168) && w.p.flags14 == 2);
                    if (cnt == 1041) assert(w.p.pos == V3(5750, -1440, 1158) && w.p.flags14 == 0x200 && w.p.state == 10);
                    if (cnt == 1193) assert(a->yaw == 1503);
                    if (cnt == 1239) assert(ball->pos == V3(2697, -1766, 5579) && ball->yaw == 1503);
                    continue;
                }
                if (cnt == 609) assert(ball->pos == V3(5148, -1239, 1965) && ball->flags14 == 0x1001);
                if (cnt == 611) assert(ball->pos == V3(5116, -1284, 2016) && ball->flags14 == 1);
                if (cnt == 749) assert(a->pos == V3(2803, -1276, 5770) && a->yaw == 1747);
                if (cnt == 837) assert(ball->pos == V3(2841, -1276, 5616) && ball->flags14 == 0x40000014);
                if (cnt == 1013) {
                    const LitActor* e = find(8);
                    assert(e && e->pos == V3(2841, -1239, 5616) && e->inflate == 370 && e->radius == 400 &&
                           e->box == (std::array<int16_t, 6>{-80, 0, -80, 80, -128, 80}));
                }
                if (cnt == 1015) assert(pillar->flags14 == 0x42000000 && pillar->reg == 8);
                if (cnt == 1031) assert(pillar->reg == 6);
                if (cnt == 1043) assert(find(8) && pillar->flags14 == 0x42000000);
                if (cnt == 1045) assert(!find(8));
                if (cnt == 1047) assert(pillar->flags14 == 0x2000000 && pillar->reg == 4);
                if (cnt == 1067) assert(a->pos == V3(4189, -1276, 4570) && a->flags14 == 0x14 && parentId(a) == 53);
                if (cnt == 1081) assert(a->pos == V3(4189, -1234, 4570) && a->flags14 == 0);
                if (cnt == 1101) assert(a->pos == V3(4305, -1307, 4360) && a->flags14 == 0x200 && a->parent == -1);
                if (cnt == 1869) assert(w.p.pos == V3(5750, -1440, 1168) && w.p.flags14 == 2);
                if (cnt == 1871) assert(w.p.pos == V3(5750, -1440, 1158) && w.p.flags14 == 0x200);
                if (cnt == 1903) assert(w.p.pos == V3(5750, -946, 1058) && ball && ball->pos == V3(4415, -1988, 3638));
                if (cnt == 2065) assert(ball && ball->pos == V3(5420, -461, 1091));
                if (cnt == 2067) assert(!ball);
            }
        }
        std::printf("L04B3 actor 98 rides pillars 57 / 53, throws ball 100 and gets it back / attached as the original\n");
    }
    std::printf("ok\n");
    return 0;
}
