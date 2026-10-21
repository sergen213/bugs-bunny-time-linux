// Overlay text interpreter (lit_overlay.cpp, GAMEPLAY.md §7.6) on real MERLIN data (BBTIME_DATA): the save prompt
// opened by S[17] = 1 emits the glyph list the Wine oracle showed (RUNTIME_REFERENCE.md §9, 0x4b373c after
// `peek.py lit 0x4b2271 poke 01`), ← selects Yes (drawn white once S[11] leaves its blink phase), ✕ requests the
// save (S[0x0a] = 6); a synthetic two-page script checks the >W wait / resume / redraw rules; and in play Merlin's
// dialog (R1, speech status S[0x17]) opens the prompt.
#include "bb/game/lit_world.h"
#include "bb/level.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
    std::printf("ok\n");
    return 0;
}
