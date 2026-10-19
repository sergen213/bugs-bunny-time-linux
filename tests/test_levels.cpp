// Real-data level decode: every LiT BZE builds a Scene; every TB BZE decodes its textures.
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
    uint64_t tris = 0, faces = 0, missingFaces = 0;
    size_t files = 0, withWorld = 0, textures = 0, texErrors = 0, props = 0, spawns = 0, meshes = 0;
    for (const fs::path& p : bzes(root / "lit/Datas/bze")) {
        ++files;
        try {
            const bb::Scene s = bb::buildLitScene(bb::loadBze(p), p.stem().string());
            CHECK(!s.placeholder, "{}", p.string());
            withWorld += s.stats.worldModels > 0;
            tris += s.stats.triangles, faces += s.stats.faces, missingFaces += s.stats.missingTextureFaces;
            textures += s.textures.size(), texErrors += s.stats.textureErrors, props += s.stats.props;
            spawns += bool(s.spawn);
            meshes += s.meshes.size();
            uint64_t idx = 0;
            for (const bb::Mesh& m : s.meshes) {
                idx += m.indices.size();
                CHECK(m.texture < int32_t(s.textures.size()), "{} {}", p.string(), m.name);
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
                 "({} undecodable), {} missing-texture faces, {} props, {} player starts",
                 files, withWorld, faces, tris, meshes, textures, texErrors, missingFaces, props, spawns);
    CHECK(files == 167 && withWorld == 79, "files {} world {}", files, withWorld);
    CHECK(texErrors == 2, "only the 24-bit TIM 217 in L03B and L03b_8 is undecodable: {}", texErrors);
    CHECK(faces >= 168963, "world faces from MODELS.md §9 (168963) plus props: {}", faces);

    // TB: textures only (geometry format differs, MODELS.md §10). 58 files, 17103 TIM references.
    size_t tbFiles = 0, tbTex = 0, tbRefs = 0, tbErr = 0;
    for (const fs::path& p : bzes(root / "tb/Datas/BZE")) {
        ++tbFiles;
        try {
            bb::SceneStats st;
            tbTex += bb::decodeTbTextures(bb::loadBze(p), p.stem().string(), &st).size();
            tbRefs += st.textureRefs, tbErr += st.textureErrors;
        } catch (const std::exception& e) {
            CHECK(false, "{}: {}", p.string(), e.what());
        }
    }
    std::println("TB: {} files, {} texture refs, {} decoded, {} errors", tbFiles, tbRefs, tbTex, tbErr);
    CHECK(tbFiles == 58 && tbRefs == 17103 && tbTex == tbRefs && tbErr == 0, "TB textures");

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
