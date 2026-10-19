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
    int32_t textureAnim = -1;    // index into Scene::textureAnims; when set it overrides `texture` (the fallback)
};

// Texture link / animated texture: faces bound to link `linkId` show frames[(startFrame + tick / holdTicks) %
// frames.size()] at 30 Hz simulation tick `tick` (Renderer::setTextureTick).
struct TextureAnim {
    uint32_t linkId = 0;
    std::vector<int32_t> frames;  // Scene::textures indices
    uint32_t startFrame = 0;
    uint32_t holdTicks = 1;
    bool timingInferred = false;  // e.g. script-driven links cycled in order
};

// Sony TOD animation (LiT, GAMEPLAY.md §6), decoded to the packet types the frame applier 0x44e400 acts on:
// 1 coord (v0..2 rot 4.12, v3..5 scale Q12, v6..8 trans; flag bits 2/4/8 say which are present), 2 TMD object
// (v0: 1-based, 0 = none), 3 parent (v0: part id, 0/0xffff = actor root), 8 object control (flag only).
struct TodPacket {
    uint16_t id = 0;             // part: 1-based position in the part list at frame start (0 = actor)
    uint8_t type = 0, flag = 0;
    std::array<int32_t, 9> v{};
};
struct Tod {
    uint16_t resolution = 0;          // 1/60 s; each frame is held resolution >> 1 ticks at 30 Hz
    std::vector<uint32_t> frameStart; // frame k = packets[frameStart[k], frameStart[k + 1])
    std::vector<TodPacket> packets;
    size_t frames() const { return frameStart.empty() ? 0 : frameStart.size() - 1; }
};
struct ActorAnim {
    uint16_t id = 0;             // chunk 0x22 tag 0x27 role = animation id used by move nodes
    Tod tod;
};
// Role-7 actor model: per TMD object its vertices (model space, PS1 axes) with the vertex-word-3 skinning slot,
// and template triangles per (object, texture, blend, sidedness) whose vertices index slots.
struct ActorVertex {
    float pos[3];
    uint16_t slot;               // word 3 & 0x7fff: index into the actor's shared vertex buffer
    bool blend;                  // word 3 & 0x8000: average with the value another part already wrote
};
struct ActorMesh {
    Mesh mesh;                   // vertex pos unused (overwritten when posed)
    uint32_t object = 0;         // 0-based TMD object these triangles belong to
    std::vector<uint16_t> slots; // per mesh vertex
};
struct ActorModel {
    uint16_t modelAsset = 0, skeletonAsset = 0; // tag 0x27 asset ids (roles 7 and 4)
    uint32_t slotCount = 0;
    std::vector<std::vector<ActorVertex>> objects;
    std::vector<ActorMesh> meshes;
    Tod skeleton;                 // one frame building the part list
    std::vector<ActorAnim> anims; // the actor's other TODs, tag 0x0f order
};
// A placed LiT entity drawn as an animated actor.
struct ActorInstance {
    uint32_t model = 0;                             // Scene::actorModels index
    std::array<int32_t, 3> pos{};                   // PS1 world units (tag 0x10)
    std::array<int16_t, 3> rot{};                   // 4.12 (tag 0x11)
    std::array<int32_t, 3> scale{4096, 4096, 4096}; // Q12 (tag 0x12)
    uint16_t anim = 0;           // initial animation id: start state (0x32 id 2, else 1) -> first step -> anim
                                 // (LOGIC.md §2); else the first TOD in tag-0x0f order showing geometry (Inferred)
    uint16_t entityId = 0;       // tag 0x27
    uint8_t chunk = 0;           // 0x07/0x0a live actor, 0x08 spawn template (not placed by the game)
    bool player = false;         // tag 0x13 == 1 (behaviour type 1: Bugs)
    uint32_t scriptOffset = 0;   // ScriptItem::offset of the entity chunk in section 1
};

struct SceneStats {
    uint32_t textureRefs = 0;      // texture list entries in the level
    uint32_t textureErrors = 0;    // entries that failed to decode (e.g. the one LiT 24-bit TIM)
    uint32_t worldModels = 0;      // chunk 0x20 world models (both games)
    uint32_t cells = 0;            // LiT world cells (0x00/0x04 headers)
    uint32_t props = 0;            // placed static prop instances
    uint32_t skippedActors = 0;    // placed multi-object entities without a TOD skeleton or flipbook actors: not drawn
    uint32_t skyDomes = 0;         // TB octree models in chunk 0x22 (sky domes, MODELS.md §10.1)
    uint32_t invalidPrims = 0;     // TB octree primitives rejected by CheckPrimValidity (not drawn)
    uint64_t worldFaces = 0;       // TB valid octree primitives (world + sky, all trees incl. the undrawn far LOD)
    uint64_t faces = 0, triangles = 0;
    // Faces whose id is neither a decoded texture nor a texture link. TB: not drawn (the original skips alias 0,
    // TEXTURE_GAP.md §1); LiT: still drawn untextured until its texture links are decoded.
    uint64_t missingTextureFaces = 0;
    uint64_t worldMissingTextureFaces = 0;   // subset of missingTextureFaces in chunk-0x20 world models
    uint64_t linkedFaces = 0;                // faces whose texture id resolves through a TextureAnim
    std::vector<uint32_t> missingTextureIds; // sorted, unique
};

struct Scene {
    GameId game = GameId::LostInTime;
    std::string level;           // e.g. "L01A"
    std::vector<Texture> textures;
    std::vector<Mesh> meshes;
    std::vector<TextureAnim> textureAnims;
    bool placeholder = false;    // true when content is NOT decoded original data (incl. galleries)
    float colorScale = 1.0f;     // vertex rgb multiplier; LiT uses PS1 modulation texel*colour/128 -> 255/128
    std::optional<std::array<float, 3>> spawn; // player start, engine space
    float spawnYaw = 0.0f;       // radians, engine Camera convention
    std::vector<ActorModel> actorModels; // LiT TOD-animated actor models (bb/actor.h poses them)
    std::vector<ActorInstance> actors;
    SceneStats stats;
    std::vector<std::string> notes; // short human-readable status lines for overlays (gaps, research state)
};

} // namespace bb
