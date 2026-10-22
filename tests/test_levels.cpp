// Real-data level decode: every LiT BZE builds a Scene (TB: test_tb_levels.cpp).
// Needs BBTIME_DATA=<dir containing lit/ and tb/>; exits 77 (ctest SKIP) otherwise.
#include "bb/level.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <random>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++failures;                                                        \
            std::println(stderr, "FAIL {}:{}: {}", __FILE__, __LINE__, #cond); \
            std::println(stderr, __VA_ARGS__);                                 \
        }                                                                      \
    } while (0)

static std::vector<fs::path> bzes(const fs::path& dir) {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(dir)) {
        std::string ext = e.path().extension().string();
        std::ranges::transform(ext, ext.begin(), ::tolower);
        if (ext == ".bze") v.push_back(e.path());
    }
    std::ranges::sort(v);
    return v;
}

int main() {
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) return 77;
    const fs::path root = env;

    // LiT: 167 files; 79 have world geometry (MODELS.md §9).
    uint64_t tris = 0, faces = 0, missingFaces = 0, worldMissing = 0, linked = 0;
    size_t files = 0, withWorld = 0, textures = 0, texErrors = 0, props = 0, spawns = 0, meshes = 0;
    for (const fs::path& p : bzes(root / "lit/Datas/bze")) {
        ++files;
        try {
            const bb::Scene s = bb::buildLitScene(bb::loadBze(p), p.stem().string());
            CHECK(!s.placeholder, "{}", p.string());
            withWorld += s.stats.worldModels > 0;
            tris += s.stats.triangles, faces += s.stats.faces, missingFaces += s.stats.missingTextureFaces;
            worldMissing += s.stats.worldMissingTextureFaces, linked += s.stats.linkedFaces;
            for (const bb::TextureAnim& a : s.textureAnims) {
                CHECK(!a.frames.empty() && a.startFrame < a.frames.size() && a.holdTicks > 0, "{} link {}", p.string(), a.linkId);
                for (int32_t f : a.frames) CHECK(f >= 0 && size_t(f) < s.textures.size(), "{} link {}", p.string(), a.linkId);
            }
            if (p.stem() == "L04C3") CHECK(s.stats.linkedFaces >= 176, "L04C3 conveyor links: {}", s.stats.linkedFaces);
            // Link timing observed in the original (WineRef alias-table traces, GAMEPLAY_STATUS.md): L04C3 type-20
            // conveyors 9 / 11 advance every 3 / 2 ticks; L02A1 type-2 shimmer 16 = flipbook list 5 x 2 ticks + 30.
            auto link = [&](uint32_t id) {
                const auto a = std::ranges::find_if(s.textureAnims, [&](const bb::TextureAnim& x) { return x.linkId == id; });
                return a == s.textureAnims.end() ? nullptr : &*a;
            };
            if (p.stem() == "L04C3") CHECK(link(9) && link(9)->holdTicks == 3 && link(11) && link(11)->holdTicks == 2, "L04C3 holds");
            if (p.stem() == "L02A1")
                CHECK(link(16) && link(16)->frames.size() == 40 && link(16)->holdTicks == 1 && !link(16)->timingInferred &&
                          link(16)->frames[9] != link(16)->frames[10] && link(16)->frames[10] == link(16)->frames[39],
                      "L02A1 link 16 flipbook");
            // Sprite frames bind the base GL texture converted for its first used mode (0x423eb0; RUNTIME_REFERENCE §27):
            // LS01 = oracle 0x52fd60 records (all 253 frames): asset 321 (ids 381-388) mode 1, 25 (158) mode 0. L02A5's
            // asset 80 has ABR-0 records on a texture its faces use opaque first: drawn opaque on the PC path.
            auto frame0 = [&](uint16_t aid) { return s.sprites.contains(aid) ? s.sprites.at(aid).front() : bb::SpriteFrame{}; };
            if (p.stem() == "LS01") {
                CHECK(frame0(321).pcBlend == bb::Blend::Additive && frame0(321).pcAlpha == 255, "LS01 sprite 321");
                CHECK(frame0(25).pcBlend == bb::Blend::Alpha && frame0(25).pcAlpha == 128, "LS01 sprite 25");
            }
            if (p.stem() == "L02A5")
                CHECK(frame0(80).blend == bb::Blend::Alpha && frame0(80).pcBlend == bb::Blend::AlphaTest, "L02A5 sprite 80");
            textures += s.textures.size(), texErrors += s.stats.textureErrors, props += s.stats.props;
            spawns += bool(s.spawn);
            meshes += s.meshes.size();
            uint64_t idx = 0;
            for (const bb::Mesh& m : s.meshes) {
                idx += m.indices.size();
                CHECK(m.texture < int32_t(s.textures.size()), "{} {}", p.string(), m.name);
                CHECK(m.textureAnim < int32_t(s.textureAnims.size()), "{} {}", p.string(), m.name);
                for (uint32_t i : m.indices) CHECK(i < m.vertices.size(), "{} {}", p.string(), m.name);
                for (const bb::Vertex& v : m.vertices)
                    CHECK(std::isfinite(v.pos[0]) && std::isfinite(v.pos[1]) && std::isfinite(v.pos[2]), "{}", m.name);
            }
            CHECK(idx == s.stats.triangles * 3, "{}: index count", p.string());
            for (const bb::Texture& t : s.textures) CHECK(t.rgba.size() == size_t(t.width) * t.height * 4, "{}", t.name);
            if (s.stats.missingTextureFaces || s.stats.textureErrors)
                std::println("  {}: {} missing-texture faces, ids {}, {} texture errors", p.stem().string(),
                             s.stats.missingTextureFaces, s.stats.missingTextureIds, s.stats.textureErrors);
        } catch (const std::exception& e) {
            CHECK(false, "{}: {}", p.string(), e.what());
        }
    }
    std::println("LiT: {} files, {} with world geometry, {} faces, {} triangles, {} meshes, {} textures "
                 "({} undecodable), {} undrawn alias-0 faces ({} world), {} linked faces, {} props, {} player starts",
                 files, withWorld, faces, tris, meshes, textures, texErrors, missingFaces, worldMissing, linked, props, spawns);
    CHECK(files == 167 && withWorld == 79, "files {} world {}", files, withWorld);
    CHECK(texErrors == 2, "only the 24-bit TIM 217 in L03B and L03b_8 is undecodable: {}", texErrors);
    CHECK(faces >= 168963, "world faces from MODELS.md §9 (168963) plus props: {}", faces);
    CHECK(worldMissing == 0, "every world face id is a TIM or a texture link (TEXTURE_GAP.md §3): {}", worldMissing);

    // Corrupt input must throw, never crash (run under ASan/UBSan): flip bytes in L01a's section data.
    const std::vector<uint8_t> good = bb::readFile(root / "lit/Datas/bze/L01a.bze");
    std::mt19937 rng(1234);
    size_t thrown = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<uint8_t> bad = good;
        for (int k = 0; k < 1 + i % 8; ++k)
            bad[bb::BzeFile::kHeaderSize + rng() % (bad.size() - bb::BzeFile::kHeaderSize)] ^= uint8_t(1 + rng() % 255);
        try {
            bb::buildLitScene(bb::parseBze(std::move(bad)), "fuzz");
        } catch (const std::exception&) {
            ++thrown;
        }
    }
    std::println("L01a corruption: 300 mutations, {} rejected with an exception, rest decoded", thrown);
    return failures ? 1 : 0;
}
