// bb_game checks: pad press window, heightfield step/fall thresholds, jump arc (GAMEPLAY §3.3: apex 389 at
// tick 15), and a deterministic scripted replay on real L01a (BBTIME_DATA, skipped without it).
#include "bb/game/lit_world.h"
#include "bb/level.h"

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

// Scripted replay on L01a: idle 10 ticks, run forward 60, jump; positions stay on the heightfield.
std::vector<V3> replayL01a(const BzeFile& bze, const ActorModel* model) {
    LitWorld w = LitWorld::load(bze, model);
    std::vector<V3> trace;
    for (int t = 0; t < 120; ++t) {
        PadInput in;
        if (t >= 10 && t < 70) in.buttons = PadUp;
        if (t >= 70 && t < 90) in.buttons = PadCross;
        w.tick(in);
        trace.push_back(w.p.pos);
        if ((w.p.flags14 & 2) && !(w.p.flags14 & 1)) assert(w.p.pos.y - w.groundY() >= -1 && w.p.pos.y - w.groundY() <= 100);
        V3 q = w.p.pos;
        Ground g;
        g.zone = w.p.zone;
        w.hf.groundAt(q, g);
        assert(!(g.kind & KindBlocked));  // never inside a wall
        if (t == 9) std::printf("L01a spawn settle: pos %d %d %d state %u ground %d\n", w.p.pos.x, w.p.pos.y, w.p.pos.z, w.p.state, w.groundY());
    }
    std::printf("L01a after replay: pos %d %d %d state %u carrots %d health %d\n", w.p.pos.x, w.p.pos.y, w.p.pos.z,
                w.p.state, w.carrots(), w.health());
    const V3 a = trace[9], b = trace[69];
    assert(std::abs(b.x - a.x) + std::abs(b.z - a.z) > 1500);  // ran ~50 u/tick for 60 ticks (minus accel/walls)
    return trace;
}

// Walk through the first L01a carrot from 300 units in front of it: it is taken, carrots counted (health 6/6).
void testCarrot(const BzeFile& bze) {
    LitWorld w = LitWorld::load(bze);
    const auto it = std::ranges::find_if(w.pickups, [](const Pickup& k) { return k.kind == PickupKind::Carrot; });
    assert(it != w.pickups.end());
    std::printf("L01a pickups: %zu (first carrot at %d %d %d)\n", w.pickups.size(), it->pos.x, it->pos.y, it->pos.z);
    w.start({it->pos.x, it->pos.y, it->pos.z - 300}, 0, w.p.zone);
    for (int t = 0; t < 30; ++t) {
        w.tick({.buttons = uint16_t(t > 2 ? PadUp : 0)});
        if (std::getenv("BBTIME_GAME_DEBUG")) std::printf("  t%d pos %d %d %d state %u\n", t, w.p.pos.x, w.p.pos.y, w.p.pos.z, w.p.state);
    }
    std::printf("carrots %d\n", w.carrots());
    assert(w.carrots() >= 1 && it->taken);
}

// Health 0 -> death state (HUD 0x44acc0), clip end -> respawn at the checkpoint with health 6 (§3.6). L01a's
// start trigger sets the checkpoint (flag 0x100000) to (23700, -640, 9655).
void testDeath(const BzeFile& bze, const ActorModel* model) {
    LitWorld w = LitWorld::load(bze, model);
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

// Every LiT level with a player + heightfield loads and survives 120 scripted ticks (crash/UB sweep; run under
// BBTIME_SANITIZE for bounds checks). Prints the logic ops the levels' player rules/triggers hit but we skip.
void sweep(const std::filesystem::path& dir) {
    int loaded = 0, skipped = 0;
    std::set<std::string> unsupported;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        std::string ext = e.path().extension().string();
        for (char& c : ext) c = char(std::tolower(uint8_t(c)));
        if (ext != ".bze") continue;
        LitWorld w;
        try {
            w = LitWorld::load(loadBze(e.path()));
        } catch (const FormatError&) {
            ++skipped;
            continue;
        }
        ++loaded;
        for (int t = 0; t < 120; ++t)
            w.tick({.buttons = uint16_t(t < 10 ? 0 : t < 60 ? PadUp : t < 80 ? PadUp | PadCross : PadRight | PadSquare)});
        unsupported.insert(w.unsupported.begin(), w.unsupported.end());
    }
    std::printf("sweep: %d levels simulated, %d without player/heightfield; not interpreted:", loaded, skipped);
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
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) { std::printf("BBTIME_DATA unset: real-data checks skipped\n"); return 0; }
    const std::filesystem::path data = env;
    testTables(data);
    const auto file = data / "lit/Datas/bze/L01a.bze";
    if (!std::filesystem::exists(file)) { std::printf("no L01a.bze: skipped\n"); return 0; }
    const BzeFile bze = loadBze(file);
    testCarrot(bze);
    sweep(file.parent_path());
    const Scene scene = buildLitScene(bze, "L01A");
    const ActorModel* model = nullptr;
    for (const ActorInstance& a : scene.actors)
        if (a.player) model = &scene.actorModels[a.model];
    assert(replayL01a(bze, model) == replayL01a(bze, model));  // deterministic
    testDeath(bze, model);
    std::printf("ok\n");
    return 0;
}
