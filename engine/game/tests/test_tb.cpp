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
// introSeen: ARCA01 with GD+0x11f = 1, as re-entered after its intro (the zone block skips template 425): the player
// traces were captured after ✕ had skipped the fly-over.
Level load(const fs::path& p, bool introSeen = true) {
    Level l{loadBze(p), {}, {}};
    l.scene = buildTbScene(l.bze, p.stem().string());
    addTbActors(l.scene, l.bze);
    l.w = TbWorld::load(l.bze, l.scene);
    if (introSeen && p.stem() == "ARCA01") l.w.GD[0x11f] = 1;
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

// ARCA01 intro = oracle local/wine/tbintro/{full,skip}/cam.csv (RUNNER=wine, /b28, counter 0x4d2708 = 2·tick + 1):
// trigger zone block 10 (any player) sets GD[0xa9] = 20 and runs director 264's block 394, which spawns template 425;
// its anim carries a camera node (0x464820) that holds the view from counter 3 until its last frame (1341), with the
// letterbox GD+0x9f 2 -> 16, then -16 -> 0. ✕ (raw edge) skips once its state is 90 ticks old.
void intro(const fs::path& dir) {
    auto fly = [](const TbWorld& w) {
        for (const TbActor& e : w.entities)
            if (e.id == 425 && !(e.f20 & 0x2000)) return true;
        return false;
    };
    {
        Level l = load(dir / "ARCA01.bze", false);
        TbWorld& w = l.w;
        struct Cam { int c; V3 eye, look; };
        const Cam want[] = {{3, {-170, -3900, 14779}, {-231, -3552, 13632}},   {201, {124, -3532, 8631}, {-319, -3109, 7599}},
                            {401, {-3208, -1519, 4254}, {-3225, -1353, 3065}}, {701, {-1970, -1059, -4325}, {-969, -840, -4948}},
                            {1001, {5185, -3439, -4259}, {6194, -3109, -3702}}, {1339, {12024, -3810, -2853}, {13018, -3295, -3283}}};
        size_t k = 0;
        int ended = -1;
        for (int i = 0; i < 740; ++i) {
            w.tick({});
            const int c = 2 * i + 1;
            // Follow camera 0x437800 after the node: spring-out, look settling 1 per tick, then the vertical band raises
            // the body to -349 (eye -369) once 20 ticks passed without a contact; every tail sample of the oracle is
            // matched exactly (local/shots/w9tb2/intro_tail_compare.csv), these rows are asserted.
            struct Tail { int c; V3 eye, look, body; };
            static const Tail tail[] = {{1345, {-4742, -299, -8236}, {-4107, -325, -7555}, {-4469, -310, -7943}},
                                        {1347, {-4793, -301, -8289}, {-4107, -324, -7555}, {-4520, -310, -7997}},
                                        {1365, {-4928, -308, -8431}, {-4107, -315, -7555}, {-4654, -310, -8139}},
                                        {1379, {-4931, -321, -8438}, {-4107, -310, -7555}, {-4658, -317, -8145}},
                                        {1401, {-4928, -342, -8434}, {-4107, -310, -7555}, {-4655, -331, -8142}},
                                        {1419, {-4928, -369, -8434}, {-4107, -310, -7555}, {-4655, -349, -8142}}};
            for (const Tail& t : tail)
                if (t.c == c) assert(w.cam.eye == t.eye && w.cam.look == t.look && w.cam.body == t.body);
            if (c == 1) assert(w.GD[0xa9] == 20 && w.GD[0x9f] == 2 && w.GD[0x11f] == 1 && w.music == 26 && fly(w));
            // Counter 1 = the load placement at Bugs' height (B+0x134 still 0 on the session's first level).
            if (c == 1) assert(w.cam.eye == (V3{-4176, -27, -7629}) && w.cam.look == (V3{-4107, -27, -7555}) && w.cam.body == w.cam.eye);
            if (c == 15) assert(w.GD[0x9f] == 16);
            if (k < std::size(want) && want[k].c == c) {
                auto near = [](const V3& a, const V3& b) { return std::abs(a.x - b.x) <= 1 && std::abs(a.y - b.y) <= 1 && std::abs(a.z - b.z) <= 1; };
                assert(near(w.cam.eye, want[k].eye) && near(w.cam.look, want[k].look));
                ++k;
            }
            if (ended < 0 && !fly(w)) ended = c, assert(int8_t(w.GD[0x9f]) == -14);
        }
        // Rest pose behind Bugs once the bars are gone (oracle eye (-4928, -369, -8434) from counter 1419).
        std::printf("  intro: camera node to counter %d, rest eye %d %d %d\n", ended, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z);
        assert(k == std::size(want) && ended == 1341 && w.GD[0x9f] == 0);
        assert(w.cam.eye == (V3{-4928, -369, -8434}) && w.cam.body == (V3{-4655, -349, -8142}));
    }
    {  // ✕ before the state is 90 ticks old does nothing; at counter 443 (oracle skip run) it ends the fly-over
        Level l = load(dir / "ARCA01.bze", false);
        TbWorld& w = l.w;
        for (int i = 0; i < 40; ++i) w.tick(keys(i == 30 ? PadCross : 0));
        assert(fly(w));
        while (w.ticks < 221) w.tick({});
        w.tick(keys(PadCross));
        assert(!fly(w) && int8_t(w.GD[0x9f]) == -14);
        // The same spring-out as the full run (oracle skip/cam.csv, every sample from 445 on matched exactly:
        // local/shots/w9tb2/skip_tail_compare.csv).
        for (int i = 0; i < 80; ++i) {
            w.tick({});
            if (2 * w.ticks - 1 == 447) assert(w.cam.eye == (V3{-4742, -299, -8236}) && w.cam.body == (V3{-4469, -310, -7943}));
        }
        assert(w.cam.eye == (V3{-4928, -369, -8434}) && w.cam.look == (V3{-4107, -310, -7555}));
    }
}

// ARCA01 pause menu = oracle local/wine/tbintro/pause_menu.png (pause.sh: ✕ skips the intro, Esc pauses): title
// "player 1 paused" (S[0x78] = 1 from the T4 pad-0 test), the menu, and the hint lines of text panel 6 (S75 / S32
// script S[0xb] = 0x1c: Enter icon "select", "'f4' exit menu" via op 0x14), Bugs / Taz health bars and lives shown.
void pauseMenu(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze", false);
    TbWorld& w = l.w;
    w.GD[0] = 28;  // ARCA01 (the app sets GD+0 on load; the HUD needs it)
    for (int i = 0; i < 161; ++i) w.tick(keys(i == 100 ? PadCross : 0));
    w.tick(keys(PadStart));
    // Text panel slides (0x4667a0, S75 starts panel 0 = the menu with record 8, panel 6 = the key hints with record
    // 2) from the tick S[4] turns 1 = oracle local/shots/w9tb2/pages_trace.csv (0x4d59a0 per tick, counter 749..795).
    std::vector<int> mx, hy;
    for (int i = 0; i < 30; ++i) {
        w.tick({});
        if (w.S[4] == 1 && mx.size() < 24) mx.push_back(w.panelPos(0)[0]), hy.push_back(w.panelPos(6)[1]);
    }
    const std::vector<int> wantX = {0, -520, -460, -400, -340, -280, -220, -160, -100, -40, 20, 62, 86, 92, 80, 56,
                                    20, -18, -38, -40, -24, 2, 10, 0};
    const std::vector<int> wantY = {0, 240, 250, 250, 240, 220, 200, 180, 160, 140, 120, 100, 80, 60, 40, 20, 0, -15,
                                    -15, -5, 0, 0, 0, 0};
    std::string px, py;
    for (size_t i = 0; i < mx.size(); ++i) px += std::format(" {}", mx[i]), py += std::format(" {}", hy[i]);
    std::printf("  pause panels: menu x%s\n  pause panels: hints y%s\n", px.c_str(), py.c_str());
    assert(mx == wantX && hy == wantY);
    std::string text;
    for (const OverlayGlyph& g : w.glyphs) text += char(g.ch);
    std::printf("  pause menu: '%s'\n", text.c_str());
    assert(w.S[9] == 1 && w.S[0x78] == 1);
    assert(text == "\x01select'f4'exitmenuplayer1pausedsavegameinventorylevelhintsoptionsquittogranwichquitgame");
    assert(w.hudSprites[0].shown && w.hudSprites[1].shown && w.hudSprites[2].shown && w.hudSprites[3].shown);
    assert(!w.unsupported.contains("S32") && !w.unsupported.contains("S75") && !w.unsupported.contains("R0x3b") && !w.unsupported.contains("T4"));
    // Inventory / era pages = oracle local/shots/w9tb2/pages_trace.csv (pages.sh: Esc, Down, Return, Right): the
    // menu sets S[4] = 3 (page mode), 0x461db0 slides the page scroll 0x4d5960 from 4800 to 4000 (S[4] 0xa3 until it
    // arrives), Right sets S[4] |= 4 -> era page S[5] = 1 (scroll 4000 -> 3200, 0xc3). Model 5's x per tick from the
    // mode change (0x462fd0) and the S[4] sequence are the oracle's tick for tick.
    w.tick(keys(PadDown));
    for (int i = 0; i < 10; ++i) w.tick({});
    w.tick(keys(PadCross));
    auto run = [&](const std::vector<int>& want, uint8_t mode) {
        std::vector<int> got;
        for (int i = 0; i < 90 && got.size() < want.size(); ++i) {
            if (got.empty() && w.S[4] != mode) { w.tick({}); continue; }
            got.push_back(w.hud[5].x);
            if (got.size() < want.size()) w.tick({});
        }
        std::string g;
        for (int v : got) g += std::format(" {}", v);
        std::printf("  page model 5 x:%s\n", g.c_str());
        assert(got == want);
    };
    run({818, 778, 738, 698, 658, 618, 578, 538, 498, 458, 418, 378, 338, 298, 258, 218, 178, 138, 98, 58, 18, -20, -46,
         -60, -62, -52, -32, -2, 32, 54, 64, 62, 50, 28, 4, -8, -8, 2, 20, 26, 20, 18}, 0xa3);
    w.tick({});
    assert(w.S[4] == 3);
    for (int i = 0; i < 30; ++i) w.tick({});
    w.tick(keys(PadRight));
    run({18, 42, 56, 60, 54, 38, 12, -24, -64, -104, -144, -184, -224, -264, -304, -344, -384, 376, 336, 296, 256, 216,
         176, 136, 96, 56, 16, -12, -28, -32, -24, -6, 21, 36, 39, 30, 15, 12}, 0xc3);
    w.tick({});
    text.clear();
    for (const OverlayGlyph& g : w.glyphs) text += char(g.ch);
    assert(w.S[4] == 3 && w.S[5] == 1 && w.hud[5].x == 18 && w.hud[6].shown && w.hud[11].shown && w.hud[12].shown);
    assert(text.find("aztecera") != std::string::npos);
    {
        Level h = load(dir / "ARCA01.bze", false);
        TbWorld& v = h.w;
        v.GD[0] = 28;
        for (int i = 0; i < 161; ++i) v.tick(keys(i == 100 ? PadCross : 0));
        v.tick(keys(PadStart));
        for (int i = 0; i < 45; ++i) v.tick({});
        for (uint16_t k : {uint16_t(PadDown), uint16_t(0), uint16_t(PadDown), uint16_t(0), uint16_t(PadCross)})
            for (int i = 0; i < 12; ++i) v.tick(keys(i < 3 ? k : 0));
        for (int i = 0; i < 70; ++i) v.tick({});
        // Level hints (oracle local/shots/w9tb2/o_hints.png, o_hints_right.png): S[4] 0, hint S[0x70] of S[0x71]; block
        // 305 draws text 109 (▸ '{' at (210, -75)) while S[0x70] < S[0x71] (R 0x2d) and 107 (◂ '}' right-aligned at
        // -210) from the second hint on.
        auto arrows = [&] {
            std::string t, a;
            for (const OverlayGlyph& g : v.glyphs) {
                t += char(g.ch);
                if (g.y == -75 && (g.ch == '{' || g.ch == '}')) a += std::format("{}{}", char(g.ch), g.x);
            }
            return std::pair{t, a};
        };
        const auto [t1, a1] = arrows();
        std::printf("  level hints: arrows '%s'\n", a1.c_str());
        assert(v.S[4] == 0 && t1.find("maybethegeniesarebeingheld") != std::string::npos && a1 == "{210");
        for (int i = 0; i < 12; ++i) v.tick(keys(i < 3 ? PadRight : 0));
        for (int i = 0; i < 40; ++i) v.tick({});
        const auto [t2, a2] = arrows();
        std::printf("  level hints (right): arrows '%s'\n", a2.c_str());
        assert(t2.find("breakallthevases") != std::string::npos && a2.find("{210") != std::string::npos && a2.find('}') != std::string::npos);
    }
}

// Music = action 126 at runtime (0 at load, 0x407480): oracle local/wine/tbintro/music_{TRQ02,VIQ02}.csv (idle after a
// /bze: boot, DAT_0063b9ec per tick). TRQ02's zone block 10 picks 46 (Track45, missing -> Track14) on the first visit
// (GD+0x13c unset), 42 afterwards; VIQ02's director sets 22, then entity 317 switches to 8 at counter 55 (tick 27).
void music(const fs::path& dir) {
    Level t = load(dir / "TRQ02.bze");
    assert(t.w.music == 0);
    t.w.tick({});
    assert(t.w.music == 46 && t.w.GD[0x13c] == 1);
    Level v = load(dir / "VIQ02.bze");
    int switched = -1;
    for (int i = 0; i < 40; ++i) {
        v.w.tick({});
        if (i == 5) assert(v.w.music == 22);
        if (switched < 0 && v.w.music == 8) switched = i;
    }
    std::printf("  music: TRQ02 46, VIQ02 22 -> 8 at tick %d\n", switched);
    assert(switched == 27);
}

void arca(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze");
    TbWorld& w = l.w;
    assert(w.music == 0 && w.edgeMask == 0xd0);  // music 0 until the level logic runs (0x407480); pad edge mask
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
        f.w.cam.prevEye = f.w.cam.eye = {-4782, -428, -8547}, f.w.cam.prevLook = f.w.cam.look = {-4085, -310, -7555};
        f.w.cam.prevBody = f.w.cam.body = {-4553, -389, -8221};
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
            assert(r.w.A4 == 0x10);  // menu "2 Players" (0x41a6e0(2) -> 0x4370c0): bit 0x10 only
            r.w.tick({}, {});
            assert(r.w.A4 == 0x12);  // switch rule (0x45eee0) adds 0x02 each 2P tick = runtime A4 0x12 (F_2p_granny.csv)
            for (int i = 0; i < 200; ++i)
                r.w.tick(keys(i % 50 < 30 ? PadUp : PadCross), keys(i % 40 < 20 ? PadLeft | PadUp : 0));
            return std::pair{r.w.actors[0].pos, r.w.actors[1].pos};
        };
        assert(run() == run());
    }
    {  // ARCA01 entities: guard 267 (template 265) patrols, spots a player in front within 2500 and attacks (runtime
       // capture local/wine/tbnpc/B_guard_attack.pkl.gz: 357 522 -> 497 504 502 501 364 435 502 501 364; Bugs hurt by
       // the thrown template 401, health 6 -> 5).
        // Seed 1 (the CRT default) picks the 401 variant that misses first (as tbrand); seed 66 runs B's sequence (522,
        // the throw that hits, 435), whose flight is checked per tick.
        TbActor* g = nullptr;
        for (const uint32_t seed0 : {1u, 66u}) {
            Level f = load(dir / "ARCA01.bze");
            TbWorld& w = f.w;
            w.rng.seed = seed0;
            for (TbActor& e : w.entities)
                if (e.id == 267) g = &e;
            assert(g && g->hp == 5 && g->behaviour == 14 && (g->view & 0x41a) == 0x41a && g->range == 7500);
            for (int i = 0; i < 30; ++i) w.tick({});
            TbActor& b = w.actors[0];
            b.pos = {g->pos.x + 1200, g->pos.y - 30, g->pos.z};  // as the capture: Bugs poked 1200 in front of it
            g->yaw = 1024;                                         // facing +x
            std::vector<std::pair<uint16_t, int>> seen;            // guard state, entry tick
            int thrown = -1, gone = -1, hit = -1, flight = -1, hitS7a = -1;
            std::vector<V3> path;                                  // the thrown 401 per tick from its spawn
            for (int i = 0; i < 300; ++i) {
                w.tick({});
                if (g->rec() && (seen.empty() || seen.back().first != g->rec()->id)) seen.emplace_back(g->rec()->id, i);
                const TbActor* p = nullptr;
                for (const TbActor& e : w.entities)
                    if (e.id == 401) p = &e;
                if (p && thrown < 0) thrown = i, flight = p->rec() ? p->rec()->id : -1;
                if (p && gone < 0 && thrown >= 0) path.push_back(p->pos);
                if (!p && thrown >= 0 && gone < 0) gone = i;
                if (hit < 0 && b.hp < 6) hit = i, hitS7a = w.S[0x7a];
            }
            std::string s;
            for (auto [id, at] : seen) s += std::format(" {}@{}", id, at);
            std::printf("  guard 267 states:%s; 401 (%d) thrown @%d, gone @%d, Bugs hurt @%d (health %d)\n", s.c_str(), flight,
                        thrown, gone, hit, b.hp);
            // Capture B per tick: 364 @55, its block 364 spawns 401 on anim frame 7 (@71) and sends it event 0x100/0x101
            // (rand) -> 394/395 (the first-tick event 2 only applies without a state). B: 395 (flagsB 0x20000) follows
            // its TOD 0xa00 point (0x45643b carry +0x58: next position = last tick's attach point) out and back to the
            // hand and removes itself on its anim's last frame (V 0x400) @128; hitting Bugs ran its program (block 397:
            // S[0x7a] = 1), so on 364's last frame the guard picks event 300 -> 435 @133, 502 @181, 501 @182, 364 @188.
            // The patrol state before the notice (B 522, local/wine/tbrand 521) and the 401 variant are rand() picks of
            // the shared stream: tbrand (rand() callers traced) throws a miss first, then 364's end -> 502 @+78, 501
            // @+79, 364 @+85, and the second throw hits. A 502 before the first 501 is there in B, not in tbrand.
            std::vector<uint16_t> ids;
            for (auto [id, at] : seen) ids.push_back(id);
            auto m = std::ranges::find(ids, 497);
            assert(m != ids.end() && m - ids.begin() >= 2 && (m[-1] == 521 || m[-1] == 522) && m[-2] == 357);
            size_t k = size_t(m - ids.begin()) + 1;
            assert(ids[k++] == 504);
            if (ids[k] == 502) ++k;
            assert(ids[k] == 501 && ids[k + 1] == 364);
            const int t364 = seen[k + 1].second;
            const auto at = [&](size_t j) { return seen[k + 1 + j].second - t364; };
            assert(thrown - t364 == 16 && gone - t364 == 73 && at(1) == 78 && flight == (seed0 == 1 ? 394 : 395));
            if (flight == 395) {
                assert(ids[k + 2] == 435 && ids[k + 3] == 502 && at(2) == 126 && ids[k + 4] == 501 && at(3) == 127 &&
                       ids[k + 5] == 364 && at(4) == 133);
                // Flight (B, guard yaw 1006; ticks 71..127, the removal tick unseen): per-tick dy exact, dx along the
                // facing within 1 (ours yaw 1024: 104 where B has 104 · sin(1006) = 103.96 truncated).
                const int dyB[] = {0, 0, -33, 6, 5, 4, 4, 3, 2, 1, 0, 0, 0, -1, -2, -3, -3, -4, -5, -5, -6, -6, -6, -6, -6, -6,
                                   -6, -6, -6, -6, -6, -5, -5, -5, -4, -4, -3, -3, -2, -1, 0, 0, 1, 2, 2, 4, 5, 6, 7, 8, 10, 11,
                                   13, 14, 8, 0};
                const int dxB[] = {0, 0, 103, 110, 105, 100, 96, 91, 86, 81, 76, 72, 67, 63, 58, 54, 50, 45, 41, 37, 33, 29, 25,
                                   21, 17, 12, 8, 4, 1, -2, -7, -13, -19, -24, -29, -33, -38, -42, -46, -50, -54, -57, -60, -63,
                                   -66, -69, -71, -73, -74, -76, -77, -79, -79, -80, -80, -19};
                assert(path.size() == std::size(dyB) + 1);
                for (size_t j = 0; j < std::size(dyB); ++j)
                    assert(path[j + 1].y - path[j].y == dyB[j] && std::abs(path[j + 1].x - path[j].x - dxB[j]) <= 1);
                assert(hit > thrown && hit < gone);  // B: health 6 -> 5 @83; where Bugs stands sets the tick
            } else {
                assert(ids[k + 2] == 502 && ids[k + 3] == 501 && at(2) == 79 && ids[k + 4] == 364 && at(3) == 85);
            }
            // A later throw hits too (B: -> 4 @221; tbrand: the second); S[0x7a] = 1 from block 397 on the hit (later
            // S 7 rand() picks reuse the slot).
            assert(hit > 0 && b.hp <= 5 && hitS7a == 1);
            // Melee (capture C: Bugs poked next to it -> 502 394, Bugs hurt state 208, health -1, 60-tick flash).
            b.pos = {g->pos.x + rotY(g->yaw, 0, 0, 300).x, g->pos.y - 30, g->pos.z + rotY(g->yaw, 0, 0, 300).z};
            const int bugsHp = b.hp;
            int hurt = -1;
            for (int i = 0; i < 200 && hurt < 0; ++i) {
                w.tick({});
                if (b.hp < bugsHp) hurt = i;
            }
            std::printf("  melee: Bugs health %d after %d ticks (state %u, guard %u)\n", b.hp, hurt, b.rec()->id, g->rec()->id);
            assert(b.hp == bugsHp - 1 && w.GD[0xa5] == b.hp && (b.f28 & 0x200) && w.flash[0] >= 0x40);
        }
        // Taz spin into the facing guard (oracle local/shots/w9tb4/F9_taz_spin_rand.pkl.gz, RUNNER=wine, script
        // local/wine/tbnpc/spin_rand_capture5.py: every rand() (0x46afe0) call with its tick and caller, tbrand/
        // rand_hook2.s, plus guard / Taz entity bytes per tick): Taz poked 900 in front of the guard; on its 502 tick
        // with Bugs and Taz idle (8), Taz poked 173 in front and both +0x82 set to 0; □ held 40 ticks; the traced seed
        // replayed per tick. The spin reaches the guard through Taz's TOD 0xa00 probe once the spin anim set it (state
        // entry clears +0x84, so the idle anim's hand point is not used on the first spin tick), not through mover
        // contacts; the hit runs the guard's block 377 -> event 0x12a -> 433, which is not vulnerable, so the guard
        // keeps its 5 health; its melee 394 then hurts Taz (190). 394's last frame draws S[0x1fe] = rand() % 2 (block
        // 404): 0 -> 435, 1 -> 500; 364 throws 401, which hurts Taz again.
        Level h = load(dir / "ARCA01.bze");
        TbWorld& w2 = h.w;
        for (TbActor& e : w2.entities)
            if (e.id == 267) g = &e;
        for (int i = 0; i < 30; ++i) w2.tick({});
        w2.tick(keys(PadR1));
        w2.tick({});
        TbActor& t = w2.actors[1];
        TbActor& bb = w2.actors[0];
        g->yaw = 1024;
        t.pos = {g->pos.x + 900, g->pos.y - 30, g->pos.z};
        auto idle502 = [&] { return g->rec()->id == 502 && t.rec()->id == 8 && bb.rec()->id == 8; };
        for (int i = 0; i < 2000 && !idle502(); ++i) w2.tick({});
        assert(idle502());
        const int hp0 = g->hp, tazHp0 = t.hp;
        g->yaw = 781;  // F9: the guard had turned to 781 toward Taz by its 502 tick (ours still faces 1024)
        const V3 front = rotY(g->yaw, 0, 0, 173);
        t.pos = {g->pos.x + front.x, g->pos.y, g->pos.z + front.z};  // F9: (161, 0, 63) from the guard
        t.stateTicks = bb.stateTicks = 0;
        const char* draws =  // rand() calls per tick (F9), from seed 0x26c3f4cf after the 502 tick
            "000002000000002000000000000000011120302000001010100000110000003000000010000000010011004300110011001200000000"
            "233001010000000000000000012000000000122111111111124342222222232222222322231000000000000000002000000000001111"
            "111111200000100110020010";
        assert(std::strlen(draws) == 240);
        uint32_t seed = 0x26c3f4cf;
        std::vector<uint16_t> gs, ts, bs;
        for (int i = 0; i < 240; ++i) {
            w2.rng.seed = seed;
            w2.tick(keys(i < 40 ? PadSquare : 0));
            for (int k = draws[i] - '0'; k > 0; --k) seed = seed * 214013u + 2531011u;
            gs.push_back(g->rec()->id), ts.push_back(t.rec()->id), bs.push_back(bb.rec()->id);
        }
        auto runs = [](std::initializer_list<std::pair<uint16_t, int>> r) {
            std::vector<uint16_t> v;
            for (auto [id, n] : r) v.insert(v.end(), size_t(n), id);
            return v;
        };
        // The guard's draws land in its own register (+0x126 = S[0x1fe], 0x4d269e). Draw order within a tick (the
        // capture's callers): the focus Taz, then Bugs (idle fidget chance draws 0x45a6df, picks 0x430f76), then the
        // others in list order (entity 270, the guard, the directors' S picks and particle effects V 0x200000), all
        // on the shared stream. The knock-back slide must stop dead (ground grip 0x443f40 rounds only a positive
        // product up), else Taz drifts 1 per tick and the guard's 401 hits him a tick late. The 900 poke resets the
        // camera as in F3_taz_spin_cam.pkl.gz (snap at |body - focus|² >= 0x5f5e11).
        const auto gWant = runs({{394, 2}, {433, 17}, {502, 1}, {394, 24}, {435, 48}, {502, 1}, {501, 6}, {364, 78},
                                 {500, 56}, {502, 1}, {501, 6}});
        const auto tWant = runs({{135, 6}, {136, 12}, {140, 5}, {190, 13}, {10, 12}, {20, 6}, {21, 45}, {32, 2}, {17, 5},
                                 {18, 14}, {190, 13}, {10, 36}, {20, 6}, {21, 25}, {32, 2}, {17, 5}, {18, 28}, {19, 5}});
        const auto bWant = runs({{8, 34}, {26, 4}, {27, 17}, {28, 6}, {8, 26}, {23, 6}, {24, 20}, {25, 8}, {8, 39}, {17, 4},
                                 {18, 17}, {19, 4}, {8, 37}, {29, 4}, {30, 14}});
        assert(gWant.size() == 240 && tWant.size() == 240 && bWant.size() == 240);
        auto show = [](const std::vector<uint16_t>& v) {
            std::string r;
            for (size_t k = 0; k < v.size(); ++k)
                if (!k || v[k] != v[k - 1]) r += std::format(" {}@{}", v[k], k);
            return r;
        };
        const bool gOk = gs == gWant, tazOk = ts == tWant, bOk = bs == bWant;
        std::printf("  Taz spin: guard health %d -> %d, Taz %d; guard%s (%s), Taz%s (%s), Bugs%s (%s)\n", hp0, g->hp, t.hp,
                    show(gs).c_str(), gOk ? "match" : "DIFFER", show(ts).c_str(), tazOk ? "match" : "DIFFER",
                    show(bs).c_str(), bOk ? "match" : "DIFFER");
        assert(gOk && tazOk && bOk && g->hp == hp0 && t.hp == tazHp0 - 2);
    }
    {  // item pickup by touch = oracle local/wine/tbnpc/H_item_pickup.pkl.gz (RUNNER=wine, item_capture.py there): Bugs
       // poked 150 beside item gdBit 473 (its box comes from TOD packet 0x900, (-107,0,-17)-(107,-215,19) r 240, not
       // tags 0x1b/0x1c). Next tick: Bugs' contact gives it +0x20 0x80, its program (global block 10026) runs at once:
       // GD+0x5a/+0x6c/+0x84 = 1, collected bit GD+0x22f 2, S[0xa8] 1, state 2nd record, +0x82 1; 31 ticks later the
       // 3rd record, rising 150 per tick from the next tick, removed (+0x20 0x2000) 7 ticks after that at y -1105.
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        TbActor* it = nullptr;
        for (TbActor& e : w.entities)
            if (e.gdBit == 473) it = &e;
        assert(it);
        for (int i = 0; i < 30; ++i) w.tick({});
        std::printf("  item 473 box %d %d %d %d %d %d r %u\n", it->box[0], it->box[1], it->box[2], it->box[3], it->box[4],
                    it->box[5], it->radius);
        assert(it->box == (std::array<int16_t, 6>{-107, 0, -17, 107, -215, 19}) && it->radius == 240);
        const auto st = [&]() { return int(it->state); };
        const int s0 = st();
        w.actors[0].pos = {it->pos.x + 150, -10, it->pos.z};
        std::array<uint8_t, 0x300> gd0 = w.GD;
        const uint8_t s168 = w.S[0xa8];
        w.tick({});
        std::printf("  item 473: state %d -> %d ticks %u GD", s0, st(), it->stateTicks);
        for (size_t i = 0; i < gd0.size(); ++i)
            if (gd0[i] != w.GD[i]) std::printf(" +%#zx %u->%u", i, gd0[i], w.GD[i]);
        std::printf(" S[0xa8] %u->%u state id %u\n", s168, w.S[0xa8], it->rec()->id);
        assert(st() == s0 + 1 && it->stateTicks == 1 && w.GD[0x5a] == 1 && w.GD[0x6c] == 1 && w.GD[0x84] == 1 &&
               w.GD[0x22f] == 2 && w.S[0xa8] == 1);
        std::vector<int> ys;
        int removed = -1;
        for (int i = 1; i <= 40 && removed < 0; ++i) {
            w.tick({});
            if (i == 31) assert(st() == s0 + 2);
            if (i < 31) assert(st() == s0 + 1);
            ys.push_back(it->pos.y);
            if (it->f20 & 0x2000) removed = i;
        }
        std::printf("  item 473 removed %d ticks after the pickup at y %d\n", removed, it->pos.y);
        assert(removed == 38 && it->pos.y == -1105 && ys[31] == -205 && ys[36] == -955);
    }
    {  // HUD coin counter (0x463170 slot 2 = GD+0x58+GD[0xa9], slide record 4): the item-473 pickup slides the coin and
       // its digits in from x+120 with overshoot, holds 60 ticks, slides out = oracle local/wine/tbnpc/hud/slide.csv
       // (RUNNER=wine, slide_capture.py: slide x per tick from the pickup tick; 999 = off screen, flag 0x200 clear).
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        w.GD[0] = 28;     // ARCA01 (the app sets GD+0 on load)
        TbActor* it = nullptr;
        for (TbActor& e : w.entities)
            if (e.gdBit == 473) it = &e;
        for (int i = 0; i < 30; ++i) w.tick({});
        assert(w.GD[0xa9] == 20);  // the intro trigger zone's block 10 (chunk 09) set the level's counter slot
        assert(!w.hud[2].shown && !w.hudSprites[11].shown);  // nothing at idle
        w.actors[0].pos = {it->pos.x + 150, -10, it->pos.z};
        std::vector<int> xs, digit;
        for (int i = 0; i < 100; ++i) {
            w.tick({});
            xs.push_back(w.hud[2].shown ? w.hud[2].x - 215 : 999);
            if (i == 40) digit = {w.hudSprites[11].shown, w.hudSprites[11].frame, w.hudSprites[11].x, w.hudSprites[11].y};
        }
        const int in[] = {86, 52, 18, -16, -41, -57, -64, -62, -55, -43, -26, -4, 16, 27, 29, 22, 10, -4, -9, -5, 1, 2, -3};
        const int out[] = {-12, -19, -21, -18, -10, 3, 21, 44, 72, 105};
        std::vector<int> want(100, 999);
        std::ranges::copy(in, want.begin());
        std::fill(want.begin() + 23, want.begin() + 84, 0);  // 60-tick hold (0x3c) once at rest
        std::ranges::copy(out, want.begin() + 84);
        std::printf("  hud coin: slide in %d ticks, out at %d, gone at %d; digit %d frame %d at (%d, %d)\n", 23,
                    int(std::ranges::find(xs, -12) - xs.begin()), int(std::ranges::find(xs, 999) - xs.begin()), digit[0], digit[1],
                    digit[2], digit[3]);
        assert(xs == want);
        assert(digit[0] && digit[1] == 1);  // GD+0x6c = 1
    }
    {  // level-change fade (0x401910 / 0x4018a0, 0x464ef0) = oracle local/wine/tbnpc/hud/fade_trace.csv (counter +2 per
       // tick): request tick fadeOut(2, 0x100) + one step = level 3840; the world keeps ticking, level -256 per tick to 0
       // (dark 3840) on the 16th frame, then the load; there fadeIn(1, 200): dark 4096, 3896 .. 96, mode 0 at counter 45.
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        for (int i = 0; i < 5; ++i) w.tick({});
        w.fadeOut(2, 0x100);
        w.fadeTick();
        std::vector<int> lv{w.fade.level}, dk{w.fade.dark};
        while (w.fade.level > 0) w.tick({}), lv.push_back(w.fade.level), dk.push_back(w.fade.dark);
        assert(lv.size() == 16 && lv.front() == 3840 && lv.back() == 0 && dk.front() == 0 && dk.back() == 3840);
        for (size_t k = 0; k < lv.size(); ++k) assert(lv[k] == 3840 - 256 * int(k) && dk[k] == 256 * int(k));
        Level g = load(dir / "ARCA01.bze");
        g.w.fadeIn(1, 200);
        std::vector<int> in;
        for (int i = 0; i < 22; ++i) g.w.tick({}), in.push_back(g.w.fade.dark);
        for (int k = 0; k < 21; ++k) assert(in[size_t(k)] == 4096 - 200 * k);  // counters 3 .. 43
        assert(in[21] == 0 && g.w.fade.mode == 0 && g.w.fade.level == 4096);  // counter 45
        std::puts("  fade: out 16 frames 3840..0, in 4096..96 then mode 0 (oracle)");
    }
    {  // RETRY (level 5) = oracle local/wine/tbnpc/retry/G0..G2: Daffy's close-up (instance 0x77: states 3 -> 348 ->
       // 394) spawns the controller 0xe3 (its register S[0x1fe] is 0: the players' block left 6 in theirs), which fades
       // out, respawns Daffy, fades in and shows "GAME OVER / DO YOU WANT TO CONTINUE? YES NO" (S30 0x7d / 0x7e); left /
       // right set S[0x7a] 1 / 2, ✕ on YES with era GD+0xf6 = 0 loads GRANNY (4).
        Level f = load(dir / "RETRY.bze");
        TbWorld& w = f.w;
        w.GD[0] = 5;
        w.fadeIn(1, 200);
        int shown = -1;
        for (int i = 0; i < 400 && shown < 0; ++i) {
            w.tick({});
            if (!w.glyphs.empty()) shown = i;
        }
        std::string text;
        for (const OverlayGlyph& g : w.glyphs) text += char(g.ch);
        std::printf("  RETRY prompt @%d: '%s' S[0x7a] %u fade %u\n", shown, text.c_str(), w.S[0x7a], w.fade.mode);
        assert(shown > 0 && text == "gameoverdoyouwanttocontinue?yesno" && w.S[0x7a] == 1);
        auto press = [&](uint16_t b) { w.tick(keys(b)), w.tick({}), w.tick({}); };
        press(PadRight);
        assert(w.S[0x7a] == 2);
        press(PadLeft);
        assert(w.S[0x7a] == 1 && !w.restart);
        press(PadCross);
        assert(w.restart && w.nextLevel == 4 && TbWorld::levelName(4) == "GRANNY");
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
        // No life left: GD+0 = 5 and the reload flag 0x4d593a (0x44ea50); the app then loads RETRY (level 5), whose
        // "Game over / continue?" screen the original shows (oracle: local/wine/tbnpc/retry/G0..G2, YES -> GRANNY).
        w.GD[0xa0] = 0;
        for (int k = 0; k < 2 && !w.restart; ++k) {
            TbActor& p = w.actors[size_t(w.focus())];
            p.hp = 1, p.dmgIn = 1, p.f20 |= 0x4000000, p.hitTr = 3;
            for (int i = 0; i < 200 && !w.restart; ++i) w.tick({});
        }
        assert(w.restart && w.nextLevel == 5 && TbWorld::levelName(5) == "RETRY" && w.GD[0xa0] == 3);
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
    intro(dir);
    music(dir);
    pauseMenu(dir);
    arca(dir);
    allLevels(dir);
    return 0;
}
