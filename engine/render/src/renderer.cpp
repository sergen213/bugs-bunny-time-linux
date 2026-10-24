#include "bb/render/renderer.h"
#include "bb/render/pc_order.h"

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <print>

namespace bb::render {
namespace {

constexpr const char* kVs3D = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
uniform mat4 uViewProj;
uniform mat4 uView;
uniform vec2 uLinearZ;  // near, far: LiT Original Fidelity depth = eye depth linear in screen space (bugs.exe glOrtho)
centroid out vec2 vUv;  // MSAA (Enhanced): never extrapolated outside the covered triangle (no-op without MSAA)
out vec4 vCol;
out float vDist;
void main() {
    vec4 p = vec4(aPos, 1.0);
    gl_Position = uViewProj * p;
    vUv = aUv;
    vCol = aCol;
    vDist = -(uView * p).z;
    if (uLinearZ.y > uLinearZ.x) gl_Position.z = (2.0 * (vDist - uLinearZ.x) / (uLinearZ.y - uLinearZ.x) - 1.0) * gl_Position.w;
})";

constexpr const char* kFs3D = R"(#version 330 core
uniform sampler2D uTex;
uniform float uAlphaRef;   // texels below are discarded (AlphaTest); negative = off
uniform vec2 uFog;         // linear fog start, end; end <= start = off
uniform vec3 uFogColor;
uniform int uAdditive;     // 1: additive/subtractive surfaces fade to black, not to the fog colour; 2: PC ABR-2 texel
                           // alpha = trunc luma of the raw texel (bugs.exe 0x420d10 case 2), alpha-blended, fog to black;
                           // 3: LiT PC ABR 0/1/3 (0x420d10 modes 0/1/3): the raw texel's max channel m (0..255) is
                           // raised to 255 (rgb += 255 - m) and gives the alpha m/2, m - m/4, m/4; the vertex alpha only
                           // names the mode (128, 255, 64); alpha-blended, fog fades the alpha
uniform float uColorScale; // vertex rgb multiplier (PS1 modulation: 255/128), result saturates
uniform float uTexGamma;   // != 1: PC texel curve floor(256·(c/256)^g) on the 0..255 texel (Settings::texGamma)
uniform bool uUvClamp;     // Enhanced non-tiling translucent mesh: UVs clamped half a texel inside (no wrap seam)
centroid in vec2 vUv;
in vec4 vCol;
in float vDist;
out vec4 oCol;
void main() {
    vec2 uv = vUv;
    if (uUvClamp) {
        vec2 h = 0.5 / vec2(textureSize(uTex, 0));
        uv = clamp(uv, h, 1.0 - h);
    }
    vec4 t = texture(uTex, uv);
    float luma = floor(dot(t.rgb * 255.0, vec3(0.299, 0.587, 0.114)) + 1e-3) / 255.0;
    vec4 vc = vCol;
    if (uAdditive == 3) {
        float mx = floor(max(t.r, max(t.g, t.b)) * 255.0 + 0.5), va = floor(vCol.a * 255.0 + 0.5);
        t.rgb += (255.0 - mx) / 255.0;
        t.a *= (va == 128.0 ? floor(mx / 2.0) : va == 64.0 ? floor(mx / 4.0) : mx - floor(mx / 4.0)) / 255.0;
        vc.a = 1.0;
    }
    if (uTexGamma != 1.0) t.rgb = floor(256.0 * pow(t.rgb * (255.0 / 256.0), vec3(uTexGamma)) + 1e-3) / 255.0;
    vec4 c = t * vc;
    if (uAdditive == 2) c = vec4(0.0, 0.0, 0.0, luma * vCol.a);
    c.rgb = min(c.rgb * uColorScale, vec3(1.0));
    if (c.a < uAlphaRef) discard;
    if (uFog.y > uFog.x) {
        float f = clamp((uFog.y - vDist) / (uFog.y - uFog.x), 0.0, 1.0);
        if (uAdditive == 3) c.a *= f;
        else c.rgb = mix(uAdditive != 0 ? vec3(0.0) : uFogColor, c.rgb, f);
    }
    oCol = c;
})";

constexpr const char* kVs2D = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aCol;
uniform vec2 uScreen;
out vec2 vUv;
out vec4 vCol;
void main() {
    gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
    vUv = aUv;
    vCol = aCol;
})";

constexpr const char* kFs2D = R"(#version 330 core
uniform sampler2D uTex;
in vec2 vUv;
in vec4 vCol;
out vec4 oCol;
void main() { oCol = texture(uTex, vUv) * vCol; })";

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::println(stderr, "shader compile failed: {}", log);
    }
    return s;
}

GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram(), v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::println(stderr, "shader link failed: {}", log);
    }
    return p;
}

GLuint makeSampler(GLint minF, GLint magF, GLint wrap, float aniso) {
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minF);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, magF);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap);
    if (aniso > 1.0f) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    return s;
}

GLuint makeTexture(uint32_t w, uint32_t h, const void* rgba, bool mips) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (mips) glGenerateMipmap(GL_TEXTURE_2D);
    else glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

} // namespace

// 16x6 atlas of ASCII 32..127, drawn with SDL's built-in 8x8 debug font (SDL_RenderDebugText) into a software
// surface: no font data to embed, no SDL3_ttf. Empty texture on failure.
Texture fontAtlas() {
    constexpr int G = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    static_assert(G == Renderer::kGlyph);
    Texture t;
    t.name = "font8x8";
    SDL_Surface* s = SDL_CreateSurface(16 * G, 6 * G, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* r = s ? SDL_CreateSoftwareRenderer(s) : nullptr;
    if (!r) {
        std::println(stderr, "font atlas: {}", SDL_GetError());
        SDL_DestroySurface(s);
        return t;
    }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int c = 32; c < 128; ++c) {
        const char str[2] = {char(c), 0};
        SDL_RenderDebugText(r, float((c - 32) % 16 * G), float((c - 32) / 16 * G), str);
    }
    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    t.width = uint32_t(s->w), t.height = uint32_t(s->h), t.hasAlpha = true;
    t.rgba.resize(size_t(t.width) * t.height * 4);
    for (uint32_t y = 0; y < t.height; ++y)
        std::copy_n(static_cast<const uint8_t*>(s->pixels) + size_t(y) * size_t(s->pitch), size_t(t.width) * 4,
                    t.rgba.data() + size_t(y) * t.width * 4);
    SDL_DestroySurface(s);
    return t;
}

Renderer::~Renderer() {
    if (glContext_) {
        freeScene();
        GLuint objs[] = {white_, font_};
        glDeleteTextures(2, objs);
        GLuint samplers[] = {sampNearest_, sampTrilinear_, sampUiNearest_, sampUiLinear_, sampLinear_};
        glDeleteSamplers(5, samplers);
        glDeleteBuffers(1, &vbo2D_);
        glDeleteVertexArrays(1, &vao2D_);
        glDeleteProgram(prog3D_);
        glDeleteProgram(prog2D_);
        SDL_GL_DestroyContext(SDL_GLContext(glContext_));
    }
    if (window_) SDL_DestroyWindow(window_);
}

std::string Renderer::init(const WindowConfig& cfg) {
    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;

    // Try 4.6 core, then 3.3 core (llvmpipe/CI); MSAA window first, then without.
    for (int ver : {46, 33}) {
        for (int samples : {cfg.msaaSamples, 0}) {
            SDL_GL_ResetAttributes();
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, ver / 10);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, ver % 10);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, samples > 0);
            SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, samples);
            window_ = SDL_CreateWindow(cfg.title, cfg.width, cfg.height, flags);
            if (!window_) continue;
            glContext_ = SDL_GL_CreateContext(window_);
            if (glContext_) {
                msaaSamples_ = samples;
                break;
            }
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
        if (glContext_) break;
    }
    if (!glContext_) return std::string("cannot create OpenGL 3.3+ core context: ") + SDL_GetError();
    SDL_GL_MakeCurrent(window_, SDL_GLContext(glContext_));
    SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &msaaSamples_);

    vendor_ = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    rendererName_ = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    version_ = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (epoxy_gl_version() >= 46 || epoxy_has_gl_extension("GL_ARB_texture_filter_anisotropic") ||
        epoxy_has_gl_extension("GL_EXT_texture_filter_anisotropic"))
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso_);

    prog3D_ = link(kVs3D, kFs3D);
    uViewProj_ = glGetUniformLocation(prog3D_, "uViewProj");
    uView_ = glGetUniformLocation(prog3D_, "uView");
    uLinearZ_ = glGetUniformLocation(prog3D_, "uLinearZ");
    uAlphaRef_ = glGetUniformLocation(prog3D_, "uAlphaRef");
    uFog_ = glGetUniformLocation(prog3D_, "uFog");
    uFogColor_ = glGetUniformLocation(prog3D_, "uFogColor");
    uAdditive_ = glGetUniformLocation(prog3D_, "uAdditive");
    uColorScale_ = glGetUniformLocation(prog3D_, "uColorScale");
    uTexGamma_ = glGetUniformLocation(prog3D_, "uTexGamma");
    uUvClamp_ = glGetUniformLocation(prog3D_, "uUvClamp");
    prog2D_ = link(kVs2D, kFs2D);
    uScreen_ = glGetUniformLocation(prog2D_, "uScreen");

    sampNearest_ = makeSampler(GL_NEAREST, GL_NEAREST, GL_REPEAT, 1.0f);
    sampLinear_ = makeSampler(GL_LINEAR, GL_LINEAR, GL_REPEAT, 1.0f);
    sampTrilinear_ = makeSampler(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_REPEAT, maxAniso_);
    sampUiNearest_ = makeSampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE, 1.0f);
    sampUiLinear_ = makeSampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE, 1.0f);

    const uint8_t whitePx[4] = {255, 255, 255, 255};
    white_ = makeTexture(1, 1, whitePx, false);
    {
        const Texture f = fontAtlas();
        font_ = f.rgba.empty() ? 0 : makeTexture(f.width, f.height, f.rgba.data(), false);
    }

    glGenVertexArrays(1, &vao2D_);
    glGenBuffers(1, &vbo2D_);
    glBindVertexArray(vao2D_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2D_);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V2), reinterpret_cast<void*>(offsetof(V2, x)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V2), reinterpret_cast<void*>(offsetof(V2, u)));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(V2), reinterpret_cast<void*>(offsetof(V2, rgba)));
    return {};
}

void Renderer::setVsync(bool on) {
    // Adaptive vsync first (tears instead of stalling when late), plain vsync otherwise.
    if (!on || !SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(on ? 1 : 0);
}

void Renderer::setFullscreen(bool on) { SDL_SetWindowFullscreen(window_, on); }

void Renderer::freeScene() {
    glDeleteTextures(GLsizei(sceneTextures_.size()), sceneTextures_.data());
    sceneTextures_.clear();
    meshes_.clear();
    actorMeshes_.clear();
    texAnims_.clear();
    const GLuint bufs[] = {sceneVbo_, sceneIbo_, actorVbo_, actorIbo_}, vaos[] = {sceneVao_, actorVao_};
    glDeleteBuffers(4, bufs);
    glDeleteVertexArrays(2, vaos);
    sceneVbo_ = sceneIbo_ = sceneVao_ = actorVbo_ = actorIbo_ = actorVao_ = 0;
}

// Fills the rgb of fully transparent texels from opaque neighbours (alpha stays 0), so bilinear filtering and
// mipmaps do not pull black fringes into colour-keyed cutouts (PS1 0x0000 = transparent black). Dilates ring by ring
// until every transparent texel has a colour (at most max(w, h) passes; none for a fully transparent texture).
std::vector<uint8_t> bleedTransparent(const Texture& t) {
    std::vector<uint8_t> px = t.rgba;
    const int w = int(t.width), h = int(t.height);
    std::vector<uint8_t> filled(size_t(w) * h);
    for (size_t i = 0; i < filled.size(); ++i) filled[i] = px[i * 4 + 3] != 0;
    for (;;) {
        std::vector<uint8_t> next = filled;
        bool changed = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (filled[i]) continue;
                int sum[3] = {}, n = 0;
                for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                    const int nx = (x + dx + w) % w, ny = (y + dy + h) % h;  // GL_REPEAT neighbourhood
                    const size_t j = size_t(ny) * w + nx;
                    if (!filled[j]) continue;
                    for (int c = 0; c < 3; ++c) sum[c] += px[j * 4 + c];
                    ++n;
                }
                if (!n) continue;
                for (int c = 0; c < 3; ++c) px[i * 4 + c] = uint8_t(sum[c] / n);
                next[i] = 1;
                changed = true;
            }
        filled.swap(next);
        if (!changed) break;
    }
    return px;
}

void Renderer::uploadScene(const Scene& scene) {
    freeScene();
    colorScale_ = scene.colorScale;
    texGamma_ = scene.pcTexGamma;
    linearOriginal_ = scene.game == GameId::LostInTime;
    for (const Texture& t : scene.textures) {
        const bool valid = t.width && t.height && t.rgba.size() == size_t(t.width) * t.height * 4;
        sceneTextures_.push_back(!valid       ? 0
                                 : t.hasAlpha ? makeTexture(t.width, t.height, bleedTransparent(t).data(), true)
                                              : makeTexture(t.width, t.height, t.rgba.data(), true));
    }
    auto glTex = [&](int32_t i) { return i >= 0 && size_t(i) < sceneTextures_.size() && sceneTextures_[size_t(i)] ? sceneTextures_[size_t(i)] : white_; };
    for (const TextureAnim& a : scene.textureAnims) {
        GpuTexAnim& g = texAnims_.emplace_back(GpuTexAnim{{}, a.startFrame, std::max(a.holdTicks, 1u)});
        for (int32_t f : a.frames) g.frames.push_back(glTex(f));
    }

    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    if (!sceneVao_) glGenVertexArrays(1, &sceneVao_);
    appendMeshes(scene.meshes, sceneVao_, verts, idx, meshes_);
    zones_ = scene.zones;
    upload(sceneVao_, sceneVbo_, sceneIbo_, verts, idx, GL_STATIC_DRAW);
}

void Renderer::appendMeshes(std::span<const Mesh> in, uint32_t vao, std::vector<Vertex>& verts, std::vector<uint32_t>& idx,
                            std::vector<GpuMesh>& out) const {
    for (const Mesh& m : in) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        glm::vec3 lo(m.vertices[0].pos[0], m.vertices[0].pos[1], m.vertices[0].pos[2]), hi = lo;
        for (const Vertex& v : m.vertices) {
            lo = glm::min(lo, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
            hi = glm::max(hi, glm::vec3(v.pos[0], v.pos[1], v.pos[2]));
        }
        const bool texOk = m.texture >= 0 && size_t(m.texture) < sceneTextures_.size() && sceneTextures_[size_t(m.texture)];
        const bool animOk = m.textureAnim >= 0 && size_t(m.textureAnim) < texAnims_.size() && !texAnims_[size_t(m.textureAnim)].frames.empty();
        const bool unitUv = std::ranges::all_of(m.vertices, [](const Vertex& v) {
            return v.uv[0] >= -1e-4f && v.uv[0] <= 1.0001f && v.uv[1] >= -1e-4f && v.uv[1] <= 1.0001f;
        });
        out.push_back({texOk ? sceneTextures_[size_t(m.texture)] : white_, m.blend, m.twoSided, int32_t(verts.size()),
                       uint32_t(idx.size()), uint32_t(m.indices.size()), (lo + hi) * 0.5f, vao, animOk ? m.textureAnim : -1, m.zone,
                       m.late, m.background, m.farClip, m.fogEnd, triCentroids(m), unitUv});
        verts.insert(verts.end(), m.vertices.begin(), m.vertices.end());
        idx.insert(idx.end(), m.indices.begin(), m.indices.end());
    }
}

// (Re)fills vbo/ibo (created on first use) and binds them to vao with the Vertex layout.
void Renderer::upload(uint32_t vao, uint32_t& vbo, uint32_t& ibo, const std::vector<Vertex>& verts,
                      const std::vector<uint32_t>& idx, uint32_t usage) {
    const bool fresh = !vbo;
    if (fresh) {
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(Vertex)), verts.data(), usage);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * sizeof(uint32_t)), idx.data(), usage);
    if (fresh) {
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, pos)));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, rgba)));
    }
    glBindVertexArray(0);
}

void Renderer::setActorMeshes(std::span<const Mesh> meshes) {
    actorMeshes_.clear();
    actorVerts_.clear();
    actorIdx_.clear();
    if (!actorVao_) glGenVertexArrays(1, &actorVao_);
    appendMeshes(meshes, actorVao_, actorVerts_, actorIdx_, actorMeshes_);
    upload(actorVao_, actorVbo_, actorIbo_, actorVerts_, actorIdx_, GL_STREAM_DRAW);  // orphans last frame's storage
}

uint32_t Renderer::createImage(const Texture& t) {
    if (!t.width || !t.height || t.rgba.size() != size_t(t.width) * t.height * 4) return 0;
    return makeTexture(t.width, t.height, t.rgba.data(), false);
}

void Renderer::destroyImage(uint32_t image) {
    GLuint t = image;
    if (t) glDeleteTextures(1, &t);
}

void Renderer::beginFrame(const Settings& s) {
    SDL_GetWindowSizeInPixels(window_, &w_, &h_);
    drawCalls_ = 0;
    glViewport(0, 0, w_, h_);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (s.fidelity == Fidelity::Enhanced && s.msaa && msaaSamples_ > 0) glEnable(GL_MULTISAMPLE);
    else glDisable(GL_MULTISAMPLE);
}

void Renderer::drawScene(const Camera& cam, const Settings& s) {
    if ((meshes_.empty() && actorMeshes_.empty()) || w_ <= 0 || h_ <= 0) return;
    flush2D();
    const bool original = s.fidelity == Fidelity::Original;
    const Viewport vp = (original && s.pillarbox) || s.frame43 ? fitAspect(w_, h_, 4.0f / 3.0f) : Viewport{0, 0, w_, h_};
    glViewport(vp.x, vp.y, vp.w, vp.h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp.x, vp.y, vp.w, vp.h);
    glClearColor(s.fogColor[0], s.fogColor[1], s.fogColor[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::mat4 view = cam.view();
    const glm::mat4 vp3 = projection(s.vfovRad(), float(vp.w) / float(vp.h), s.zNear, s.zFar) * view;
    const bool zoned = walkZones(zones_, vp3, zoneView_, zoneTable_, zoneDrawn_);
    auto shown = [&](const GpuMesh& m) { return !zoned || m.zone < 0 || zoneDrawn_[size_t(m.zone)]; };
    glUseProgram(prog3D_);
    glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, glm::value_ptr(vp3));
    glUniformMatrix4fv(uView_, 1, GL_FALSE, glm::value_ptr(view));
    const bool pcLit = original && linearOriginal_;  // bugs.exe depth and blended order (pc_order.h)
    glUniform2f(uLinearZ_, pcLit ? s.zNear : 0.0f, pcLit ? s.zFar : 0.0f);
    // Fog per mesh: the background layer never (TB has no fog; it sits beyond any fog range), Mesh::fogEnd in Original
    // Fidelity (LiT per-object D), else the scene's.
    auto fog = [&](const GpuMesh& m) {
        if (!s.fog || m.background) return glm::vec2(0.0f);
        return original && m.fogEnd > 0.0f ? glm::vec2(0.75f * m.fogEnd, m.fogEnd) : glm::vec2(s.fogStart, s.fogEnd);
    };
    // The background layer's far plane: a TB dome with Mesh::farClip (0x465710 flag 0x2000000) clips at 64000, any
    // other at the world's zFar.
    const glm::mat4 vpBack = projection(s.vfovRad(), float(vp.w) / float(vp.h), s.zNear, std::max(s.zFar, 64000.0f)) * view;
    glUniform3fv(uFogColor_, 1, s.fogColor);
    glUniform1f(uColorScale_, s.colorScale(colorScale_));
    glUniform1f(uTexGamma_, s.texGamma(texGamma_));
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, original ? (linearOriginal_ ? sampLinear_ : sampNearest_) : sampTrilinear_);
    uint32_t boundVao = 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // Front faces are CCW in engine space (scene.h); decoders emit that order. Double-sided primitives
    // (LiT flag bit 1) come as twoSided meshes.
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);

    auto pcSemi = [&](const GpuMesh& m) {
        return original && linearOriginal_ && (m.blend == Blend::Alpha || m.blend == Blend::Additive) &&
               (m.texture != white_ || m.textureAnim >= 0);
    };
    // tri / tris: a run of the mesh's triangles (bugs.exe sorted pass); ref: alpha-test reference of a blended mesh.
    auto draw = [&](const GpuMesh& m, uint32_t tri = 0, uint32_t tris = UINT32_MAX, float ref = -1.0f) {
        if (m.vao != boundVao) glBindVertexArray(boundVao = m.vao);
        if (m.background) glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, glm::value_ptr(m.farClip > 0.0f ? vpBack : vp3));
        const glm::vec2 f = fog(m);
        glUniform2f(uFog_, f.x, f.y);
        if (m.twoSided) glDisable(GL_CULL_FACE);
        else glEnable(GL_CULL_FACE);
        if (m.textureAnim >= 0) {
            const GpuTexAnim& a = texAnims_[size_t(m.textureAnim)];
            const int32_t lt = size_t(m.textureAnim) < linkTex_.size() ? linkTex_[size_t(m.textureAnim)] : -1;
            glBindTexture(GL_TEXTURE_2D, lt < 0 ? a.frames[(a.start + texTick_ / a.hold) % a.frames.size()]
                                         : size_t(lt) < sceneTextures_.size() && sceneTextures_[size_t(lt)] ? sceneTextures_[size_t(lt)] : white_);
        } else {
            glBindTexture(GL_TEXTURE_2D, m.texture);
        }
        glUniform1i(uUvClamp_, !original && m.blend != Blend::Opaque && m.blend != Blend::AlphaTest && m.unitUv);
        glUniform1f(uAlphaRef_, m.blend == Blend::AlphaTest ? 0.5f : ref);
        // Original Fidelity draws ABR 2 the way the PC exe's GL path does (luma alpha, SRC_ALPHA / 1-SRC_ALPHA), and
        // LiT's textured ABR 0/1/3 as bugs.exe's per-mode texel alpha (0x420d10), also alpha-blended.
        glUniform1i(uAdditive_, pcSemi(m) ? 3 : original && m.blend == Blend::Subtract ? 2 : m.blend == Blend::Additive || m.blend == Blend::Subtract);
        glDrawElementsBaseVertex(GL_TRIANGLES, GLsizei(std::min(m.indexCount - 3 * tri, 3 * tris)), GL_UNSIGNED_INT,
                                 reinterpret_cast<void*>(size_t(m.firstIndex + 3 * tri) * sizeof(uint32_t)), m.baseVertex);
        ++drawCalls_;
    };

    // Per layer (Mesh::background: TB's OT layer 0, then the depth clear, then the world, 0x422eb0):
    // pass 1 opaque, pass 2 alpha-tested (both depth-writing, unsorted), pass 3 translucent (alpha, additive,
    // subtractive) per polygon back to front by mean eye z, the order bugs.exe draws its blended polygons in
    // (0x413440, pc_order.h), without depth writes outside bugs.exe's own blended pass (LiT Original Fidelity).
    const bool pcOrder = original && linearOriginal_;
    auto blended = [&](const GpuMesh* m, uint32_t tri = 0, uint32_t tris = UINT32_MAX, float ref = -1.0f) {
        // Destination alpha is kept at 1 so the window never turns see-through under a compositor.
        const bool pcSub = original && m->blend == Blend::Subtract;
        if (m->blend == Blend::Alpha || pcSub || pcSemi(*m)) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        glBlendEquationSeparate(m->blend == Blend::Subtract && !pcSub ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
        draw(*m, tri, tris, ref);
    };
    for (const bool bg : {true, false}) {
        if (!bg) glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, glm::value_ptr(vp3));
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        for (Blend pass : {Blend::Opaque, Blend::AlphaTest})
            for (const std::vector<GpuMesh>* list : {&meshes_, &actorMeshes_})
                for (const GpuMesh& m : *list)
                    if (m.blend == pass && m.background == bg && shown(m)) draw(m);
        std::vector<const GpuMesh*> translucent;
        for (const std::vector<GpuMesh>* list : {&meshes_, &actorMeshes_})
            for (const GpuMesh& m : *list)
                if (m.blend != Blend::Opaque && m.blend != Blend::AlphaTest && m.background == bg && shown(m))
                    translucent.push_back(&m);
        glEnable(GL_BLEND);
        glDepthMask(pcOrder ? GL_TRUE : GL_FALSE);
        // bugs.exe (LiT Original Fidelity, pc_order.h): untextured blended faces first, then layer 1 sorted per
        // triangle, then layer 2 (Mesh::late); all depth-writing, alpha test GREATER 0 except the sorted pass.
        constexpr float kGreater0 = 0.5f / 255.0f;
        auto apart = [&](const GpuMesh& m) { return pcOrder && ((m.texture == white_ && m.textureAnim < 0) || m.late); };
        for (const GpuMesh* m : translucent)
            if (pcOrder && m->texture == white_ && m->textureAnim < 0 && !m->late) blended(m, 0, UINT32_MAX, kGreater0);
        std::vector<PcTri> tris;
        const glm::vec4 zRow(view[0][2], view[1][2], view[2][2], view[3][2]);  // eye-space z of a world point
        for (uint32_t i = 0; i < translucent.size(); ++i) {
            const GpuMesh& m = *translucent[i];
            if (apart(m)) continue;
            for (uint32_t t = 0; t < m.tris.size(); ++t) tris.push_back({glm::dot(zRow, glm::vec4(m.tris[t], 1.0f)), i, t});
        }
        drawPcSorted(tris, [&](uint32_t i, uint32_t t, uint32_t n) { blended(translucent[i], t, n); });
        for (const GpuMesh* m : translucent)
            if (pcOrder && m->late) blended(m, 0, UINT32_MAX, kGreater0);
        if (bg) {
            glDepthMask(GL_TRUE);
            glClear(GL_DEPTH_BUFFER_BIT);  // scissored to the viewport
        }
    }
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_CULL_FACE);

    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glViewport(0, 0, w_, h_);
}

void Renderer::quad2D(uint32_t tex, uint32_t sampler, float x, float y, float w, float h,
                      float u0, float v0, float u1, float v1, const uint8_t rgba[4]) {
    if (tex != batchTex_ || sampler != batchSampler_) {
        flush2D();
        batchTex_ = tex;
        batchSampler_ = sampler;
    }
    static constexpr uint8_t kWhite[4] = {255, 255, 255, 255};
    if (!rgba) rgba = kWhite;
    auto v = [&](float px, float py, float u, float vv) {
        return V2{px, py, u, vv, {rgba[0], rgba[1], rgba[2], rgba[3]}};
    };
    batch_.insert(batch_.end(), {v(x, y, u0, v0), v(x + w, y, u1, v0), v(x + w, y + h, u1, v1),
                                 v(x, y, u0, v0), v(x + w, y + h, u1, v1), v(x, y + h, u0, v1)});
}

void Renderer::drawImage(uint32_t image, float x, float y, float w, float h, bool smooth, const uint8_t rgba[4]) {
    if (image) quad2D(image, smooth ? sampUiLinear_ : sampUiNearest_, x, y, w, h, 0, 0, 1, 1, rgba);
}

void Renderer::drawRect(float x, float y, float w, float h, const uint8_t rgba[4]) {
    quad2D(white_, sampUiNearest_, x, y, w, h, 0, 0, 1, 1, rgba);
}

void Renderer::multiplyRect(float x, float y, float w, float h, const uint8_t rgb[4]) {
    flush2D();
    drawRect(x, y, w, h, rgb);
    flush2D(true);
}

void Renderer::drawText(float x, float y, int scale, std::string_view text, const uint8_t rgba[4]) {
    const float g = float(kGlyph * scale);
    float cx = x;
    for (char ch : text) {
        if (ch == '\n') {
            cx = x;
            y += g + float(scale) * 2.0f;
            continue;
        }
        int c = static_cast<unsigned char>(ch);
        if (c < 32 || c > 127) c = '?';
        if (c != ' ') {
            const float u = float((c - 32) % 16) / 16.0f, v = float((c - 32) / 16) / 6.0f;
            quad2D(font_, sampUiNearest_, cx, y, g, g, u, v, u + 1.0f / 16.0f, v + 1.0f / 6.0f, rgba);
        }
        cx += g;
    }
}

void Renderer::flush2D(bool multiply) {
    if (batch_.empty()) return;
    glUseProgram(prog2D_);
    glUniform2f(uScreen_, float(std::max(w_, 1)), float(std::max(h_, 1)));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, batchTex_);
    glBindSampler(0, batchSampler_);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    if (multiply) glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    else glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glBindVertexArray(vao2D_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2D_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batch_.size() * sizeof(V2)), batch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch_.size()));
    ++drawCalls_;
    batch_.clear();
}

bool Renderer::screenshot(const std::filesystem::path& path) {
    flush2D();
    std::vector<uint8_t> px(size_t(w_) * h_ * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w_, h_, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    std::vector<uint8_t> flipped(px.size());
    const size_t row = size_t(w_) * 3;
    for (int y = 0; y < h_; ++y)
        std::copy_n(px.data() + size_t(h_ - 1 - y) * row, row, flipped.data() + size_t(y) * row);

    if (path.extension() == ".png") {
        SDL_Surface* s = SDL_CreateSurfaceFrom(w_, h_, SDL_PIXELFORMAT_RGB24, flipped.data(), int(row));
        const bool ok = s && SDL_SavePNG(s, path.string().c_str());
        if (!ok) std::println(stderr, "screenshot: {}", SDL_GetError());
        SDL_DestroySurface(s);
        return ok;
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w_ << ' ' << h_ << "\n255\n";
    out.write(reinterpret_cast<const char*>(flipped.data()), std::streamsize(flipped.size()));
    return bool(out);
}

void Renderer::present() {
    flush2D();
    SDL_GL_SwapWindow(window_);
}

} // namespace bb::render
