// TB level decode. Synthetic: section-1 walker rules (TB_LEVELS.md §2-3). Real data (BBTIME_DATA=<dir containing
// tb/>, else exit 77 = ctest SKIP): every TB BZE builds a Scene whose totals match `tools/re/tb_world.py --check`,
// and 300 corrupted copies of ARCA01 throw or decode without crashing (run under ASan/UBSan).
#include "bb/tb_level.h"

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

static bool throws(std::vector<uint8_t> d) {
    try {
        bb::parseTbScript(d);
    } catch (const bb::FormatError&) {
        return true;
    }
    return false;
}

static void synthetic() {
    std::vector<uint8_t> d(3 + 36);  // 2c 3a (3d <36 bytes pool sizes>)
    d[0] = 0x2c, d[1] = 0x3a, d[2] = 0x3d;
    d.insert(d.end(), {0x2d, 0x2c, 0x28, 0x29, 1, 0, 0, 0, 2, 0, 0, 0, 0x2d, 0x4a, 0x2c, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0,
                       0x2d, 0x2b, 0x2c, 0x20, 0x24, 0, 0, 0, 0, 0, 0, 0, 0, 0x62, 0x2d, 0x2e});
    const bb::TbScript s = bb::parseTbScript(d, 9);
    CHECK(s.chunks.size() == 4 && s.end == d.size() - 1, "{} chunks", s.chunks.size());
    CHECK(s.chunks[0].section == 3 && s.chunks[1].section == 3 && s.chunks[2].section == 9 && s.chunks[3].section == 4,
          "D switches on 0x4a / 0x2b");
    CHECK(s.chunks[3].tags.size() == 2 && s.chunks[3].tags[1].id == 0x62 && s.chunks[3].tags[1].data.empty(), "0x62 has no payload");
    CHECK(s.chunks[1].tag(0x29).size() == 8 && s.chunks[1].tag(0x29)[0] == 1 && s.chunks[1].tag(0x10).empty(), "tag lookup");

    CHECK(throws({0x2c, 0x20, 0x99, 0x2d, 0x2e}), "unknown tag");
    CHECK(throws({0x2c, 0x01, 0x2d, 0x2e}), "unknown chunk type");
    CHECK(throws({0x2f, 0x2e}), "unknown top-level byte");
    CHECK(throws({0x2b, 0x2e}), "0x2b before the pool sizes");
    CHECK(throws({0x2c, 0x28, 0x29, 0, 0, 0}), "truncated payload");
    CHECK(throws({0x2c, 0x28, 0x2d}), "missing end byte");
    CHECK(!throws({0x2c, 0x44, 0x03, 0x44, 0x05, 0x2d, 0x2e}), "pseudo-chunk");
}

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
    synthetic();
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) return failures ? 1 : 77;
    const fs::path dir = fs::path(env) / "tb/Datas/BZE";

    size_t files = 0, spawns = 0, textures = 0, meshes = 0;
    uint64_t world = 0, sky = 0, worldFaces = 0, invalid = 0, refs = 0, texErrors = 0, faces = 0, missing = 0, props = 0,
             skipped = 0;
    for (const fs::path& p : bzes(dir)) {
        ++files;
        try {
            const bb::BzeFile bze = bb::loadBze(p);
            const std::vector<uint8_t> s1 = bze.payload(size_t(bze.find(1)));
            CHECK(bb::parseTbScript(s1).end == s1.size() - 1, "{}: end byte is the last byte", p.string());
            const bb::Scene s = bb::buildTbScene(bze, p.stem().string());
            CHECK(!s.placeholder && s.game == bb::GameId::TimeBusters, "{}", p.string());
            spawns += bool(s.spawn);
            world += s.stats.worldModels, sky += s.stats.skyDomes, worldFaces += s.stats.worldFaces;
            invalid += s.stats.invalidPrims, refs += s.stats.textureRefs, texErrors += s.stats.textureErrors;
            faces += s.stats.faces, missing += s.stats.missingTextureFaces, props += s.stats.props;
            skipped += s.stats.skippedActors, textures += s.textures.size(), meshes += s.meshes.size();
            uint64_t idx = 0;
            for (const bb::Mesh& m : s.meshes) {
                idx += m.indices.size();
                CHECK(m.texture < int32_t(s.textures.size()), "{} {}", p.string(), m.name);
                for (uint32_t i : m.indices) CHECK(i < m.vertices.size(), "{} {}", p.string(), m.name);
                for (const bb::Vertex& v : m.vertices)
                    CHECK(std::isfinite(v.pos[0]) && std::isfinite(v.pos[1]) && std::isfinite(v.pos[2]), "{}", m.name);
            }
            CHECK(idx == s.stats.triangles * 3, "{}: index count", p.string());
            if (p.stem() == "ARCA01")  // Bugs start (-4107, -27, -7555) PS1 -> engine (x, -y, -z), TB_LEVELS.md §5
                CHECK(s.spawn && (*s.spawn)[0] == -4107.0f && (*s.spawn)[1] == 27.0f && (*s.spawn)[2] == 7555.0f, "ARCA01 spawn");
        } catch (const std::exception& e) {
            CHECK(false, "{}: {}", p.string(), e.what());
        }
    }
    std::println("TB: {} files, {} world models, {} sky domes, {} octree faces, {} invalid prims, {} faces total, "
                 "{} meshes, {} texture refs ({} errors, {} unique), {} missing-texture faces, {} props, {} skinned actors "
                 "skipped, {} player starts",
                 files, world, sky, worldFaces, invalid, faces, meshes, refs, texErrors, textures, missing, props, skipped, spawns);
    // tb_world.py --check: world models 123, sky domes 59, world faces 115250 + sky faces 23043, invalid prims 48.
    CHECK(files == 58 && spawns == 58, "files {} spawns {}", files, spawns);
    CHECK(world == 123 && sky == 59, "world {} sky {}", world, sky);
    CHECK(worldFaces == 115250 + 23043 && invalid == 48, "octree faces {} invalid {}", worldFaces, invalid);
    CHECK(refs == 17103 && texErrors == 0, "texture refs {} errors {}", refs, texErrors);

    const std::vector<uint8_t> good = bb::readFile(dir / "ARCA01.bze");
    std::mt19937 rng(4321);
    size_t thrown = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<uint8_t> bad = good;
        for (int k = 0; k < 1 + i % 8; ++k)
            bad[bb::BzeFile::kHeaderSize + rng() % (bad.size() - bb::BzeFile::kHeaderSize)] ^= uint8_t(1 + rng() % 255);
        try {
            bb::buildTbScene(bb::parseBze(std::move(bad)), "fuzz");
        } catch (const std::exception&) {
            ++thrown;
        }
    }
    std::println("ARCA01 corruption: 300 mutations, {} rejected with an exception, rest decoded", thrown);
    return failures ? 1 : 0;
}
