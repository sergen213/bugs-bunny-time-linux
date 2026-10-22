// Overlay text interpreter (lit_overlay.cpp, GAMEPLAY.md §7.6) on real MERLIN data (BBTIME_DATA): the save prompt
// opened by S[17] = 1 emits the glyph list the Wine oracle showed (RUNTIME_REFERENCE.md §9, 0x4b373c after
// `peek.py lit 0x4b2271 poke 01`), ← selects Yes (drawn white once S[11] leaves its blink phase), ✕ requests the
// save (S[0x0a] = 6); a synthetic two-page script checks the >W wait / resume / redraw rules; and in play Merlin's
// dialog (R1, speech status S[0x17]) opens the prompt.
#include "bb/game/lit_world.h"
#include "bb/level.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace bb;
using namespace bb::game;

namespace {

std::string text(const LitWorld& w) {
    std::string s;
    for (const OverlayGlyph& g : w.glyphs) s += char(g.ch);
    return s;
}

void testPrompt(LitWorld w) {
    assert(w.overlayText.size() == 0x2a54);
    w.loadUpdate();
    w.tick({});
    w.S[17] = 1;
    w.tick({});
    // Oracle (counter after the poke): "Do you want to save your game?" centred on x 0 at y 50, palette 1; Yes / No
    // at x -50 / 30, y 70, palette 3 (S[18] = 0: neither highlighted).
    assert(text(w) == "Doyouwanttosaveyourgame?YesNo");
    constexpr int16_t kX[29] = {-141, -130, -114, -104, -94, -78, -63, -52, -41, -27, -19, -3, 5, 16, 26,
                                40,   50,   60,   70,   85,  95,  106, 117, 125, -50, -37, -29, 30, 43};
    for (size_t i = 0; i < 29; ++i) {
        const OverlayGlyph& g = w.glyphs[i];
        assert(g.x == kX[i]);
        assert(g.y == (i < 24 ? 50 : 70));
        assert(g.pal == (i < 24 ? 1 : 3));
    }
    w.tick({.buttons = PadLeft});  // C57 ← -> S[18] = 1 (Yes)
    assert(w.S[18] == 1);
    w.tick({});
    int white = 0;
    for (int t = 0; t < 25; ++t, w.tick({}))  // S[11] cycles 0..19: > 10 draws the selection with script 70 (white)
        if (w.glyphs.size() == 29 && w.glyphs[24].pal == 0 && w.glyphs[27].pal == 3) ++white;
    assert(white >= 9);
    assert(w.S[0x0a] == 0);
    w.tick({.buttons = PadCross});  // ✕ with Yes -> save request
    assert(w.S[0x0a] == 6);
    assert(!w.unsupported.contains("A49"));
}

void testPages(LitWorld w) {
    const std::string s = std::string("\x04\x00\x05\x00", 4) + '\0' + ">CX0 >CY0 A>W2 B>W1 " + '\0';
    w.overlayText.assign(s.begin(), s.end());
    w.loadUpdate();
    assert(w.overlay(1) == 0);  // spawns the type-32 actor (template 68) and draws page 1 at once
    assert(text(w) == "A" && w.glyphs[0].x == 0 && w.glyphs[0].y == 0);
    w.tick({});  // its first behaviour call only clears the 0x8000 "already drawn" bit
    assert(text(w).empty());
    w.tick({});  // waiting: page 1 again
    assert(text(w) == "A");
    w.tick({});  // waited 2 ticks: resume after ">W2" with a fresh pen (x -200, y -60; the skipped ' ' advances 6)
    assert(text(w) == "B" && w.glyphs[0].x == -194 && w.glyphs[0].y == -60);
    w.tick({});  // page 2 waited 1 tick, nothing follows: actor freed
    assert(text(w).empty());
    for (const LitActor& a : w.actors) assert(a.dead || a.type != 32);
}

// In play: Bugs beside Merlin (entity 0x104df, type 14 at (11750, -670, 4210)) presses R1 (action): Merlin's dialog
// starts speech 47 (A81 -> S[0x17] 8, 2, 4), state 129 opens the prompt with Yes selected (S[17] = S[18] = 1) when the
// speech ends (4824 ms), ✕ then requests the save.
void testMerlin(LitWorld w) {
    w.start({11750, -670, 4360}, 0x800, -1);  // within 175 of Merlin (his state 2 -> 1 gate), facing him
    w.loadUpdate();
    int opened = -1, speaking = 0;
    for (int t = 0; t < 400 && opened < 0; ++t) {
        w.tick({.buttons = uint16_t(t >= 100 && t < 102 ? PadR1 : 0)});  // Merlin waits in state 172 from ~t60
        speaking += w.S[0x17] == 4;
        if (w.S[17] == 1) opened = t;
    }
    assert(speaking >= 144 && speaking <= 146);  // 4824 ms on the 30 Hz clock
    std::printf("merlin prompt opened at tick %d\n", opened);
    assert(opened > 0 && w.S[18] == 1);
    w.tick({});
    assert(text(w) == "Doyouwanttosaveyourgame?YesNo");
    w.tick({.buttons = PadCross});
    assert(w.S[0x0a] == 6);
}

} // namespace

// L01A F1 help screen (S[12] = 3, entity 0x1d99b script 210) vs the Wine oracle's 0x4b373c list (2026-10-19, config
// with the joystick enabled: ">DJ n" = key + "/Button<j>"): text, x, y and palette of all 178 glyphs.
void testHelp(const BzeFile& bze, const Scene& scene) {
    LitWorld w = LitWorld::load(bze, &scene);
    w.joystick = true;
    w.loadUpdate();
    for (int t = 0; t < 40; ++t) w.tick({});
    const V3 at = w.p.pos;
    w.tick({.help = true});  // request -> S[12] = 3 at the end of this tick
    assert(w.S[12] == 3);
    w.tick({.buttons = PadUp});  // paused: Bugs stands still, the help director draws
    assert(w.p.pos.x == at.x && w.p.pos.z == at.z);
    assert(text(w) == "HelpMenuLookACrouchS/Button1JumpD/Button2KickF/Button3RollE/Button4ActionR/Button5CamerarightQ/Button7"
                      "CameraleftW/Button6F1HelpF2SoundOn/OffF3MusicOn/OffF4QuitEscPauseEnterSelect");
    constexpr int16_t kX[178] = {
        -40,  -30,  -22,  -15,  -1,   12,   20,   31,   -200, -191, -181, -171, -25,  -200, -189, -180, -170, -160, -151,
        -25,  -16,  -7,   2,    12,   20,   28,   38,   49,   -200, -189, -179, -168, -25,  -14,  -5,   4,    14,   22,
        30,   40,   51,   -200, -189, -184, -175, -25,  -17,  -8,   1,    11,   19,   27,   37,   48,   -200, -189, -179,
        -172, -25,  -16,  -7,   2,    12,   20,   28,   38,   49,   -200, -187, -178, -170, -165, -155, -25,  -14,  -5,
        4,    14,   22,   30,   40,   51,   -200, -189, -178, -167, -159, -150, -133, -124, -119, -109, -101, -25,  -11,
        -2,   7,    17,   25,   33,   43,   54,   -200, -189, -178, -167, -159, -150, -133, -126, -118, -111, -25,  -8,
        1,    10,   20,   28,   36,   46,   57,   -200, -192, -179, -169, -161, -154, -200, -192, -177, -168, -158, -148,
        -137, -122, -109, -98,  -89,  -76,  -69,  -200, -192, -176, -163, -153, -145, -140, -125, -112, -101, -92,  -79,
        -72,  -200, -192, -176, -162, -152, -147, -200, -191, -183, -168, -157, -146, -136, -128, -200, -191, -180, -172,
        -164, -149, -140, -132, -125, -117, -108};
    constexpr int8_t kRowY[17] = {-115, -100, -85, -70, -55, -40, -25, -10, 5, 20, 35, 50, 65, 80, 95};
    constexpr const char* kPal =
        "33333333333323333332222222223333222222222333322222222233332222222223333332222222223333333333322222222233333333"
        "33222222222333333333333333333333333333333333333333333333333333333333";
    assert(w.glyphs.size() == 178);
    int row = -1;
    for (size_t i = 0; i < 178; ++i) {
        const OverlayGlyph& g = w.glyphs[i];
        if (i == 0 || g.y != w.glyphs[i - 1].y) ++row;
        assert(g.x == kX[i] && g.y == kRowY[row] && g.pal == uint8_t(kPal[i] - '0'));
    }
    assert(row == 14);
    w.tick({.help = true});  // F1 again: 2 (resuming), 0 at the end of the tick
    assert(w.S[12] == 0);
    w.tick({});
    assert(text(w).find("HelpMenu") == std::string::npos);
}

// L01A Esc pause menu (S[12] = 1, GAMEPLAY.md §7.6) vs the Wine oracle (2026-10-21, local/wine/compare/w9lit/:
// o_pause{.png,_glyphs.txt,_hud.txt}, o_pause_hud2.txt with poked G, o_quitask.png): the menu's 44 glyphs, the
// pause totals 0x449d80 (HUD slot templates / origins / depth), Quit Game -> "Do you really want to exit the
// level?" -> Yes = level 3 (hub) with the pause cleared, Esc resumes.
void testPause(const BzeFile& bze, const Scene& scene) {
    LitWorld w0 = LitWorld::load(bze, &scene);
    w0.loadUpdate();
    for (int t = 0; t < 40; ++t) w0.tick({});
    LitWorld w = w0;
    w.tick({.esc = true});
    assert(w.S[12] == 1 && w.S[14] == 1 && w.S[16] == 1);
    for (int t = 0; t < 3; ++t) w.tick({});  // the pause director's overlay page
    assert(text(w) == "GamePausedContinueVideoControlsSoundQuitGame");
    constexpr int16_t kG[44][3] = {
        {-55, -66, 1}, {-43, -66, 1}, {-32, -66, 1}, {-21, -66, 1}, {-7, -66, 1}, {4, -66, 1},  {15, -66, 1},
        {25, -66, 1},  {33, -66, 1},  {41, -66, 1},  {-40, -45, 0}, {-29, -45, 0}, {-19, -45, 0}, {-8, -45, 0},
        {0, -45, 0},   {5, -45, 0},   {16, -45, 0},  {26, -45, 0},  {-25, -30, 3}, {-12, -30, 3}, {-7, -30, 3},
        {2, -30, 3},   {10, -30, 3},  {-40, -15, 3}, {-29, -15, 3}, {-19, -15, 3}, {-8, -15, 3}, {0, -15, 3},
        {9, -15, 3},   {19, -15, 3},  {26, -15, 3},  {-27, 0, 3},   {-18, 0, 3},   {-8, 0, 3},    {2, 0, 3},
        {13, 0, 3},    {-45, 14, 3},  {-31, 14, 3},  {-21, 14, 3},  {-16, 14, 3},  {-2, 14, 3},   {10, 14, 3},
        {21, 14, 3},   {32, 14, 3}};
    for (size_t i = 0; i < 44; ++i) assert(w.glyphs[i].x == kG[i][0] && w.glyphs[i].y == kG[i][1] && w.glyphs[i].pal == kG[i][2]);
    // Oracle HUD slots 0x4b23a0 (shown, template +0x1c, part +0xa8..+0xb0) for {slot, template, x, y, z}.
    using Slot = std::array<int, 5>;
    const auto shown = [](const LitWorld& v) {
        std::vector<Slot> s;
        for (size_t i = 0; i < v.hud.size(); ++i)
            if (v.hud[i].shown) s.push_back({int(i), v.hud[i].tmpl, v.hud[i].x, v.hud[i].y, v.hud[i].z});
        return s;
    };
    assert((shown(w) == std::vector<Slot>{{0, 486, -228, -80, 1200}, {1, 472, -200, -80, 1200}, {2, 472, -176, -80, 1200},
                                          {3, 485, 156, -80, 1200},  {4, 472, 180, -80, 1200},  {5, 472, 204, -80, 1200},
                                          {6, 472, 228, -80, 1200},  {7, 487, 136, 80, 1200},   {8, 472, 180, 80, 1200},
                                          {9, 472, 204, 80, 1200},   {10, 472, 228, 80, 1200},  {14, 489, -32, -80, 1200},
                                          {15, 489, 0, -80, 1200},   {16, 489, 32, -80, 1200},  {22, 472, -176, 82, 1200},
                                          {23, 924, -152, 82, 1200}}));
    LitWorld r = w;  // Esc again: 2, then 0 at the end of the tick (HUD hidden)
    r.tick({.esc = true});
    assert(r.S[12] == 0 && shown(r).empty());
    // Quit Game: ↓ x4, ✕ -> the confirm page (Yes white), ✕ -> level 3, unpaused.
    for (int k = 0; k < 4; ++k) {
        w.tick({.buttons = PadDown});
        for (int t = 0; t < 6; ++t) w.tick({});
    }
    assert(w.menu[1][2] == 4 && w.menu[1][3] == 5);  // oracle 0x4b3e80: item 1 {0, 0, 4, 5}
    w.tick({.buttons = PadCross});
    for (int t = 0; t < 4; ++t) w.tick({});
    assert(text(w) == "GamePausedDoyoureallywanttoexitthelevel?YesNo" && w.glyphs[40].pal == 0 && w.glyphs[43].pal == 3);
    w.tick({.buttons = PadCross});
    w.tick({});
    assert(w.nextLevel == 3 && w.S[12] == 0);
    // Totals with poked values (golden 300, clocks 57, carrots 17, level lines 4/12, 3/9, 2/5): o_pause_hud2.txt.
    LitWorld v = w0;
    v.G[3] = 17, v.G[7] = 44, v.G[0xfc] = 1, v.G[5] = 57, v.G[4] = 2;
    v.G[0xb6] = 0x20, v.G[0x20] = 4, v.G[0xb7] = 12, v.G[0xb8] = 0x21, v.G[0x21] = 3, v.G[0xb9] = 9, v.G[0xba] = 5;
    v.tick({.esc = true});
    assert((shown(v) == std::vector<Slot>{
                {0, 486, -228, -80, 1200}, {1, 473, -200, -80, 1200}, {2, 479, -176, -80, 1200}, {3, 485, 156, -80, 1200},
                {4, 475, 180, -80, 1200},  {5, 472, 204, -80, 1200},  {6, 472, 228, -80, 1200},  {7, 487, 136, 80, 1200},
                {8, 472, 180, 80, 1200},   {9, 477, 204, 80, 1200},   {10, 479, 228, 80, 1200},  {14, 489, -32, -80, 1200},
                {15, 489, 0, -80, 1200},   {16, 489, 32, -80, 1200},  {21, 479, -200, 82, 1200}, {22, 480, -176, 82, 1200},
                {23, 924, -152, 82, 1200}, {29, 485, -96, 58, 1500},  {30, 472, -28, 58, 1500},  {31, 476, -4, 58, 1500},
                {32, 923, 20, 58, 1500},   {33, 473, 44, 58, 1500},   {34, 474, 68, 58, 1500},   {35, 487, -96, 30, 1500},
                {36, 472, -28, 30, 1500},  {37, 475, -4, 30, 1500},   {38, 923, 20, 30, 1500},   {39, 472, 44, 30, 1500},
                {40, 481, 68, 30, 1500},   {41, 925, -96, 86, 1500},  {42, 472, -28, 86, 1500},  {43, 474, -4, 86, 1500},
                {44, 923, 20, 86, 1500},   {45, 472, 44, 86, 1500},   {46, 477, 68, 86, 1500}}));
}

// Template 68's font: 130 glyphs, each a whole 16x16 TIM except the icons (MERLIN glyphs 0x67..0x6e 20x16 / 64x32
// / 32x16 button and arrow images).
void testFont(const Scene& scene) {
    assert(scene.overlayFont.size() == 130);
    for (const int32_t t : scene.overlayFont) assert(t >= 0);
    const Texture& a = scene.textures[size_t(scene.overlayFont[0x25 - 1])];  // 'A'
    assert(a.width == 16 && a.height == 16);
    const Texture& icon = scene.textures[size_t(scene.overlayFont[0x6b - 1])];  // record 106 (tex 0x77)
    assert(icon.width == 64 && icon.height == 32);
}

int main() {
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) { std::printf("BBTIME_DATA unset: skipped\n"); return 77; }
    const auto file = std::filesystem::path(env) / "lit/Datas/bze/Merlin.bze";
    if (!std::filesystem::exists(file)) { std::printf("no Merlin.bze: skipped\n"); return 77; }
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "MERLIN");
    const LitWorld w = LitWorld::load(bze, &scene);
    testFont(scene);
    testPrompt(w);
    testPages(w);
    testMerlin(w);
    const auto l01a = std::filesystem::path(env) / "lit/Datas/bze/L01a.bze";
    const BzeFile b2 = loadBze(l01a);
    testHelp(b2, buildLitScene(b2, "L01A"));
    testPause(b2, buildLitScene(b2, "L01A"));
    std::printf("ok\n");
    return 0;
}
