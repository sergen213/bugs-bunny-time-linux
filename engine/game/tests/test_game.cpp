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
        if (t == 30) assert(w.cam.eye == (V3{23613, -1000, 11550}) && w.cam.flags == 0x440 && w.cam.target.y == w.p.pos.y - 400);
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
    assert(w.carrots() >= 1 && w.actors[idx].dead);
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
        for (int t = 0; t < 240; ++t)
            w.tick({.buttons = uint16_t(t < 10 ? 0 : t < 60 ? PadUp : t < 80 ? PadUp | PadCross : PadRight | PadSquare)});
        unsupported.insert(w.unsupported.begin(), w.unsupported.end());
    }
    std::printf("sweep: %d levels simulated (%d actors), %d without player/heightfield; not interpreted:", loaded, actors, skipped);
    for (const std::string& u : unsupported) std::printf(" %s", u.c_str());
    std::printf("\n");
    assert(loaded >= 79);  // GAMEPLAY §4.1: 79 LiT BZEs carry chunk 0x36
}

} // namespace

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
    testCarrot(bze, scene);
    assert(testExit(bze, scene) == testExit(bze, scene));  // deterministic
    sweep(file.parent_path());
    assert(replayL01a(bze, scene) == replayL01a(bze, scene));  // deterministic
    testDeath(bze, scene);
    std::printf("ok\n");
    return 0;
}
