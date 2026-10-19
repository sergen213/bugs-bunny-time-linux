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
    {  // call friend = runtime trace D: spot (326, 148) from Bugs for camYaw ~490; refused at cooldown 19, ok at 45
        Level f = load(dir / "ARCA01.bze");
        TbWorld& c = f.w;
        for (int i = 0; i < 60; ++i) c.tick({});
        const V3 b = c.actors[0].pos;
        c.tick(keys(PadL1));
        const V3 t = c.actors[1].pos;
        std::printf("  call friend: %d cooldown %u offset %d %d (trace 326 148)\n", c.lastCallFriend, c.cooldown, t.x - b.x, t.z - b.z);
        assert(c.lastCallFriend == 1 && c.cooldown == 0 && std::abs(t.x - b.x - 326) <= 2 && std::abs(t.z - b.z - 148) <= 2);
        for (int i = 0; i < 19; ++i) c.tick({});
        c.tick(keys(PadL1));
        assert(c.lastCallFriend == -1 && c.cooldown == 20);
        for (int i = 0; i < 30; ++i) c.tick({});
        c.tick(keys(PadL1));
        assert(c.lastCallFriend == 1);
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
    std::puts("tb ARCA01 ok");
}

void allLevels(const fs::path& dir) {
    int played = 0, skipped = 0;
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
            ++played;
        } catch (const FormatError& e) {
            std::printf("  %s: not playable: %s\n", p.stem().string().c_str(), e.what());
            ++skipped;
        }
    }
    std::printf("tb levels: %d played 120 ticks, %d without collision/players\n", played, skipped);
    assert(played + skipped == int(files.size()) && played >= 50);
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
