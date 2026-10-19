// SDL3 window + OpenGL (4.6 core, 3.3 core fallback) renderer for bb::Scene, plus a tiny 2D layer
// (images, rects, 8x8 bitmap text) for menus and diagnostics.
#pragma once
#include "bb/render/camera.h"
#include "bb/scene.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace bb::render {

enum class Fidelity : uint8_t {
    Original, // nearest filtering, no aniso, no MSAA, optional 4:3 pillarbox
    Enhanced, // trilinear + anisotropic, MSAA, Hor+ at any aspect
};

struct WindowConfig {
    int width = 1280, height = 720;
    bool fullscreen = false;
    bool hidden = false;
    int msaaSamples = 4; // default-framebuffer samples; 0 = none. Enabled/disabled per frame via Settings::msaa
    const char* title = "bbtime";
};

struct Settings {
    Fidelity fidelity = Fidelity::Enhanced;
    float vfovDeg = 60.0f;   // vertical FOV at 4:3 (Hor+ for wider). Original value: Research Required
    bool pillarbox = true;   // Original mode only: draw 3D in a centered 4:3 viewport
    bool msaa = true;        // Enhanced mode only
    bool fog = true;
    float fogStart = 30.0f, fogEnd = 120.0f; // linear fog, view-space distance; fogEnd <= fogStart disables
    float fogColor[3] = {0.45f, 0.6f, 0.85f};
    float zNear = 0.1f, zFar = 2000.0f;
};

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

    // Replaces the GPU copy of the scene (textures + one shared VBO/IBO).
    void uploadScene(const Scene& scene);

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
    struct GpuMesh { uint32_t texture; Blend blend; int32_t baseVertex; uint32_t firstIndex, indexCount; glm::vec3 center; };
    struct V2 { float x, y, u, v; uint8_t rgba[4]; };

    void flush2D();
    void quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, const uint8_t rgba[4]);
    void freeScene();

    SDL_Window* window_ = nullptr;
    void* glContext_ = nullptr;
    std::string vendor_, rendererName_, version_;
    float maxAniso_ = 1.0f;
    int msaaSamples_ = 0;
    int w_ = 0, h_ = 0, drawCalls_ = 0;

    uint32_t prog3D_ = 0, prog2D_ = 0;
    int uViewProj_ = -1, uView_ = -1, uAlphaRef_ = -1, uFog_ = -1, uFogColor_ = -1, uAdditive_ = -1, uScreen_ = -1;
    uint32_t sampNearest_ = 0, sampTrilinear_ = 0, sampUiNearest_ = 0, sampUiLinear_ = 0;
    uint32_t white_ = 0, font_ = 0;

    uint32_t sceneVao_ = 0, sceneVbo_ = 0, sceneIbo_ = 0;
    std::vector<uint32_t> sceneTextures_;
    std::vector<GpuMesh> meshes_;

    uint32_t vao2D_ = 0, vbo2D_ = 0;
    std::vector<V2> batch_;
    uint32_t batchTex_ = 0, batchSampler_ = 0;
};

} // namespace bb::render
