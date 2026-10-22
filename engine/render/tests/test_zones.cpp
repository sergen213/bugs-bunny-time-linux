#undef NDEBUG
#include "bb/bze.h"
#include "bb/level.h"
#include "bb/render/camera.h"
#include "bb/render/zones.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <glm/gtc/matrix_transform.hpp>

using namespace bb;
using namespace bb::render;

// Eye at the origin looking down -Z, 60° at 4:3. Zone 0 has a portal ahead (-> 1) and one behind (-> 2); zone 1 a
// portal far to the right that only a wide FOV sees (-> 3); zone 3 (depth limit 1) one ahead (-> 4, flagged).
static ZonePortal portal(uint16_t to, float x, float z) {
    return {to, {{{x - 1, -1, z}, {x + 1, -1, z}, {x - 1, 1, z}, {x + 1, 1, z}}}};
}

int main() {
    std::vector<WorldZone> zones(5);
    for (WorldZone& z : zones) z.present = true, z.depth = 3;
    zones[3].depth = 1;
    zones[0].portals = {portal(1, 0, -10), portal(2, 0, 10)};
    zones[1].portals = {portal(3, -14, -10)};
    zones[3].portals = {portal(4, 0, -20)};
    std::vector<ZoneEntry> t;
    std::vector<uint8_t> drawn;
    const Camera cam{};
    auto run = [&](float vfovDeg, ZoneView v) {
        return walkZones(zones, projection(glm::radians(vfovDeg), 4.0f / 3.0f, 0.1f, 100.0f) * cam.view(), v, t, drawn);
    };
    assert(!run(60, {}));  // no zone view: draw everything
    assert(run(60, {0, 0, false}));
    assert(drawn == (std::vector<uint8_t>{1, 1, 0, 0, 0}));
    assert(t.size() == 2 && t[0].zone == 0 && t[1].zone == 1 && !t[1].flagged);
    // 120° (Hor+ wide): the side portal shows zone 3, whose own portal exceeds its depth limit (drawn, not walked).
    assert(run(120, {0, 0, false}));
    assert(drawn == (std::vector<uint8_t>{1, 1, 0, 1, 0}));
    assert(t.size() == 4 && t[3].zone == 4 && t[3].flagged);
    // Eye in zone 2 behind the player (zone 0): both drawn, the table restarts from the player pass, zone 2 appended.
    assert(run(60, {2, 0, false}));
    assert(drawn == (std::vector<uint8_t>{1, 1, 1, 0, 0}));
    assert(t.size() == 3 && t[0].zone == 0 && t[1].zone == 1 && t[2].zone == 2);
    // Camera node: the player pass is skipped.
    assert(run(60, {2, 0, true}));
    assert(drawn == (std::vector<uint8_t>{0, 0, 1, 0, 0}));

    // Real data (BBTIME_DATA): L01A tables of the Wine original (Proton-GE /b04 at 640x480, peeks of 0x4b3460 with
    // the eye 0x4b38c0 / look 0x4b38cc, camera = player zone 2; RUNTIME_REFERENCE.md §25) at the exe's 51.28°.
    const char* data = std::getenv("BBTIME_DATA");
    if (!data) return 0;
    const std::filesystem::path bze = std::filesystem::path(data) / "lit/Datas/bze/L01a.bze";
    if (!std::filesystem::exists(bze)) return 0;
    const Scene s = buildLitScene(loadBze(bze), "L01A");
    struct Probe { glm::vec3 eye, look; std::vector<std::pair<int16_t, bool>> table; };
    const Probe probes[] = {{{23613, -1000, 11550}, {23700, -1039, 9655}, {{2, false}}},
                            {{23822, -1140, 9936}, {22455, -843, 9822}, {{2, false}, {3, false}, {4, true}}},
                            {{22064, -1140, 11396}, {23017, -843, 10857}, {{2, false}}},
                            {{22067, -409, 12073}, {22037, 331, 11096}, {{2, false}}}};
    for (const Probe& p : probes) {
        auto eng = [](glm::vec3 v) { return glm::vec3(v.x, -v.y, -v.z); };  // PS1 -> engine (scene.h)
        const glm::mat4 vp = projection(kOriginalVFovRad, 4.0f / 3.0f, 10.0f, 60000.0f) *
                             glm::lookAt(eng(p.eye), eng(p.look), glm::vec3(0, 1, 0));
        assert(walkZones(s.zones, vp, {2, 2, false}, t, drawn));
        assert(t.size() == p.table.size());
        for (size_t i = 0; i < t.size(); ++i) assert(t[i].zone == p.table[i].first && t[i].flagged == p.table[i].second);
    }
}
