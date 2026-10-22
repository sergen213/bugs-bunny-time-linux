// Mods (bb/mods.h): resolver keys + load order + conflicts, validation, PNG texture override, glTF clip round trip.
#include "bb/actor.h"
#include "bb/mods.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <print>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace bb;

namespace {

void writeText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

Texture solid(uint32_t w, uint32_t h, std::array<uint8_t, 4> c) {
    Texture t{.name = "x", .width = w, .height = h, .rgba = {}, .hasAlpha = c[3] < 255};
    for (uint32_t i = 0; i < w * h; ++i) t.rgba.insert(t.rgba.end(), c.begin(), c.end());
    return t;
}

// One-part rigid actor: skeleton creates part 1 showing object 0; clip 5 has 3 frames rotating / moving it.
Scene actorScene() {
    Scene s;
    s.game = GameId::LostInTime;
    s.level = "T01";
    s.textures.push_back(solid(2, 2, {255, 255, 255, 255}));
    s.textures[0].name = "T01/tex/7";
    ActorModel m;
    m.modelAsset = 3;
    m.slotCount = 3;
    m.objects = {{{{0, 0, 0}, 0, false}, {{100, 0, 0}, 1, false}, {{0, 100, 0}, 2, false}}};
    ActorMesh am;
    am.object = 0;
    am.mesh.texture = 0;
    am.mesh.vertices.resize(3);
    am.mesh.vertices.assign(3, Vertex{{0, 0, 0}, {0, 0}, {128, 128, 128, 255}});
    am.mesh.indices = {0, 1, 2};
    am.slots = {0, 1, 2};
    m.meshes.push_back(am);
    m.skeleton = {.resolution = 2, .frameStart = {0, 2, 2}, .packets = {{1, 8, 0, {}}, {1, 2, 0, {1}}}, .packed = false};
    Tod clip{.resolution = 4, .frameStart = {0}, .packets = {}, .packed = false};
    const std::array<std::array<int32_t, 9>, 3> keys{{{100, 1024, -300, 4096, 4096, 4096, 0, 0, 0},
                                                      {-200, 512, 900, 4096, 8192, 4096, 50, -20, 7},
                                                      {0, 0, 2048, 2048, 4096, 4096, 100, 0, 0}}};
    for (const auto& k : keys) {
        clip.packets.push_back({1, 1, 0xe, k});
        clip.packets.push_back({0, 9, 0, {-10, -20, -30, 10, 20, 30, 40}});  // gameplay box: must survive a replacement
        clip.frameStart.push_back(uint32_t(clip.packets.size()));
    }
    m.anims.push_back({5, clip});
    s.actorModels.push_back(m);
    s.actors.push_back({.model = 0, .pos = {}, .rot = {}, .scale = {4096, 4096, 4096}, .anim = 5, .entityId = 1, .chunk = 7, .player = true, .scriptOffset = 0});
    return s;
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / std::format("bbtime_test_mods_{}", ::getpid());
    fs::remove_all(root);

    // keyOf: kind + extension rules.
    assert(mods::keyOf("textures/lit/L01A/12.PNG") == "textures/lit/l01a/12");
    assert(mods::keyOf("textures/lit/L01A/12.jpg").empty());
    assert(mods::keyOf("models/tb/bugs.gltf") == "models/tb/bugs" && mods::keyOf("models/tb/bugs.bin").empty());
    assert(mods::keyOf("music/lit/Track5.ogg") == "music/lit/track5" && mods::keyOf("mod.json").empty());

    // Two mods overriding the same texture: the later one in the enabled list wins; conflicts list both.
    for (const char* id : {"a", "b"}) {
        mods::Info i;
        i.id = i.name = id;
        i.game = "lit";
        i.dir = root / id;
        assert(mods::writeInfo(i).empty());
    }
    assert(mods::savePng(root / "a/textures/lit/t01/7.png", solid(4, 4, {255, 0, 0, 255})));
    assert(mods::savePng(root / "b/textures/lit/t01/7.png", solid(8, 8, {0, 255, 0, 0})));
    assert(mods::savePng(root / "a/textures/lit/t01/9.png", solid(1, 1, {1, 2, 3, 255})));
    const std::vector<std::string> ab{"a", "b", "missing"};
    const mods::ModSet set = mods::ModSet::load(root, ab);
    assert(set.mods().size() == 2 && set.problems().size() == 1);
    assert(set.find("textures/lit/t01/7")->parent_path().parent_path().parent_path().parent_path().filename() == "b");
    assert(set.find("textures/lit/t01/9") && !set.find("textures/lit/t01/8"));
    assert(set.keys("textures/lit/t01/").size() == 2);
    const auto conflicts = set.conflicts();
    assert(conflicts.size() == 1 && conflicts.begin()->first == "textures/lit/t01/7" && conflicts.begin()->second == (std::vector<std::string>{"a", "b"}));
    const std::vector<std::string> ba{"b", "a"};
    assert(mods::ModSet::load(root, ba).find("textures/lit/t01/7")->string().find("/a/") != std::string::npos);
    assert(mods::scan(root).size() == 2);

    // Texture override: any size, transparency turns the opaque faces into alpha-tested ones.
    {
        Scene s = actorScene();
        s.meshes.push_back({.name = "w", .texture = 0, .blend = Blend::Opaque, .vertices = {}, .indices = {}, .twoSided = false, .textureAnim = -1});
        mods::apply(s, set);
        assert(s.textures[0].width == 8 && s.textures[0].hasAlpha && s.textures[0].rgba[1] == 255);
        assert(s.meshes[0].blend == Blend::AlphaTest && s.actorModels[0].meshes[0].mesh.blend == Blend::AlphaTest);
        assert(set.appliedCount() == 1);
    }

    // Validation: bad PNG, bad path, non-numeric id, glTF without part nodes.
    writeText(root / "a/textures/lit/t01/10.png", "not a png");
    writeText(root / "a/textures/lit/t01/abc.png", "x");
    writeText(root / "a/stuff/notes.txt", "x");
    writeText(root / "a/models/lit/bad.gltf", R"({"asset":{"version":"2.0"},"nodes":[{"name":"x"}]})");
    {
        const auto issues = mods::validate(mods::readInfo(root / "a"));
        auto has = [&](const std::string& file, bool error) {
            return std::ranges::any_of(issues, [&](const mods::Issue& i) { return i.file == file && i.error == error; });
        };
        assert(has("textures/lit/t01/10.png", true) && has("textures/lit/t01/abc.png", true));
        assert(has("stuff/notes.txt", false) && has("models/lit/bad.gltf", true));
        assert(!has("textures/lit/t01/7.png", true));
    }

    // glTF: export the actor, re-import its clip unchanged (rotation within one 4.12 unit, boxes kept), then a model
    // whose geometry is the exported one: same triangle count.
    {
        Scene s = actorScene();
        const fs::path out = root / "export";
        assert(mods::exportModels(s, out) >= 2);
        {  // launcher viewer: the exported model's one triangle, rest pose
            const mods::Preview p = mods::loadPreview(out / "models/lit/bugs.gltf");
            size_t n = 0;
            for (const auto& grp : p.groups) n += grp.vertices.size();
            assert(n == 3 * 9);
        }
        mods::Info i;
        i.id = i.name = "c";
        i.dir = root / "c";
        assert(mods::writeInfo(i).empty());
        fs::create_directories(root / "c/animations/lit/bugs");
        fs::copy_file(out / "animations/lit/bugs/5.gltf", root / "c/animations/lit/bugs/5.gltf");
        fs::copy_file(out / "animations/lit/bugs/5.gltf", root / "c/animations/lit/bugs/77.gltf");  // a new clip
        fs::create_directories(root / "c/models/lit");
        fs::copy_file(out / "models/lit/bugs.gltf", root / "c/models/lit/bugs.gltf");
        const std::vector<std::string> c{"c"};
        const mods::ModSet cs = mods::ModSet::load(root, c);
        const Tod before = s.actorModels[0].anims[0].tod;
        mods::apply(s, cs);
        for (const std::string& n : s.notes) std::println("note: {}", n);
        const ActorModel& m = s.actorModels[0];
        assert(m.anims.size() == 2 && findAnim(m, 77) >= 0);
        const Tod& after = m.anims[0].tod;
        assert(after.frames() == before.frames() && after.resolution == before.resolution);
        size_t boxes = 0;
        for (size_t f = 0; f < after.frames(); ++f) {
            AnimState a, b;
            for (uint32_t k = after.frameStart[f]; k < after.frameStart[f + 1]; ++k) {
                const TodPacket& p = after.packets[k];
                boxes += p.type == 9;
                if (p.type != 1) continue;
                const TodPacket& o = before.packets[before.frameStart[f]];
                // Same rotation matrix (Euler angles may differ by a full turn): compare the posed corner.
                TodPart x, y;
                x.rot = {p.v[0], p.v[1], p.v[2]}, x.scale = {p.v[3], p.v[4], p.v[5]};
                y.rot = {o.v[0], o.v[1], o.v[2]}, y.scale = {o.v[3], o.v[4], o.v[5]};
                rebuildPart(x), rebuildPart(y);
                for (size_t e = 0; e < 9; ++e) assert(std::abs(x.m[e] - y.m[e]) < 0.004f);
                for (size_t e = 3; e < 9; ++e) assert(std::abs(p.v[e] - o.v[e]) <= 1);
            }
        }
        assert(boxes == 3);
        size_t tris = 0;
        for (const ActorMesh& am : m.meshes) tris += am.mesh.indices.size() / 3;
        assert(tris == 1 && m.slotCount == 3);  // welded to the original slots: nothing new
    }

    // Models the simulation builds itself (TB pickups): applyToModel uses the active set, ids "<level>/<asset>".
    {
        const Scene s = actorScene();
        ActorModel built = s.actorModels[0];
        mods::applyToModel(s, built);  // no active mods: untouched
        assert(built.anims.size() == 1);
        mods::Info i;
        i.id = i.name = "d";
        i.dir = root / "d";
        assert(mods::writeInfo(i).empty());
        fs::create_directories(root / "d/animations/lit/t01/3");
        fs::copy_file(root / "export/animations/lit/bugs/5.gltf", root / "d/animations/lit/t01/3/88.gltf");
        const std::vector<std::string> d{"d"};
        mods::setActive(mods::ModSet::load(root, d));
        mods::applyToModel(s, built);
        assert(built.anims.size() == 2 && findAnim(built, 88) >= 0);
        mods::setActive({});
    }
    fs::remove_all(root);
    std::println("mods: ok");
}
