// LiT title front end (title.bze logic, GAMEPLAY.md §8.1.1) on real data (BBTIME_DATA), against the Wine oracle
// (RUNTIME_REFERENCE.md §14): camera-node view, intro (Taz → Bugs out of the hole), PRESS ENTER → slot menu (page 14,
// Load selected), Savedata summaries in S[0x2b..], A100 cursor skipping empty slots, new game request, A128 quit.
#include "bb/game/lit_save.h"
#include "bb/game/lit_world.h"
#include "bb/level.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace bb;
using namespace bb::game;
namespace fs = std::filesystem;

namespace {

std::string text(const LitWorld& w) {
    std::string s;
    for (const OverlayGlyph& g : w.glyphs) s += char(g.ch);
    return s;
}

const LitActor* byId(const LitWorld& w, uint16_t id) {
    for (const LitActor& a : w.actors)
        if (!a.dead && a.entityId == id) return &a;
    return nullptr;
}

} // namespace

int main() {
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) { std::printf("BBTIME_DATA unset: skipped\n"); return 77; }
    const auto file = fs::path(env) / "lit/Datas/bze/title.bze";
    if (!fs::exists(file)) { std::printf("no title.bze: skipped\n"); return 77; }
    const BzeFile bze = loadBze(file);
    const Scene scene = buildLitScene(bze, "TITLE");

    // Savedata with slot 2 = 333 golden carrots / 124 clocks (the oracle save, RUNTIME_REFERENCE.md §9).
    const fs::path dir = fs::temp_directory_path() / "bbtime_test_lit_title";
    fs::create_directories(dir);
    LitSaveData sd;
    sd.raw[4] = 2;
    sd.raw[8 + 0x2c] = 0x4d, sd.raw[9 + 0x2c] = 0x01, sd.raw[10 + 0x2c] = 124;
    std::ofstream(litSaveDataPath(dir), std::ios::binary).write(reinterpret_cast<const char*>(sd.raw.data()), sd.raw.size());

    LitWorld w = LitWorld::load(bze, &scene);
    w.level = 0;
    w.loadUpdate();
    // Oracle: the camera node of the actor at (0, 146, 0) holds eye (1, 146, 0), look (1, 146, 1000), flags 0x3xx.
    assert(w.cam.eye.x == 1 && w.cam.eye.y == 146 && w.cam.eye.z == 0);
    assert(w.cam.target.x == 1 && w.cam.target.y == 146 && w.cam.target.z == 1000);
    assert((w.cam.flags & 0x300) == 0x300);

    bool prompt = false;
    for (int t = 0; t < 230; ++t) {
        PadInput in;
        if (t >= 200 && t < 203) in.buttons = PadStart;  // Enter (C79 mask 0x80) once "PRESS ENTER" shows
        w.tick(in);
        prompt |= text(w) == "PRESSENTER";
        litFrontEnd(w, dir);
    }
    assert(prompt);
    // Taz's spin reached entity 49: Bugs (266) left the hidden start state 2 (out of the hole / idle with fidgets).
    const LitActor* bugs = byId(w, 266);
    const LitActor* logo = byId(w, 20);
    assert(bugs && w.defs[size_t(bugs->def)].states[size_t(bugs->state)].id != 2);
    assert(logo && w.defs[size_t(logo->def)].states[size_t(logo->state)].id == 129);
    assert(!byId(w, 43));
    // Copyright plane (type 0 at (0, 465, 871), scale 1.2): one rigid object, no TOD root scale (0x43eee0).
    for (const LitActor& a : w.actors)
        if (a.type == 0 && a.pos.z == 871) {
            std::vector<Mesh> out;
            poseActor(scene.actorModels[size_t(w.defs[size_t(a.def)].model)], a.animState, {a.pos.x, a.pos.y, a.pos.z},
                      {a.rot[0], a.yaw, a.rot[2]}, a.scale, out);
            float xhi = 0;
            for (const Mesh& m : out) for (const Vertex& v : m.vertices) xhi = std::max(xhi, v.pos[0]);
            std::printf("copyright plane half width %g\n", xhi);
            assert(a.animState.parts.size() == 1 && xhi > 500 && xhi < 620);
        }
    // Oracle after Enter: S[3] = 2, page S[0x1b] = 14, S[0x24] = 1 (Load game highlighted).
    assert(w.S[3] == 2 && w.S[0x1b] == 14 && w.S[0x24] == 1);
    assert(w.saveMask == 2 && w.S[4] == 2);
    assert((w.S[0x2d] | w.S[0x2e] << 8) == 333 && w.S[0x3c] == 124 && w.S[0x2b] == 0);
    assert(text(w).find("LoadgameNewgameQuitgame") == 0);
    assert(text(w).find("1.Empty") != std::string::npos && text(w).find("333") != std::string::npos && text(w).find("124") != std::string::npos);
    assert(text(w).find("2.Empty") == std::string::npos);
    // A93: cursor on slot 2 draws only its summary line green (palette 2, x -180.., y -75).
    w.G[0x9f] = 2;
    w.glyphs.clear();
    w.act({.act = 93, .x = 0x9f}, nullptr);
    int green = 0;
    for (const OverlayGlyph& g : w.glyphs) green += g.pal == 2 && g.y == -75 && g.x < 0;
    assert(green >= 3 && green == int(std::count_if(w.glyphs.begin(), w.glyphs.end(), [](const OverlayGlyph& g) { return g.pal == 2; })));
    // A100 (Load): the cursor jumps from an empty slot to the saved one, both directions.
    w.G[0x9f] = 1;
    w.pad.raw = PadDown;
    w.act({.act = 100, .x = 0x9f}, nullptr);
    assert(w.G[0x9f] == 2);
    w.G[0x9f] = 5;
    w.pad.raw = PadUp;
    w.act({.act = 100, .x = 0x9f}, nullptr);
    assert(w.G[0x9f] == 2);
    w.pad.raw = 0;

    // New game in slot 4 (S[0x0a] = 4): G reset, next level 56 CCINTRO, page 16 (Start / Go back).
    w.G[0x9f] = 4;
    w.G[3] = 50;
    w.S[0x0a] = 4;
    const auto g = litFrontEnd(w, dir);
    assert(g && g->slot() == 4 && w.G[0x0e] == 0x38 && w.G[1] == 6 && w.G[0] == 3 && w.G[3] == 0);
    assert(w.S[0x0a] == 0 && w.S[0x1b] == 0x10 && w.S[0x25] == 1);
    // Load of an empty slot is ignored (mask), of slot 2 with an unreadable file: page 4 "Load failed".
    w.G[0x9f] = 4, w.S[0x0a] = 5;
    assert(!litFrontEnd(w, dir));
    w.G[0x9f] = 2, w.S[0x0a] = 5;
    assert(litFrontEnd(w, dir) && w.S[0x1b] == 4 && w.G[0x9f] == 0);

    // A128 Quit game.
    assert(!w.quitRequest);
    w.act({.act = 128}, nullptr);
    assert(w.quitRequest);
    for (const char* op : {"A93", "A100", "A128", "C96", "C79"}) assert(!w.unsupported.contains(op));
    fs::remove_all(dir);
    std::printf("ok\n");
    return 0;
}
