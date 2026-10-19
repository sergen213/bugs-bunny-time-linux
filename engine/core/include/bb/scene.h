// Backend-neutral scene data shared by format decoders (bb::core) and renderers.
// Decoders fill these; renderers only read them. Keep this POD-ish and stable.
#pragma once
#include <cstdint>
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

struct Vertex {
    float pos[3];                // world space, game units converted to float (20.12 fixed -> /4096)
    float uv[2];                 // normalized 0..1
    uint8_t rgba[4];             // vertex colour / gouraud
};

enum class Blend : uint8_t { Opaque, AlphaTest, Alpha, Additive };

struct Mesh {
    std::string name;
    int32_t texture = -1;        // index into Scene::textures, -1 = untextured
    Blend blend = Blend::Opaque;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangle list
};

struct Scene {
    GameId game = GameId::LostInTime;
    std::string level;           // e.g. "L01A"
    std::vector<Texture> textures;
    std::vector<Mesh> meshes;
    bool placeholder = false;    // true when content is NOT decoded original data
};

} // namespace bb
