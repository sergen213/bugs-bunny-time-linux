// Backend-neutral scene data shared by format decoders (bb::core) and renderers.
// Decoders fill these; renderers only read them. Keep this POD-ish and stable.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bb {

enum class GameId : uint8_t { LostInTime, TimeBusters };

struct Texture {
    std::string name;            // stable id, e.g. "L01A/tex/12"
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width*height*4, straight (non-premultiplied) alpha
    bool hasAlpha = false;       // any texel with alpha < 255 (colour-key or translucency)
};

// Engine space: right-handed, +Y up, camera yaw 0 looks down -Z. Decoded game data keeps game units
// (LiT: 1 unit = 1 PS1 GTE unit, levels span ~0..40000). PS1 data (+X right, +Y down, +Z forward) maps as
// (x, y, z) -> (x, -y, -z): a 180 degree rotation about X, so handedness and winding are preserved.
struct Vertex {
    float pos[3];                // world space
    float uv[2];                 // normalized 0..1
    uint8_t rgba[4];             // vertex colour / gouraud; rgb is scaled by Scene::colorScale when drawn
};

// PS1 semi-transparency (ABR) maps to: 0 B/2+F/2 -> Alpha with vertex alpha 128; 1 B+F -> Additive;
// 2 B-F -> Subtract; 3 B+F/4 -> Additive with vertex alpha 64.
enum class Blend : uint8_t { Opaque, AlphaTest, Alpha, Additive, Subtract };

struct Mesh {
    std::string name;
    int32_t texture = -1;        // index into Scene::textures, -1 = untextured
    Blend blend = Blend::Opaque;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangle list; front faces are counter-clockwise in engine space
    bool twoSided = false;       // no back-face culling
};

struct SceneStats {
    uint32_t textureRefs = 0;      // texture list entries in the level
    uint32_t textureErrors = 0;    // entries that failed to decode (e.g. the one LiT 24-bit TIM)
    uint32_t worldModels = 0;      // LiT chunk 0x20 models
    uint32_t cells = 0;            // LiT world cells (0x00/0x04 headers)
    uint32_t props = 0;            // placed static prop instances
    uint32_t skippedActors = 0;    // placed entities whose model is multi-object (skinned): not drawn
    uint64_t faces = 0, triangles = 0;
    uint64_t missingTextureFaces = 0;        // textured faces whose id has no decoded texture: drawn untextured
    std::vector<uint32_t> missingTextureIds; // sorted, unique
};

struct Scene {
    GameId game = GameId::LostInTime;
    std::string level;           // e.g. "L01A"
    std::vector<Texture> textures;
    std::vector<Mesh> meshes;
    bool placeholder = false;    // true when content is NOT decoded original data (incl. galleries)
    float colorScale = 1.0f;     // vertex rgb multiplier; LiT uses PS1 modulation texel*colour/128 -> 255/128
    std::optional<std::array<float, 3>> spawn; // player start, engine space
    float spawnYaw = 0.0f;       // radians, engine Camera convention
    SceneStats stats;
    std::vector<std::string> notes; // short human-readable status lines for overlays (gaps, research state)
};

} // namespace bb
