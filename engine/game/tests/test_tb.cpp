// TB gameplay checks (GAMEPLAY_TB.md): collision query on a synthetic one-quad mesh (tb_ground.py selftest), and with
// BBTIME_DATA real ARCA01 replays — Bugs jump apex 550 @ tick 10 / Taz 490 @ 10 (§2.3), run speed 50 (§2.1), R1 switch
// with the camera one tick late (§4.1), call-friend 45-tick cooldown (§4.2) — plus every level for 120 ticks.
#include "bb/game/tb_world.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
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
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; };
    for (int i = 0; !TbWorld::levelName(i).empty(); ++i)  // GD+0 = the level, as the app's load (0x4018a0)
        if (lower(std::string(TbWorld::levelName(i))) == lower(p.stem().string())) l.w.GD[0] = uint8_t(i);
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
    {  // "Quit to Granwich" -> YES = oracle local/wine/tbintro/w13_quit_granwich.sh (w13_quit_trace.csv: S[0x12] 0x3a,
       // ✕ on YES sets GD+0 = 4 and 0x4d593a = 1 on the same tick, S[9] 0; the reload is GRANNY).
        Level h = load(dir / "ARCA01.bze", false);
        TbWorld& v = h.w;
        v.GD[0] = 28;
        for (int i = 0; i < 161; ++i) v.tick(keys(i == 100 ? PadCross : 0));
        v.tick(keys(PadStart));
        for (int i = 0; i < 45; ++i) v.tick({});
        // Pads as the oracle script's: Down ×4 12 ticks apart, Return, then (screenshot, trace start) ~2 s before Left,
        // when the prompt has finished sliding in; until then its YES/NO selection takes no input.
        for (uint16_t k : std::initializer_list<uint16_t>{PadDown, 0, PadDown, 0, PadDown, 0, PadDown, 0, PadCross})
            for (int i = 0; i < 12; ++i) v.tick(keys(i < 3 ? k : 0));
        for (int i = 0; i < 60; ++i) v.tick({});
        for (int i = 0; i < 18; ++i) v.tick(keys(i < 3 ? PadLeft : 0));
        assert(!v.restart && v.S[0x12] == 0x3a);
        // ✕ steps the menu to state 321, whose block waits until its +0x82 passes 5 before V 0x1000 (GD+0 = 4); the
        // next frame's head (0x4014c0) unpauses before the fade-out.
        for (int i = 0; i < 10 && !v.restart; ++i) v.tick(keys(i < 3 ? PadCross : 0));
        v.reloadStart();
        std::printf("  quit to Granwich: restart %d next level %d\n", v.restart, v.nextLevel);
        assert(v.restart && v.nextLevel == 4 && v.S[9] == 0);
    }
}

// Line of sight (W 0x82000000, 0x45a660 -> 0x4463e0 query 9 with the actor pass): no PC level's logic uses it (all
// W items of the 58 levels' entity, zone and animation blocks), so this checks the actor pass on AZBOSS02, whose four
// active entities have flags10 & 8 (the query's mask) after 60 ticks: rays from Bugs to them and twice as far.
void lineOfSight(const fs::path& dir) {
    Level h = load(dir / "AZBOSS02.bze");
    TbWorld& v = h.w;
    for (int i = 0; i < 60; ++i) v.tick({});
    TbActor& bugs = v.actors[0];
    const V3 s{bugs.pos.x, bugs.pos.y - 150, bugs.pos.z};
    int boxes = 0;
    for (TbActor& e : v.entities) {
        if (!e.active || !(e.flags10 & 8)) continue;
        const V3 d{e.pos.x - s.x, e.pos.y - 50 - s.y, e.pos.z - s.z}, d2{2 * d.x, 2 * d.y, 2 * d.z};
        V3 da = d, db = d2, dc = d2;
        const auto a = v.rayHit(bugs, s, da, 5, 9), b = v.rayHit(bugs, s, db, 5, 9), c = v.rayHit(bugs, s, dc, 5, 8);
        auto name = [&](const std::optional<TbActor*>& r) { return !r ? "free" : !*r ? "polygon" : *r == &e ? "it" : "other"; };
        std::printf("  line of sight: id %u r %u f0 %x: to %s, past %s, without actors %s\n", e.id, e.radius, e.flags0, name(a), name(b), name(c));
        auto len2 = [](const V3& v) { return int64_t(v.x) * v.x + int64_t(v.y) * v.y + int64_t(v.z) * v.z; };
        assert(len2(dc) < len2(d2) && (!a || len2(da) <= len2(d)));  // 0x4463e0 writes the hit delta back
        assert(c && !*c);  // the polygons alone: the wall behind
        if (e.radius) {    // the three oriented boxes (flags0 0x40000) take the ray before that wall
            assert(a && *a == &e && b && *b == &e && (e.flags0 & 0x40000));
            ++boxes;
        } else {           // radius 0 is never listed (0x448920): a box in front takes its ray
            assert(a && *a && *a != &e);
        }
    }
    assert(boxes == 3);
}

// Level select = oracle local/wine/tbintro/w13_levelselect.sh (RUNNER=wine, ARCA01): "RULETHEEARTH" arms the pad
// cheats (0x4d095c 1), ○○○○□□□ sets GD+0x8c 1; in the pause menu Up wraps to the eighth entry, ✕ opens template 0x1d
// state 312 (w13_ls_page.png) with S[0x21] 1, Down 2, ✕ sets GD+0 = 7, 0x4d593a 1 and S[9] 2 on one tick.
void levelSelect(const fs::path& dir) {
    Level h = load(dir / "ARCA01.bze", false);
    TbWorld& v = h.w;
    v.GD[0] = 28;
    for (int i = 0; i < 161; ++i) v.tick(keys(i == 100 ? PadCross : 0));
    std::array<bool, 256> held{};
    for (const uint8_t k : {19, 22, 38, 18, 20, 35, 18, 18, 30, 19, 20, 35}) {
        held[k] = true, v.cheatKeys(held);
        held[k] = false, v.cheatKeys(held);
    }
    assert(v.cheats.on);
    for (const uint16_t k : std::initializer_list<uint16_t>{PadCircle, PadCircle, PadCircle, PadCircle, PadSquare, PadSquare, PadSquare})
        for (int i = 0; i < 8; ++i) v.tick(keys(i < 3 ? k : 0));
    assert(v.GD[0x8c] == 1 && v.GD[0xab] == 0xff);
    v.tick(keys(PadStart));
    for (int i = 0; i < 45; ++i) v.tick({});
    // Pads as the oracle script's: Up, 600 ms, Return, then 2.5 s (the page slides in) before Down, 600 ms, Return.
    for (const uint16_t k : std::initializer_list<uint16_t>{PadUp, 0, PadCross})
        for (int i = 0; i < 12; ++i) v.tick(keys(i < 3 ? k : 0));
    for (int i = 0; i < 64; ++i) v.tick({});
    const TbActor* menu = nullptr;
    for (const TbActor& e : v.entities)
        if (e.rec() && e.rec()->id == 312) menu = &e;
    std::string text;
    for (const OverlayGlyph& g : v.glyphs) text += char(g.ch);
    std::printf("  level select: page %d S[0x21] %d '%s'\n", menu != nullptr, v.S[0x21], text.substr(0, 60).c_str());
    assert(menu && v.S[0x21] == 1 && text.find("azca01azcasg01") != std::string::npos && text.find("granwich") != std::string::npos);
    for (int i = 0; i < 18; ++i) v.tick(keys(i < 3 ? PadDown : 0));
    assert(v.S[0x21] == 2);
    for (int i = 0; i < 40 && !v.restart; ++i) v.tick(keys(i < 3 ? PadCross : 0));
    // The menu's event 199 (after V 0x1000 GD+0 = 7) resumes, S[9] = 2, during the fade's world ticks (0x401910 runs
    // 0x4011c0 16 times without the 0x4014c0 pause head): the oracle sample with 0x4d593a 1 reads S[9] 2.
    v.reloadStart();
    while (v.fade.level > 0) v.tick({});
    std::printf("  level select: restart %d next level %d S[9] %d\n", v.restart, v.nextLevel, v.S[9]);
    assert(v.restart && v.nextLevel == 7 && v.S[9] == 2);
}

// Language (GD+0x93, 0x4347c0 clamps > 6 to 0): the language section (EN 6, FR 5, DE..NL 7..11) and entry of the
// 7 × {off, size} speech (0x4b / 0x4c) and text (0x48) tags. A carried GD keeps its language over `language`.
void language(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze", false);
    auto pauseText = [&](uint8_t lang, const std::array<uint8_t, 0x300>* gd) {
        TbWorld w = TbWorld::load(l.bze, l.scene, gd, lang);
        w.GD[0] = 28;
        for (int i = 0; i < 161; ++i) w.tick(keys(i == 100 ? PadCross : 0));
        w.tick(keys(PadStart));
        for (int i = 0; i < 30; ++i) w.tick({});
        std::string t;
        for (const OverlayGlyph& g : w.glyphs) t += char(g.ch);
        assert(w.sfx.speech && !w.sfx.speech->empty() && w.GD[0x93] == (gd ? (*gd)[0x93] : lang > 6 ? 0 : lang));
        return t;
    };
    const std::string en = pauseText(0, nullptr), fr = pauseText(1, nullptr), de = pauseText(2, nullptr);
    std::printf("  language: FR '%s'\n  language: DE '%s'\n", fr.c_str(), de.c_str());
    // Real pause menu text of the three sections (ARCA01 chunk 0x48: EN "Quit to Granwich", FR "Sauvegarder la
    // partie" / "Quitter la partie", DE "Spielstand speichern" / "Spiel beenden"); language 9 clamps to EN.
    assert(en.find("quittogranwich") != std::string::npos && en.find("savegame") != std::string::npos);
    assert(fr.find("sauvegarderlapartie") != std::string::npos && fr.find("quitterlapartie") != std::string::npos);
    assert(de.find("spielstandspeichern") != std::string::npos && de.find("spielbeenden") != std::string::npos);
    assert(pauseText(9, nullptr) == en);
    std::array<uint8_t, 0x300> gd{};
    gd[0x93] = 1, gd[0xa0] = 3, gd[0xa5] = gd[0xa6] = 6;
    assert(pauseText(0, &gd) == fr);
}

// Music = action 126 at runtime (0 at load, 0x407480): oracle local/wine/tbintro/music_{TRQ02,VIQ02}.csv (idle after a
// /bze: boot, 0x63b9ec per tick). TRQ02's zone block 10 picks 46 (Track45, missing -> Track14) on the first visit
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
    {  // R1 switch: GD+0xa4 flips on the press tick, the focus pointer on the next tick's pass, the camera's latch one
       // tick later (§4.1; runtime trace C, oracle w13/climb)
        assert(w.focus() == 0 && w.camFocus == 0);
        w.tick(keys(PadR1));
        assert(w.focus() == 0 && w.camFocus == 0 && w.GD[0xa4] == 12);
        w.tick(keys(PadR1));  // held: latch, no second switch
        assert(w.focus() == 1 && w.camFocus == 0);
        w.tick({});
        assert(w.camFocus == 1);
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
        // Every tick exact since the turn step 0x453de0 applies a remainder < 4 whole and Left is -0x80 (wave 12).
        std::printf("  steer worst |dv| %d (x16)\n", worst);
        assert(worst == 0);
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
    {  // idle fidgets = runtime trace G (local/wine/tbplay/G_idle_rng.csv, GRANNY: Bugs at (17593, -2190, 8336)): Bugs
       // 8 -> 20/21/22, 29/30/31, 26/27/28, 17/18/19, 23/24/25; rand() (0x46afe0) replayed from the traced seed stream
       // (Bugs draws first). GRANNY's role-40 link is asset 144 (6 frames; oracle peek of Bugs +0x98, w13_links.py), so
       // state 23 holds 10 ticks; ARCA01's is asset 31 (4 frames, the same oracle ends a poked state 23 after 5).
        Level f = load(dir / "Granny.bze");
        TbWorld& w = f.w;
        for (int i = 0; i < 100 && !(w.actors[0].rec() && w.actors[0].rec()->id == 8); ++i) w.tick({});
        TbActor& bugs = w.actors[0];
        assert(bugs.rec()->id == 8);
        bugs.stateTicks = 7;  // trace G T0 = tick 379: state 8, +0x82 = 7
        const char* steps =
            "33232231232121212231212132323344524224222242221212121213322514265222121212123211111133111111113112422222224222"
            "23332511111131020213010301010311030101040101030101010301010101120113031413010101010103131311332112531251111212"
            "12333313113113131114431311131111111311322231122111111111221111311111223131111224324422531141331111141212121534"
            "25133533541332121412121412121412141232173233164310120142132312232411241122312111113111311111131111111113313224"
            "53714121212121233121311232411111111111111113311113441522352325232323252552123432321412123212351414361212141234"
            "12123214121212452212121212111111111111122120000000000000000000010011001102100000000000000000000000001000211111"
            "11111111111111111111111112220120000001311131111112312431223122312253423142312351425144312252323122532224411313"
            "11311311111111111113111222233111111111231111111121324112211221124112211223122111201120223011201120114021203140"
            "11201120101010101030";
        const int want[][2] = {{35, 20},  {39, 21},  {67, 22},  {71, 8},   {116, 29}, {120, 30}, {150, 31}, {158, 8},
                               {184, 26}, {188, 27}, {209, 28}, {215, 8},  {242, 20}, {246, 21}, {267, 22}, {271, 8},
                               {307, 26}, {311, 27}, {339, 28}, {345, 8},  {371, 20}, {375, 21}, {407, 22}, {411, 8},
                               {443, 26}, {447, 27}, {467, 28}, {473, 8},  {513, 20}, {517, 21}, {534, 22}, {538, 8},
                               {593, 17}, {597, 18}, {625, 19}, {629, 8},  {688, 23}, {698, 24}, {763, 25}, {775, 8},
                               {798, 26}, {802, 27}, {887, 28}, {893, 8}};
        const size_t n = std::strlen(steps);
        assert(n == 900);
        uint32_t seed = 0x64e07fb7;
        size_t next = 0, matched = 0;
        uint16_t st = 8;
        for (size_t i = 1; i <= n; ++i) {
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
            assert(r.w.GD[0xa4] == 0x10);  // menu "2 Players" (0x41a6e0(2) -> 0x4370c0): bit 0x10 only
            r.w.tick({}, {});
            assert(r.w.GD[0xa4] == 0x12);  // switch rule (0x45eee0) adds 0x02 each 2P tick = runtime GD[0xa4] 0x12 (F_2p_granny.csv)
            for (int i = 0; i < 200; ++i)
                r.w.tick(keys(i % 50 < 30 ? PadUp : PadCross), keys(i % 40 < 20 ? PadLeft | PadUp : 0));
            return std::pair{r.w.actors[0].pos, r.w.actors[1].pos};
        };
        assert(run() == run());
    }
    {  // ARCA01 entities: guard 267 (template 265) patrols, spots a player in front within 2500 and attacks (runtime
       // capture local/wine/tbnpc/B_guard_attack.pkl.gz: 357 522 -> 497 504 502 501 364 435 502 501 364; Bugs hurt by
       // the thrown template 401, health 6 -> 5).
        // The patrol pick after 357 (519..522; E0_start_idle picks 519, local/wine/tbrand 521, B 522) and the 401
        // variant are rand() picks of the stream every entity shares since the boot (the exe's seed at the level start
        // is not 1: E0 has 583 draws by counter 389), and which entities draw in a tick follows their whole history
        // (B's guard 266 patrols far off, ours picks in step with 267). So B's seed is replayed per tick as in the F9
        // test below, from B row 0 = guard 357 frame 15 (+0x153 1), yaw 956, Bugs (+0x82 19) at (+1200, -30, +40);
        // the picks stay lotteries, each outcome is checked against the capture that shows it: run 0 has Bugs in
        // B's fidget 21 (anim 38 frame 1, +0x153 0) and must throw B's 395, run 1 in ours (24). (Since view flag 1
        // keeps the zone-2 directors unticked, our own per-tick draws still differ from B's: E0's other emitters.)
        TbActor* g = nullptr;
        for (const bool bFidget : {true, false}) {
            Level f = load(dir / "ARCA01.bze");
            TbWorld& w = f.w;
            for (TbActor& e : w.entities)
                if (e.id == 267) g = &e;
            assert(g && g->hp == 5 && g->behaviour == 14 && (g->view & 0x41a) == 0x41a && g->range == 7500);
            auto at357 = [&](uint16_t fr, uint8_t cd) {
                return g->rec() && g->rec()->id == 357 && g->anim.frame == fr && g->anim.countdown == cd;
            };
            for (int i = 0; i < 400 && !at357(14, 1); ++i) w.tick({});
            TbActor& b = w.actors[0];
            assert(at357(14, 1) && (!bFidget || w.setState(b, 21)));  // two ticks ahead: fresh anim frame 1 hold 1, 0
            w.tick({}), w.tick({});
            assert(at357(15, 1) && (!bFidget || (b.rec()->id == 21 && b.anim.frame == 1 && b.anim.countdown == 0)));
            g->yaw = 956;
            b.pos = {g->pos.x + 1200, g->pos.y - 30, g->pos.z + 40};
            b.stateTicks = 19;
            const char* drawsB =  // rand() calls per tick (B rows 1..330), from seed 0x5459d0a9 (row 0)
                "312112031201130101121100000000000000111111311111111112212120000000200010000011001100201010102010301040101110"
                "301030101122312141412144634343431641637352524240301010203210302120103030201010101001303020321011101010312130"
                "101010101010103010101010101010101010101121412221532171414133312121213121301020101011201120122043105221322152"
                "213221";
            uint32_t seedB = 0x5459d0a9;
            std::vector<std::pair<uint16_t, int>> seen{{357, 0}};  // guard state, entry row
            int thrown = -1, gone = -1, hit = -1, flight = -1, hitS7a = -1;
            std::vector<V3> path;  // the thrown 401 per tick from its spawn
            for (int row = 1; row <= int(std::strlen(drawsB)); ++row) {
                w.rng.seed = seedB;
                w.tick({});
                for (int k = drawsB[row - 1] - '0'; k > 0; --k) seedB = seedB * 214013u + 2531011u;
                if (seen.back().first != g->rec()->id) seen.emplace_back(g->rec()->id, row);
                const TbActor* p = nullptr;
                for (const TbActor& e : w.entities)
                    if (e.id == 401) p = &e;
                if (p && thrown < 0) thrown = row, flight = p->rec() ? p->rec()->id : -1;
                if (p && gone < 0 && thrown >= 0) path.push_back(p->pos);
                if (!p && thrown >= 0 && gone < 0) gone = row;
                if (hit < 0 && b.hp < 6) hit = row, hitS7a = w.S[0x7a];
            }
            std::string s;
            for (auto [id, at] : seen) s += std::format(" {}@{}", id, at);
            std::printf("  guard 267 states:%s; 401 (%d) thrown @%d, gone @%d, Bugs hurt @%d (health %d)\n", s.c_str(),
                        flight, thrown, gone, hit, b.hp);
            // B per row: patrol @3, 497 @4, 504 @16, 502 @48, 501 @49, 364 @55; its block 364 spawns 401 on anim frame
            // 7 (@71) and sends it event 0x100/0x101 (rand) -> 394/395 (the first-tick event 2 only applies without a
            // state); either removes itself on its anim's last frame (V 0x400) @128.
            assert(seen.size() >= 7 && seen[1].first >= 519 && seen[1].first <= 522 && seen[1].second == 3);
            const std::vector<std::pair<uint16_t, int>> notice{{497, 4}, {504, 16}, {502, 48}, {501, 49}, {364, 55}};
            assert(std::equal(notice.begin(), notice.end(), seen.begin() + 2));
            assert(thrown == 71 && gone == 128 && (flight == 394 || flight == 395) && (!bFidget || flight == 395));
            const auto ids = [&](std::initializer_list<std::pair<uint16_t, int>> want) {
                return seen.size() >= 7 + want.size() && std::equal(want.begin(), want.end(), seen.begin() + 7);
            };
            if (flight == 395) {
                // B: 395 (flagsB 0x20000) follows its TOD 0xa00 point (0x45643b carry +0x58: next position = last
                // tick's attach point) out and back to the hand; hitting Bugs ran its program (block 397: S[0x7a] =
                // 1), so on 364's last frame the guard picks event 300 -> 435 @133, 502 @181, 501 @182, 364 @188.
                assert(ids({{435, 133}, {502, 181}, {501, 182}, {364, 188}}));
                // Flight (ticks 71..127, the removal tick unseen): per-tick dy exact, dx along the facing within 1.
                const int dyB[] = {0, 0, -33, 6, 5, 4, 4, 3, 2, 1, 0, 0, 0, -1, -2, -3, -3, -4, -5, -5, -6, -6, -6, -6, -6,
                                   -6, -6, -6, -6, -6, -6, -5, -5, -5, -4, -4, -3, -3, -2, -1, 0, 0, 1, 2, 2, 4, 5, 6, 7, 8,
                                   10, 11, 13, 14, 8, 0};
                const int dxB[] = {0, 0, 103, 110, 105, 100, 96, 91, 86, 81, 76, 72, 67, 63, 58, 54, 50, 45, 41, 37, 33, 29,
                                   25, 21, 17, 12, 8, 4, 1, -2, -7, -13, -19, -24, -29, -33, -38, -42, -46, -50, -54, -57,
                                   -60, -63, -66, -69, -71, -73, -74, -76, -77, -79, -79, -80, -80, -19};
                assert(path.size() == std::size(dyB) + 1);
                for (size_t j = 0; j < std::size(dyB); ++j)
                    assert(path[j + 1].y - path[j].y == dyB[j] && std::abs(path[j + 1].x - path[j].x - dxB[j]) <= 1);
                assert(hit == 83);  // B: health 6 -> 5 @83
            } else {
                // tbrand: the variant that misses; 364's end -> 502 @+78, 501 @+79, 364 @+85; the second throw hits.
                assert(ids({{502, 133}, {501, 134}, {364, 140}}) && hit > 140);
            }
            assert(b.hp <= 5 && hitS7a == 1);  // S[0x7a] = 1 from block 397 on the hit
            // Melee (capture C: Bugs poked next to it -> 502 394, Bugs hurt state 208, health -1, 60-tick flash).
            b.pos = {g->pos.x + rotY(g->yaw, 0, 0, 300).x, g->pos.y - 30, g->pos.z + rotY(g->yaw, 0, 0, 300).z};
            const int bugsHp = b.hp;
            int hurt = -1;
            for (int i = 0; i < 200 && hurt < 0; ++i) {
                w.tick({});
                if (b.hp < bugsHp) hurt = i;
            }
            std::printf("  melee: Bugs health %d after %d ticks (state %u, guard %u)\n", b.hp, hurt, b.rec()->id,
                        g->rec()->id);
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
        // F9's press row: Taz health 4 (the 900 approach cost him 2 there; ours depends on the unreplayed stream).
        t.hp = 4;
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
        w.reloadStart();
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
    {  // RETRY speech = oracle local/wine/tbintro/w13_speech.sh (w13_speech_trace.csv; the exe has its DirectSound
       // device under Wine, so the audio path): Daffy's state 348 (mode 0x82, anim 417) asks for clip 417 + 0x9a = 59
       // at counter 35 (S[0x22] 2, 0x4cca40 59), 4 at 37; the stream ends at 403 (S[0x22] 0), 0x4cca40 -1 at 405.
       // Counter c = tick index (c - 1) / 2 after the load: every class-14 entity's +0x82 is 0 at counter 1 and 2 at
       // counter 3 (local/wine/tbintro/w13_retry82.csv), so the first entity tick runs at counter 1. Daffy (slot 22)
       // leaves state 3 at counter 33 = tick 16 (lA "R 0x10: +0x82 > 15") and asks at tick 17, as bbtime does.
       // Without audio the exe times the clip: 6000 / 44100 s.
        for (const bool audio : {true, false}) {
            Level f = load(dir / "RETRY.bze");
            TbWorld& w = f.w;
            w.GD[0] = 5;
            w.fadeIn(1, 200);
            w.speechAudio = audio;
            std::vector<int> st;
            for (int i = 0; i < 205; ++i) {
                w.speechEnded = audio && i == 201;  // the app's stream end, at the oracle's tick (counter 403)
                w.tick({});
                st.push_back(w.S[0x22]);
            }
            const auto first = [&](int v) { return int(std::ranges::find(st, v) - st.begin()); };
            const int zero = int(std::find(st.begin() + first(4), st.end(), 0) - st.begin());
            std::printf("  speech (%s): clip %u, S[0x22] 2 @%d, 4 @%d, 0 @%d\n", audio ? "audio" : "timed", w.speechClip, first(2), first(4), zero);
            assert(w.speechClip == 59 && first(2) == 17 && first(4) == 18 && zero == (audio ? 201 : 23));
        }
    }
    {  // death and respawn: the focus at 1 health dies (state 382), the partner takes over (GD+0xa2 1); then the
       // partner dies too -> a life is spent and both return at the checkpoint with 6 health (0x44ea50).
        Level f = load(dir / "ARCA01.bze");
        TbWorld& w = f.w;
        for (int i = 0; i < 30; ++i) w.tick({});
        // The guards by the checkpoint (266 / 267) go: whether they throw at the respawned pair is a rand pick.
        for (TbActor& e : w.entities)
            if (e.id == 266 || e.id == 267) e.f20 |= 0x2000;
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

// Granny pond = oracle local/wine/compare/w12/tb/water.csv (water.sh / water_capture.py there, RUNNER=wine): Bugs poked
// to (22000, -1600, 7700) above the water zone (origin (19028, -320, 3455), extents 6000 x 8500, height -820, +0xc
// 0x1000) falls in: the zone raises +0x24 0x40 (0x20 while depth +0x1a2 < 200) and gravity stops (+0x20 0x200); his
// logic enters swim state 222, the plunge (0x1000000) brakes, the rise (0x800000) floats him to depth 17. Then ↑ swims
// (216 / 217 / 218, camera-relative steering), ↑← turns, ✕ jumps out (232) and he plunges in again. Pads replayed per
// tick from the trace; every row (state, position, vy, +0x20, +0x24 & 0x60, depth, yaw) equals the oracle.
void water(const fs::path& dir) {
    struct Row { uint16_t held; uint16_t state; int x, y, z, vy; uint32_t f20, f24; int depth, yaw; };
    static const Row trace[] = {
        {0x0, 23, 22000, -1590, 7700, 160, 0x0, 0x0, 0, 920}, {0x0, 23, 22000, -1570, 7700, 320, 0x0, 0x0, 0, 920}, {0x0, 23, 22000, -1540, 7700, 480, 0x0, 0x0, 0, 920},
        {0x0, 23, 22000, -1500, 7700, 640, 0x0, 0x0, 0, 920}, {0x0, 23, 22000, -1450, 7700, 800, 0x0, 0x0, 0, 920}, {0x0, 52, 22000, -1390, 7700, 960, 0x20, 0x0, 0, 920},
        {0x0, 52, 22000, -1320, 7700, 1120, 0x20, 0x0, 0, 920}, {0x0, 52, 22000, -1240, 7700, 1280, 0x20, 0x0, 0, 920}, {0x0, 52, 22000, -1150, 7700, 1440, 0x20, 0x0, 0, 920},
        {0x0, 52, 22000, -1050, 7700, 1600, 0x220, 0x60, 90, 920}, {0x0, 222, 22000, -950, 7700, 1600, 0x220, 0x60, 190, 920}, {0x0, 222, 22000, -896, 7700, 864, 0x1000000, 0x40, 244, 920},
        {0x0, 222, 22000, -868, 7700, 448, 0x1000000, 0x40, 272, 920}, {0x0, 222, 22000, -857, 7700, 176, 0x1000000, 0x40, 283, 920}, {0x0, 222, 22000, -860, 7700, -48, 0x800000, 0x40, 280, 920},
        {0x0, 222, 22000, -871, 7700, -176, 0x800000, 0x40, 269, 920}, {0x0, 222, 22000, -889, 7700, -288, 0x800000, 0x40, 251, 920}, {0x0, 222, 22000, -913, 7700, -384, 0x800000, 0x40, 227, 920},
        {0x0, 222, 22000, -939, 7700, -416, 0x800000, 0x40, 201, 920}, {0x0, 222, 22000, -965, 7700, -416, 0x800000, 0x60, 175, 920}, {0x0, 222, 22000, -991, 7700, -416, 0x800000, 0x60, 149, 920},
        {0x0, 222, 22000, -1017, 7700, -416, 0x800000, 0x60, 123, 920}, {0x0, 222, 22000, -1043, 7700, -416, 0x800000, 0x60, 97, 920}, {0x0, 222, 22000, -1069, 7700, -416, 0x800000, 0x60, 71, 920},
        {0x0, 222, 22000, -1087, 7700, -288, 0x800000, 0x60, 53, 920}, {0x0, 222, 22000, -1098, 7700, -176, 0x800000, 0x60, 42, 920}, {0x0, 222, 22000, -1103, 7700, -80, 0x800000, 0x60, 37, 920},
        {0x0, 222, 22000, -1107, 7700, -64, 0x800000, 0x60, 33, 920}, {0x0, 222, 22000, -1111, 7700, -64, 0x800000, 0x60, 29, 920}, {0x0, 222, 22000, -1115, 7700, -64, 0x800000, 0x60, 25, 920},
        {0x0, 222, 22000, -1119, 7700, -64, 0x800000, 0x60, 21, 920}, {0x0, 222, 22000, -1123, 7700, -64, 0x800000, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920}, {0x0, 222, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 920},
        {0x1000, 216, 22000, -1123, 7700, 0, 0x0, 0x60, 17, 912}, {0x1000, 216, 22001, -1123, 7701, 0, 0x0, 0x60, 17, 906}, {0x1000, 216, 22003, -1123, 7702, 0, 0x0, 0x60, 17, 901},
        {0x1000, 216, 22006, -1123, 7703, 0, 0x0, 0x60, 17, 897}, {0x1000, 216, 22010, -1123, 7704, 0, 0x0, 0x60, 17, 894}, {0x1000, 216, 22014, -1123, 7705, 0, 0x0, 0x60, 17, 892},
        {0x1000, 216, 22018, -1123, 7706, 0, 0x0, 0x60, 17, 890}, {0x1000, 216, 22022, -1123, 7707, 0, 0x0, 0x60, 17, 888}, {0x1000, 216, 22026, -1123, 7708, 0, 0x0, 0x60, 17, 886},
        {0x1000, 216, 22030, -1123, 7709, 0, 0x0, 0x60, 17, 885}, {0x1000, 217, 22034, -1123, 7710, 0, 0x0, 0x60, 17, 884}, {0x1000, 217, 22040, -1123, 7712, 0, 0x0, 0x60, 17, 883},
        {0x1000, 217, 22048, -1123, 7715, 0, 0x0, 0x60, 17, 882}, {0x1000, 217, 22058, -1123, 7719, 0, 0x0, 0x60, 17, 881}, {0x1000, 217, 22070, -1123, 7723, 0, 0x0, 0x60, 17, 880},
        {0x1000, 217, 22083, -1123, 7727, 0, 0x0, 0x60, 17, 879}, {0x1000, 217, 22097, -1123, 7732, 0, 0x0, 0x60, 17, 879}, {0x1000, 217, 22112, -1123, 7737, 0, 0x0, 0x60, 17, 879},
        {0x1000, 217, 22128, -1123, 7741, 0, 0x0, 0x60, 17, 880}, {0x1000, 217, 22145, -1123, 7745, 0, 0x0, 0x60, 17, 881}, {0x1000, 217, 22163, -1123, 7750, 0, 0x0, 0x60, 17, 881},
        {0x1000, 217, 22182, -1123, 7754, 0, 0x0, 0x60, 17, 882}, {0x1000, 217, 22202, -1123, 7758, 0, 0x0, 0x60, 17, 883}, {0x1000, 0, 22202, -1123, 7758, 0, 0x0, 0x60, 17, 883},
        {0x1000, 217, 22242, -1123, 7766, 0, 0x0, 0x60, 17, 885}, {0x1000, 217, 22262, -1123, 7770, 0, 0x0, 0x60, 17, 886}, {0x1000, 217, 22282, -1123, 7774, 0, 0x0, 0x60, 17, 887},
        {0x1000, 217, 22302, -1123, 7778, 0, 0x0, 0x60, 17, 888}, {0x1000, 217, 22322, -1123, 7782, 0, 0x0, 0x60, 17, 889}, {0x1000, 217, 22342, -1123, 7786, 0, 0x0, 0x60, 17, 890},
        {0x1000, 217, 22362, -1123, 7790, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22383, -1123, 7794, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22404, -1123, 7798, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22425, -1123, 7802, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22446, -1123, 7806, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22467, -1123, 7810, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22488, -1123, 7814, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22509, -1123, 7818, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22530, -1123, 7822, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22551, -1123, 7826, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22572, -1123, 7830, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22593, -1123, 7834, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22614, -1123, 7838, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22635, -1123, 7842, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22656, -1123, 7846, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22677, -1123, 7850, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22698, -1123, 7854, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22719, -1123, 7858, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22740, -1123, 7862, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22761, -1123, 7866, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22782, -1123, 7870, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22803, -1123, 7874, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22824, -1123, 7878, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22845, -1123, 7882, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22866, -1123, 7886, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22887, -1123, 7890, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22908, -1123, 7894, 0, 0x0, 0x60, 17, 891},
        {0x1000, 217, 22929, -1123, 7898, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22950, -1123, 7902, 0, 0x0, 0x60, 17, 891}, {0x1000, 217, 22971, -1123, 7906, 0, 0x0, 0x60, 17, 891},
        {0x0, 218, 22993, -1123, 7910, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23014, -1123, 7913, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23034, -1123, 7916, 0, 0x0, 0x60, 17, 891},
        {0x0, 218, 23053, -1123, 7919, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23071, -1123, 7922, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23088, -1123, 7925, 0, 0x0, 0x60, 17, 891},
        {0x0, 218, 23104, -1123, 7928, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23120, -1123, 7931, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23136, -1123, 7934, 0, 0x0, 0x60, 17, 891},
        {0x0, 218, 23152, -1123, 7937, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23168, -1123, 7940, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23184, -1123, 7943, 0, 0x0, 0x60, 17, 891},
        {0x0, 218, 23200, -1123, 7946, 0, 0x0, 0x60, 17, 891}, {0x0, 218, 23216, -1123, 7949, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23232, -1123, 7952, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23246, -1123, 7954, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23258, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23268, -1123, 7955, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23276, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23283, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23289, -1123, 7955, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23294, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23298, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23301, -1123, 7955, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23303, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891},
        {0x0, 222, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 891}, {0x8000, 216, 23304, -1123, 7955, 0, 0x0, 0x60, 17, 827}, {0x9000, 216, 23305, -1123, 7956, 0, 0x0, 0x60, 17, 763},
        {0x9000, 216, 23307, -1123, 7958, 0, 0x0, 0x60, 17, 699}, {0x9000, 216, 23310, -1123, 7961, 0, 0x0, 0x60, 17, 635}, {0x9000, 216, 23314, -1123, 7964, 0, 0x0, 0x60, 17, 571},
        {0x9000, 216, 23318, -1123, 7967, 0, 0x0, 0x60, 17, 521}, {0x9000, 216, 23321, -1123, 7971, 0, 0x0, 0x60, 17, 483}, {0x9000, 216, 23324, -1123, 7975, 0, 0x0, 0x60, 17, 455},
        {0x9000, 216, 23327, -1123, 7979, 0, 0x0, 0x60, 17, 433}, {0x9000, 216, 23330, -1123, 7983, 0, 0x0, 0x60, 17, 417}, {0x9000, 217, 23333, -1123, 7987, 0, 0x0, 0x60, 17, 404},
        {0x9000, 217, 23338, -1123, 7993, 0, 0x0, 0x60, 17, 394}, {0x9000, 217, 23344, -1123, 8001, 0, 0x0, 0x60, 17, 386}, {0x9000, 217, 23351, -1123, 8011, 0, 0x0, 0x60, 17, 380},
        {0x9000, 217, 23359, -1123, 8022, 0, 0x0, 0x60, 17, 374}, {0x9000, 217, 23368, -1123, 8034, 0, 0x0, 0x60, 17, 369}, {0x9000, 217, 23378, -1123, 8047, 0, 0x0, 0x60, 17, 364},
        {0x9000, 217, 23389, -1123, 8061, 0, 0x0, 0x60, 17, 359}, {0x9000, 217, 23400, -1123, 8076, 0, 0x0, 0x60, 17, 353}, {0x9000, 217, 23411, -1123, 8092, 0, 0x0, 0x60, 17, 347},
        {0x9000, 217, 23421, -1123, 8109, 0, 0x0, 0x60, 17, 341}, {0x9000, 217, 23431, -1123, 8127, 0, 0x0, 0x60, 17, 334}, {0x9000, 217, 23441, -1123, 8145, 0, 0x0, 0x60, 17, 326},
        {0x9000, 217, 23451, -1123, 8164, 0, 0x0, 0x60, 17, 318}, {0x9000, 217, 23461, -1123, 8183, 0, 0x0, 0x60, 17, 309}, {0x9000, 217, 23470, -1123, 8202, 0, 0x0, 0x60, 17, 300},
        {0x9000, 217, 23479, -1123, 8221, 0, 0x0, 0x60, 17, 290}, {0x9000, 217, 23488, -1123, 8240, 0, 0x0, 0x60, 17, 280}, {0x9000, 217, 23496, -1123, 8259, 0, 0x0, 0x60, 17, 269},
        {0x9000, 217, 23504, -1123, 8278, 0, 0x0, 0x60, 17, 258}, {0x1000, 217, 23513, -1123, 8296, 0, 0x0, 0x60, 17, 322}, {0x40, 232, 23523, -1123, 8315, -1968, 0x1, 0x60, 17, 370},
        {0x40, 232, 23530, -1236, 8329, -1808, 0x1, 0x0, 17, 406}, {0x40, 232, 23535, -1339, 8339, -1648, 0x1, 0x0, 17, 433}, {0x40, 232, 23538, -1432, 8346, -1488, 0x1, 0x0, 17, 453},
        {0x40, 232, 23540, -1515, 8351, -1328, 0x1, 0x0, 17, 468}, {0x40, 232, 23541, -1588, 8354, -1168, 0x1, 0x0, 17, 479}, {0x40, 232, 23541, -1651, 8356, -1008, 0x1, 0x0, 17, 487},
        {0x0, 232, 23541, -1704, 8357, -848, 0x1, 0x0, 17, 493}, {0x0, 232, 23541, -1747, 8357, -688, 0x1, 0x0, 17, 498}, {0x0, 232, 23541, -1780, 8357, -528, 0x1, 0x0, 17, 502},
        {0x0, 232, 23541, -1803, 8357, -368, 0x1, 0x0, 17, 505}, {0x0, 232, 23541, -1816, 8357, -208, 0x1, 0x0, 17, 507}, {0x0, 232, 23541, -1819, 8357, -48, 0x1, 0x0, 17, 508},
        {0x0, 232, 23541, -1812, 8357, 112, 0x421, 0x0, 17, 509}, {0x0, 232, 23541, -1795, 8357, 272, 0x421, 0x0, 17, 510}, {0x0, 232, 23541, -1768, 8357, 432, 0x421, 0x0, 17, 511},
        {0x0, 232, 23541, -1731, 8357, 592, 0x421, 0x0, 17, 514}, {0x0, 232, 23541, -1684, 8357, 752, 0x421, 0x0, 17, 514}, {0x0, 232, 23541, -1627, 8357, 912, 0x421, 0x0, 17, 514},
        {0x0, 232, 23541, -1560, 8357, 1072, 0x421, 0x0, 17, 514}, {0x0, 232, 23541, -1483, 8357, 1232, 0x421, 0x0, 17, 514}, {0x0, 0, 23541, -1483, 8357, 1232, 0x421, 0x0, 17, 514},
        {0x0, 232, 23541, -1299, 8357, 1552, 0x421, 0x0, 17, 514}, {0x0, 232, 23541, -1192, 8357, 1712, 0x421, 0x0, 17, 514}, {0x0, 232, 23541, -1075, 8357, 1872, 0x621, 0x60, 65, 514},
        {0x0, 222, 23541, -958, 8357, 1872, 0x621, 0x60, 182, 514}, {0x0, 222, 23541, -895, 8357, 1008, 0x1000000, 0x40, 245, 514}, {0x0, 222, 23541, -861, 8357, 544, 0x1000000, 0x40, 279, 514},
        {0x0, 222, 23541, -845, 8357, 256, 0x1000000, 0x40, 295, 514}, {0x0, 222, 23541, -845, 8357, 0, 0x1000000, 0x40, 295, 514}, {0x0, 222, 23541, -858, 8357, -208, 0x800000, 0x40, 282, 514},
        {0x0, 222, 23541, -878, 8357, -320, 0x800000, 0x40, 262, 514}, {0x0, 222, 23541, -904, 8357, -416, 0x800000, 0x40, 236, 514}, {0x0, 222, 23541, -930, 8357, -416, 0x800000, 0x40, 210, 514},
        {0x0, 222, 23541, -956, 8357, -416, 0x800000, 0x60, 184, 514}, {0x0, 222, 23541, -982, 8357, -416, 0x800000, 0x60, 158, 514}, {0x0, 222, 23541, -1008, 8357, -416, 0x800000, 0x60, 132, 514},
        {0x0, 222, 23541, -1034, 8357, -416, 0x800000, 0x60, 106, 514}, {0x0, 222, 23541, -1060, 8357, -416, 0x800000, 0x60, 80, 514}, {0x0, 222, 23541, -1078, 8357, -288, 0x800000, 0x60, 62, 514},
        {0x0, 222, 23541, -1089, 8357, -176, 0x800000, 0x60, 51, 514}, {0x0, 222, 23541, -1094, 8357, -80, 0x800000, 0x60, 46, 514}, {0x0, 222, 23541, -1098, 8357, -64, 0x800000, 0x60, 42, 514},
        {0x0, 222, 23541, -1102, 8357, -64, 0x800000, 0x60, 38, 514}, {0x0, 222, 23541, -1106, 8357, -64, 0x800000, 0x60, 34, 514}, {0x0, 222, 23541, -1110, 8357, -64, 0x800000, 0x60, 30, 514},
        {0x0, 222, 23541, -1114, 8357, -64, 0x800000, 0x60, 26, 514}, {0x0, 222, 23541, -1118, 8357, -64, 0x800000, 0x60, 22, 514}, {0x0, 222, 23541, -1122, 8357, -64, 0x800000, 0x60, 18, 514},
        {0x0, 222, 23541, -1122, 8357, 0, 0x0, 0x60, 18, 514}, {0x0, 222, 23541, -1122, 8357, 0, 0x0, 0x60, 18, 514}, {0x0, 222, 23541, -1122, 8357, 0, 0x0, 0x60, 18, 514},
    };
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    TbActor& b = w.actors[0];
    // The welcome box and Granny's dialog hold the pads (GD+0x9f bars): ✕ taps close them here (the oracle took e,
    // Return). Then the oracle's pre-poke state (counter 1643): idle fidget 23 and its camera (view block 0x4d5120;
    // the dialogs' camera scripts, not ported, leave ours elsewhere).
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int i = 0; i < 1200 && w.GD[0x9f]; ++i) w.tick(keys(i % 45 < 3 ? PadCross : 0));
    for (int i = 0; i < 120; ++i) w.tick({});
    assert(!w.GD[0x9f] && b.pos == (V3{16110, -2100, 7046}) && b.yaw == 920 && b.f20 == 0x102);
    w.cam.eye = w.cam.prevEye = {14937, -2758, 6988}, w.cam.look = w.cam.prevLook = {16110, -2420, 7046};
    w.cam.body = w.cam.prevBody = {15321, -2647, 7007};
    w.setState(0, 23);
    b.pos = {22000, -1600, 7700};
    int bad = 0, n = 0;
    for (const Row& r : trace) {
        w.tick(keys(r.held));
        // state 0: the oracle sampled that counter before the tick's entity pass (stale copy of the previous row)
        const bool fidget = r.state >= 17 && r.state <= 31;
        const bool ok = !r.state || ((fidget || b.rec()->id == r.state) && b.pos == (V3{r.x, r.y, r.z}) && int16_t(b.vel[1]) == r.vy &&
                                     b.f20 == r.f20 && (b.f24 & 0x60) == r.f24 && b.depth == r.depth &&
                                     (b.yaw & 0xfff) == (r.yaw & 0xfff));
        if (!ok && bad++ < 12)
            std::printf("  water row %d: ours %u (%d %d %d) vy %d f20 %#x f24 %#x d %d yaw %d | oracle %u (%d %d %d) vy %d f20 %#x f24 %#x d %d yaw %d\n",
                        n, b.rec()->id, b.pos.x, b.pos.y, b.pos.z, int16_t(b.vel[1]), b.f20, b.f24 & 0x60, b.depth, b.yaw,
                        r.state, r.x, r.y, r.z, r.vy, r.f20, r.f24, r.depth, r.yaw);
        ++n;
    }
    // Renderer query: the surface the depth was measured from (zone top -320 - 820).
    assert(w.waterSurface(b) == std::optional<int32_t>(-1140));
    std::string un;
    for (const std::string& u : w.unsupported) un += " " + u;
    std::printf("  water: %d / %d rows differ from the oracle; unsupported:%s\n", bad, n, un.c_str());
    assert(bad == 0);
}

// Granny pond swim sounds + dive = oracle local/wine/compare/w13/tb/swim_sfx (run.sh / run_jump.sh, RUNNER=wine; capture.py
// records Bugs, GD and the voice table 0x63b740 per tick, tocsv.py -> swim.csv / jump.csv). Bugs poked to (22000,
// -1600, 7700) as `water`; swim: ↑ swims, □ dives (220: pitch −20 a tick, the move follows the pitch, 223 / 224 to the
// pond floor, 225), □ again (227 / 226 / 224); jump: ✕ jumps out of the surface (232) standing and swimming. Every
// row's state, position and pitch / yaw / roll equal the oracle, and so do Bugs' sound starts: id 7 on the entry
// tick and at the dive's frame, id 19 two ticks into the jump, each started by the next mixer pass at levels 100 / 100
// (pan 0, volume from program volume and GD+0x97 = 0x80).
struct SwimRow { uint16_t held, state; int16_t x, y, z, pitch, yaw, roll; uint16_t sound; };
int swimTrace(const fs::path& dir, const char* name, std::span<const SwimRow> trace, V3 look) {
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    TbActor& b = w.actors[0];
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int i = 0; i < 1200 && w.GD[0x9f]; ++i) w.tick(keys(i % 45 < 3 ? PadCross : 0));
    for (int i = 0; i < 120; ++i) w.tick({});
    assert(w.sfx.volume == 0x80);
    w.cam.eye = w.cam.prevEye = {14937, -2758, 6988}, w.cam.look = w.cam.prevLook = look;
    w.cam.body = w.cam.prevBody = {15321, -2647, 7007};
    // The oracle's pre-poke idle fidget (17-31, rand()-driven, trace G) is not reproduced: Bugs falls in state 52 and
    // the fidget rows compare position only.
    w.setState(0, 52);
    b.pos = {22000, -1600, 7700};
    const int32_t me = int32_t(b.uid) + 1;
    int bad = 0, n = 0, sounds = 0;
    std::vector<size_t> started;  // Bugs' voices started last tick: the next mixer pass sets their levels
    for (const SwimRow& r : trace.subspan(1)) {
        const auto vb = w.sfx.voices;
        w.tick(keys(r.held));
        for (size_t v : started)
            if (w.sfx.voices[v].lv != std::array<uint8_t, 2>{100, 100}) ++bad, std::printf("  %s row %d: levels %d/%d\n", name, n, w.sfx.voices[v].lv[0], w.sfx.voices[v].lv[1]);
        started.clear();
        uint16_t snd = 0;
        for (size_t v = 0; v < 24; ++v)
            if (w.sfx.voices[v].state == 0x8001 && vb[v].state != 0x8001 && w.sfx.voices[v].emitter == me)
                snd = w.sfx.table[size_t(w.sfx.voices[v].entry)].id, started.push_back(v);
        sounds += snd != 0;
        const bool fidget = r.state >= 17 && r.state <= 31;
        // state 0: a stale / torn sample (taken before or during the tick's entity pass); its voice table is read
        // after Bugs' update, so the sound still compares.
        const bool ok = snd == r.sound && (!r.state || ((fidget || b.rec()->id == r.state) && b.pos == (V3{r.x, r.y, r.z}) &&
                                                         ((b.pitch - r.pitch) & 0xfff) == 0 && ((b.yaw - r.yaw) & 0xfff) == 0 &&
                                                         ((b.roll - r.roll) & 0xfff) == 0));
        if (!ok && bad++ < 40)
            std::printf("  %s row %d: ours %u (%d %d %d) p %d y %d r %d snd %u | oracle %u (%d %d %d) p %d y %d r %d snd %u\n", name, n,
                        b.rec()->id, b.pos.x, b.pos.y, b.pos.z, b.pitch, b.yaw, b.roll, snd, r.state, r.x, r.y, r.z, r.pitch, r.yaw, r.roll, r.sound);
        ++n;
    }
    std::printf("  %s: %d / %d rows differ from the oracle (%d Bugs sound starts)\n", name, bad, n, sounds);
    return bad;
}

void swim(const fs::path& dir) {
    static const SwimRow dive[] = {
        {0x0, 20, 22000, -1600, 7700, 4096, 920, 0, 0}, {0x0, 21, 22000, -1590, 7700, 4096, 920, 0, 0}, {0x0, 21, 22000, -1570, 7700, 4096, 920, 0, 0},
        {0x0, 21, 22000, -1540, 7700, 4096, 920, 0, 0}, {0x0, 21, 22000, -1500, 7700, 4096, 920, 0, 0}, {0x0, 21, 22000, -1450, 7700, 4096, 920, 0, 0},
        {0x0, 52, 22000, -1390, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1320, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1240, 7700, 4096, 920, 0, 0},
        {0x0, 52, 22000, -1150, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1050, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -950, 7700, 0, 920, 0, 7},
        {0x0, 222, 22000, -896, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -868, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -857, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -860, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -871, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -889, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -913, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -939, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -965, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -991, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1017, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1043, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1069, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1087, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1098, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1103, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1107, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1111, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1115, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1119, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x1000, 216, 22000, -1123, 7700, 0, 912, 0, 0}, {0x1000, 216, 22001, -1123, 7701, 0, 906, 0, 0}, {0x1000, 216, 22003, -1123, 7702, 0, 901, 0, 0},
        {0x1000, 216, 22006, -1123, 7703, 0, 897, 0, 0}, {0x1000, 216, 22010, -1123, 7704, 0, 894, 0, 0}, {0x1000, 216, 22014, -1123, 7705, 0, 892, 0, 0},
        {0x1000, 216, 22018, -1123, 7706, 0, 890, 0, 0}, {0x1000, 216, 22022, -1123, 7707, 0, 888, 0, 0}, {0x1000, 216, 22026, -1123, 7708, 0, 886, 0, 0},
        {0x1000, 216, 22030, -1123, 7709, 0, 885, 0, 0}, {0x1000, 217, 22034, -1123, 7710, 0, 884, 0, 0}, {0x1000, 217, 22040, -1123, 7712, 0, 883, 0, 0},
        {0x1000, 217, 22048, -1123, 7715, 0, 882, 0, 0}, {0x1000, 217, 22058, -1123, 7719, 0, 881, 0, 0}, {0x1000, 217, 22070, -1123, 7723, 0, 880, 0, 0},
        {0x1000, 217, 22083, -1123, 7727, 0, 879, 0, 0}, {0x1000, 217, 22097, -1123, 7732, 0, 879, 0, 0}, {0x1000, 217, 22112, -1123, 7737, 0, 879, 0, 0},
        {0x1000, 217, 22128, -1123, 7741, 0, 880, 0, 0}, {0x1000, 217, 22145, -1123, 7745, 0, 881, 0, 0}, {0x1000, 217, 22163, -1123, 7750, 0, 881, 0, 0},
        {0x1000, 217, 22182, -1123, 7754, 0, 882, 0, 0}, {0x1000, 217, 22202, -1123, 7758, 0, 883, 0, 0}, {0x1000, 217, 22222, -1123, 7762, 0, 884, 0, 0},
        {0x1000, 217, 22242, -1123, 7766, 0, 885, 0, 0}, {0x1000, 217, 22262, -1123, 7770, 0, 886, 0, 0}, {0x1000, 217, 22282, -1123, 7774, 0, 887, 0, 0},
        {0x1000, 217, 22302, -1123, 7778, 0, 888, 0, 0}, {0x1000, 0, 22302, -1123, 7778, 0, 888, 0, 0}, {0x1000, 217, 22342, -1123, 7786, 0, 890, 0, 0},
        {0x1000, 217, 22362, -1123, 7790, 0, 891, 0, 0}, {0x1000, 217, 22383, -1123, 7794, 0, 891, 0, 0}, {0x1000, 217, 22404, -1123, 7798, 0, 891, 0, 0},
        {0x1000, 217, 22425, -1123, 7802, 0, 891, 0, 0}, {0x1000, 217, 22446, -1123, 7806, 0, 891, 0, 0}, {0x1000, 217, 22467, -1123, 7810, 0, 891, 0, 0},
        {0x1000, 217, 22488, -1123, 7814, 0, 891, 0, 0}, {0x1000, 217, 22509, -1123, 7818, 0, 891, 0, 0}, {0x1000, 217, 22530, -1123, 7822, 0, 891, 0, 0},
        {0x1000, 217, 22551, -1123, 7826, 0, 891, 0, 0}, {0x1000, 217, 22572, -1123, 7830, 0, 891, 0, 0}, {0x0, 218, 22594, -1123, 7834, 0, 891, 0, 0},
        {0x0, 218, 22615, -1123, 7837, 0, 891, 0, 0}, {0x0, 218, 22635, -1123, 7840, 0, 891, 0, 0}, {0x0, 218, 22654, -1123, 7843, 0, 891, 0, 0},
        {0x0, 218, 22672, -1123, 7846, 0, 891, 0, 0}, {0x0, 218, 22689, -1123, 7849, 0, 891, 0, 0}, {0x0, 218, 22705, -1123, 7852, 0, 891, 0, 0},
        {0x0, 218, 22721, -1123, 7855, 0, 891, 0, 0}, {0x0, 218, 22737, -1123, 7858, 0, 891, 0, 0}, {0x0, 218, 22753, -1123, 7861, 0, 891, 0, 0},
        {0x0, 218, 22769, -1123, 7864, 0, 891, 0, 0}, {0x0, 218, 22785, -1123, 7867, 0, 891, 0, 0}, {0x0, 218, 22801, -1123, 7870, 0, 891, 0, 0},
        {0x0, 218, 22817, -1123, 7873, 0, 891, 0, 0}, {0x0, 222, 22833, -1123, 7876, 0, 891, 0, 0}, {0x0, 222, 22847, -1123, 7878, 0, 891, 0, 0},
        {0x0, 222, 22859, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22869, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22877, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22884, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22890, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22895, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22899, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22902, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22904, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x0, 222, 22905, -1123, 7879, 0, 891, 0, 0}, {0x80, 220, 22905, -1123, 7879, 0, 891, 0, 0},
        {0x80, 220, 22909, -1122, 7880, -20, 891, 0, 0}, {0x80, 220, 22917, -1120, 7882, -40, 891, 0, 0}, {0x80, 220, 22928, -1117, 7885, -60, 891, 0, 0},
        {0x80, 220, 22942, -1113, 7889, -80, 891, 0, 0}, {0x80, 220, 22958, -1108, 7894, -100, 891, 0, 0}, {0x80, 220, 22976, -1103, 7900, -120, 891, 0, 0},
        {0x80, 220, 22996, -1097, 7906, -140, 891, 0, 0}, {0x80, 220, 23017, -1090, 7912, -160, 891, 0, 0}, {0x80, 220, 23039, -1082, 7918, -180, 891, 0, 0},
        {0x80, 220, 23062, -1073, 7924, -200, 891, 0, 0}, {0x80, 220, 23086, -1063, 7930, -220, 891, 0, 0}, {0x80, 220, 23111, -1052, 7936, -240, 891, 0, 0},
        {0x80, 220, 23137, -1040, 7942, -260, 891, 0, 0}, {0x80, 220, 23164, -1028, 7948, -280, 891, 0, 0}, {0x80, 220, 23190, -1015, 7953, -300, 891, 0, 0},
        {0x80, 220, 23216, -1001, 7958, -320, 891, 0, 0}, {0x80, 220, 23241, -986, 7963, -340, 891, 0, 0}, {0x80, 220, 23266, -970, 7968, -360, 891, 0, 0},
        {0x80, 220, 23291, -953, 7973, -380, 891, 0, 0}, {0x80, 220, 23315, -936, 7978, -400, 891, 0, 0}, {0x0, 220, 23338, -918, 7983, -420, 891, 0, 7},
        {0x0, 220, 23361, -899, 7988, -440, 891, 0, 0}, {0x0, 220, 23383, -880, 7993, -460, 891, 0, 0}, {0x0, 220, 23405, -860, 7997, -480, 891, 0, 0},
        {0x0, 220, 23426, -839, 8001, -500, 891, 0, 0}, {0x0, 220, 23447, -818, 8005, -520, 891, 0, 0}, {0x0, 220, 23467, -796, 8009, -540, 891, 0, 0},
        {0x0, 220, 23486, -773, 8013, -560, 891, 0, 0}, {0x0, 220, 23504, -750, 8017, -580, 891, 0, 0}, {0x0, 220, 23522, -726, 8021, -600, 891, 0, 0},
        {0x0, 220, 23539, -702, 8025, -620, 891, 0, 0}, {0x0, 223, 23555, -677, 8028, -640, 891, 0, 0}, {0x0, 224, 23571, -652, 8031, -660, 891, 0, 0},
        {0x0, 224, 23585, -630, 8033, -630, 891, 0, 0}, {0x0, 224, 23597, -611, 8034, -600, 891, 0, 0}, {0x0, 224, 23608, -594, 8035, -570, 891, 0, 0},
        {0x0, 224, 23618, -579, 8036, -540, 891, 0, 0}, {0x0, 224, 23627, -566, 8037, -510, 891, 0, 0}, {0x0, 224, 23635, -555, 8038, -480, 891, 0, 0},
        {0x0, 224, 23642, -545, 8039, -450, 891, 0, 0}, {0x0, 224, 23648, -536, 8040, -420, 891, 0, 0}, {0x0, 224, 23653, -528, 8041, -390, 891, 0, 0},
        {0x0, 224, 23658, -521, 8042, -360, 891, 0, 0}, {0x0, 224, 23663, -517, 8043, -330, 891, 0, 0}, {0x0, 224, 23668, -519, 8044, -300, 891, 0, 0},
        {0x0, 224, 23673, -522, 8045, -270, 891, 0, 0}, {0x0, 224, 23678, -524, 8046, -240, 891, 0, 0}, {0x0, 224, 23684, -526, 8047, -210, 891, 0, 0},
        {0x0, 224, 23690, -529, 8048, -180, 891, 0, 0}, {0x0, 224, 23696, -529, 8049, -150, 891, 0, 0}, {0x0, 224, 23702, -530, 8050, -120, 891, 0, 0},
        {0x0, 224, 23708, -531, 8051, -90, 891, 0, 0}, {0x0, 224, 23714, -531, 8052, -60, 891, 0, 0}, {0x0, 224, 23720, -531, 8053, -30, 891, 0, 0},
        {0x0, 224, 23725, -534, 8054, 0, 891, 0, 0}, {0x0, 224, 23725, -535, 8055, 0, 891, 0, 0}, {0x0, 224, 23726, -535, 8056, 0, 891, 0, 0},
        {0x0, 224, 23726, -536, 8058, 0, 891, 0, 0}, {0x0, 224, 23726, -537, 8060, 0, 891, 0, 0}, {0x0, 224, 23726, -538, 8062, 0, 891, 0, 0},
        {0x0, 224, 23726, -539, 8064, 0, 891, 0, 0}, {0x0, 224, 23726, -540, 8066, 0, 891, 0, 0}, {0x0, 224, 23726, -541, 8068, 0, 891, 0, 0},
        {0x0, 224, 23726, -542, 8070, 0, 891, 0, 0}, {0x0, 224, 23726, -543, 8071, 0, 891, 0, 0}, {0x0, 224, 23726, -544, 8072, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8073, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x4000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x1000, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x40, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 891, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 851, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 811, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 771, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 731, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 691, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 651, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 611, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 571, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 531, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 491, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 451, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 411, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 371, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 331, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 291, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 251, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 211, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 171, 0, 0},
        {0x8000, 225, 23726, -545, 8074, 0, 131, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 91, 0, 0}, {0x8000, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0}, {0x0, 225, 23726, -545, 8074, 0, 51, 0, 0},
        {0x80, 227, 23726, -545, 8074, 0, 51, 0, 0}, {0x80, 227, 23726, -545, 8075, 0, 51, 0, 0}, {0x80, 227, 23726, -545, 8077, 0, 51, 0, 0},
        {0x80, 227, 23724, -545, 8080, 0, 51, 0, 0}, {0x80, 227, 23716, -545, 8084, 0, 51, 0, 0}, {0x80, 227, 23695, -546, 8089, 0, 51, 0, 0},
        {0x80, 226, 23695, -546, 8095, 0, 51, 0, 0}, {0x80, 226, 23696, -546, 8103, 0, 51, 0, 0}, {0x80, 226, 23698, -546, 8113, 0, 51, 0, 0},
        {0x80, 226, 23700, -546, 8125, 0, 51, 0, 0}, {0x80, 226, 23702, -546, 8139, 0, 51, 0, 0}, {0x0, 224, 23704, -546, 8154, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8167, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8179, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8190, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8200, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8209, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8217, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8224, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8230, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8236, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8242, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8248, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8254, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8260, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8266, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8272, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8278, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8284, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8290, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8296, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8302, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8308, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8314, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8320, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8326, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8332, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8338, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8344, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8350, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8356, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8362, 0, 51, 0, 0},
        {0x0, 224, 23705, -546, 8368, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8374, 0, 51, 0, 0}, {0x0, 224, 23705, -546, 8380, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8386, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8391, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8395, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8398, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8400, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x40, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0},
        {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}, {0x0, 225, 23705, -546, 8401, 0, 51, 0, 0}
    };
    static const SwimRow jump[] = {
        {0x0, 18, 22000, -1600, 7700, 4096, 920, 0, 0}, {0x0, 18, 22000, -1590, 7700, 4096, 920, 0, 0}, {0x0, 18, 22000, -1570, 7700, 4096, 920, 0, 0},
        {0x0, 18, 22000, -1540, 7700, 4096, 920, 0, 0}, {0x0, 18, 22000, -1500, 7700, 4096, 920, 0, 0}, {0x0, 18, 22000, -1450, 7700, 4096, 920, 0, 0},
        {0x0, 52, 22000, -1390, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1320, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1240, 7700, 4096, 920, 0, 0},
        {0x0, 52, 22000, -1150, 7700, 4096, 920, 0, 0}, {0x0, 52, 22000, -1050, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -950, 7700, 0, 920, 0, 7},
        {0x0, 222, 22000, -896, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -868, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -857, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -860, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -871, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -889, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -913, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -939, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -965, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -991, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1017, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1043, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1069, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1087, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1098, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1103, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1107, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1111, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1115, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1119, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1123, 7700, 0, 920, 0, 0},
        {0x40, 232, 22000, -1123, 7700, 0, 920, 0, 0}, {0x40, 232, 22000, -1236, 7700, 0, 920, 0, 0}, {0x40, 232, 22000, -1339, 7700, 0, 920, 0, 19},
        {0x40, 232, 22000, -1432, 7700, 0, 920, 0, 0}, {0x40, 232, 22000, -1515, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1588, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1651, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1704, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1747, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1780, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1803, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1816, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1819, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1812, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1795, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1768, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1731, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1684, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1627, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1560, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1483, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1396, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1299, 7700, 0, 920, 0, 0}, {0x0, 232, 22000, -1192, 7700, 0, 920, 0, 0},
        {0x0, 232, 22000, -1075, 7700, 0, 920, 0, 0}, {0x0, 0, 22000, -958, 7700, 0, 920, 0, 7}, {0x0, 222, 22000, -895, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -861, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -845, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -845, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -858, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -878, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -904, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -930, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -956, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -982, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1008, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1034, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1060, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1078, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1089, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1094, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1098, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1102, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1106, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1110, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1114, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1118, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0}, {0x0, 222, 22000, -1122, 7700, 0, 920, 0, 0},
        {0x1000, 216, 22000, -1122, 7700, 0, 908, 0, 0}, {0x1000, 216, 22001, -1122, 7701, 0, 899, 0, 0}, {0x1000, 216, 22003, -1122, 7702, 0, 892, 0, 0},
        {0x1000, 216, 22006, -1122, 7703, 0, 886, 0, 0}, {0x1000, 216, 22010, -1122, 7704, 0, 882, 0, 0}, {0x1000, 216, 22014, -1122, 7705, 0, 878, 0, 0},
        {0x1000, 216, 22018, -1122, 7706, 0, 875, 0, 0}, {0x1000, 216, 22022, -1122, 7707, 0, 873, 0, 0}, {0x1000, 216, 22026, -1122, 7708, 0, 871, 0, 0},
        {0x1000, 216, 22030, -1122, 7709, 0, 869, 0, 0}, {0x1000, 217, 22034, -1122, 7710, 0, 867, 0, 0}, {0x1000, 217, 22040, -1122, 7712, 0, 866, 0, 0},
        {0x1000, 217, 22048, -1122, 7715, 0, 865, 0, 0}, {0x1000, 217, 22058, -1122, 7719, 0, 864, 0, 0}, {0x1000, 217, 22070, -1122, 7724, 0, 863, 0, 0},
        {0x1000, 217, 22083, -1122, 7729, 0, 863, 0, 0}, {0x1000, 217, 22097, -1122, 7734, 0, 863, 0, 0}, {0x1000, 217, 22112, -1122, 7739, 0, 863, 0, 0},
        {0x1000, 217, 22128, -1122, 7744, 0, 863, 0, 0}, {0x1000, 217, 22145, -1122, 7749, 0, 864, 0, 0}, {0x1000, 217, 22163, -1122, 7754, 0, 865, 0, 0},
        {0x1000, 217, 22182, -1122, 7759, 0, 866, 0, 0}, {0x1000, 217, 22201, -1122, 7764, 0, 867, 0, 0}, {0x1000, 217, 22221, -1122, 7769, 0, 867, 0, 0},
        {0x1000, 217, 22240, -1122, 7774, 0, 868, 0, 0}, {0x1000, 217, 22260, -1122, 7779, 0, 868, 0, 0}, {0x1000, 217, 22279, -1122, 7784, 0, 869, 0, 0},
        {0x1000, 0, 22279, -1122, 7784, 0, 869, 0, 0}, {0x1000, 217, 22318, -1122, 7794, 0, 870, 0, 0}, {0x1000, 217, 22338, -1122, 7799, 0, 870, 0, 0},
        {0x1000, 217, 22358, -1122, 7804, 0, 870, 0, 0}, {0x1000, 217, 22378, -1122, 7809, 0, 870, 0, 0}, {0x1000, 217, 22398, -1122, 7814, 0, 870, 0, 0},
        {0x1000, 217, 22418, -1122, 7819, 0, 870, 0, 0}, {0x1000, 217, 22438, -1122, 7824, 0, 870, 0, 0}, {0x1000, 217, 22458, -1122, 7829, 0, 870, 0, 0},
        {0x1000, 217, 22478, -1122, 7834, 0, 870, 0, 0}, {0x1000, 217, 22498, -1122, 7839, 0, 870, 0, 0}, {0x1000, 217, 22518, -1122, 7844, 0, 870, 0, 0},
        {0x1000, 217, 22538, -1122, 7849, 0, 870, 0, 0}, {0x1000, 217, 22558, -1122, 7854, 0, 870, 0, 0}, {0x1000, 217, 22578, -1122, 7859, 0, 870, 0, 0},
        {0x1000, 217, 22598, -1122, 7864, 0, 870, 0, 0}, {0x1000, 217, 22618, -1122, 7869, 0, 870, 0, 0}, {0x1000, 217, 22638, -1122, 7874, 0, 870, 0, 0},
        {0x1000, 217, 22658, -1122, 7879, 0, 870, 0, 0}, {0x1000, 217, 22678, -1122, 7884, 0, 870, 0, 0}, {0x1000, 217, 22698, -1122, 7889, 0, 870, 0, 0},
        {0x1000, 217, 22718, -1122, 7894, 0, 870, 0, 0}, {0x1000, 217, 22738, -1122, 7899, 0, 870, 0, 0}, {0x1000, 217, 22758, -1122, 7904, 0, 870, 0, 0},
        {0x1000, 217, 22778, -1122, 7909, 0, 870, 0, 0}, {0x1000, 217, 22798, -1122, 7914, 0, 870, 0, 0}, {0x1000, 217, 22818, -1122, 7919, 0, 870, 0, 0},
        {0x1000, 217, 22838, -1122, 7924, 0, 870, 0, 0}, {0x1000, 217, 22858, -1122, 7929, 0, 870, 0, 0}, {0x1000, 217, 22878, -1122, 7934, 0, 870, 0, 0},
        {0x1000, 217, 22898, -1122, 7939, 0, 870, 0, 0}, {0x1000, 217, 22918, -1122, 7944, 0, 870, 0, 0}, {0x1000, 217, 22938, -1122, 7949, 0, 870, 0, 0},
        {0x1000, 217, 22958, -1122, 7954, 0, 870, 0, 0}, {0x1000, 217, 22978, -1122, 7959, 0, 870, 0, 0}, {0x40, 232, 22999, -1122, 7964, 0, 870, 0, 0},
        {0x40, 232, 23014, -1235, 7967, 0, 870, 0, 0}, {0x40, 0, 23014, -1235, 7967, 0, 870, 0, 19}, {0x40, 232, 23033, -1431, 7970, 0, 870, 0, 0},
        {0x40, 232, 23039, -1514, 7970, 0, 870, 0, 0}, {0x0, 232, 23043, -1587, 7970, 0, 870, 0, 0}, {0x1000, 232, 23056, -1650, 7973, 0, 869, 0, 0},
        {0x1000, 232, 23076, -1703, 7978, 0, 868, 0, 0}, {0x1000, 232, 23101, -1746, 7984, 0, 868, 0, 0}, {0x1000, 232, 23130, -1779, 7991, 0, 868, 0, 0},
        {0x1000, 232, 23162, -1802, 7999, 0, 869, 0, 0}, {0x1000, 232, 23196, -1815, 8008, 0, 870, 0, 0}, {0x1000, 232, 23231, -1818, 8017, 0, 871, 0, 0},
        {0x1000, 232, 23267, -1811, 8026, 0, 873, 0, 0}, {0x1000, 232, 23304, -1794, 8035, 0, 873, 0, 0}, {0x1000, 232, 23342, -1767, 8044, 0, 875, 0, 0},
        {0x1000, 232, 23380, -1730, 8053, 0, 876, 0, 0}, {0x1000, 232, 23418, -1683, 8062, 0, 876, 0, 0}, {0x1000, 232, 23456, -1626, 8071, 0, 878, 0, 0},
        {0x1000, 232, 23494, -1559, 8080, 0, 878, 0, 0}, {0x1000, 232, 23532, -1482, 8089, 0, 878, 0, 0}, {0x1000, 232, 23570, -1395, 8098, 0, 878, 0, 0},
        {0x1000, 232, 23608, -1298, 8107, 0, 878, 0, 0}, {0x1000, 232, 23646, -1191, 8116, 0, 878, 0, 0}, {0x1000, 232, 23684, -1074, 8125, 0, 879, 0, 0},
        {0x1000, 222, 23722, -957, 8134, 0, 879, 0, 7}, {0x0, 222, 23755, -894, 8141, 0, 879, 0, 0}, {0x0, 222, 23773, -861, 8148, 0, 879, 0, 0},
        {0x0, 222, 23752, -848, 8154, 0, 879, 0, 0}, {0x0, 222, 23751, -848, 8159, 0, 879, 0, 0}, {0x0, 222, 23751, -856, 8163, 0, 879, 0, 0},
        {0x0, 222, 23749, -871, 8166, 0, 879, 0, 0}, {0x0, 222, 23749, -892, 8168, 0, 879, 0, 0}, {0x0, 222, 23749, -918, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -944, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -970, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -996, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1022, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1048, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1074, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1092, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1103, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1108, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1112, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1116, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1120, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0},
        {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}, {0x0, 222, 23749, -1124, 8169, 0, 879, 0, 0}
    };
    const int bad = swimTrace(dir, "swim", dive, {16110, -2419, 7046}) + swimTrace(dir, "jump", jump, {16110, -2420, 7046});
    assert(bad == 0);
}

// Particle update 0x453350 / 0x452e80 = oracle local/wine/compare/w13/fx/granny.pkl.gz (fx.sh granny, RUNNER=wine):
// Bugs dropped into the Granny pond; his splash slot 6 (12 drops, gravity 195, drag 275) re-born on the surface in
// slot 4 (flat rings), the ring slot 5 and the director's sparkles 13. Each sampled tick's slot records and particle
// lists (tb_fx_granny.inc) are loaded, fxTick runs once under the next sample's camera, and every slot of that next
// sample whose particle count the step explains (no emission in between) must equal ours field for field.
#include "tb_fx_granny.inc"
void fx(const fs::path& dir) {
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    using Slot = TbWorld::FxSlot;
    using Part = TbWorld::FxPart;
    auto slots = [](const char* s) {
        std::array<Slot, 20> out{};
        for (const char* p = s; *p;) {
            const size_t i = size_t(std::strtoul(p, const_cast<char**>(&p), 10));
            uint8_t r[0x9c];
            for (size_t k = 0; k < sizeof r; ++k) r[k] = uint8_t(std::stoul(std::string(p + 1 + 2 * k, 2), nullptr, 16));
            p += 1 + 2 * sizeof r + 1;
            auto h = [&](size_t o) { return int16_t(r[o] | r[o + 1] << 8); };
            auto u = [&](size_t o) { return uint32_t(uint16_t(h(o))) | uint32_t(uint16_t(h(o + 2))) << 16; };
            Slot& t = out[i];
            t.accel = {h(0), h(2), h(4)}, t.flags = uint16_t(h(6)), t.offset = {h(0x10), h(0x12), h(0x14)};
            t.off = {h(0x18), h(0x1a), h(0x1c)}, t.rgb1 = {r[0x20], r[0x21], r[0x22]}, t.rgb2 = {r[0x24], r[0x25], r[0x26]};
            for (auto [c, o] : {std::pair{&t.ang, 0x28u}, {&t.size, 0x34u}, {&t.bright, 0x40u}})
                c->v = {h(o), h(o + 2), h(o + 4)}, c->r0 = h(o + 6), c->r1 = h(o + 8), c->at = int8_t(r[o + 10]);
            t.speed = h(0x4c), t.drag = h(0x4e), t.spread = h(0x50), t.count = r[0x52], t.delay = r[0x53], t.life = r[0x54];
            t.f55 = r[0x55], t.sizeSpread = h(0x56), t.sprite = u(0x8c) ? 0 : -1, t.frames = r[0x90], t.hold = r[0x91];
            t.f92 = h(0x92), t.frame = r[0x98], t.sub = r[0x99], t.surface = h(0x9a);
        }
        return out;
    };
    auto parts = [](const char* s) {
        std::array<std::vector<Part>, 20> out{};
        for (const char* p = s; *p;) {
            char* e = nullptr;
            long v[13];
            for (long& x : v) x = std::strtol(p, &e, 10), p = e;
            ++p;  // ';'
            Part q;
            q.pos = {int16_t(v[1]), int16_t(v[2]), int16_t(v[3])}, q.frame = uint8_t(v[4]), q.age = int8_t(v[5]);
            q.vel = {int16_t(v[6]), int16_t(v[7]), int16_t(v[8])}, q.size = int16_t(v[9]), q.bright = int16_t(v[10]);
            q.hold = uint8_t(v[11]), q.cycle = uint8_t(v[12]);
            out[size_t(v[0])].push_back(q);
        }
        return out;
    };
    auto same = [](const Part& a, const Part& b) {
        return a.pos == b.pos && a.vel == b.vel && a.frame == b.frame && a.age == b.age && a.size == b.size &&
               a.bright == b.bright && a.hold == b.hold && a.cycle == b.cycle;
    };
    // The sampler reads the running exe between and sometimes during frames: a sample whose particles did not all age
    // since the previous one (ages tick every frame) caught the update midway and is not used.
    auto clean = [&](size_t k) {
        if (k == 0 || kFxOracle[k].counter != kFxOracle[k - 1].counter + 2) return false;
        const auto p0 = parts(kFxOracle[k - 1].parts), p1 = parts(kFxOracle[k].parts);
        for (size_t i = 0; i < 20; ++i)
            if (p0[i].size() == p1[i].size())
                for (size_t j = 0; j < p1[i].size(); ++j)
                    if (p1[i][j].age == p0[i][j].age) return false;
        return true;
    };
    int compared = 0, bad = 0, rebirths = 0, used = 0;
    for (size_t k = 0; k + 1 < std::size(kFxOracle); ++k) {
        const FxOracleTick& a = kFxOracle[k];
        const FxOracleTick& b = kFxOracle[k + 1];
        if (!clean(k) || !clean(k + 1)) continue;
        ++used;
        w.fx_ = slots(a.slots);
        const auto pa = parts(a.parts), pb = parts(b.parts);
        size_t n = 0;
        for (size_t i = 0; i < 20; ++i) w.fx_[i].parts = pa[i], n += pa[i].size();
        w.fxFree_ = 120 - n;
        w.cam.eye = {b.eye[0], b.eye[1], b.eye[2]}, w.cam.look = {b.look[0], b.look[1], b.look[2]};
        w.fxTick();
        for (size_t i = 0; i < 20; ++i) {
            const auto& ours = w.fx_[i].parts;
            // a new emission (age 1 at the tail, fed by the entities' logic) replacing an expired one: not this step's
            if (ours.size() != pb[i].size() || (!ours.empty() && pb[i].back().age == 1 && ours.back().age != 1)) continue;
            if (pb[i].size() > pa[i].size()) rebirths += int(pb[i].size() - pa[i].size());
            for (size_t j = 0; j < ours.size(); ++j, ++compared)
                if (!same(ours[j], pb[i][j]) && bad++ < 8)
                    std::printf("  fx %d slot %zu #%zu: ours (%d %d %d) v (%d %d %d) a %d s %d b %d h %d c %d | oracle (%d %d %d) v (%d %d %d) a %d s %d b %d h %d c %d\n",
                                b.counter, i, j, ours[j].pos[0], ours[j].pos[1], ours[j].pos[2], ours[j].vel[0], ours[j].vel[1],
                                ours[j].vel[2], ours[j].age, ours[j].size, ours[j].bright, ours[j].hold, ours[j].cycle,
                                pb[i][j].pos[0], pb[i][j].pos[1], pb[i][j].pos[2], pb[i][j].vel[0], pb[i][j].vel[1], pb[i][j].vel[2],
                                pb[i][j].age, pb[i][j].size, pb[i][j].bright, pb[i][j].hold, pb[i][j].cycle);
        }
    }
    std::printf("  fx: %d steps, %d particle states compared (%d surface re-births), %d differ\n", used, compared, rebirths, bad);
    assert(bad == 0 && compared > 300 && rebirths >= 12);
}

// No logic item of the 58 levels uses the line-of-sight condition (W sub-mode 0x82000000): rayHit's actor pass is
// unreachable from the PC data, so only the synthetic AZBOSS02 check above exercises it.
void lineOfSightUnused(const fs::path& dir) {
    int items = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const BzeFile bze = loadBze(e.path());
        const std::vector<uint8_t> s1 = bze.payload(size_t(bze.find(1)));
        for (const TbChunk& ch : parseTbScript(s1).chunks)
            for (const TbTag& t : ch.tags) {
                if (t.id != 0x57 || t.data.size() < 8) continue;
                ++items;
                assert((uint32_t(t.data[7]) << 24 & 0x8f000000u) != 0x82000000u);
            }
    }
    assert(items > 1000);
}

// Data scan: every 'R' / 'S' / 'U' item of the 58 levels (section-1 tags) runs once on Bugs of one ARCA01 world (U
// targets rewritten to self, selector 0xc); none may be left unsupported. allLevels only meets what fires early.
void opsScan(const fs::path& dir) {
    Level host = load(dir / "ARCA01.bze");
    int items = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const BzeFile bze = loadBze(e.path());
        for (const TbChunk& ch : parseTbScript(bze.payload(size_t(bze.find(1)))).chunks) {
            bool inBlock = false;  // as parseBlocks: 'N'..'`' items after an 'M' header, until another tag
            for (const TbTag& t : ch.tags) {
                if (t.id == 0x4d || t.id < 0x4e || t.id > 0x60) inBlock = t.id == 0x4d;
                if (!inBlock || (t.id != 0x52 && t.id != 0x53 && t.id != 0x55)) continue;
                TbLogicItem it;
                it.tag = t.id;
                std::copy_n(t.data.begin(), std::min(t.data.size(), it.d.size()), it.d.begin());
                if (t.id == 0x55) it.d[8] = 0xc, it.d[9] = it.d[10] = it.d[11] = 0;
                host.w.runItem(host.w.actors[0], it);
                ++items;
            }
        }
    }
    std::string un;
    for (const std::string& u : host.w.unsupported) un += " " + u;
    std::printf("  ops scan: %d R / S / U items, unsupported:%s\n", items, un.c_str());
    assert(host.w.unsupported.empty() && items > 5000);
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
            assert(l.w.poolUsed() <= l.w.poolCap && l.w.poolCap < (1 << 30));  // the load fits the pool
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
    assert(!unsupported.contains("camera track path"));  // no path recorder (flags1 0x40) in any PC level
}

// 0x439700 scripted points ('_' op 0x15, B+0x3c 0x400 eye / 0x800 look): placed, 0x4000 makes the eye the restored
// pose, one-shot 0x10000 drops the eye point after its tick, 0x20 eases the look by 1/4.
void camScriptPoints(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 5; ++i) w.tick({});
    const V3 e{100, -500, 200}, k{900, -400, 1000}, k2{1300, -440, 1400};
    w.camScript.eye = e, w.camScript.look = k, w.camScript.flags |= 0xc00 | 0x4000 | 0x10000;
    w.tick({});
    assert(w.cam.eye == e && w.cam.look == k && w.cam.body == e && w.cam.prevEye == e && w.cam.prevBody == e);
    assert((w.camScript.flags & 0x14c00) == 0x800);
    w.camScript.look = k2, w.camScript.flags |= 0x20;
    w.tick({});
    assert(w.cam.look == (V3{k.x - ((k.x - k2.x) >> 2), k.y - ((k.y - k2.y) >> 2), k.z - ((k.z - k2.z) >> 2)}) && w.cam.eye == e);
    w.camScript.flags &= ~0x820u;
    w.tick({});  // the follow resumes from the restored pose
    assert(w.cam.eye != e);
    std::puts("  camera script points: placed, eased, one-shot");
}

// Demo playback (0x4014c0 / 0x4347c0 / 0x4615b0) needs 0x4d4fae = 1, set only by 0x385 idle ticks while GD+0 is 0
// (TITLE) or 2 (LANGUAGE); its stream is chunk 0x46 tag 0x47 of the reloaded level (-> 0x4d2700, GD+0x8a = 1). The PC
// disc ships neither level, no level-change V (0x1000) targets them, and the exe has no record / playback switch
// (0x415980 parses /bze: /b /x /y /no_sync /no_skip /full /win /ogl /opengl /soft* /skip_intro /cons /fntp /dll: /comp
// /ask /?): the 8 PS1-era streams below are dead data.
void demoAbsent(const fs::path& dir) {
    std::set<std::string> streams;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::string stem = e.path().stem().string();
        for (char& c : stem) c = char(std::toupper(uint8_t(c)));
        assert(stem != "TITLE" && stem != "LANGUAGE");
        const BzeFile bze = loadBze(e.path());
        const std::vector<uint8_t> s1 = bze.payload(size_t(bze.find(1)));
        for (const TbChunk& ch : parseTbScript(s1).chunks) {
            if (ch.type == 0x46 && !ch.tag(0x47).empty()) streams.insert(stem);
            for (const TbTag& t : ch.tags)
                if (t.id == 0x56 && t.data.size() >= 8) {
                    const uint32_t u = uint32_t(t.data[0] | t.data[1] << 8 | t.data[2] << 16 | uint32_t(t.data[3]) << 24);
                    const uint32_t p1 = uint32_t(t.data[4] | t.data[5] << 8 | t.data[6] << 16 | uint32_t(t.data[7]) << 24);
                    if (!(u & 0x80000000u) && (u & 0x1000)) assert(p1 != 0 && p1 != 2);
                }
        }
    }
    std::string list;
    for (const std::string& s : streams) list += " " + s;
    std::printf("demo streams (unreachable on PC):%s\n", list.c_str());
    assert(streams == (std::set<std::string>{"ARCASG02", "ARR01", "AZCA01", "AZCASG05", "GRANNY", "TRBOSS01", "VIBOSS01", "VICASG01"}));
}

// 0x461100 / 0x460f80 on synthetic reports: remap preset 2 (□ -> L1 0x4, ✕ -> □ 0x80; the digital remap is held back
// while paused), the joystick axis (DirectInput -127..128, dead zone 2000) and the 0x4d..0xb1 dead zone.
void padLayer() {
    const uint16_t preset2[8] = {4, 8, 0x10, 0x80, 0x20, 1, 0x40, 2};
    uint8_t words[16];
    for (size_t k = 0; k < 8; ++k) words[2 * k] = uint8_t(preset2[k]), words[2 * k + 1] = 0;
    TbPad p;
    PadInput in = keys(PadSquare | PadCross | PadUp);
    tbPadTick(p, in, 0xd0, words, false);
    assert(p.held == (PadUp | 4 | 0x80) && p.raw == (PadUp | PadSquare | PadCross) && p.sy == 0x7f && p.sx == 0);
    tbPadTick(p, in, 0xd0, words, true);
    assert(p.held == (PadUp | PadSquare | PadCross));
    assert(tbJoyAxis(127) == 0x80 && tbJoyAxis(152) == 0x80 && tbJoyAxis(0) == 0x01 && tbJoyAxis(255) == 0xff);
    in = {};
    in.analog = true, in.lx = 40, in.ly = 200;  // inside the report's 0x10..200 band: the analog branch
    tbPadTick(p, in, 0xd0, words, false);
    const int bx = tbJoyAxis(40), by = tbJoyAxis(200);
    assert(bx >= 0x10 && bx < 0x4d && by > 0xb1 && by <= 200);
    assert(p.sx == (bx - 0x4d) * 128 / 78 && p.sy == -((by - 0xb2) * 128) / 78 && (p.held & (PadLeft | PadDown)) == (PadLeft | PadDown));
    in.lx = 0;  // the stick's end sets the d-pad bit: digital branch, -0x80
    tbPadTick(p, in, 0xd0, words, false);
    assert(p.sx == -0x80 && (p.raw & PadLeft));
    std::puts("tb pad layer ok");
}

// View flag 1 (0x45eee0 @0x45f5d2) and the visible-zone table 0x4d56a0 the render's portal walk fills (0x465710 /
// 0x406840). ARBOSS01 tables read live under Wine (RUNNER=wine, /skip_intro /b42) together with the eye 0x4d5124 and
// look 0x4d5130 in one memsrv read (local/wine/compare/w13/zones/samples*.json): zones exact, rectangles within 2
// (the read is not frame-synchronous). ARCA01 (no portals): the zone-2 directors stay unticked from the zone-1 camera
// (capture B: +0x24 = 4, never 0x8000).
void zones(const fs::path& dir) {
    {
        Level f = load(dir / "ARBOSS01.bze");
        TbWorld& w = f.w;
        struct Sample { V3 eye, look; std::vector<std::array<int32_t, 5>> table; };
        const Sample samples[] = {
            {{2518, 1513, -18992}, {2541, 1647, -17788}, {{1, -256, -106, 256, 109}, {6, -320, 61, -202, 237}, {9, -320, 136, -246, 240}}},
            {{-3027, 21942, -8544}, {-2500, 22937, -9000},
             {{6, -256, -106, 256, 109}, {9, -83, 170, 61, 240}, {1, -29, 68, 264, 240}, {12, -164, 132, -48, 240},
              {5, -163, 48, -7, 240}, {7, -37, 110, 147, 240}, {8, -24, 140, 181, 240}}},
            {{-2676, 26150, -8206}, {-2500, 27135, -9000},
             {{6, -256, -106, 256, 109}, {9, 24, 225, 130, 240}, {1, -24, 170, 140, 240}, {12, -62, 178, 30, 240},
              {5, -194, 148, 107, 240}, {7, 64, 199, 178, 240}, {8, 85, 229, 245, 240}}},
            {{-3091, 18125, -9238}, {-2500, 19101, -9000},
             {{6, -256, -106, 256, 109}, {9, -249, 178, -57, 240}, {1, -316, 163, 267, 240}, {5, -320, 213, 136, 240},
              {7, -275, 126, 56, 240}, {8, -223, 64, -2, 240}}},
            {{-3086, 11633, -9676}, {-2500, 12514, -9000},
             {{6, -256, -106, 256, 109}, {1, -320, 144, 249, 240}, {7, -320, 212, 3, 240}, {8, -320, 144, -139, 240}}},
        };
        assert(w.findZone({2526, 1558, -18594}) == 1);  // the first sample's camera body
        for (const Sample& s : samples) {
            w.cam.eye = s.eye, w.cam.look = s.look, w.camZone = uint8_t(s.table[0][0]);
            w.portalWalk();
            for (size_t i = 0; i < w.visZones.size(); ++i) {
                const TbWorld::VisZone& v = w.visZones[i];
                if (i >= s.table.size()) { assert(v.zone == -1); continue; }
                const auto& t = s.table[i];
                assert(v.zone == t[0] && std::abs(v.x0 - t[1]) <= 2 && std::abs(v.y0 - t[2]) <= 2 &&
                       std::abs(v.x1 - t[3]) <= 2 && std::abs(v.y1 - t[4]) <= 2);
            }
        }
    }
    Level f = load(dir / "ARCA01.bze");
    TbWorld& w = f.w;
    std::vector<const TbActor*> dirs;
    for (const TbActor& e : w.entities)
        if (e.behaviour == 16 && (e.view & 1) && e.zone == 2) dirs.push_back(&e);
    assert(dirs.size() == 12);
    for (int i = 0; i < 300; ++i) {
        w.tick({});
        assert(w.camZone == 1 && w.visZones[0].zone == 1 && w.visZones[1].zone == -1);
        for (const TbActor* d : dirs) assert(!d->active && (d->f24 & 4) && !(d->f24 & 0x8000));
    }
    std::puts("tb zones ok (ARBOSS01 portal tables, ARCA01 zone-2 directors unticked)");
}

// Entity classes 21 / 18 / 5 and the 0x453de0 ride / push blocks = oracle local/wine/compare/w13/classes (cap.py /
// pushpoke.py there, RUNNER=wine; tb_classes.inc by mkinc.py): each world is ticked from the capture's first row with
// the captured state poked in, then every later row must match field for field.
#include "tb_classes.inc"
int mismatches = 0;
template <class... T> void expect(bool ok, int c, const char* what, T... v) {
    if (ok) return;
    if (++mismatches <= 12) {
        std::printf("  counter %d: %s", c, what);
        ((std::printf(" %d", int(v))), ...);
        std::puts("");
    }
}
void classes(const fs::path& dir) {
    {  // ARCA02: Bugs poked 400 above the class-21 platform (amp 3350 along z, speed 25) falls on, rides on +0x104.
        Level l = load(dir / "ARCA02.bze");
        TbWorld& w = l.w;
        w.tick({});
        std::array<TbActor*, 4> p{};
        for (TbActor* e : w.list())
            for (size_t k = 0; k < 4; ++k)
                if (e->behaviour == 21 && e->home.x == kRideHome[k][0] && e->home.z == kRideHome[k][1]) p[k] = e;
        for (TbActor* e : p) assert(e && "class-21 centre = spawn + record (0, 0, amp / 2)");
        for (size_t k = 0; k < 4; ++k) p[k]->pos = {kRide[0].plat[k][0], kRide[0].plat[k][1], kRide[0].plat[k][2]}, p[k]->phase = kRide[0].plat[k][3];
        TbActor& b = w.actors[0];
        b.pos = {kRide[0].bugs[0], kRide[0].bugs[1], kRide[0].bugs[2]};
        mismatches = 0;
        for (size_t i = 1; i < std::size(kRide); ++i) {
            const RideRow& r = kRide[i];
            for (int t = 0; t < (r.c - kRide[i - 1].c) / 2; ++t) w.tick({});
            for (size_t k = 0; k < 4; ++k)
                expect(p[k]->pos == V3{r.plat[k][0], r.plat[k][1], r.plat[k][2]} && p[k]->phase == r.plat[k][3], r.c, "platform",
                       k, p[k]->pos.x, p[k]->pos.y, p[k]->pos.z, p[k]->phase);
            expect(b.pos == V3{r.bugs[0], r.bugs[1], r.bugs[2]}, r.c, "bugs pos", b.pos.x, b.pos.y, b.pos.z, r.bugs[0], r.bugs[1], r.bugs[2]);
            expect((b.parent >= 0) == bool(r.par) && b.rideRec == bool(r.rec), r.c, "bugs parent", b.parent, b.rideRec);
            expect(!r.par || b.local == V3{r.loc[0], r.loc[1], r.loc[2]}, r.c, "bugs local", b.local.x, b.local.y, b.local.z, r.loc[0], r.loc[1], r.loc[2]);
            expect((b.f20 & 0x10a) == (r.f20 & 0x10a), r.c, "bugs f20", b.f20, r.f20);
            expect(std::max({p[0]->mass, p[1]->mass, p[2]->mass, p[3]->mass}) == r.mass, r.c, "mass", p[0]->mass, r.mass);
        }
        std::printf("  ride: %d mismatches over %zu rows\n", mismatches, std::size(kRide));
        assert(!mismatches);
    }
    {  // VICASG04: Bugs poked 600 above the class-18 seesaw lands right of its pivot; his moment (local x · mass) tilts
       // it to the 0x164 limit (+0x42 4096 -> 8192 with him on it, +0x82 held at 0 while ridden).
        Level l = load(dir / "VICASG04.bze");
        TbWorld& w = l.w;
        w.tick({});
        TbActor* s = nullptr;
        for (TbActor* e : w.list())
            if (e->behaviour == 18) s = e;
        assert(s);
        for (int i = 0; i < 120; ++i) w.tick({});  // past the level-start walk-in: Bugs idle as in the capture
        s->stateTicks = kSeesaw[0].t82;
        TbActor& b = w.actors[0];
        b.pos = {kSeesaw[0].bugs[0], kSeesaw[0].bugs[1], kSeesaw[0].bugs[2]};
        // The exe listed (ticked) the seesaw every tick; our spring camera stays behind the poke, so view from Bugs.
        w.freeView = std::array<V3, 2>{V3{15450, -5000, 11500}, s->pos};
        mismatches = 0;
        for (size_t i = 1; i < std::size(kSeesaw); ++i) {
            const SeesawRow& r = kSeesaw[i];
            for (int t = 0; t < (r.c - kSeesaw[i - 1].c) / 2; ++t) w.tick({});
            expect(s->roll == r.roll && s->phase == r.phase && s->mass == r.mass && s->stateTicks == r.t82, r.c, "seesaw",
                   s->roll, s->phase, s->mass, s->stateTicks, r.roll, r.phase, r.mass, r.t82);
            expect(b.pos == V3{r.bugs[0], r.bugs[1], r.bugs[2]}, r.c, "bugs pos", b.pos.x, b.pos.y, b.pos.z, r.bugs[0], r.bugs[1], r.bugs[2]);
            expect((b.parent >= 0) == bool(r.par), r.c, "bugs parent", b.parent);
            expect(!r.par || b.local == V3{r.loc[0], r.loc[1], r.loc[2]}, r.c, "bugs local", b.local.x, b.local.y, b.local.z, r.loc[0], r.loc[1], r.loc[2]);
            expect((b.f20 & 0x10a) == (r.f20 & 0x10a), r.c, "bugs f20", b.f20, r.f20);
        }
        std::printf("  seesaw: %d mismatches over %zu rows\n", mismatches, std::size(kSeesaw));
        assert(!mismatches);
    }
    {  // VIBOSS01 class 40 (0x44f7b0 mode 0x1000: the seesaw rule with the +0x40 limit, 200): Bugs poked 600 above the
       // plank, 500 left of its pivot, lands on it and tilts it to -200 (+0x42 4096 -> 8192).
        Level l = load(dir / "VIBOSS01.bze");
        TbWorld& w = l.w;
        w.tick({});
        TbActor* s = nullptr;
        for (TbActor* e : w.list())
            if (e->behaviour == 40 && e->pos == V3{kC40Pivot[0], kC40Pivot[1], kC40Pivot[2]}) s = e;
        assert(s);
        w.freeView = std::array<V3, 2>{V3{s->pos.x, s->pos.y - 1500, s->pos.z - 2500}, s->pos};
        for (int i = 0; i < 120; ++i) w.tick({});
        assert(s->inst);
        s->stateTicks = kC40[0].t82;
        TbActor& b = w.actors[0];
        b.pos = {kC40[0].bugs[0], kC40[0].bugs[1], kC40[0].bugs[2]};
        mismatches = 0;
        for (size_t i = 1; i < std::size(kC40); ++i) {
            const SeesawRow& r = kC40[i];
            for (int t = 0; t < (r.c - kC40[i - 1].c) / 2; ++t) w.tick({});
            expect(s->roll == r.roll && s->phase == r.phase && s->mass == r.mass && s->stateTicks == r.t82, r.c, "plank",
                   s->roll, s->phase, s->mass, s->stateTicks, r.roll, r.phase, r.mass, r.t82);
            expect(b.pos == V3{r.bugs[0], r.bugs[1], r.bugs[2]}, r.c, "bugs pos", b.pos.x, b.pos.y, b.pos.z, r.bugs[0], r.bugs[1], r.bugs[2]);
            expect((b.parent >= 0) == bool(r.par), r.c, "bugs parent", b.parent);
            expect(!r.par || b.local == V3{r.loc[0], r.loc[1], r.loc[2]}, r.c, "bugs local", b.local.x, b.local.y, b.local.z, r.loc[0], r.loc[1], r.loc[2]);
        }
        std::printf("  class 40 plank: %d mismatches over %zu rows\n", mismatches, std::size(kC40));
        assert(!mismatches);
    }
    {  // AZCASG02: the class-5 block (spawn x 21250) driven as the 0x453de0 push block drives it: +0x58 := C, +0x24 |=
       // 0x10 between ticks (C = (60, 0, 0) for 1.5 s, then (0, 0, -60)); it slides +x into the wall, then -z. A tick
       // runs with the C sampled after the previous one; the poker races the tick (row 26577 reads a cleared carry):
       // the tick of 26567 ran without it (+0x194 kept 0xe one tick longer, nothing moved).
        Level l = load(dir / "AZCASG02.bze");
        TbWorld& w = l.w;
        w.tick({});
        TbActor* k = nullptr;
        for (TbActor* e : w.list())
            if (e->behaviour == 5 && e->spawn.x == 21250) k = e;
        assert(k && k->inst);
        // Seeded from the second row: the first poke tick's timing against the capture is not known.
        k->pos = {kPoke[1].pos[0], kPoke[1].pos[1], kPoke[1].pos[2]}, k->vel = {kPoke[1].vel[0], kPoke[1].vel[1], kPoke[1].vel[2]};
        w.freeView = std::array<V3, 2>{V3{21000, -1500, -1500}, k->pos};
        mismatches = 0;
        for (size_t i = 2; i < std::size(kPoke); ++i) {
            const PokeRow& r = kPoke[i];
            for (int t = 0; t < (r.c - kPoke[i - 1].c) / 2; ++t) {
                const PokeRow& p = kPoke[i - 1];
                if ((p.poke[0] || p.poke[2]) && r.c != 26567) k->carry = {p.poke[0], p.poke[1], p.poke[2]}, k->f24 |= 0x10;
                w.tick({});
            }
            expect(k->pos == V3{r.pos[0], r.pos[1], r.pos[2]} && uint16_t(k->hover) == r.mask, r.c, "block", k->pos.x, k->pos.y,
                   k->pos.z, k->hover, r.pos[0], r.pos[1], r.pos[2], r.mask);
        }
        std::printf("  push block: %d mismatches over %zu rows\n", mismatches, std::size(kPoke));
        assert(!mismatches);
    }
    {  // ARCA01 class 2 (links 312..314, anim 215: one step looping, flags 0x3201): GAME_DATA pokes switch each to anim
       // 217 the next tick (0x1201, alias word 344 -> 343, then 0x3201).
        Level l = load(dir / "ARCA01.bze");
        TbWorld& w = l.w;
        std::array<TbActor*, 3> e{};
        for (TbActor* a : w.list())
            for (size_t k = 0; k < 3; ++k)
                if (a->behaviour == 2 && a->texLink == kLinkIds[k]) e[k] = a;
        for (TbActor* a : e) assert(a);
        for (int i = 0; i < 40; ++i) w.tick({});
        mismatches = 0;
        for (size_t i = 0; i < std::size(kLinks); ++i) {
            const LinkRow& r = kLinks[i];
            if (i) {
                if (r.c == 1491) w.GD[0xd8] = 1;
                if (r.c == 1551) w.GD[0xd9] = 1;
                if (r.c == 1611) w.GD[0xda] = 1;
                w.tick({});
            }
            for (size_t k = 0; k < 3; ++k) {
                const TbActor& a = *e[k];
                const auto al = w.textureLinks.find(kLinkIds[k]);
                expect(al != w.textureLinks.end() && al->second == r.alias[k] && a.curAnim == r.anim[k] && a.spr.frame == r.frame[k] &&
                           a.spr.flags == r.flags[k] && a.spr.reload == r.reload[k] && a.spr.countdown == r.count[k],
                       r.c, "link", k, al == w.textureLinks.end() ? -1 : al->second, a.curAnim, a.spr.frame, a.spr.flags, a.spr.reload,
                       a.spr.countdown, r.alias[k], r.anim[k], r.flags[k]);
            }
        }
        std::printf("  class 2 links: %d mismatches over %zu rows\n", mismatches, std::size(kLinks));
        assert(!mismatches);
    }
    {  // VIQ02: one life of a class-4 spark (template 75, thrown by entity 74): a record every 2 ticks; its block removes
       // it on the 9th tick, when the flipbook wraps (+0x14e = 0x2000).
        Level l = load(dir / "VIQ02.bze");
        TbWorld& w = l.w;
        w.tick({});
        TbActor* a = w.spawn(75, {-10473, -1135, 4922}, 0);
        assert(a);
        w.freeView = std::array<V3, 2>{V3{-10736, -1500, 6254}, a->pos};
        mismatches = 0;
        for (const SprRow& r : kSpark) {
            w.tick({});
            expect(a->stateTicks == r.t82 && a->spr.frame == r.frame && a->spr.flags == r.flags && a->spr.reload == r.reload &&
                       a->spr.countdown == r.count && a->spr.last == r.last && a->radius == r.radius &&
                       a->box == std::array<int16_t, 6>{r.box[0], r.box[1], r.box[2], r.box[3], r.box[4], r.box[5]},
                   r.c, "spark", a->stateTicks, a->spr.frame, a->spr.flags, a->spr.reload, a->spr.countdown, a->spr.last, a->box[0], a->box[4], a->radius);
        }
        w.tick({});
        expect(a->f20 & 0x2000, 0, "spark not removed on its 9th tick");
        std::printf("  class 4 spark: %d mismatches over %zu rows\n", mismatches, std::size(kSpark));
        assert(!mismatches);
    }
    {  // ARCASG03 class 33 (id 72) hung on Taz's part: world heading 0 (own yaw = minus Taz's), anim 211 looping.
        Level l = load(dir / "ARCASG03.bze");
        TbWorld& w = l.w;
        // Taz's logic spawns it on his part at the level start; aligned on its tick count +0x82 (the capture's first row).
        const TbActor* h = nullptr;
        while (!h || h->stateTicks < kHung[0].t82) {
            w.tick({});
            for (TbActor* a : w.list())
                if (a->behaviour == 33 && a->id == 72 && !(a->f20 & 0x2000)) h = a;
            assert(w.ticks < 2000);
        }
        mismatches = 0;
        for (size_t i = 0; i < std::size(kHung); ++i) {
            const HungRow& r = kHung[i];
            if (i) w.tick({});
            if (i && r.t82 == kHung[i - 1].t82) continue;  // a stale sample (the previous row repeated: 901, 1081)
            expect(h->pos == V3{r.pos[0], r.pos[1], r.pos[2]} && h->yaw == r.yaw && h->localYaw == r.localYaw && h->anim.frame == r.frame &&
                       h->stateTicks == r.t82,
                   r.c, "class 33", h->pos.x, h->pos.y, h->pos.z, h->yaw, h->localYaw, h->anim.frame, h->stateTicks, r.pos[0], r.pos[2], r.yaw,
                   r.localYaw, r.frame, r.t82);
        }
        std::printf("  class 33: %d mismatches over %zu rows\n", mismatches, std::size(kHung));
        assert(!mismatches);
    }
}

// Carry = oracle local/wine/compare/w13/carry/run.sh (RUNNER=wine, Granny, dialogs dismissed): Bugs poked before the
// carriable 267 (flags0 0x2000) with the state-flagsB-2 probe up (+0x20 0x200000, 0x44e7d0); △ enters the grab state
// 147 (flagsA 0x20), whose attach-part frame hangs the item on his hand (0x453de0 grab block); state 176 (flagsB
// 0x40000) puts it down at his feet (release block, 0x450650 clamp: nothing hit); □ (state 6460 in the capture: his
// kick) and △ again repeat it.
#include "tb_carry.inc"
void carry(const fs::path& dir) {
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int k = 0; k < 5; ++k) {  // the welcome and Granny text boxes
        for (int i = 0; i < 2; ++i) w.tick(keys(PadCross));
        for (int i = 0; i < 40; ++i) w.tick({});
    }
    TbActor& b = w.actors[0];
    TbActor* item = nullptr;
    for (TbActor& e : w.entities)
        if (e.id == 267) item = &e;
    assert(item);
    b.pos = {kCarry[0].bugs[0], kCarry[0].bugs[1], kCarry[0].bugs[2]}, b.yaw = 0;
    mismatches = 0;
    int c = kCarry[0].c, grabs = 0;
    for (size_t i = 1; i < std::size(kCarry); ++i) {
        const CarryRow& r = kCarry[i];
        for (; c < r.c; c += 2) w.tick(c + 2 == 1637 || c + 2 == 2005 ? keys(PadTriangle) : c + 2 == 1821 ? keys(PadSquare) : PadInput{});
        grabs += item->parent == 0 && r.par && !kCarry[i - 1].par;
        expect(b.pos == V3{r.bugs[0], r.bugs[1], r.bugs[2]} && (b.f20 & 0x200102) == (r.f20 & 0x200102), r.c, "bugs", b.pos.x,
               b.pos.y, b.pos.z, b.f20, r.bugs[0], r.bugs[1], r.bugs[2], r.f20);
        expect(item->pos == V3{r.item[0], r.item[1], r.item[2]} && (item->parent == 0) == bool(r.par) && (item->f20 & 0x418102) == (r.if20 & 0x418102),
               r.c, "item", item->pos.x, item->pos.y, item->pos.z, item->parent, item->f20, r.item[0], r.item[1], r.item[2], r.par, r.if20);
    }
    std::printf("  carry: %d mismatches over %zu rows, %d grabs\n", mismatches, std::size(kCarry), grabs);
    assert(!mismatches && grabs == 2);
}

// Blob shadows / floor shade = oracle local/wine/compare/w13/shadow/run.sh (RUNNER=wine; capture.py hooks 0x464090's
// tail and logs every placement). ARCA01 after the ✕ skip: the listed entities' shadows (players, item 267, props,
// guard 417). GRANNY, dialogs dismissed: Bugs and Taz poked 200 above the shade floor (surface 1, flag 0x80): the ray
// finds it while they fall (target 0x10, +0x28 0x1000), the shade steps 2 per tick to 16, the shadow lies on the floor.
// The poke waits for Taz's return to idle 21 from a fidget, rand synced per tick: the idle fidget is block 15's roll
// (last frame shown, +0x82 > 30, rand & 0xffff > 0x55f0), so both players' fidgets (32 / 17 / 18 / 19 / 20) match.
#include "tb_shadow.inc"
void shadows(const fs::path& dir) {
    // {rot x, y, z, x, y, z}: rot y is the entity's yaw (+0xf6; spinning props differ by the run's timing); x / z = the
    // hips' origin for animated entities, so only compared where both runs are in the same frame (`xz`).
    auto same = [](const TbWorld::Shadow& s, const TbActor& e, const int* o, bool xz = true) {
        return s.rot[0] == o[0] && s.rot[1] == e.yaw && s.rot[2] == o[2] && s.pos.y == o[4] && (!xz || (s.pos.x == o[3] && s.pos.z == o[5]));
    };
    auto find = [](const TbWorld& w, uint32_t uid) -> const TbWorld::Shadow* {
        for (const TbWorld::Shadow& s : w.shadows)
            if (s.uid == uid) return &s;
        return nullptr;
    };
    mismatches = 0;
    {
        Level l = load(dir / "ARCA01.bze");
        TbWorld& w = l.w;
        for (int i = 0; i < 40; ++i) w.tick({});
        for (int i = 0; i < 3; ++i) w.tick(keys(PadCross));
        for (int i = 0; i < 150; ++i) w.tick({});
        int found = 0;
        for (const auto& o : kArcaShadows) {
            const TbActor* e = nullptr;
            for (const TbActor* a : w.list())
                if (a->id == o[0] && a->pos == V3{o[1], o[2], o[3]}) e = a;
            if (!e) continue;  // the guard walks: only entities at the oracle's place are compared
            ++found;
            const TbWorld::Shadow* s = find(w, e->uid);
            expect(s && same(*s, *e, o + 4, e->anim.parts.empty()), 907, "arca shadow", o[0], s ? s->rot[0] : 0, s ? s->rot[1] : 0, s ? s->rot[2] : 0,
                   s ? s->pos.x : 0, s ? s->pos.y : 0, s ? s->pos.z : 0, o[7], o[8], o[9]);
        }
        assert(found >= 6 && w.shadows.size() >= 6);
    }
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int k = 0; k < 5; ++k) {  // the welcome and Granny text boxes
        for (int i = 0; i < 2; ++i) w.tick(keys(PadCross));
        for (int i = 0; i < 40; ++i) w.tick({});
    }
    // Sync point (capture.py): Taz just back in his idle 21 from a fidget (20 -> 21, the clip restarted) at the oracle's
    // +0x82; Bugs set to the oracle's state and age, the rand seed from the oracle.
    for (int i = 0, prev = 0; i < 3000; ++i) {
        const TbActor& t = w.actors[1];
        if (prev == 20 && t.rec()->id == 21) {
            while (w.actors[1].stateTicks < kShadeStart[2]) w.tick({});
            break;
        }
        prev = t.rec()->id;
        w.tick({});
    }
    assert(w.actors[1].rec()->id == 21 && w.actors[1].stateTicks == kShadeStart[2]);
    w.actors[0].pos = {5034, -2800, 6551}, w.actors[1].pos = {5227, -2800, 6416};
    {  // Bugs into idle as the end of a sequence does (event 1's transition, its clip restarted): not the pre-sync fidget
        TbActor& a = w.actors[0];
        for (size_t i = 0; i < a.trans.size(); ++i)
            if (a.trans[i].event == 1) a.tr = int32_t(i);
        const bool entered = w.setState(a, uint16_t(kShadeStart[4]));
        assert(entered && a.tr >= 0 && a.trans[size_t(a.tr)].seq[0] == kShadeStart[4]);
        a.stateTicks = uint16_t(kShadeStart[1]);
    }
    uint32_t seed = kShadeStart[3];
    for (const ShadeRow& r : kShade) {
        w.rng.seed = seed, seed = r.seed;
        w.tick({});
        if (r.torn) continue;
        for (uint32_t k = 0; k < 2; ++k) {
            const TbActor& a = w.actors[k];
            const int* o = r.v[k];
            const TbWorld::Shadow* s = find(w, a.uid);
            expect(int(a.f28 & 0x1000) == (o[0] & 0x1000) && a.shade == o[1] && a.shadeTarget == o[2] && a.pos == V3{o[3], o[4], o[5]} &&
                       s && same(*s, a, o + 6) && int(a.rec()->id) == r.st[k],
                   r.c, k ? "taz shade" : "bugs shade", a.rec()->id, a.shade, a.shadeTarget, a.pos.y, s ? s->rot[0] : 0, s ? s->rot[2] : 0,
                   s ? s->pos.x : 0, s ? s->pos.y : 0, s ? s->pos.z : 0, o[1], o[2], o[4], o[6], o[8], o[9], o[10], o[11]);
        }
    }
    // The follow camera after chasing the poked players through the shade corner (0x437800's sweep 0x440360 against the
    // walls): the oracle's view read between counters 1973 and 1979 (o_GRANNY_shade.txt, eye / look).
    for (int c = std::end(kShade)[-1].c; c < 1979; c += 2) {
        w.tick({});
        if (c + 2 >= 1973)
            expect(w.cam.eye == V3{6201, -3258, 6399} && w.cam.look == V3{5034, -2908, 6552}, c + 2, "shade camera", w.cam.eye.x,
                   w.cam.eye.y, w.cam.eye.z, w.cam.look.x, w.cam.look.y, w.cam.look.z);
    }
    std::printf("  shadows: %d mismatches (ARCA01 %zu listed shadows, GRANNY shade %zu ticks)\n", mismatches, std::size(kArcaShadows),
                std::size(kShade));
    assert(!mismatches);
}

// Shadow on an entity shape = oracle local/wine/compare/w13/shapen/run.sh (RUNNER=wine): TRCA01, Bugs poked to
// (683, -900, -11998), 184 off the centre of entity 299 (flags1 0x200000, a sphere shape): while he falls the shadow ray
// of 0x464090 hits the sphere (0x4463e0 / 0x43cb60) and the shadow lies at its surface turned by the sphere's normal.
void shapeShadow(const fs::path& dir) {
    // {Bugs y, shadow rot x, rot z, shadow y} per tick from the poke
    static const int16_t kRows[][4] = {{-890, 0, 479, -136}, {-870, 0, 479, -136}, {-840, 0, 479, -136}, {-800, 0, 479, -136},
                                       {-750, 0, 479, -136}, {-690, 0, 479, -136}, {-620, 0, 479, -136}, {-540, 0, 479, -136},
                                       {-450, 0, 479, -136}, {-350, 0, 479, -136}, {-240, 0, 479, -136}};
    Level l = load(dir / "TRCA01.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 60; ++i) w.tick({});
    w.actors[0].pos = {683, -900, -11998};
    mismatches = 0;
    for (const auto& r : kRows) {
        w.tick({});
        const TbWorld::Shadow* s = nullptr;
        for (const TbWorld::Shadow& e : w.shadows)
            if (e.uid == 0) s = &e;
        expect(s && w.actors[0].pos.y == r[0] && s->rot[0] == r[1] && s->rot[2] == r[2] && s->pos.y == r[3], int(&r - kRows), "shape shadow",
               w.actors[0].pos.y, s ? s->rot[0] : 0, s ? s->rot[2] : 0, s ? s->pos.y : 0, r[0], r[1], r[2], r[3]);
    }
    std::printf("  shadow on an entity sphere: %d mismatches over %zu ticks\n", mismatches, std::size(kRows));
    assert(!mismatches);
}

// Throw = oracle local/wine/compare/w13/throw/run.sh (RUNNER=wine, Granny): item 267 poked throwable (flags0 0x2000 ->
// 0x800, so block 147 raises event 1 instead of 108), △ grabs it (147 -> carry idle 150); 10 ticks into 150 Bugs is
// poked to (11800, -1400, 6871) facing the wall near x 11430 and □ throws (state 160, flagsA 0x40): the release
// block detaches it (+0x20 0x10000) and the 0x450650 clamp pulls it back from behind the wall (11355 -> 11523), from
// where its thrown flight rebounds.
#include "tb_throw.inc"
void throwTrace(const fs::path& dir) {
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int k = 0; k < 5; ++k) {
        for (int i = 0; i < 2; ++i) w.tick(keys(PadCross));
        for (int i = 0; i < 40; ++i) w.tick({});
    }
    TbActor& b = w.actors[0];
    TbActor* item = nullptr;
    for (TbActor& e : w.entities)
        if (e.id == 267) item = &e;
    assert(item);
    item->flags0 = (item->flags0 & ~0x2000u) | 0x800;
    b.pos = {kThrow[0].bugs[0], kThrow[0].bugs[1], kThrow[0].bugs[2]}, b.yaw = 0;
    int grab = 0, poke = 0, thr = 0;
    for (size_t i = 1; i < std::size(kThrow); ++i) {
        if (!grab && kThrow[i].st == 147) grab = kThrow[i].c;
        if (!poke && kThrow[i].yaw) poke = kThrow[i].c;
        if (!thr && kThrow[i].st == 160) thr = kThrow[i].c;
    }
    mismatches = 0;
    int c = kThrow[0].c, released = 0;
    for (size_t i = 1; i < std::size(kThrow); ++i) {
        const ThrowRow& r = kThrow[i];
        // The oracle's rand stream (its menu boot drew differently). A row with the previous row's seed was read while
        // the tick ran (the counter steps first): it holds the previous tick, so it is neither checked nor seeded from.
        const bool stale = r.seed == kThrow[i - 1].seed;
        if (!stale && (i < 2 || kThrow[i - 1].seed != kThrow[i - 2].seed)) w.rng.seed = kThrow[i - 1].seed;
        for (; c < r.c; c += 2) {
            if (c + 2 == poke) b.pos = {r.bugs[0], r.bugs[1], r.bugs[2]}, b.yaw = r.yaw;
            w.tick(c + 2 == grab ? keys(PadTriangle) : c + 2 == thr ? keys(PadSquare) : PadInput{});
        }
        if (stale) continue;
        released += item->parent < 0 && kThrow[i - 1].par && !r.par;
        // States from the grab on (the fidget before it runs on Bugs' anim / +0x82 state from the oracle's boot).
        expect((r.c < grab || b.rec()->id == r.st) && b.pos == V3{r.bugs[0], r.bugs[1], r.bugs[2]}, r.c, "bugs", b.rec()->id, b.pos.x,
               b.pos.y, b.pos.z, r.st, r.bugs[0], r.bugs[1], r.bugs[2]);
        expect(item->pos == V3{r.item[0], r.item[1], r.item[2]} && (item->parent >= 0) == bool(r.par) && (item->f20 & 0x418122) == (r.if20 & 0x418122),
               r.c, "item", item->pos.x, item->pos.y, item->pos.z, item->parent, item->f20, r.item[0], r.item[1], r.item[2], r.par, r.if20);
    }
    std::printf("  throw: %d mismatches over %zu rows, %d releases\n", mismatches, std::size(kThrow), released);
    assert(!mismatches && released == 1);
}

// Look view + class 30 = oracle local/wine/compare/w13/class30/run.sh (RUNNER=wine, ARCA01 /b28, fly-over skipped with
// ✕, GD+0xa2 |= 4, 10 coins): L2 + R2 held enters the look view (0x439700, Bugs state 237, the eye 400 behind his head),
// Left turns it 40 a tick to -520, ✕ fires: block 237's '^' spawns template 151 (class 30, 0x44d1f0) at the look point
// along the look angles; it flies 3 ticks and hits the wall (removed, +0x20 0x2000). Release: the follow camera again.
#include "tb_class30.inc"
void class30(const fs::path& dir) {
    Level l = load(dir / "ARCA01.bze", false);
    TbWorld& w = l.w;
    while (w.ticks < 221) w.tick({});
    w.tick(keys(PadCross));  // the fly-over skip at counter 443 (the intro test's)
    while (2 * int(w.ticks) - 1 < kShot[0].c) w.tick({});
    w.GD[0xa2] |= 4, w.GD[0x58] = 10;
    int on = 0, off = 0, fire = 0;
    for (size_t i = 1; i < std::size(kShot); ++i) {
        if (!on && (kShot[i].fd4 & 2)) on = kShot[i].c;
        if (on && !off && !(kShot[i].fd4 & 2)) off = kShot[i].c;
        if (!fire && kShot[i].shot) fire = kShot[i].c;
    }
    TbActor& b = w.actors[0];
    mismatches = 0;
    int c = kShot[0].c;
    for (size_t i = 1; i < std::size(kShot); ++i) {
        const ShotRow& r = kShot[i];
        const bool stale = r.seed == kShot[i - 1].seed;
        if (!stale && (i < 2 || kShot[i - 1].seed != kShot[i - 2].seed)) w.rng.seed = kShot[i - 1].seed;
        for (; c < r.c; c += 2) {
            // The oracle presses and lets go of L2 one tick before R2: the ticks before the look view and after it hold
            // one shoulder alone, which steps the follow camera's L2/R2 accumulator (view +0xe4: +0x5000 / -0x5000).
            uint16_t k = c + 2 >= on && c + 2 < off ? PadL2 | PadR2 : c + 4 == on ? PadL2 : c + 2 == off ? PadR2 : 0;
            if (c + 2 == r.c && r.lk[1] < kShot[i - 1].lk[1]) k |= PadLeft;
            if (c + 2 == fire) k |= PadCross;
            w.tick(keys(k));
        }
        if (stale) continue;
        const TbActor* p = nullptr;
        for (const TbActor& e : w.entities)
            if (e.id == 151) p = &e;
        const auto lk = w.lookStick();
        expect(w.cam.eye == V3{r.eye[0], r.eye[1], r.eye[2]} && w.cam.body == V3{r.body[0], r.body[1], r.body[2]} &&
                   w.cam.look == V3{r.look[0], r.look[1], r.look[2]} && lk[0] == r.lk[0] && lk[1] == r.lk[1] &&
                   w.lookMode() == bool(r.fd4 & 2) && b.yaw == r.yaw && (r.c < on || b.rec()->id == r.st),
               r.c, "view", w.cam.eye.x, w.cam.eye.y, w.cam.eye.z, w.cam.body.x, w.cam.body.z, lk[0], lk[1], w.lookMode(), b.rec()->id, r.eye[0], r.eye[1],
               r.eye[2], r.body[0], r.body[2], r.lk[0], r.lk[1], r.st);
        // A removed shot (+0x20 0x2000) frees its slot at the end of that tick; the capture keeps reading the dead slot.
        const bool live = r.shot && !(kShot[i - 1].shot && (kShot[i - 1].f20 & 0x2000));
        expect(bool(p) == live && (!p || (p->pos == V3{r.pos[0], r.pos[1], r.pos[2]} && p->vel[0] == r.vel[0] && p->vel[1] == r.vel[1] &&
                                         p->vel[2] == r.vel[2] && p->pitch == r.ang[0] && p->yaw == r.ang[1] && p->roll == r.ang[2] &&
                                         (p->f20 & 0x2000) == (r.f20 & 0x2000) && p->stateTicks == r.t82)),
               r.c, "shot", bool(p), p ? p->pos.x : 0, p ? p->pos.y : 0, p ? p->pos.z : 0, p ? p->vel[1] : 0, p ? p->stateTicks : 0, live, r.pos[0],
               r.pos[1], r.pos[2], r.vel[1], r.t82);
    }
    std::printf("  look view + class 30: %d mismatches over %zu rows (look %d..%d, shot %d)\n", mismatches, std::size(kShot), on, off, fire);
    assert(!mismatches && fire);
}

// Homing jump = oracle local/wine/compare/w13/homing/run.sh (RUNNER=wine): TRBOSS02, the boss spawn (template 322) on
// cannon 255 leaves it into state 496 (flagsB 0x100000) and jumps at Bugs (held at the oracle's (1532, 0, -811)):
// 0x44d990 aims the apex over his box middle, the rise speed for that height and |d| / flight ticks along the yaw to
// him. The level's own logic launches it; from there each tick matches the oracle's rows 1587..1595.
void homing(const fs::path& dir) {
    // {counter, x, y, z, vel x, y, z} of the oracle's id-322 entity (yaw 1459 throughout)
    static const int16_t kRows[][7] = {{1587, -1726, -267, 1752, 2064, -1968, -1648}, {1589, -1597, -380, 1649, 2064, -1808, -1648},
                                       {1591, -1468, -483, 1546, 2064, -1648, -1648}, {1593, -1339, -576, 1443, 2064, -1488, -1648},
                                       {1595, -1210, -659, 1340, 2064, -1328, -1648}};
    Level l = load(dir / "TRBOSS02.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 4; ++i) w.tick({});
    // The spawn on cannon 255 (-1965, 0, 1941) (by uid: entities move when others spawn), Bugs held at the oracle's spot.
    w.actors[0].pos = {1532, 0, -811};
    w.tick({});
    int32_t uid = -1;
    for (const TbActor& a : w.entities)
        if (a.id == 322 && a.pos == V3{-1965, 0, 1941}) uid = int32_t(a.uid);
    assert(uid >= 0);
    int t = 0;
    for (; t < 3000 && w.entity(uid)->rec()->id != 496; ++t) {
        w.actors[0].pos = {1532, 0, -811};
        w.tick({});
    }
    mismatches = 0;
    for (const auto& r : kRows) {
        if (&r != kRows) w.tick({});
        const TbActor* e = w.entity(uid);
        expect(e->rec()->id == 496 && e->parent < 0 && e->yaw == 1459 && e->pos == V3{r[1], r[2], r[3]} && e->vel[0] == r[4] &&
                   e->vel[1] == r[5] && e->vel[2] == r[6],
               r[0], "homing", e->rec()->id, e->yaw, e->pos.x, e->pos.y, e->pos.z, e->vel[0], e->vel[1], e->vel[2]);
    }
    std::printf("  homing jump: %d mismatches over %zu ticks (launch at tick %d)\n", mismatches, std::size(kRows), t);
    assert(!mismatches);
}


// Pole climb = oracle local/wine/compare/w13/climb/run.sh (RUNNER=wine), tb_climb.inc: GRANNY, dialogs dismissed, Bugs
// poked 200 south of pole 19 (flags0 0x10000, its state faces the camera eye), the recorded pad words replayed per tick
// (rand seed synced when captured). ✕ then ↑ jumps into the pole (grab 0x456fb0, event 87), ↑ climbs (101) to the top
// limit (0x44f690 2: +0x1a0 0xc -> 8), ← / → circle it (116 / 117, state +0x1c), R1 hands the focus to Taz (the pole
// turns to the new eye, Bugs with it), ↓ climbs down (99) to the bottom, where the mover's ground contact lets go.
#include "tb_climb.inc"
int climbTrace(const fs::path& dir, std::span<const ClimbRow> rows, int from) {
    Level l = load(dir / "Granny.bze");
    TbWorld& w = l.w;
    for (int i = 0; i < 60; ++i) w.tick({});
    for (int k = 0; k < 5; ++k) {  // the welcome and Granny text boxes
        for (int i = 0; i < 2; ++i) w.tick(keys(PadCross));
        for (int i = 0; i < 40; ++i) w.tick({});
    }
    TbActor& b = w.actors[0];
    b.pos = {rows[0].pos[0], rows[0].pos[1], rows[0].pos[2]}, b.yaw = 0;
    mismatches = 0;
    int grabs = 0;
    for (size_t i = 1; i < rows.size(); ++i) {
        const ClimbRow& r = rows[i];
        if (rows[i - 1].seed) w.rng.seed = rows[i - 1].seed;
        w.tick(keys(r.raw));
        grabs += r.par && !rows[i - 1].par;
        if (r.c < from || r.stale) continue;
        expect(b.rec()->id == r.st && b.pos == V3{r.pos[0], r.pos[1], r.pos[2]} && b.yaw == r.yaw && (b.parent >= 0) == bool(r.par) &&
                   b.f20 == r.f20 && b.climb == r.climb && (!r.par || (b.local == V3{r.loc[0], r.loc[1], r.loc[2]} && b.localYaw == r.ly)) &&
                   w.cam.eye == V3{r.eye[0], r.eye[1], r.eye[2]},
               r.c, "climb", b.rec()->id, b.pos.x, b.pos.y, b.pos.z, b.yaw, b.parent, b.local.x, b.local.y, b.local.z, b.localYaw, b.f20,
               b.climb, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z, w.focus());
    }
    std::printf("  climb %d..%d: %d mismatches over %zu rows from %d, %d grabs\n", rows.front().c, rows.back().c, mismatches, rows.size(), from, grabs);
    return grabs;
}
void climb(const fs::path& dir) {
    assert(climbTrace(dir, kClimbB, 1771) == 1 && !mismatches);
    assert(climbTrace(dir, kClimbA, 1803) == 1 && !mismatches);
}

// Debug: BBTIME_TB_ONLY=slots BBTIME_TB_LEVEL=Granny BBTIME_TB_TICKS=31: each tick's taken pool slots (entity id, class,
// +0x82, position; '-' a slot without a TbActor), the level fresh with its intro (as the oracle's spawnlog.py runs).
void slotDump(const fs::path& dir) {
    Level l = load(dir / (std::string(std::getenv("BBTIME_TB_LEVEL")) + ".bze"), false);
    for (int i = 0, n = std::atoi(std::getenv("BBTIME_TB_TICKS")); i < n; ++i) {
        l.w.tick({});
        std::map<int, const TbActor*> by;
        for (const TbActor& a : l.w.actors) by[a.slot] = &a;
        for (const TbActor& a : l.w.entities) by[a.slot] = &a;
        std::printf("tick %d used %d first free %d\n", i + 1, l.w.poolUsed(), l.w.poolFirstFree());
        for (size_t s = 0; s < l.w.slotUsed.size(); ++s)
            if (const auto it = by.find(int(s)); it != by.end())
                std::printf("%zu id %d cls %d t82 %d pos (%d, %d, %d)\n", s, it->second->id, it->second->behaviour, it->second->stateTicks,
                            it->second->pos.x, it->second->pos.y, it->second->pos.z);
            else if (l.w.slotUsed[s]) std::printf("%zu -\n", s);
    }
}

// Oracle checks = local/wine/compare/w13/checks/run.sh (RUNNER=wine), tb_checks.inc: the recorded pad words and rand
// seeds replayed per row, the capture's pokes applied at their counters; every fresh row (the player's +0x82 moved on)
// from `from` must match: player state, position, yaw, +0x20, the view eye / look, the shake 0x4d53d4..0x4d53da (speed,
// amplitude, phase, offset) and the traced entities (same id, the nearest one: position, yaw, +0x20, parented).
#include "tb_checks.inc"
// Our pool against the exe's slot table (-1 free, else the slot's id +0x34): each slot taken or free alike, and a
// slot holding one of our entities holds the same id there (templates and the view / world / HUD holder entities are
// slots without a TbActor here). Prints and counts the differences.
int slotDiffs(const TbWorld& w, std::span<const int16_t> exe) {
    std::map<int, uint16_t> ids;
    for (const TbActor& a : w.actors) ids[a.slot] = a.id;
    for (const TbActor& a : w.entities) ids[a.slot] = a.id;
    int n = 0;
    for (size_t s = 0; s < std::max(exe.size(), w.slotUsed.size()); ++s) {
        const bool used = s < w.slotUsed.size() && w.slotUsed[s];
        const int16_t e = s < exe.size() ? exe[s] : -1;
        const auto it = ids.find(int(s));
        if (used != (e >= 0) || (it != ids.end() && it->second != e)) {
            ++n;
            std::printf("    slot %zu: exe %d, ours %s %d\n", s, e, used ? "used" : "free", it != ids.end() ? it->second : -1);
        }
    }
    return n;
}
template <size_t N, size_t P>
void checkTrace(TbWorld& w, const char* name, const ChkRow (&rows)[N], const ChkPoke (&pokes)[P], int from, int* shook = nullptr,
                int to = INT_MAX, bool cam = true, uint16_t skipId = 0, const std::function<void(const ChkRow&)>& each = {},
                size_t who = 0, std::span<const int16_t> cut = {}) {
    TbActor& b = w.actors[who];
    auto poke = [&](int c) {
        for (const ChkPoke& p : pokes) {
            if (p.c != c) continue;
            if (p.kind == 0 || p.kind == 3) b.pos = {p.v[0], p.v[1], p.v[2]};
            if (p.kind == 0) b.yaw = int16_t(p.v[3]);
            if (p.kind == 1) b.pos.x = p.v[0], b.pos.z = p.v[1];
            if (p.kind == 2) w.GD[size_t(p.v[0])] = uint8_t(p.v[1]);
            if (p.kind == 4) {  // the exe's slot count cut to its first free slot: ours is cut the same, its slots equal
                expect(w.poolFirstFree() == p.v[0] && w.poolUsed() == p.v[1] && w.poolCap == p.v[2], c, "pool", w.poolFirstFree(),
                       w.poolUsed(), w.poolCap, p.v[0], p.v[1], p.v[2]);
                std::printf("  pool cut at %d: exe %d of %d slots used (first free %d), ours %d of %d (%d)\n", c, p.v[1], p.v[2], p.v[0],
                            w.poolUsed(), w.poolCap, w.poolFirstFree());
                if (!cut.empty()) expect(slotDiffs(w, cut) == 0, c, "pool slots");
                w.poolCap = p.v[0];
            }
        }
    };
    poke(rows[0].c);
    mismatches = 0;
    int n = 0;
    for (size_t i = 1; i < N; ++i) {
        const ChkRow& r = rows[i];
        if (rows[i - 1].seed) w.rng.seed = rows[i - 1].seed;
        for (int k = rows[i - 1].c + 2; k < r.c; k += 2) w.tick(keys(r.raw));  // a tick the capture missed: this row's pad
        w.tick(keys(r.raw));
        poke(r.c);
        if (each) each(r);
        if (r.c > to) break;
        if (r.c < from || r.stale) continue;
        ++n;
        const auto& s = w.camShake;
        if (shook && s.amp) ++*shook;
        expect(b.rec()->id == r.st && b.pos == V3{r.pos[0], r.pos[1], r.pos[2]} && b.yaw == r.yaw && b.f20 == r.f20 &&
                   (!cam || (w.cam.eye == V3{r.eye[0], r.eye[1], r.eye[2]} && w.cam.look == V3{r.look[0], r.look[1], r.look[2]} &&
                             s.speed == r.sh[0] && s.amp == r.sh[1] && s.phase == r.sh[2] && s.off == r.sh[3])),
               r.c, name, b.rec()->id, b.pos.x, b.pos.y, b.pos.z, b.yaw, b.f20, w.cam.eye.x, w.cam.eye.y, w.cam.eye.z, w.cam.look.x,
               w.cam.look.y, w.cam.look.z, s.speed, s.amp, s.phase, s.off, -1, r.st, r.pos[0], r.pos[1], r.pos[2], r.yaw, r.f20,
               r.eye[0], r.eye[1], r.eye[2], r.look[0], r.look[1], r.look[2], r.sh[0], r.sh[1], r.sh[2], r.sh[3]);
        for (size_t k = 0; k < r.n; ++k) {
            const ChkEnt& e = r.e[k];
            if (e.id == skipId) continue;
            const TbActor* m = nullptr;
            int64_t best = INT64_MAX;
            for (const TbActor& a : w.entities) {
                if (a.id != e.id) continue;
                const int64_t dx = a.pos.x - e.pos[0], dy = a.pos.y - e.pos[1], dz = a.pos.z - e.pos[2], d = dx * dx + dy * dy + dz * dz;
                if (d < best) best = d, m = &a;
            }
            expect(m && m->pos == V3{e.pos[0], e.pos[1], e.pos[2]} && ((m->yaw ^ e.yaw) & 0xfff) == 0 && (m->f20 & 0x80102) == (e.f20 & 0x80102) &&
                       (m->parent >= 0) == bool(e.par),
                   r.c, "entity", e.id, m ? m->pos.x : 0, m ? m->pos.y : 0, m ? m->pos.z : 0, m ? m->yaw : 0, m ? m->f20 : 0, e.pos[0],
                   e.pos[1], e.pos[2], e.yaw, e.f20);
        }
    }
    std::printf("  %s %d..%d: %d mismatches over %d rows from %d, %d shaking\n", name, rows[0].c, rows[N - 1].c, mismatches, n, from, shook ? *shook : 0);
}
// The level loaded fresh and ticked to the capture's first counter (2 · tick - 1) with no input.
void toCounter(TbWorld& w, int c) {
    while (2 * int(w.ticks) - 1 < c) w.tick({});
}
void checks(const fs::path& dir) {
    {  // P the pool layout from the level load with no input: GRANNY at counter 61 (pool.py), ARCA01 with its intro at 201
       // (spawnlog.py): each slot taken or free as the exe's, an entity's slot holding its id there (slots 0..16 view /
       // world, the loader's chunk slots, the HUD holder 0x462430, 0x460b00 spawns in the lowest free slot, a destroyed
       // entity's slot freed after its own tick 0x460630 or by the list walk 0x45eee0, which spares the one after each).
        const struct { const char* level; bool intro; int c; std::span<const int16_t> exe; } kPools[] = {
            {"Granny.bze", false, 61, kPoolGranny}, {"ARCA01.bze", true, 201, kPoolArca}};
        for (const auto& p : kPools) {
            Level l = load(dir / p.level, !p.intro);
            toCounter(l.w, p.c);
            const int n = slotDiffs(l.w, p.exe);
            std::printf("  pool %s at %d: %d slot differences, %d used\n", p.level, p.c, n, l.w.poolUsed());
            assert(n == 0);
        }
    }
    {  // A GRANNY: ○ in a jump glides (66, flagsA 0x8000 vert -19; with GD+0xad 2: 67, vert +20); △ in a jump slams
       // (118 -> 120 -> 121): anim 75's asset logic S 41 (30, 20) shakes the look point.
        Level l = load(dir / "Granny.bze");
        TbWorld& w = l.w;
        for (int i = 0; i < 60; ++i) w.tick({});
        for (int k = 0; k < 5; ++k) {
            for (int i = 0; i < 2; ++i) w.tick(keys(PadCross));
            for (int i = 0; i < 40; ++i) w.tick({});
        }
        int shook = 0;
        bool made = false;  // the slam's template 80 (made here; I: not with the pool full)
        checkTrace(w, "glide / slam", kChkGranny, kChkGrannyPokes, 1745, &shook, INT_MAX, true, 0, [&](const ChkRow&) {
            for (const TbActor& e : w.entities) made = made || e.id == 80;
        });
        assert(!mismatches && shook >= 10 && made);
    }
    {  // B ARCA01: the dig spot (entity 260, +0x24 0x400) → 124 / 126 underground, poked under the 0x10000000 block,
       // ↑ (131), let go (130), ✕: S 49 (100, 30) by the look point's distance, the block pushed up.
        Level l = load(dir / "ARCA01.bze", false);
        TbWorld& w = l.w;
        toCounter(w, kChkArca[0].c);
        int shook = 0;
        checkTrace(w, "dig / lift", kChkArca, kChkArcaPokes, kChkArcaPokes[0].c, &shook);  // from the poke: the fly-over node
        assert(!mismatches && shook >= 10);                                                 // camera is §35's
    }
    {  // C ARQ01: boss 234 drops (364) on Bugs below it and lands (375): S 49 (100, 50), its amplitude 40 by the look
       // point's distance. Compared from the follow camera's take-over (the level's scripted intro holds the view until
       // 987) to the capture's end: boss 226's hit (hit transition 0 -> 3 -> 377) runs 377's logic at once, its W
       // (last frame shown) false after the state entry cleared +0x14e, so 377 -> 189 jumps before the mover (1187);
       // Bugs down (1489) leaves Taz in: Bugs is no longer ticked, the view follows Taz, who stands where the mover
       // pushed him out of Bugs' load box (tag 0x1b / 0x1c, before the first tick's tag-0x61 record).
        Level l = load(dir / "ARQ01.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkArq[0].c);
        int shook = 0;
        checkTrace(w, "boss drop", kChkArq, kChkArqPokes, 987, &shook);
        assert(!mismatches && shook > 10);
    }
    {  // D VIQ01: entity 226 (state 333, flagsA 0x8000: +0x20 0x80000, vert 0) and the view from the level start (the
       // load placement on template 181 at the origin, the first follow tick from there: vel (0, -221705, 0), flags
       // 0x290; 226 listed by its sphere alone before its model instance +0x9c exists); Bugs poked above the pond
       // falls in, 394 spawns template 88 whose V 0x800000 sets its y to the water zone's surface (-1673); Bugs
       // drowns (383, 8 at 1583) and, Taz in, leaves the view list (no longer ticked: he stays in 8).
        Level l = load(dir / "VIQ01.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkViq[0].c);
        int steps = 0;
        checkTrace(w, "water snap", kChkViq, kChkViqPokes, kChkViq[0].c, nullptr, INT_MAX, true, 0, [&](const ChkRow& r) {
            for (size_t k = 0; k < r.n; ++k) steps += r.e[k].id == 226;
        });
        assert(!mismatches && steps >= 30);
    }
    {  // E TRQ02, the player Taz: R1 after the walk-in, □ into the gum machine 307 spawns gum 148, △ on it eats it
       // (147 -> 161, the gum parented), 245..248 -> 250 -> 252 / 251 / 253: flagsA 0x8000 with vert 0x4067 = S[0x67]
       // (the S form of the vertical move), ↑ floats him, 249 -> 50 down.
        Level l = load(dir / "TRQ02.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkGum[0].c);
        std::set<uint16_t> seen;
        checkTrace(w, "gum float", kChkGum, kChkGumPokes, kChkGum[0].c, nullptr, INT_MAX, true, 0,
                   [&](const ChkRow&) { seen.insert(w.actors[1].rec()->id); }, 1);
        assert(!mismatches && seen.count(250) && seen.count(251) && seen.count(252) && seen.count(253));
    }
    {  // F TRQ02, the player Taz poked beside entity 309: event 350 -> 394 -> 395 / 396 (flagsA 0x80000800, vert 5)
       // rises to -2277 -> 514; 308 hangs on it.
        Level l = load(dir / "TRQ02.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkLift[0].c);
        int top = 0;
        checkTrace(w, "309 lift", kChkLift, kChkLiftPokes, kChkLift[0].c, nullptr, INT_MAX, true, 0, [&](const ChkRow&) {
            for (const TbActor& e : w.entities) if (e.id == 309) top = std::min(top, e.pos.y);
        }, 1);
        assert(!mismatches && top == -2277);
    }
    {  // H TRQ02, the player Taz poked inside entity 309's box (it unticked, no record yet): the mover pushes him out
       // (-163, +171, -146) at once, he falls to the floor.
        Level l = load(dir / "TRQ02.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkInside[0].c);
        checkTrace(w, "inside 309", kChkInside, kChkInsidePokes, kChkInside[0].c, nullptr, INT_MAX, true, 0, {}, 1);
        assert(!mismatches);
    }
    {  // I GRANNY from the level start (the oracle's whole history replayed: dialogs dismissed, Bugs poked as A, ✕ ○
       // glide), the pool's slot count cut to its first free slot (0x4d270e) before the butt slam: our slot table equals
       // the exe's there (per-slot pool, the lowest free slot 0x460b00), and the slam's template 80 is not made (made in
       // A, and in the oracle without the cut).
        Level l = load(dir / "Granny.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkPool[0].c);
        bool made = false;
        checkTrace(w, "pool full", kChkPool, kChkPoolPokes, kChkPool[0].c, nullptr, INT_MAX, true, 0, [&](const ChkRow&) {
            for (const TbActor& e : w.entities) made = made || e.id == 80;
        }, 0, kChkPoolSlots);
        std::printf("  pool full: template 80 %s\n", made ? "made" : "not made");
        assert(!mismatches && !made);
    }
    {  // J VICASG04, the whole capture: the text boxes dismissed, Bugs poked by spawner 231 (it makes snowball 241),
       // behind 241, △ picks it up, poked beside entity 131 (scale 1.25): its grab (state 350, flagsA 0x20) waits for a
       // frame with an attach point (+0x84 / +0x7c set per applied frame, 0x468280, through its scaled root) and hangs
       // Bugs there at once; Bugs' program (0x8000, event 0x97 -> 240) lets the snowball go (V 0x200) through that chain
       // (0x44e560), so it flies from beside 131's hand; 131 then throws Bugs (241) alone: 0x44e6d0 finds no child.
        Level l = load(dir / "VICASG04.bze");
        TbWorld& w = l.w;
        toCounter(w, kChkThrow[0].c);
        checkTrace(w, "grab / throw", kChkThrow, kChkThrowPokes, kChkThrow[0].c);
        assert(!mismatches);
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    synthetic();
    padLayer();
    const char* data = std::getenv("BBTIME_DATA");
    const fs::path dir = data ? findCi(data, {"tb", "Datas", "BZE"}) : fs::path();
    if (dir.empty()) {
        std::puts("BBTIME_DATA/tb not set: real-data part skipped");
        return 0;
    }
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "homing") return homing(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "classes") return classes(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "ops") return opsScan(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "carry") return carry(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "throw") return throwTrace(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "class30") return class30(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "shadows") return shadows(dir), shapeShadow(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "climb") return climb(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "checks") return checks(dir), 0;
    if (const char* only = std::getenv("BBTIME_TB_ONLY"); only && std::string(only) == "slots") return slotDump(dir), 0;
    lineOfSightUnused(dir);
    opsScan(dir);
    intro(dir);
    music(dir);
    pauseMenu(dir);
    lineOfSight(dir);
    levelSelect(dir);
    language(dir);
    arca(dir);
    zones(dir);
    classes(dir);
    carry(dir);
    throwTrace(dir);
    class30(dir);
    shadows(dir);
    shapeShadow(dir);
    homing(dir);
    climb(dir);
    checks(dir);
    water(dir);
    fx(dir);
    swim(dir);
    allLevels(dir);
    camScriptPoints(dir);
    demoAbsent(dir);
    return 0;
}
