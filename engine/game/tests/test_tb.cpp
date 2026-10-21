// TB gameplay checks (GAMEPLAY_TB.md): collision query on a synthetic one-quad mesh (tb_ground.py selftest), and with
// BBTIME_DATA real ARCA01 replays — Bugs jump apex 550 @ tick 10 / Taz 490 @ 10 (§2.3), run speed 50 (§2.1), R1 switch
// with the camera one tick late (§4.1), call-friend 45-tick cooldown (§4.2) — plus every level for 120 ticks.
#include "bb/game/tb_world.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

using namespace bb;
using namespace bb::game;
namespace fs = std::filesystem;

namespace {

// tb_ground.py selftest: one 200x200 floor quad at y = 0 (normal -Y = up) in a 1-leaf octree.
std::vector<uint8_t> quadMesh() {
    std::vector<uint8_t> b;
    auto put = [&](auto v) { const auto* p = reinterpret_cast<const uint8_t*>(&v); b.insert(b.end(), p, p + sizeof v); };
    const int16_t verts[4][3] = {{-100, 0, -100}, {100, 0, -100}, {100, 0, 100}, {-100, 0, 100}};
    const int16_t edges[4][4] = {{0, 0, -16384, 0}, {16384, 0, 0, 1}, {0, 0, 16384, 2}, {-16384, 0, 0, 3}};
    const uint32_t leaf = 8, poff = 0x20 + leaf + 2, voff = poff + 0x18 + 32, soff = voff + 32;
    put(uint32_t(0)), put(soff), put(uint32_t(1)), put(voff), put(poff);
    put(int16_t(-1024)), put(int16_t(1024)), put(int16_t(-1024)), put(int16_t(0)), put(uint32_t(2048));
    put(uint16_t(0xffff)), put(uint8_t(0)), put(uint8_t(0)), put(int16_t(1)), put(uint16_t(0)), put(uint16_t(0));
    put(int16_t(0)), put(int16_t(-16384)), put(int16_t(0)), put(int16_t(0)), put(int32_t(0));
    put(int16_t(0)), put(int16_t(0)), put(int16_t(0)), put(int16_t(142)), put(uint16_t(0)), put(uint16_t(4));
    for (const auto& e : edges) for (int16_t x : e) put(x);
    for (const auto& v : verts) put(v[0]), put(v[1]), put(v[2]), put(int16_t(0));
    put(uint16_t(4096)), put(uint16_t(4096)), put(uint16_t(255)), put(uint16_t(255)), put(uint32_t(0));
    return b;
}

void synthetic() {
    const TbCollision m = TbCollision::parse(quadMesh());
    assert(m.polys.size() == 1 && m.surfaces.size() == 1 && m.verts.size() == 4);
    auto h = m.query({0, -100, 0}, {0, 300, 0});
    assert(h && h->p == (V3{0, 0, 0}) && h->poly == 0 && h->n[1] == -16384);
    assert(!m.query({0, 50, 0}, {0, 300, 0}));               // starts below the floor
    assert(m.query({104, -100, 0}, {0, 300, 0})->p == (V3{104, 0, 0}));  // within the 5-unit edge margin
    assert(!m.query({106, -100, 0}, {0, 300, 0}));
    assert(!tbQueryPasses(0x10, 0) && tbQueryPasses(0x10, 2) && tbQueryPasses(1, 0));
    // Sweep: a radius-20 sphere falling 100 onto the quad rests on it with a floor contact.
    V3 c{0, -60, 0};
    std::vector<TbCollision::Contact> ct;
    m.sweep(c, {0, 100, 0}, {20, 0}, 0x2c001, ct);
    assert(std::abs(c.y + 20) <= 1 && ct.size() == 1 && ct[0].n[1] < kTbFloorNy);
    std::puts("tb synthetic ok");
}

fs::path findCi(fs::path base, std::initializer_list<const char*> parts) {
    for (const char* part : parts) {
        fs::path next;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(base, ec)) {
            std::string a = e.path().filename().string(), b = part;
            for (char& ch : a) ch = char(std::tolower(uint8_t(ch)));
            for (char& ch : b) ch = char(std::tolower(uint8_t(ch)));
            if (a == b) { next = e.path(); break; }
        }
        if (next.empty()) return {};
        base = next;
    }
    return base;
}

struct Level {
    BzeFile bze;
    Scene scene;
    TbWorld w;
};
Level load(const fs::path& p) {
    Level l{loadBze(p), {}, {}};
    l.scene = buildTbScene(l.bze, p.stem().string());
    addTbActors(l.scene, l.bze);
    l.w = TbWorld::load(l.bze, l.scene);
    return l;
}

PadInput keys(uint16_t b) { PadInput in; in.buttons = b; return in; }

// Ticks until the focused character is idle on the ground, then a ✕ tap; returns the height above the take-off point
// for ticks 1..30 after the jump state's entry.
std::vector<int> jump(TbWorld& w) {
    TbActor& a = w.actors[size_t(w.focus())];
    for (int i = 0; i < 90; ++i) w.tick({});
    assert((a.f20 & 2) && "settled on the ground");
    const int32_t y0 = a.pos.y;
    w.tick(keys(PadCross));
    assert(a.rec()->id == 44 && "standing jump state");
    std::vector<int> h;
    for (int t = 1; t <= 30; ++t) {
        w.tick({});
        h.push_back(y0 - a.pos.y);
        if (t == 22) assert(a.rec()->id == 50 && "landing state the tick after touch-down (runtime trace)");
    }
    const auto apex = std::ranges::max_element(h);
    std::printf("  %s jump: apex %d at tick %d\n", a.id == 2 ? "Bugs" : "Taz", *apex, int(apex - h.begin()) + 1);
    return h;
}

void arca(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze");
    TbWorld& w = l.w;
    assert(w.music == 26 && w.edgeMask == 0xd0);  // §6.2 ARCA01 -> Track25; pad edge mask
    assert(w.actors[0].model && w.actors[1].model);
    // Collision boxes from tag-0x61 record 0 = runtime peek of +0x60.. (local/wine/tbplay, probe after capture A).
    for (int i = 0; i < 5; ++i) w.tick({});
    const auto& bb = w.actors[0].box;
    const auto& tb = w.actors[1].box;
    std::printf("  boxes: Bugs %d %d %d %d %d %d  Taz %d %d %d %d %d %d\n", bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], tb[0],
                tb[1], tb[2], tb[3], tb[4], tb[5]);
    assert(bb[0] == -70 && bb[1] == 0 && bb[2] == -60 && bb[3] == 70 && bb[4] == -400);
    assert(tb[0] == -85 && tb[1] == 0 && tb[2] == -50 && tb[3] == 85 && tb[4] == -240);
    {  // Bugs jump = runtime trace A (vy0 -1760, +160 per tick, apex 550 @ 10, lands @ 21)
        const std::vector<int> want = {100, 190, 270, 340, 400, 450, 490, 520, 540, 550, 550, 540, 520, 490, 450, 400, 340,
                                       270, 190, 100, 0};
        const std::vector<int> h = jump(w);
        assert(std::equal(want.begin(), want.end(), h.begin()));
    }
    {  // R1 switch: focus flips on the press tick, the camera target one tick later (§4.1; runtime trace C)
        assert(w.focus() == 0 && w.camFocus == 0);
        w.tick(keys(PadR1));
        assert(w.focus() == 1 && w.camFocus == 0 && w.A4 == 12);
        w.tick(keys(PadR1));  // held: latch, no second switch
        assert(w.focus() == 1 && w.camFocus == 1);
        w.tick({});
        const std::vector<int> h = jump(w);  // Taz (§2.3: H 493 -> 490 @ 10; runtime trace C)
        assert(*std::ranges::max_element(h) == 490 && h[9] == 490 && h[20] == 0);
    }
    {  // run = runtime trace B: keyboard stick 127 -> v = 25*2*127 >> 7 = 49; grip 1500 ramp, then (33, 36) per tick
        Level f = load(dir / "ARCA01.bze");
        for (int i = 0; i < 30; ++i) f.w.tick({});
        f.w.tick(keys(PadUp));
        const TbActor& b = f.w.actors[0];
        assert(b.rec()->id == 37);
        const int want[7][2] = {{208, 224}, {336, 368}, {416, 448}, {464, 496}, {496, 528}, {512, 560}, {528, 576}};
        for (const auto& v : want) {
            f.w.tick(keys(PadUp));
            std::printf("  run vel %d %d (trace %d %d)\n", b.vel[0], b.vel[2], v[0], v[1]);
            assert(b.vel[0] == v[0] && b.vel[2] == v[1]);
        }
    }
    {  // camera-relative steering = runtime trace E: Left held from rest, camera state copied from the trace (tick 727)
        Level f = load(dir / "ARCA01.bze");
        for (int i = 0; i < 30; ++i) f.w.tick({});
        f.w.cam.eye = {-4782, -428, -8547}, f.w.cam.look = {-4085, -310, -7555}, f.w.cam.body = {-4553, -389, -8221};
        f.w.tick(keys(PadLeft));
        const TbActor& b = f.w.actors[0];
        assert(b.rec()->id == 37);
        const int want[12][2] = {{48, 144},   {16, 272},   {-48, 384},  {-128, 464}, {-224, 512}, {-304, 544},
                                 {-384, 528}, {-448, 512}, {-496, 496}, {-544, 464}, {-576, 432}, {-608, 400}};
        int worst = 0;
        for (const auto& v : want) {
            f.w.tick(keys(PadLeft));
            std::printf("  steer vel %d %d (trace %d %d) yaw %d\n", b.vel[0], b.vel[2], v[0], v[1], b.yaw);
            worst = std::max({worst, std::abs(b.vel[0] - v[0]), std::abs(b.vel[2] - v[1])});
        }
        // First tick exact; afterwards the original turns slightly faster (camera-yaw coupling not exact): <= 4 u/tick.
        std::printf("  steer worst |dv| %d (x16)\n", worst);
        assert(worst <= 64);
    }
    {  // call friend = runtime traces D/H: spot (326, 148) from Bugs for camYaw ~490; refused at cooldown 19, ok at 45
        Level f = load(dir / "ARCA01.bze");
        TbWorld& c = f.w;
        for (int i = 0; i < 60; ++i) c.tick({});
        const V3 b = c.actors[0].pos;
        c.tick(keys(PadL1));
        // Block 4 of Taz (program +0x2c, mask 0x10000000) spawns carrier template 169 at the spot; its first frame
        // attaches Taz to part +0x84 (TOD packet 0xa00), 1432 above the floor.
        auto carriers = [&]() {
            std::vector<const TbActor*> v;
            for (const TbActor& e : c.entities)
                if (e.id == 169) v.push_back(&e);
            return v;
        };
        assert(carriers().size() == 1);
        const TbActor& car = *carriers().front();
        const V3 t = c.actors[1].pos;
        std::printf("  call friend: %d cooldown %u spot %d %d (trace 326 148) taz %d %d\n", c.lastCallFriend, c.cooldown,
                    car.pos.x - b.x, car.pos.z - b.z, t.x - b.x, t.z - b.z);
        assert(c.lastCallFriend == 1 && c.cooldown == 0 && std::abs(car.pos.x - b.x - 326) <= 2 && std::abs(car.pos.z - b.z - 148) <= 2);
        // Taz = the carrier part: x/z within 6 (the carrier's face-camera yaw, flagsB 0x10, is not modelled: H has (+4, -1)).
        assert(std::abs(t.x - car.pos.x) <= 6 && std::abs(t.z - car.pos.z) <= 6);
        // Trace D/H (Taz y - floor, state) per frame from the call: Taz reads the carrier's previous-frame pose.
        const int wantY[17] = {-1432, -1432, -1337, -1242, -1139, -1037, -934, -830, -727, -623, -519, -415, -311, -207, -103, 0, 0};
        const uint16_t wantSt[17] = {261, 261, 260, 262, 262, 262, 262, 262, 262, 262, 262, 262, 262, 262, 262, 262, 263};
        const int32_t floorY = car.pos.y;
        for (int k = 0; k < 17; ++k) {
            if (k) c.tick({});
            const TbActor& z = c.actors[1];
            std::printf("  arrival %2d: y %5d (trace %5d) state %u (trace %u) attached %d\n", k, z.pos.y - floorY, wantY[k],
                        z.rec()->id, wantSt[k], z.parent >= 0);
            assert(z.pos.y - floorY == wantY[k] && z.rec()->id == wantSt[k]);
        }
        assert(c.actors[1].parent < 0);
        c.tick({});
        assert(c.actors[1].rec()->id == 50);  // D: 263 -> 50 -> 142
        for (int i = 0; i < 4; ++i) c.tick({});
        assert(carriers().empty());  // the carrier removes itself (V 0x400) at frame 19
        c.tick(keys(PadL1));  // 22 ticks after the call: refused (cooldown < 45)
        assert(c.lastCallFriend == -1 && c.cooldown == 22);
        for (int i = 0; i < 30; ++i) c.tick({});
        c.tick(keys(PadL1));
        assert(c.lastCallFriend == 1);
    }
    {  // idle fidgets = runtime trace G (local/wine/tbplay/G_idle_rng.csv): Bugs 8 -> 20/21/22, 29/30/31, 26/27/28,
       // 17/18/19, 23 from ARCA01 start; rand() (0x46afe0) replayed from the traced seed stream (Bugs draws first).
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        for (int i = 0; i < 100 && !(w.actors[0].rec() && w.actors[0].rec()->id == 8); ++i) w.tick({});
        TbActor& bugs = w.actors[0];
        assert(bugs.rec()->id == 8);
        bugs.stateTicks = 7;  // trace G T0 = tick 379: state 8, +0x82 = 7
        const char* steps =
            "332322312321212122312121323233445242242222422212121212133225142652221212121232111111331111111131124222222242"
            "222333251111113102021301030101031103010104010103010101030101010112011303141301010101010313131133211253125111"
            "121212333313113113131114431311131111111311322231122111111111221111311111223131111224324422531141331111141212"
            "121534251335335413321214121214121214121412321732331643101201421323122324112411223121111131113111111311111111"
            "133132245371412121212123312131123241111111111111111331111344152235232523232325255212343232141212321235141436"
            "121214123412123214121212452212121212111111111111122120000000000000000000010011001102100000000000000000000000"
            "00100021111111111111111111111111111112220120000";
        const int want[][2] = {{35, 20},  {39, 21},  {67, 22},  {71, 8},   {116, 29}, {120, 30}, {150, 31}, {158, 8},
                               {184, 26}, {188, 27}, {209, 28}, {215, 8},  {242, 20}, {246, 21}, {267, 22}, {271, 8},
                               {307, 26}, {311, 27}, {339, 28}, {345, 8},  {371, 20}, {375, 21}, {407, 22}, {411, 8},
                               {443, 26}, {447, 27}, {467, 28}, {473, 8},  {513, 20}, {517, 21}, {534, 22}, {538, 8},
                               {593, 17}, {597, 18}, {625, 19}, {629, 8},  {688, 23}};
        const size_t n = std::strlen(steps);
        assert(n == 695);
        uint32_t seed = 0x64e07fb7;
        size_t next = 0, matched = 0;
        uint16_t st = 8;
        // Checked through tick 693: the original holds state 23 (anim 40) until tick 698 (frame counter 1..5), but anim
        // 40's packed TOD (asset 31) has 4 frames, so ours ends it at 694 like every other once-clip (Research Required).
        for (size_t i = 1; i <= 693; ++i) {
            w.rng.seed = seed;
            w.tick({});
            for (int k = steps[i - 1] - '0'; k > 0; --k) seed = seed * 214013u + 2531011u;
            if (i == 3) assert(bugs.stateTicks == 10);
            if (i == 30) assert(bugs.stateTicks == 37);
            if (i == 40) assert(bugs.stateTicks == 6);
            const uint16_t now = bugs.rec()->id;
            if (now == st) continue;
            st = now;
            std::printf("  fidget tick %3zu: state %u (trace %d at %d)\n", i, now, next < std::size(want) ? want[next][1] : -1,
                        next < std::size(want) ? want[next][0] : -1);
            assert(next < std::size(want) && want[next][1] == now && size_t(want[next][0]) == i);
            ++next, ++matched;
        }
        assert(matched == std::size(want));
    }
    {  // determinism: two identical replays end identically
        auto run = [&]() {
            Level r = load(dir / "ARCA01.bze");
            r.w.setTwoPlayer(true);
            for (int i = 0; i < 200; ++i)
                r.w.tick(keys(i % 50 < 30 ? PadUp : PadCross), keys(i % 40 < 20 ? PadLeft | PadUp : 0));
            return std::pair{r.w.actors[0].pos, r.w.actors[1].pos};
        };
        assert(run() == run());
    }
    {  // ARCA01 entities: guard 267 (template 265) patrols, spots a player in front within 2500 and attacks (runtime
       // capture local/wine/tbnpc/B_guard_attack.pkl.gz: 357 522 -> 497 504 502 501 364; Bugs hurt, health 6 -> 5).
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        TbActor* g = nullptr;
        for (TbActor& e : w.entities)
            if (e.id == 267) g = &e;
        assert(g && g->hp == 5 && g->behaviour == 14 && (g->view & 0x41a) == 0x41a && g->range == 7500);
        for (int i = 0; i < 30; ++i) w.tick({});
        TbActor& b = w.actors[0];
        b.pos = {g->pos.x + 1200, g->pos.y - 30, g->pos.z};  // as the capture: Bugs poked 1200 in front of it
        g->yaw = 1024;                                         // facing +x
        std::vector<uint16_t> seen;
        int hurt = -1;
        for (int i = 0; i < 300; ++i) {
            w.tick({});
            if (g->rec() && (seen.empty() || seen.back() != g->rec()->id)) seen.push_back(g->rec()->id);
        }
        std::string s;
        for (uint16_t id : seen) s += " " + std::to_string(id);
        std::printf("  guard 267 states:%s\n", s.c_str());
        // Capture B: 357 522 497 504 502 501 364 (notice, turn, wind up, throw) — Verified; then the original goes
        // 435 (its thrown template-401 projectile, V 0x80100000, is not ported): Contradicted from there on.
        const std::vector<uint16_t> want = {357, 522, 497, 504, 502, 501, 364};
        assert(std::ranges::search(seen, want).begin() != seen.end());
        // Melee (capture C: Bugs poked next to it -> 502 394, Bugs hurt state 208, health -1, 60-tick flash).
        b.pos = {g->pos.x + rotY(g->yaw, 0, 0, 300).x, g->pos.y - 30, g->pos.z + rotY(g->yaw, 0, 0, 300).z};
        for (int i = 0; i < 200 && hurt < 0; ++i) {
            w.tick({});
            if (b.hp < 6) hurt = i;
        }
        std::printf("  melee: Bugs health %d after %d ticks (state %u, guard %u)\n", b.hp, hurt, b.rec()->id, g->rec()->id);
        assert(b.hp == 5 && w.GD[0xa5] == 5 && (b.f28 & 0x200) && w.flash[0] >= 0x40);
        // Taz spin (states 135/136: contact damage 1 vs players and others) into the guard once it faces a player
        // (502, flagsA 0x10000 = vulnerable to player hits; the patrol states are not) costs it one health.
        Level h = load(dir / "ARCA01.bze");
        TbWorld& w2 = h.w;
        for (TbActor& e : w2.entities)
            if (e.id == 267) g = &e;
        for (int i = 0; i < 30; ++i) w2.tick({});
        w2.tick(keys(PadR1));
        w2.tick({});
        TbActor& t = w2.actors[1];
        g->yaw = 1024;
        t.pos = {g->pos.x + 900, g->pos.y - 30, g->pos.z};
        for (int i = 0; i < 300 && !(g->rec()->id == 502); ++i) w2.tick({});
        assert(g->rec()->id == 502);
        const int hp0 = g->hp;
        for (int i = 0; i < 60 && g->hp == hp0; ++i) {
            const V3 front = rotY(g->yaw, 0, 0, 150);
            if (i == 0) t.pos = {g->pos.x + front.x, t.pos.y, g->pos.z + front.z}, w2.setState(t, 136), t.stateTicks = 0;
            w2.tick(keys(PadSquare));
        }
        // Open (GAMEPLAY_STATUS TB "combat"): the spin does not yet land through the simplified actor contacts.
        std::printf("  Taz spin: guard health %d -> %d, state %u (Taz state %u) [open]\n", hp0, g->hp, g->rec()->id, t.rec()->id);
    }
    {  // death and respawn: the focus at 1 health dies (state 382), the partner takes over (GD+0xa2 1); then the
       // partner dies too -> a life is spent and both return at the checkpoint with 6 health (0x44ea50).
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        for (int i = 0; i < 30; ++i) w.tick({});
        w.checkpoint = {-2391, -52, -6293, 489};
        for (int k = 0; k < 2; ++k) {
            TbActor& p = w.actors[size_t(w.focus())];
            p.hp = 1, p.dmgIn = 1, p.f20 |= 0x4000000;  // an enemy hit (contact damage 1)
            p.hitTr = 3;
            bool died = false;
            for (int i = 0; i < 200; ++i) {
                w.tick({});
                died = died || (p.rec() && p.rec()->id == 382);
            }
            std::printf("  down %d: died %d A2 %02x focus %d lives %d hp %d/%d\n", k, died, w.GD[0xa2], w.focus(), w.GD[0xa0],
                        w.actors[0].hp, w.actors[1].hp);
            assert(died);
            if (k == 0) assert((w.GD[0xa2] & 1) && w.focus() == 1);
        }
        assert(w.GD[0xa0] == 2 && !(w.GD[0xa2] & 3) && w.actors[0].hp == 6 && w.actors[1].hp == 6);
    }
    std::puts("tb ARCA01 ok");
}

void allLevels(const fs::path& dir) {
    int played = 0, skipped = 0;
    std::set<std::string> unsupported;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) files.push_back(e.path());
    std::ranges::sort(files);
    for (const fs::path& p : files) {
        try {
            Level l = load(p);
            for (int i = 0; i < 120; ++i) {
                const uint16_t k[] = {PadUp, PadUp | PadCross, PadR1, PadLeft, PadSquare, PadL1, PadCircle, 0};
                l.w.tick(keys(k[(i / 15) % 8]));
            }
            unsupported.insert(l.w.unsupported.begin(), l.w.unsupported.end());
            ++played;
        } catch (const FormatError& e) {
            std::printf("  %s: not playable: %s\n", p.stem().string().c_str(), e.what());
            ++skipped;
        }
    }
    std::string list;
    for (const std::string& s : unsupported) list += " " + s;
    std::printf("tb levels: %d played 120 ticks, %d without collision/players; unsupported:%s\n", played, skipped, list.c_str());
    assert(played + skipped == int(files.size()) && played >= 50);
    for (const char* tag : {"W", "Z", "T", "[", "]", "^", "`"}) assert(!unsupported.contains(tag));  // whole tags ported
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    synthetic();
    const char* data = std::getenv("BBTIME_DATA");
    const fs::path dir = data ? findCi(data, {"tb", "Datas", "BZE"}) : fs::path();
    if (dir.empty()) {
        std::puts("BBTIME_DATA/tb not set: real-data part skipped");
        return 0;
    }
    arca(dir);
    allLevels(dir);
    return 0;
}
