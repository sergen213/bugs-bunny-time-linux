#undef NDEBUG
#include "bb/render/water_waves.h"

#include <cassert>
#include <cmath>

using namespace bb::render;

// Max |height| over cells at distance [r0, r1) from p.
static float ring(const WaveField& w, glm::vec2 p, float r0, float r1) {
    float m = 0.0f;
    for (float z = -r1; z <= r1; z += WaveField::kCell)
        for (float x = -r1; x <= r1; x += WaveField::kCell)
            if (const float d = std::hypot(x, z); d >= r0 && d < r1) m = std::max(m, std::abs(w.height(p + glm::vec2(x, z))));
    return m;
}

int main() {
    const glm::vec2 c(1000.0f, -2000.0f);
    // Open water: a splash spreads outwards as a ring and damps to rest.
    {
        WaveField w([](glm::vec2) { return true; });
        assert(w.heights().empty());  // nothing kicked yet
        w.follow(c);
        w.ripple(c, 1.0f);
        assert(std::abs(w.height(c)) > 20.0f && ring(w, c, 600.0f, 900.0f) == 0.0f);
        float sum = 0.0f;
        for (float h : w.heights()) sum += h;
        assert(std::abs(sum) < 1e-2f);  // zero-volume kick
        for (int i = 0; i < 60; ++i) w.step(1.0f / 60.0f);  // 1 s at 300 units/s
        assert(ring(w, c, 150.0f, 450.0f) > 0.2f);       // the front is ~300 out
        const float early = ring(w, c, 0.0f, 1500.0f);
        for (int i = 0; i < 60 * 4; ++i) w.step(1.0f / 60.0f);
        assert(ring(w, c, 0.0f, 3000.0f) < early * 0.1f);  // damped
        for (int i = 0; i < 60 * 5; ++i) w.step(1.0f / 60.0f);
        assert(w.heights().empty());  // quiet: the renderer stops uploading it
    }
    // A wall (shore at x >= c.x + 300): waves stop at it and reflect back, so the water in front of it moves more than
    // in open water, and nothing gets through.
    float open = 0.0f, walled = 0.0f;
    for (int pass = 0; pass < 2; ++pass) {
        WaveField w([&](glm::vec2 p) { return pass == 0 || p.x < c.x + 300.0f; });
        w.follow(c);
        w.ripple(c, 1.0f);
        float m = 0.0f;
        for (int i = 0; i < 90; ++i) {
            w.step(1.0f / 60.0f);
            if (i >= 60) m = std::max(m, std::abs(w.height(c + glm::vec2(150.0f, 0.0f))));  // between splash and wall
            if (pass == 1) assert(w.height(c + glm::vec2(450.0f, 0.0f)) == 0.0f && w.wall(c + glm::vec2(450.0f, 0.0f)));
        }
        (pass ? walled : open) = m;
    }
    assert(walled > open * 1.3f);
    // A wading body leaves a wake around it and is a wall itself (its cells stay at rest).
    {
        WaveField w([](glm::vec2) { return true; });
        w.follow(c);
        glm::vec2 p = c;
        for (int i = 0; i < 30; ++i) {
            const glm::vec2 move(10.0f, 0.0f);  // 300 units/s along +x
            p += move;
            w.wade(p, 40.0f, move, 1.0f / 30.0f);
            w.step(1.0f / 30.0f);
        }
        assert(ring(w, p, 50.0f, 150.0f) > 0.5f && w.height(p) == 0.0f);
    }
    // Surfaces: a 1000x1000 pond at y 50 with a bank (ground at y 60) over its x > 700 part and a bed (y -200).
    {
        const glm::vec3 water[6] = {{0, 50, 0}, {1000, 50, 0}, {1000, 50, 1000}, {0, 50, 0}, {1000, 50, 1000}, {0, 50, 1000}};
        const glm::vec3 ground[12] = {{700, 60, 0}, {1000, 60, 0}, {1000, 60, 1000}, {700, 60, 0}, {1000, 60, 1000}, {700, 60, 1000},
                                      {0, -200, 0}, {1000, -200, 0}, {1000, -200, 1000}, {0, -200, 0}, {1000, -200, 1000}, {0, -200, 1000}};
        WaterSurfaces s;
        s.build(water, ground);
        assert(s.height({500, 500}) == 50.0f && !s.height({1500, 500}) && !s.height({-100, 500}));
        assert(s.wet({300, 300}) && !s.wet({900, 300}) && !s.wet({1500, 300}));
    }
    return 0;
}
