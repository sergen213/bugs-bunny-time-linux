// Interactive water for the Enhanced stylised water (Settings::water, Vulkan scene.frag shadeWater; no original
// counterpart). Ported from the Joan of Arc port's water_waves (a damped 2D wave equation on a camera-following grid),
// rescaled from Joan's ~80-unit units to Bugs' ~300 (kCharUnit). Pure CPU, allocation-free per frame.
#pragma once
#include <functional>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <vector>

namespace bb::render {

// Water surface heights and shore walls over the level, rasterised once per level from the water-classified scene
// triangles (top surface wins) and the opaque ground: a cell is wet when water covers it and no ground lies at or up
// to two character heights above the surface there (banks, posts, rocks; higher bridges leave it open).
class WaterSurfaces {
public:
    // Flat triangle lists (3 engine-space points each).
    void build(std::span<const glm::vec3> water, std::span<const glm::vec3> ground);
    std::optional<float> height(glm::vec2 xz) const;  // top water surface over engine (x, z)
    bool wet(glm::vec2 xz) const;
    bool empty() const { return h_.empty(); }

private:
    int index(glm::vec2 xz) const;  // -1 outside
    glm::vec2 origin_{0.0f};
    float cell_ = 0.0f;
    int nx_ = 0, nz_ = 0;
    std::vector<float> h_;      // NaN = no water
    std::vector<uint8_t> dry_;  // ground at / above the surface
};

class WaveField {
public:
    static constexpr int kN = 256;                // grid cells per side
    static constexpr float kCell = 15.0f;         // world units: a 3840-unit (~13 Bugs) window
    static constexpr float kSpeed = 300.0f;       // units/s
    static constexpr float kStep = 1.0f / 60.0f;  // fixed substep: c * dt / cell = 0.33, inside CFL (1/sqrt 2)

    // wet(engine x, z): open water there; called once per cell as it scrolls into the window.
    explicit WaveField(std::function<bool(glm::vec2)> wet);

    // Scrolls the window by whole cells so `centre` (engine x, z) is in its middle; shifted-in cells start at rest.
    void follow(glm::vec2 centre);
    // Runs the fixed substeps owed after dt seconds; bodies added since the last call are walls for them.
    void step(float dt);
    // Displacement kick at engine x, z: `amplitude` down at the centre, a rim up around it (Mexican hat of width
    // sigma, zero volume). Walls and this step's bodies are not touched; the volume they clip comes off the free cells.
    void kick(glm::vec2 p, float amplitude, float sigma);
    // A body of radius r wading at engine x, z, moved by `move` (engine xz units) this frame: a wall for the next
    // step() (waves scatter around it) and, moving, a bow mound ahead and a trough behind (the wake).
    void wade(glm::vec2 p, float r, glm::vec2 move, float dt);
    // Splash / ripple of strength 0..1 (1 = something falling in) at engine x, z.
    void ripple(glm::vec2 p, float strength);

    float height(glm::vec2 p) const;  // the cell holding p, 0 outside the window
    bool wall(glm::vec2 p) const;     // the cell holding p is a static wall (or outside the window)
    // Heights row-major (row = engine +z, column = engine +x; cell (x, z) covers origin + [x, x+1) * kCell); empty
    // once nothing kicked for kQuiet seconds (the field has damped to rest).
    std::span<const float> heights() const;
    glm::vec2 origin() const { return glm::vec2(corner_) * kCell; }

private:
    static constexpr float kQuiet = 8.0f;
    int cellIndex(glm::vec2 p) const;  // -1 outside the window
    void shift(std::vector<float>& a, int dx, int dz, float fill);

    std::function<bool(glm::vec2)> wet_;
    std::vector<float> h_, prev_;    // current and previous substep heights
    std::vector<float> open_;        // 1 open water, 0 static wall, -1 not yet asked (the window border is always a wall)
    std::vector<float> mask_;        // open_ with the bodies of the current step cut out
    std::vector<float> scratch_;
    std::vector<glm::vec3> bodies_;  // x, z, radius; reserved
    glm::ivec2 corner_{0};           // window origin in cells
    bool placed_ = false;
    float owed_ = 0.0f, idle_ = kQuiet + 1.0f;
};

} // namespace bb::render
