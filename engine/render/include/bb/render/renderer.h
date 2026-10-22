// SDL3 window + OpenGL (4.6 core, 3.3 core fallback) renderer for bb::Scene, plus a tiny 2D layer
// (images, rects, 8x8 bitmap text) for menus and diagnostics.
#pragma once
#include "bb/render/camera.h"
#include "bb/render/zones.h"
#include "bb/scene.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace bb::render {

enum class Fidelity : uint8_t {
    Original, // nearest filtering, no aniso, no MSAA, optional 4:3 pillarbox
    Enhanced, // trilinear + anisotropic, MSAA, Hor+ at any aspect
};

// Enhanced dynamic sun shadows (Vulkan backend only; the originals have no dynamic lights, docs/ROADMAP.md "Ray
// tracing"). Rt / RtAo fall back to Map on devices without ray queries.
enum class Shadows : uint8_t {
    Off,
    Map,  // cascaded shadow map
    Rt,   // ray-query shadows
    RtAo, // ray-query shadows + ray-traced ambient occlusion
};

// Enhanced FSR 3.1.4 upscaling (Vulkan backend only). Render scale per axis: Quality 1/1.5, Balanced 1/1.7,
// Performance 1/2, NativeAa 1 (FSR as anti-aliasing).
enum class Upscaling : uint8_t { Off, Quality, Balanced, Performance, NativeAa };

struct WindowConfig {
    int width = 1280, height = 720;
    bool fullscreen = false;
    bool hidden = false;
    int msaaSamples = 4; // default-framebuffer samples; 0 = none. Enabled/disabled per frame via Settings::msaa
    const char* title = "bbtime";
};

struct Settings {
    Fidelity fidelity = Fidelity::Enhanced;
    float vfovDeg = 60.0f;   // Enhanced: user vertical FOV at 4:3 (Hor+ for wider aspects)
    bool pillarbox = true;   // Original mode only: draw 3D in a centered 4:3 viewport
    bool frame43 = false;    // both modes: the original 4:3 framing (51.28°, pillarbox), e.g. 3-D menus under 4:3 text
    bool msaa = true;        // Enhanced mode only
    Shadows shadows = Shadows::RtAo; // Enhanced mode only, not with frame43 (3-D menus keep the original look)
    bool reflections = true;         // ray-traced reflections on water / ice / polished floors (Enhanced, ray queries)
    Upscaling upscaling = Upscaling::Off; // Enhanced, Vulkan; replaces MSAA while on
    bool frameGeneration = false;    // FSR frame generation (Enhanced, Vulkan; upscaling off = FSR native AA)
    bool fog = true;
    float fogStart = 30.0f, fogEnd = 120.0f; // linear fog, view-space distance; fogEnd <= fogStart disables
    // Clear and fog colour: black in both exes (LiT SetBackColor 0x40cc10 is only ever called with 0,0,0 and its fog
    // fades polygons to what lies behind via vertex alpha; TB clears once to 0,0,0 at 0x422950 and has no fog colour.
    // RUNTIME_REFERENCE.md §5).
    float fogColor[3] = {0.0f, 0.0f, 0.0f};
    float zNear = 0.1f, zFar = 2000.0f;
    // Projection FOV for both backends. Original Fidelity is fixed to the exes' 51.28° (camera.h); its 4:3 pillarbox
    // gives the original framing, and without the pillarbox wider windows get Hor+ (the exe would stretch instead).
    // Enhanced keeps the user setting (default 60°, wider than the original by choice) with Hor+.
    float vfovRad() const { return fidelity == Fidelity::Original || frame43 ? kOriginalVFovRad : glm::radians(vfovDeg); }
    // Vertex rgb multiplier and texel gamma for both backends. Original Fidelity = the PC exes' GL path: colour bytes
    // /255 (no doubling) and Scene::pcTexGamma on texels, measured against the originals (RUNTIME_REFERENCE.md §7).
    // Enhanced: the scene's PS1 modulation (Scene::colorScale, texel·colour/128, the brighter console look) and
    // linear texels, by choice.
    float colorScale(float sceneScale) const { return fidelity == Fidelity::Original ? 1.0f : sceneScale; }
    float texGamma(float sceneGamma) const { return fidelity == Fidelity::Original ? sceneGamma : 1.0f; }
};

// CPU helpers shared with other backends.
// Copy of t.rgba with fully transparent texels' rgb filled from opaque neighbours (alpha kept), see renderer.cpp.
std::vector<uint8_t> bleedTransparent(const Texture& t);
// 128x48 RGBA atlas, 16x6 glyphs (8x8) of ASCII 32..127, white on transparent. Empty on failure.
Texture fontAtlas();

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    // Creates the window and GL context. Returns an error message on failure (empty on success).
    std::string init(const WindowConfig& cfg);
    SDL_Window* window() const { return window_; }
    void setVsync(bool on);
    void setFullscreen(bool on);

    std::string_view glVendor() const { return vendor_; }
    std::string_view glRenderer() const { return rendererName_; }
    std::string_view glVersion() const { return version_; }
    float maxAnisotropy() const { return maxAniso_; }
    int msaaSamples() const { return msaaSamples_; }

    // Replaces the GPU copy of the scene (textures + one shared VBO/IBO). Clears actor meshes.
    void uploadScene(const Scene& scene);
    // Per-frame dynamic meshes (posed actors, bb/actor.h poseActor): engine space, Mesh::texture indexes the
    // uploaded scene's textures. Streamed to the GPU; kept until the next call; drawn by drawScene in the same
    // opaque / alpha-test / translucent passes as the scene.
    void setActorMeshes(std::span<const Mesh> meshes);
    // 30 Hz simulation tick selecting the current frame of Scene::textureAnims (Mesh::textureAnim).
    void setTextureTick(uint32_t tick) { texTick_ = tick; }
    // LiT zone/portal visibility (zones.h) for the next drawScene calls; default = draw every zone.
    void setZoneView(const ZoneView& v) { zoneView_ = v; }
    // Visible-zone table of the last drawScene (0x4b3460 order); empty = no zone walk (everything drawn).
    const std::vector<ZoneEntry>& zoneTable() const { return zoneTable_; }

    // 2D images (e.g. title-screen BMPs). Returns a GL texture name, 0 on invalid input.
    uint32_t createImage(const Texture& tex);
    void destroyImage(uint32_t image);

    void beginFrame(const Settings& s);              // queries drawable size, clears
    void drawScene(const Camera& cam, const Settings& s);
    // 2D layer in window pixels, origin top-left. Batched; flushed on texture change and endFrame().
    void drawImage(uint32_t image, float x, float y, float w, float h, bool smooth, const uint8_t rgba[4] = nullptr);
    void drawRect(float x, float y, float w, float h, const uint8_t rgba[4]);
    void drawText(float x, float y, int scale, std::string_view text, const uint8_t rgba[4]);
    static constexpr int kGlyph = 8; // glyph cell size in pixels at scale 1

    // Reads the back buffer. Call after drawing, before present(). .png via SDL, anything else as binary PPM.
    bool screenshot(const std::filesystem::path& path);
    void present();

    int width() const { return w_; }
    int height() const { return h_; }
    int drawCalls() const { return drawCalls_; }

private:
    struct GpuMesh { uint32_t texture; Blend blend; bool twoSided; int32_t baseVertex; uint32_t firstIndex, indexCount; glm::vec3 center; uint32_t vao; int32_t textureAnim; int16_t zone; };
    struct GpuTexAnim { std::vector<uint32_t> frames; uint32_t start, hold; };
    struct V2 { float x, y, u, v; uint8_t rgba[4]; };

    void flush2D();
    void quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, const uint8_t rgba[4]);
    void freeScene();
    void appendMeshes(std::span<const Mesh> in, uint32_t vao, std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                      std::vector<GpuMesh>& out) const;
    static void upload(uint32_t vao, uint32_t& vbo, uint32_t& ibo, const std::vector<Vertex>& verts,
                       const std::vector<uint32_t>& idx, uint32_t usage);

    SDL_Window* window_ = nullptr;
    void* glContext_ = nullptr;
    std::string vendor_, rendererName_, version_;
    float maxAniso_ = 1.0f;
    int msaaSamples_ = 0;
    int w_ = 0, h_ = 0, drawCalls_ = 0;

    uint32_t prog3D_ = 0, prog2D_ = 0;
    int uViewProj_ = -1, uView_ = -1, uAlphaRef_ = -1, uFog_ = -1, uFogColor_ = -1, uAdditive_ = -1, uScreen_ = -1,
        uColorScale_ = -1, uTexGamma_ = -1;
    float colorScale_ = 1.0f, texGamma_ = 1.0f;  // Scene::colorScale / Scene::pcTexGamma of the uploaded scene
    bool linearOriginal_ = false;  // LiT: bugs.exe GL sets GL_LINEAR min/mag (0x4220c4), no mipmaps
    uint32_t sampNearest_ = 0, sampTrilinear_ = 0, sampUiNearest_ = 0, sampUiLinear_ = 0, sampLinear_ = 0;
    uint32_t white_ = 0, font_ = 0;

    uint32_t sceneVao_ = 0, sceneVbo_ = 0, sceneIbo_ = 0;
    std::vector<uint32_t> sceneTextures_;
    std::vector<GpuMesh> meshes_;
    std::vector<GpuTexAnim> texAnims_;
    uint32_t texTick_ = 0;
    std::vector<WorldZone> zones_;  // Scene::zones of the uploaded scene
    ZoneView zoneView_;
    std::vector<ZoneEntry> zoneTable_;
    std::vector<uint8_t> zoneDrawn_;
    uint32_t actorVao_ = 0, actorVbo_ = 0, actorIbo_ = 0;
    std::vector<GpuMesh> actorMeshes_;
    std::vector<Vertex> actorVerts_; // staging, reused per frame
    std::vector<uint32_t> actorIdx_;

    uint32_t vao2D_ = 0, vbo2D_ = 0;
    std::vector<V2> batch_;
    uint32_t batchTex_ = 0, batchSampler_ = 0;
};

} // namespace bb::render
