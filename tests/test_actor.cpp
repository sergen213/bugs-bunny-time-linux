// LiT TOD actor animation (bb/actor.h): synthetic decode/timing/posing checks, then (with BBTIME_DATA) every LiT
// level's actor models and animations play to the end and pose on every tick, and one pose is compared with
// tools/re/anim.py (needs python3 + Pillow; skipped otherwise). Exit 77 only if BBTIME_DATA is unset.
#include "bb/actor.h"
#include "bb/level.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <sstream>

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

// --- synthetic TOD writer
struct TodWriter {
    std::vector<uint8_t> b;
    std::vector<std::vector<uint32_t>> frames;
    void w32(std::vector<uint8_t>& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i))); }
    void packet(uint16_t id, uint8_t type, uint8_t flag, std::vector<uint32_t> payload) {
        frames.back().push_back(id | uint32_t(type) << 16 | uint32_t(flag) << 20 | uint32_t(payload.size() + 1) << 24);
        frames.back().insert(frames.back().end(), payload.begin(), payload.end());
    }
    static uint32_t s16x2(int a, int b) { return uint32_t(uint16_t(a)) | uint32_t(uint16_t(b)) << 16; }
    std::vector<uint8_t> build(uint16_t res) {
        std::vector<uint8_t> o{0x50, 0, uint8_t(res), uint8_t(res >> 8)};
        w32(o, uint32_t(frames.size()));
        for (size_t f = 0; f < frames.size(); ++f) {
            size_t npk = 0;
            for (size_t i = 0; i < frames[f].size(); i += (frames[f][i] >> 24)) ++npk;
            w32(o, uint32_t(frames[f].size() + 2) | uint32_t(npk) << 16);
            w32(o, uint32_t(f));
            for (uint32_t v : frames[f]) w32(o, v);
        }
        return o;
    }
};

static bool near(const std::array<float, 3>& a, std::array<float, 3> b, float eps = 1e-3f) {
    return std::abs(a[0] - b[0]) < eps && std::abs(a[1] - b[1]) < eps && std::abs(a[2] - b[2]) < eps;
}

static void synthetic() {
    TodWriter sk;  // skeleton: parts 1, 2; objects 1, 2; part 2 under part 1
    sk.frames.emplace_back();
    sk.packet(1, 8, 0, {});
    sk.packet(2, 8, 0, {});
    sk.packet(1, 2, 0, {1});
    sk.packet(2, 2, 0, {2});
    sk.packet(2, 3, 0, {1});
    TodWriter an;  // res 4: two ticks per frame; 3 frames, the third never shown
    an.frames.emplace_back();
    an.packet(1, 1, 0xa, {TodWriter::s16x2(0, 0x400), 0, 10, 20, 30});  // rot Ry(90 deg) + trans
    an.packet(2, 1, 0x8, {0, 100, 0});
    an.packet(0, 9, 0, {TodWriter::s16x2(-1, -2), TodWriter::s16x2(-3, 4), TodWriter::s16x2(5, 6), 77});
    an.frames.emplace_back();
    an.packet(1, 1, 0x8, {0, 0, 0});
    an.frames.emplace_back();
    an.packet(1, 8, 0xb, {});

    bb::ActorModel m;
    m.skeleton = bb::parseTod(sk.build(1), 0);
    m.anims.push_back({7, bb::parseTod(an.build(4), 0)});
    m.objects = {{{{100, 0, 0}, 0, false}}, {{{0, 0, 0}, 1, false}, {{0, 0, 50}, 0, true}}};
    m.slotCount = 2;
    CHECK(m.anims[0].tod.frames() == 3 && m.anims[0].tod.resolution == 4 && m.skeleton.frames() == 1, "decode");

    bb::AnimState st;
    CHECK(bb::animTick(m, st, 99, bb::AnimLoop) == bb::AnimMissing, "missing anim");
    st = {};
    std::vector<int32_t> tx;
    for (int t = 0; t < 6; ++t) {
        CHECK(bb::animTick(m, st, 7, t == 0 ? bb::AnimLoop : 0) == bb::AnimPlaying, "loop tick {}", t);
        tx.push_back(st.parts.at(0).trans[0]);
        if (t == 0) {
            CHECK(st.parts.size() == 2 && st.parts[0].parent == -1 && st.parts[1].parent == 0, "skeleton");
            CHECK(st.box == (std::array<int16_t, 6>{-1, -2, -3, 4, 5, 6}) && st.radius == 77, "bbox packet");
            const auto s = bb::poseSlots(m, st);
            // part 1: Ry(90) (100,0,0) + (10,20,30) = (10,20,-70); part 2 under part 1 at (0,100,0): slot 1 =
            // (10,120,30), blend vertex (0,0,50) -> (60,120,30) averaged into slot 0 -> (35,70,-20).
            CHECK(near(s[1], {10, 120, 30}) && near(s[0], {35, 70, -20}), "pose {} {} {} / {} {} {}", s[0][0], s[0][1],
                  s[0][2], s[1][0], s[1][1], s[1][2]);
        }
    }
    // frames held 2 ticks, frame 2 never applied, loop back to frame 0 on tick 5
    CHECK(tx == (std::vector<int32_t>{10, 10, 0, 0, 10, 10}), "loop timing");
    CHECK(!(st.parts[0].flags & 0x8000), "last frame never applied");

    st = {};
    std::vector<uint32_t> r;
    for (int t = 0; t < 6; ++t) r.push_back(bb::animTick(m, st, 7, t == 0 ? bb::AnimOnce : 0));
    CHECK(r == (std::vector<uint32_t>{0x200, 0x200, 0x200, 0x100, 0x100, 0x100}), "once: finished on tick 4");
    st = {};
    r.clear();
    for (int t = 0; t < 5; ++t) r.push_back(bb::animTick(m, st, 7, t == 0 ? bb::AnimOnce | bb::AnimHold : 0));
    CHECK(r == (std::vector<uint32_t>{0x200, 0x200, 0x200, 0x200, 0x200}) && (st.flags & 0x100), "hold");

    bb::applyTodFrame(st, m.anims[0].tod, 2);  // hide packet
    CHECK((st.parts[0].flags & 0x8000) && st.parts[0].scale == (std::array<int32_t, 3>{0, 0, 0}), "hide");
    std::vector<bb::Mesh> out;
    bb::poseActor(m, st, {0, 0, 0}, {0, 0, 0}, {4096, 4096, 4096}, out);  // no meshes in this model: nothing emitted
    CHECK(out.empty(), "no meshes");
}

// anim.py pose: skeleton frame 0 + animation frames 0..frame, slots sorted, OBJ y = -y.
static void compareWithAnimPy(const fs::path& bze, const bb::Scene& s) {
    const bb::ActorInstance* pl = nullptr;
    for (const bb::ActorInstance& a : s.actors)
        if (a.player) pl = &a;
    CHECK(pl, "{}: no player actor", bze.string());
    if (!pl) return;
    const bb::ActorModel& m = s.actorModels[pl->model];
    const int ai = bb::findAnim(m, pl->anim);
    const size_t frame = std::min<size_t>(3, m.anims[size_t(ai)].tod.frames() - 2);
    const fs::path dir = fs::temp_directory_path() / "bbtime_test_actor";
    fs::create_directories(dir);
    const std::string cmd = std::format("python3 '{}/../tools/re/anim.py' pose '{}' 0x{:x} {} {} '{}' > /dev/null 2>&1",
                                        BBTIME_TESTS_DIR, bze.string(), pl->scriptOffset, pl->anim, frame, dir.string());
    if (std::system(cmd.c_str()) != 0) {
        std::println("anim.py comparison skipped (python3/Pillow unavailable?)");
        return;
    }
    std::ifstream f(dir / std::format("{}_{:x}_anim{:x}_f{}.obj", bze.stem().string(), pl->scriptOffset, pl->anim, frame));
    std::vector<std::array<float, 3>> ref;
    for (std::string line; std::getline(f, line);)
        if (line.starts_with("v ")) {
            std::istringstream is(line.substr(2));
            std::array<float, 3> v{};
            is >> v[0] >> v[1] >> v[2];
            ref.push_back({v[0], -v[1], v[2]});
        }
    bb::AnimState st;
    bb::applyTodFrame(st, m.skeleton, 0);
    st.parts[0].parent = -1;
    for (size_t k = 0; k <= frame; ++k) bb::applyTodFrame(st, m.anims[size_t(ai)].tod, k);
    std::vector<std::array<float, 3>> mine;
    for (const auto& v : bb::poseSlots(m, st))
        if (!std::isnan(v[0])) mine.push_back(v);
    float worst = 0;
    for (size_t i = 0; i < std::min(mine.size(), ref.size()); ++i)
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(mine[i][size_t(k)] - ref[i][size_t(k)]));
    CHECK(!ref.empty() && mine.size() == ref.size() && worst < 0.05f, "anim.py: {} vs {} slots, max diff {}", mine.size(),
          ref.size(), worst);
    std::println("anim.py comparison {} anim {} frame {}: {} slots, max |diff| {:.4f}", bze.stem().string(), pl->anim,
                 frame, mine.size(), worst);
    fs::remove_all(dir);
}

int main() {
    synthetic();
    const char* env = std::getenv("BBTIME_DATA");
    if (!env || !*env) {
        std::println("synthetic ok ({} failures); BBTIME_DATA unset: real-data part skipped", failures);
        return failures ? 1 : 77;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(fs::path(env) / "lit/Datas/bze"))
        if (e.path().extension() == ".bze") files.push_back(e.path());
    std::ranges::sort(files);
    uint64_t models = 0, actors = 0, anims = 0, poses = 0, players = 0, badObject = 0, emptyPoses = 0, tris = 0;
    std::vector<bb::Mesh> out;
    for (const fs::path& p : files) {
        try {
            const bb::Scene s = bb::buildLitScene(bb::loadBze(p), p.stem().string());
            models += s.actorModels.size(), actors += s.actors.size();
            for (const bb::ActorInstance& a : s.actors) players += a.player;
            for (const bb::ActorModel& m : s.actorModels) {
                for (const bb::ActorMesh& am : m.meshes) {
                    CHECK(am.slots.size() == am.mesh.vertices.size() && am.object < m.objects.size(), "{}", am.mesh.name);
                    for (uint16_t sl : am.slots) CHECK(sl < m.slotCount, "{} slot {}", am.mesh.name, sl);
                }
                for (const bb::ActorAnim& an : m.anims) {
                    if (&m.anims[size_t(bb::findAnim(m, an.id))] != &an) continue;  // duplicate id: first one plays
                    ++anims;
                    const size_t n = an.tod.frames();
                    const unsigned hold = (an.tod.resolution >> 1 & 0xff) ? (an.tod.resolution >> 1 & 0xff) : 256;
                    const size_t expect = n < 2 ? 1 : (n - 1) * hold;  // tick on which a once-clip finishes
                    bb::AnimState st;
                    size_t t = 0;
                    for (uint32_t r = 0; r != bb::AnimFinished && t <= expect;) {
                        r = bb::animTick(m, st, an.id, t ? 0 : bb::AnimOnce);
                        ++t;
                        for (const bb::TodPart& part : st.parts) {
                            badObject += part.object >= int32_t(m.objects.size());
                            CHECK(part.parent >= -2 && part.parent < int32_t(st.parts.size()), "{} parent", p.string());
                        }
                        out.clear();
                        bb::poseActor(m, st, {1000, 2000, 3000}, {0, 0x200, 0}, {4096, 4096, 4096}, out);
                        ++poses;
                        emptyPoses += out.empty();
                        for (const bb::Mesh& mesh : out) {
                            tris += mesh.indices.size() / 3;
                            for (uint32_t i : mesh.indices) CHECK(i < mesh.vertices.size(), "{}", p.string());
                            for (const bb::Vertex& v : mesh.vertices)
                                CHECK(std::isfinite(v.pos[0]) && std::isfinite(v.pos[1]) && std::isfinite(v.pos[2]), "{}",
                                      p.string());
                        }
                    }
                    CHECK(t == expect, "{} anim {}: finished on tick {}, expected {}", p.stem().string(), an.id, t, expect);
                }
            }
            if (p.stem() == "L01a") compareWithAnimPy(p, s);
        } catch (const std::exception& e) {
            CHECK(false, "{}: {}", p.string(), e.what());
        }
    }
    std::println("LiT: {} files, {} actor models, {} placed actors ({} player), {} anims, {} poses ({} empty), {} triangles "
                 "posed, {} parts with out-of-range TMD objects (skipped)",
                 files.size(), models, actors, players, anims, poses, emptyPoses, tris, badObject);
    CHECK(files.size() == 167 && models > 0 && players > 0, "coverage");
    return failures ? 1 : 0;
}
