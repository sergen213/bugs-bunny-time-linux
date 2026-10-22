// Vulkan backend smoke: device init, scene + actor + 2D drawing, culling convention, MSAA on/off, resize,
// screenshot readback, zero validation messages. Exit 77 when there is no display or no Vulkan 1.3 device.
#include "bb/render_vk/renderer.h"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <glm/glm.hpp>
#include <memory>
#include <print>
#include <string>

namespace {

bb::Mesh quad(float x0, float y0, float x1, float y1, float z, int tex, std::array<uint8_t, 4> c) {
    bb::Mesh m;
    m.texture = tex;
    for (auto [x, y, u, v] : {std::array{x0, y0, 0.f, 1.f}, {x1, y0, 1.f, 1.f}, {x1, y1, 1.f, 0.f}, {x0, y1, 0.f, 0.f}})
        m.vertices.push_back({{x, y, z}, {u, v}, {c[0], c[1], c[2], c[3]}});
    m.indices = {0, 1, 2, 0, 2, 3}; // counter-clockwise seen from +Z (the camera): front face, culled if wrong
    return m;
}

std::array<int, 3> pixel(const std::filesystem::path& ppm, int x, int y) {
    std::ifstream in(ppm, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, max = 0;
    in >> magic >> w >> h >> max;
    in.get();
    in.seekg(std::streamoff(size_t(y) * w * 3 + size_t(x) * 3), std::ios::cur);
    uint8_t px[3] = {};
    in.read(reinterpret_cast<char*>(px), 3);
    return {px[0], px[1], px[2]};
}

bool near(std::array<int, 3> a, std::array<int, 3> b) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(a[i] - b[i]) > 8) return false;
    return true;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep the measurements when an assert aborts
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println("skip: SDL_Init: {}", SDL_GetError());
        return 77;
    }
    int rc = 0;
    {
        auto rp = std::make_unique<bb::render_vk::Renderer>();
        const bb::render::WindowConfig wc{.width = 320, .height = 240, .hidden = true};
        std::string err = rp->init(wc, true);
        if (!err.empty()) {
            std::println("validation layer unavailable ({}), retrying without", err);
            rp = std::make_unique<bb::render_vk::Renderer>();
            err = rp->init(wc, false);
        }
        if (!err.empty()) {
            std::println("skip: {}", err);
            rp.reset();
            SDL_Quit();
            return 77;
        }
        bb::render_vk::Renderer& r = *rp;
        std::println("{} | {} | {} | {} | MSAA x{} aniso {}", r.glVendor(), r.glRenderer(), r.glVersion(), r.diagnostics(),
                     r.msaaSamples(), r.maxAnisotropy());
        r.setVsync(false);

        bb::Scene scene;
        scene.textures.push_back({.name = "red", .width = 2, .height = 2, .rgba = std::vector<uint8_t>(16, 255)});
        for (size_t i = 0; i < 4; ++i) scene.textures[0].rgba[i * 4 + 1] = scene.textures[0].rgba[i * 4 + 2] = 0;
        scene.meshes.push_back(quad(-1, -1, 1, 1, -5, 0, {255, 255, 255, 255}));
        r.uploadScene(scene);
        const bb::Mesh actor = quad(-2.0f, -0.25f, -1.5f, 0.25f, -4, -1, {0, 255, 0, 255});
        r.setActorMeshes({&actor, 1});

        // Per process: ctest runs render_vk.init and render_vk.init_no_rt at the same time.
        const auto shot = std::filesystem::temp_directory_path() / std::format("bbtime_vk_init_{}.ppm", SDL_GetCurrentThreadID());
        const uint8_t blue[4] = {0, 0, 255, 255};
        bb::render::Settings s;
        s.fog = false;
        s.shadows = bb::render::Shadows::Off;
        for (int pass = 0; pass < 4; ++pass) { // MSAA off/on, then the same after a resize
            if (pass == 2) {
                SDL_SetWindowSize(r.window(), 400, 300);
                SDL_SyncWindow(r.window());
            }
            s.msaa = pass % 2;
            for (int f = 0; f < 3; ++f) {
                r.beginFrame(s);
                r.drawScene({}, s);
                r.drawRect(0, 0, 20, 20, blue);
                r.drawText(30, 2, 1, "VK", blue);
                if (f == 2) assert(r.screenshot(shot));
                r.present();
            }
            const int w = r.width(), h = r.height();
            const auto c = pixel(shot, w / 2, h / 2), a = pixel(shot, int(w * 69 / 320.0f), h / 2), q = pixel(shot, 5, 5);
            std::println("pass {} {}x{}: centre {} {} {} actor {} {} {} rect {} {} {} draws {}", pass, w, h, c[0], c[1], c[2], a[0], a[1],
                         a[2], q[0], q[1], q[2], r.drawCalls());
            assert(near(c, {255, 0, 0}));
            assert(near(a, {0, 255, 0}));
            assert(near(q, {0, 0, 255}));
            assert(r.drawCalls() == 3); // scene quad + actor quad + one 2D batch
        }

        // Enhanced sun shadows: a 200x200 roof at eye height over a grey floor 200 below. With the first camera's yaw 0
        // the sun (renderer.cpp drawScene) is toward (0.32, 0.82, 0.47): the shadow shifts (-0.40, -0.58) x/z per unit of
        // height, to x -179..21, z -1016..-816. It must darken the floor there in every mode (ray tracing falls back to
        // the shadow map without ray queries), and not beside it.
        bb::Scene lit;
        auto flat = [](float x0, float x1, float y, float z0, float z1, std::array<uint8_t, 4> c) {
            bb::Mesh m;
            m.twoSided = true;
            for (auto [x, z] : {std::array{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}})
                m.vertices.push_back({{x, y, z}, {0.f, 0.f}, {c[0], c[1], c[2], c[3]}});
            m.indices = {0, 1, 2, 0, 2, 3};
            return m;
        };
        lit.meshes.push_back(flat(-2000, 2000, -200, -1900, 0, {128, 128, 128, 255}));
        lit.meshes.push_back(flat(-100, 100, 0, -900, -700, {128, 128, 128, 255}));
        r.uploadScene(lit);
        r.setActorMeshes({});
        auto project = [&](glm::vec3 p) {
            const float aspect = float(r.width()) / float(r.height());
            const glm::vec4 c = bb::render::projection(s.vfovRad(), aspect, s.zNear, s.zFar) * bb::render::Camera{}.view() * glm::vec4(p, 1.0f);
            return std::array{int((c.x / c.w * 0.5f + 0.5f) * float(r.width())), int((0.5f - c.y / c.w * 0.5f) * float(r.height()))};
        };
        // Positions are projected after each capture: a compositor may still apply the earlier resize (Wayland
        // configure) while the first frames of a mode render.
        const glm::vec3 inShadowW{-80, -200, -900}, outsideW{300, -200, -900};
        for (auto mode : {bb::render::Shadows::Map, bb::render::Shadows::Rt, bb::render::Shadows::RtAo}) {
            s.shadows = mode;
            for (int f = 0; f < 3; ++f) {
                r.beginFrame(s);
                r.drawScene({}, s);
                if (f == 2) assert(r.screenshot(shot));
                r.present();
            }
            const auto inShadow = project(inShadowW), outside = project(outsideW);
            const auto a = pixel(shot, inShadow[0], inShadow[1]), b = pixel(shot, outside[0], outside[1]);
            std::println("shadows {} ({}): shadowed {} lit {}", int(mode), r.diagnostics(), a[0], b[0]);
            assert(b[0] > 100 && a[0] < b[0] * 3 / 4);
        }

        // FSR 3.1.4 (upscaling, then also frame generation) on the same scene: the shadow survives the upscale and the
        // frame interpolated before each rendered frame is presented once FSR has a history.
        if (r.fsrSupported()) {
            s.shadows = bb::render::Shadows::Map;
            for (bool fg : {false, true}) {
                s.upscaling = bb::render::Upscaling::Quality;
                s.frameGeneration = fg;
                for (int f = 0; f < 4; ++f) {
                    r.beginFrame(s);
                    r.drawScene({}, s);
                    r.drawRect(0, 0, 20, 20, blue);
                    if (f == 3) assert(r.screenshot(shot));
                    r.present();
                    if (f == 3) assert(r.frameGenerated() == fg);
                }
                const auto inShadow = project(inShadowW), outside = project(outsideW);
                const auto a = pixel(shot, inShadow[0], inShadow[1]), b = pixel(shot, outside[0], outside[1]), q = pixel(shot, 5, 5);
                std::println("FSR quality{}: shadowed {} lit {} rect {} {} {} | {}", fg ? " + frame generation" : "", a[0], b[0], q[0], q[1], q[2],
                             r.diagnostics());
                assert(b[0] > 100 && a[0] < b[0] * 3 / 4 && near(q, {0, 0, 255}));
            }
            s.upscaling = bb::render::Upscaling::Off;
            s.frameGeneration = false;
        } else {
            std::println("FSR unsupported: {}", r.diagnostics());
        }

        // FSR reactive mask: an additive glow (no motion vectors: translucent) sliding over a static grey wall must not
        // leave a trail where it was four frames ago.
        if (r.fsrSupported()) {
            bb::Scene wall;
            wall.meshes.push_back(quad(-20, -20, 20, 20, -10, -1, {100, 100, 100, 255}));
            r.uploadScene(wall);
            s.shadows = bb::render::Shadows::Off;
            s.upscaling = bb::render::Upscaling::Quality;
            int trail = 0;
            for (int f = 0; f < 24; ++f) {
                const float x = -3.0f + 0.25f * float(f); // 0.25 units / frame at z -5: ~10 pixels at 400 wide
                bb::Mesh glow = quad(x, -0.5f, x + 0.5f, 0.5f, -5, -1, {220, 220, 220, 255});
                glow.blend = bb::Blend::Additive;
                r.setActorMeshes({&glow, 1});
                r.beginFrame(s);
                r.drawScene({}, s);
                if (f == 23) assert(r.screenshot(shot));
                r.present();
            }
            // The glow's left edge four frames ago, now behind it: wall only.
            const auto p = project({-3.0f + 0.25f * 19.0f + 0.1f, 0.0f, -5.0f}), bg = project({0.0f, 2.0f, -5.0f});
            const auto c = pixel(shot, p[0], p[1]), b = pixel(shot, bg[0], bg[1]);
            trail = c[0] - b[0];
            std::println("FSR reactive mask: trail behind a moving additive glow {} (wall {})", trail, b[0]);
            assert(trail < 20);
            s.upscaling = bb::render::Upscaling::Off;
        }

        // Ray-traced reflections: a blue translucent water plane (classified as water: renderer.cpp
        // reflectiveMaterials) in front of a red wall. The water just before the wall mirrors it (reddens) only with
        // reflections on; without ray queries the setting does nothing.
        bb::Scene pond;
        pond.textures.push_back({.name = "blue", .width = 2, .height = 2, .rgba = std::vector<uint8_t>(16, 0)});
        for (size_t i = 0; i < 4; ++i) pond.textures[0].rgba[i * 4 + 2] = pond.textures[0].rgba[i * 4 + 3] = 255;
        pond.meshes.push_back(flat(-3000, 3000, -200, -4000, 0, {255, 255, 255, 160}));
        pond.meshes.back().texture = 0;
        pond.meshes.back().blend = bb::Blend::Alpha;
        bb::Mesh wall = quad(-1500, -200, 1500, 800, -2000, -1, {255, 0, 0, 255});
        wall.twoSided = true;
        pond.meshes.push_back(wall);
        r.uploadScene(pond);
        r.setActorMeshes({});
        s.shadows = bb::render::Shadows::Off;
        int red[2] = {};
        for (bool on : {false, true}) {
            s.reflections = on;
            for (int f = 0; f < 3; ++f) {
                r.beginFrame(s);
                r.drawScene({}, s);
                if (f == 2) assert(r.screenshot(shot));
                r.present();
            }
            const auto onWater = project({0, -200, -1000});
            const auto c = pixel(shot, onWater[0], onWater[1]);
            red[on] = c[0];
            std::println("reflections {}: water {} {} {}", on ? "on" : "off", c[0], c[1], c[2]);
        }
        assert(r.rayTracing() ? red[1] > red[0] + 20 : red[1] == red[0]);
        std::filesystem::remove(shot);
        std::filesystem::remove(std::filesystem::path(shot).replace_filename(shot.stem().string() + "-generated.ppm")); // BBTIME_VK_SHOT_GENERATED
        if (r.validationMessages()) {
            std::println("FAIL: {} validation messages", r.validationMessages());
            rc = 1;
        }
    }
    SDL_Quit();
    return rc;
}
