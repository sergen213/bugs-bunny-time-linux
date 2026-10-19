#include "level_hook.h"

#include "bb/level.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <glm/glm.hpp>
#include <string>

namespace bb {
namespace {

namespace fs = std::filesystem;
using RGBA = std::array<uint8_t, 4>;

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
}

// Case-insensitive path resolution (installs mix Datas/bze, Datas/BZE, L01a.bze, CCIntro.bze ...).
fs::path findPath(fs::path base, std::initializer_list<std::string_view> parts) {
    for (std::string_view part : parts) {
        std::error_code ec;
        fs::path next;
        for (const auto& e : fs::directory_iterator(base, ec))
            if (iequals(e.path().filename().string(), part)) { next = e.path(); break; }
        if (next.empty()) return {};
        base = next;
    }
    return base;
}

template <class F> Texture makeTexture(const char* name, uint32_t n, F texel) {
    Texture t{name, n, n, std::vector<uint8_t>(size_t(n) * n * 4), false};
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const RGBA c = texel(x, y);
            std::copy(c.begin(), c.end(), t.rgba.begin() + (size_t(y) * n + x) * 4);
            t.hasAlpha |= c[3] < 255;
        }
    return t;
}

// Quad a-b-c-d (counter-clockwise), UVs 0..uvScale.
void quad(Mesh& m, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, float uvScale, RGBA col) {
    const uint32_t base = uint32_t(m.vertices.size());
    const glm::vec3 p[4] = {a, b, c, d};
    const float uv[4][2] = {{0, uvScale}, {uvScale, uvScale}, {uvScale, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i)
        m.vertices.push_back({{p[i].x, p[i].y, p[i].z}, {uv[i][0], uv[i][1]}, {col[0], col[1], col[2], col[3]}});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void box(Mesh& m, glm::vec3 c, glm::vec3 h, RGBA col) {
    auto shade = [&](float k) { return RGBA{uint8_t(col[0] * k), uint8_t(col[1] * k), uint8_t(col[2] * k), col[3]}; };
    const glm::vec3 lo = c - h, hi = c + h;
    quad(m, {lo.x, hi.y, hi.z}, {hi.x, hi.y, hi.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, 1, shade(1.0f));  // top
    quad(m, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, 1, shade(0.8f));  // +z
    quad(m, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, 1, shade(0.6f));  // -z
    quad(m, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, 1, shade(0.7f));  // +x
    quad(m, {lo.x, lo.y, lo.z}, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, 1, shade(0.5f));  // -x
}

Mesh mesh(std::string name, int32_t tex, Blend b) { return Mesh{std::move(name), tex, b, {}, {}, false}; }

// PLACEHOLDER: synthetic test scene exercising every blend mode, fog and filtering. Not original data.
Scene placeholderScene(GameId game, const std::string& level) {
    Scene s;
    s.game = game;
    s.level = level;
    s.placeholder = true;

    // 0: checker (filtering/aniso test), 1: holes (alpha test), 2: soft disc (alpha/additive).
    s.textures.push_back(makeTexture("placeholder/checker", 64, [](uint32_t x, uint32_t y) {
        const uint8_t v = ((x / 8 + y / 8) & 1) ? 230 : 70;
        return RGBA{v, v, v, 255};
    }));
    s.textures.push_back(makeTexture("placeholder/fence", 64, [](uint32_t x, uint32_t y) {
        const bool bar = (x % 16) < 4 || (y % 32) < 4;
        return RGBA{150, 100, 50, uint8_t(bar ? 255 : 0)};
    }));
    s.textures.push_back(makeTexture("placeholder/disc", 64, [](uint32_t x, uint32_t y) {
        const float r = std::hypot(float(x) - 31.5f, float(y) - 31.5f) / 32.0f;
        return RGBA{255, 255, 255, uint8_t(255.0f * std::clamp(1.0f - r, 0.0f, 1.0f))};
    }));

    const RGBA tint = game == GameId::LostInTime ? RGBA{255, 230, 200, 255} : RGBA{200, 230, 255, 255};
    Mesh ground = mesh("ground", 0, Blend::Opaque);
    quad(ground, {-100, 0, 100}, {100, 0, 100}, {100, 0, -100}, {-100, 0, -100}, 50, tint);
    s.meshes.push_back(std::move(ground));

    Mesh blocks = mesh("blocks", -1, Blend::Opaque);
    for (int i = 0; i < 8; ++i) {
        const float a = float(i) * 0.785398f;
        box(blocks, {std::sin(a) * 6.0f, 1.0f, std::cos(a) * 6.0f}, {0.7f, 1.0f, 0.7f},
            RGBA{uint8_t(80 + i * 20), uint8_t(200 - i * 15), uint8_t(120 + (i % 3) * 40), 255});
    }
    for (int z = 1; z <= 12; ++z) // receding pillars to show linear fog
        for (int x : {-12, 12})
            box(blocks, {float(x), 3.0f, -float(z) * 10.0f}, {1.0f, 3.0f, 1.0f}, RGBA{220, 220, 220, 255});
    s.meshes.push_back(std::move(blocks));

    Mesh fence = mesh("fence", 1, Blend::AlphaTest);
    fence.twoSided = true;
    quad(fence, {-8, 0, -10}, {8, 0, -10}, {8, 4, -10}, {-8, 4, -10}, 4, RGBA{255, 255, 255, 255});
    s.meshes.push_back(std::move(fence));

    // Three overlapping translucent panes: correct only if drawn back-to-front.
    const RGBA panes[3] = {{255, 60, 60, 140}, {60, 255, 60, 140}, {60, 60, 255, 140}};
    for (int i = 0; i < 3; ++i) {
        Mesh m = mesh("glass" + std::to_string(i), -1, Blend::Alpha);
        m.twoSided = true;
        const float x = -2.5f + float(i) * 1.5f, z = -2.0f - float(i) * 1.5f;
        quad(m, {x - 1.5f, 0.2f, z}, {x + 1.5f, 0.2f, z}, {x + 1.5f, 3.0f, z}, {x - 1.5f, 3.0f, z}, 1, panes[i]);
        s.meshes.push_back(std::move(m));
    }

    Mesh glow = mesh("glow", 2, Blend::Additive);
    glow.twoSided = true;
    for (int i = 0; i < 3; ++i) {
        const float x = -4.0f + float(i) * 4.0f;
        quad(glow, {x - 1, 4, -6}, {x + 1, 4, -6}, {x + 1, 6, -6}, {x - 1, 6, -6}, 1,
             RGBA{uint8_t(i == 0 ? 255 : 80), uint8_t(i == 1 ? 255 : 80), uint8_t(i == 2 ? 255 : 80), 255});
    }
    s.meshes.push_back(std::move(glow));
    return s;
}

// TB: real decoded textures laid out as a wall of quads (z = 0, facing +Z). A gallery, NOT a level.
Scene tbGallery(const BzeFile& bze, const std::string& level) {
    Scene s;
    s.game = GameId::TimeBusters;
    s.level = level;
    s.placeholder = true;
    s.textures = decodeTbTextures(bze, level, &s.stats);
    const int n = int(s.textures.size()), cols = std::max(1, int(std::ceil(std::sqrt(float(n) * 16.0f / 9.0f))));
    const int rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; ++i) {
        const Texture& t = s.textures[size_t(i)];
        const float k = 1.0f / float(std::max(t.width, t.height));  // longest side = 1 unit, cell pitch 1.2
        const float w = float(t.width) * k, h = float(t.height) * k;
        const float x = (float(i % cols) - float(cols - 1) * 0.5f) * 1.2f, y = (float(rows - 1 - i / cols) + 0.5f) * 1.2f;
        Mesh m = mesh(t.name, i, t.hasAlpha ? Blend::AlphaTest : Blend::Opaque);
        m.twoSided = true;
        quad(m, {x - w / 2, y - h / 2, 0}, {x + w / 2, y - h / 2, 0}, {x + w / 2, y + h / 2, 0}, {x - w / 2, y + h / 2, 0}, 1,
             RGBA{255, 255, 255, 255});
        s.meshes.push_back(std::move(m));
    }
    const float span = float(std::max(cols, rows)) * 1.2f;
    s.spawn = std::array<float, 3>{0.0f, float(rows) * 0.6f, span * 0.55f};
    s.notes.push_back(std::format("TEXTURE GALLERY - not a level: {} of {} TB textures decoded", n, s.stats.textureRefs));
    s.notes.push_back("TB geometry: Research Required (octree model format, MODELS.md 10)");
    return s;
}

} // namespace

Scene loadLevelScene(GameId game, const fs::path& dataRoot, const std::string& level) {
    const bool lit = game == GameId::LostInTime;
    const fs::path file = findPath(dataRoot, {lit ? "lit" : "tb", "Datas", "bze", level + ".bze"});
    if (file.empty()) {
        Scene s = placeholderScene(game, level);
        s.notes.push_back(std::format("PLACEHOLDER - not original data: {}.bze not found under {}", level, dataRoot.string()));
        return s;
    }
    try {
        const BzeFile bze = loadBze(file);
        return lit ? buildLitScene(bze, level) : tbGallery(bze, level);
    } catch (const std::exception& e) {
        Scene s = placeholderScene(game, level);
        s.notes.push_back(std::format("PLACEHOLDER - not original data: decode failed: {}", e.what()));
        return s;
    }
}

} // namespace bb
