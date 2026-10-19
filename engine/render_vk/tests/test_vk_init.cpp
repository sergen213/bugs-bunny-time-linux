// Vulkan backend smoke: device init, scene + actor + 2D drawing, culling convention, MSAA on/off, resize,
// screenshot readback, zero validation messages. Exit 77 when there is no display or no Vulkan 1.3 device.
#include "bb/render_vk/renderer.h"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

        const auto shot = std::filesystem::temp_directory_path() / "bbtime_vk_init.ppm";
        const uint8_t blue[4] = {0, 0, 255, 255};
        bb::render::Settings s;
        s.fog = false;
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
        std::filesystem::remove(shot);
        if (r.validationMessages()) {
            std::println("FAIL: {} validation messages", r.validationMessages());
            rc = 1;
        }
    }
    SDL_Quit();
    return rc;
}
