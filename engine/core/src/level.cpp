#include "bb/level.h"
#include "bb/actor.h"
#include "bb/levelscript.h"
#include "bb/tim.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

namespace bb {
namespace {

struct In {
    std::span<const uint8_t> d;
    const char* what;
    void need(size_t p, size_t n) const {
        if (p > d.size() || d.size() - p < n)
            throw FormatError(std::format("{}: {} bytes at offset 0x{:x} past end (0x{:x})", what, n, p, d.size()));
    }
    uint8_t u8(size_t p) const { need(p, 1); return d[p]; }
    uint16_t u16(size_t p) const { need(p, 2); return uint16_t(d[p] | d[p + 1] << 8); }
    uint32_t u32(size_t p) const { need(p, 4); return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
    float f32(size_t p) const { return std::bit_cast<float>(u32(p)); }
};

std::span<const uint8_t> tagData(const ScriptItem& it, uint8_t id) {
    for (const ScriptTag& t : it.tags)
        if (t.id == id) return t.data;
    return {};
}

uint32_t le32(std::span<const uint8_t> b, size_t p) { return uint32_t(b[p] | b[p + 1] << 8 | b[p + 2] << 16 | uint32_t(b[p + 3]) << 24); }

// Polygon record layouts, MODELS.md §5 (descriptor tables 0x4692b8.. in bugs.exe v1.6). Offsets in bytes;
// uv 0 = implicit UVs, tex/tpage 0 = untextured.
struct Poly { uint8_t type, stride, n, idx[4], uv[4], tex, tpage, col[4]; };
constexpr Poly kPolys[] = {
    {0x34, 24, 3, {20, 22, 14}, {4, 8, 12}, 6, 10, {16, 16, 16}},
    {0x38, 28, 4, {20, 22, 24, 26}, {4, 8, 12, 14}, 6, 10, {16, 16, 16, 16}},
    {0x3c, 32, 3, {28, 30, 14}, {4, 8, 12}, 6, 10, {16, 20, 24}},
    {0x40, 40, 4, {32, 34, 36, 38}, {4, 8, 12, 14}, 6, 10, {16, 20, 24, 28}},
    {0x48, 24, 3, {16, 18, 20}, {}, 0, 0, {4, 8, 12}},
    {0x4a, 28, 3, {20, 22, 24}, {}, 4, 6, {8, 12, 16}},
    {0x4c, 28, 4, {20, 22, 24, 26}, {}, 0, 0, {4, 8, 12, 16}},
    {0x4e, 32, 4, {24, 26, 28, 30}, {}, 4, 6, {8, 12, 16, 20}},
};
// Grouped types walked by FUN_00423eb0 whose layout is unknown; never present in LiT data, skipped.
uint8_t otherGroupStride(uint8_t t) {
    switch (t) {
    case 0x14: return 16; case 0x18: return 20; case 0x1c: return 24; case 0x20: return 28;
    case 0x24: return 20; case 0x28: return 24; case 0x2c: return 32; case 0x30: return 36;
    case 0x64: return 16;  // sprite / billboard (MODELS.md §8), not drawn by the world renderer
    default: return 0;
    }
}

// Implicit UVs (types 0x4a/0x4e) in (u, v), per vertex index slot i0..i3. The spec lists the quad as
// i0 (0,1) i1 (1,1) i2 (1,0) i3 (0,0) in emitter slot order, where slot 2 holds i3 and slot 3 holds i2; mapped
// back to record order that is the untwisted square below. Visually irrelevant (single-colour textures).
constexpr float kImplicitUv[4][2] = {{0, 1}, {1, 1}, {0, 0}, {1, 0}};

// PS1 placement: p = R * (S * v) + t, then engine = (x, -y, -z) (scene.h).
struct Xform {
    float s[3] = {1, 1, 1}, t[3] = {0, 0, 0}, cy = 1, sy = 0;  // rotation about PS1 Y only
    std::array<float, 3> apply(float x, float y, float z) const {
        x *= s[0], y *= s[1], z *= s[2];
        const float rx = cy * x + sy * z, rz = -sy * x + cy * z;  // PsyQ RotMatrix, Y component
        return {rx + t[0], -(y + t[1]), -(rz + t[2])};
    }
};

// 4.12 angle (0x1000 = 360 degrees) -> radians.
float angle(int16_t a) { return float(a) * 2.0f * std::numbers::pi_v<float> / 4096.0f; }

struct Builder {
    Scene& s;
    std::map<uint32_t, int32_t> tex{};    // texture id -> Scene::textures index
    std::map<uint32_t, int32_t> links{};  // texture link id -> Scene::textureAnims index (TEXTURE_GAP.md)
    std::set<uint32_t> missing{};
    std::map<std::tuple<uint32_t, int32_t, Blend, bool>, size_t> meshOf{};
    ActorModel* actor = nullptr;            // set while building an actor model: meshes go to actor->meshes
    std::vector<uint16_t>* slots = nullptr; // the current actor mesh's slot list
    bool world = false;                     // building a chunk-0x20 world model (stats split)

    // Face texture id -> texture index (>= 0), texture link (-2 - anim index), or -1 = alias 0: the original resolves
    // ids through the alias table DAT_004e5e80 and never draws bucket 0 (TEXTURE_GAP.md §1).
    int32_t resolve(uint32_t id) {
        if (auto it = tex.find(id); it != tex.end()) return it->second;
        if (auto it = links.find(id); it != links.end()) {
            ++s.stats.linkedFaces;
            return -2 - it->second;
        }
        ++s.stats.missingTextureFaces;
        s.stats.worldMissingTextureFaces += world;
        missing.insert(id);
        return -1;
    }
    // Static texture of a key (a link binds its start frame) and its texture-anim index.
    int32_t fallback(int32_t key) const {
        if (key >= 0) return key;
        const TextureAnim& a = s.textureAnims[size_t(-2 - key)];
        return a.frames[a.startFrame];
    }
    bool hasAlpha(int32_t key) const {
        if (key >= 0) return s.textures[size_t(key)].hasAlpha;
        return std::ranges::any_of(s.textureAnims[size_t(-2 - key)].frames, [&](int32_t f) { return s.textures[size_t(f)].hasAlpha; });
    }

    // key: -1 untextured, else as resolve().
    Mesh& mesh(uint32_t group, const std::string& gname, int32_t key, Blend b, bool twoSided) {
        const int32_t t = key == -1 ? -1 : fallback(key), anim = key <= -2 ? -2 - key : -1;
        auto what = [&] { return anim >= 0 ? std::format("link{}", s.textureAnims[size_t(anim)].linkId) : std::format("tex{}", t); };
        if (actor) {  // group = object index
            for (ActorMesh& am : actor->meshes)
                if (am.object == group && am.mesh.texture == t && am.mesh.textureAnim == anim && am.mesh.blend == b &&
                    am.mesh.twoSided == twoSided)
                    return slots = &am.slots, am.mesh;
            ActorMesh& am = actor->meshes.emplace_back();
            am.object = group;
            am.mesh.name = std::format("{}/obj{}/{}{}", gname, group, what(), twoSided ? "/2s" : "");
            am.mesh.texture = t;
            am.mesh.textureAnim = anim;
            am.mesh.blend = b;
            am.mesh.twoSided = twoSided;
            slots = &am.slots;
            return am.mesh;
        }
        auto [it, fresh] = meshOf.try_emplace({group, key, b, twoSided}, s.meshes.size());
        if (fresh) {
            Mesh& m = s.meshes.emplace_back();
            m.name = std::format("{}/{}{}", gname, what(), twoSided ? "/2s" : "");
            m.texture = t;
            m.textureAnim = anim;
            m.blend = b;
            m.twoSided = twoSided;
        }
        return s.meshes[it->second];
    }

    // One model ("PC TMD", MODELS.md §2-6) at sec4[off, off+size). `group` names the mesh group; world models
    // advance it per cell header, props pass a unique value.
    void model(std::span<const uint8_t> sec4, uint32_t off, uint32_t size, const Xform& xf, uint32_t& group,
               bool world, const std::string& gname) {
        this->world = world;
        if (off > sec4.size() || sec4.size() - off < size)
            throw FormatError(std::format("model 0x{:x}+0x{:x} outside section 4 (0x{:x})", off, size, sec4.size()));
        const In in{sec4.subspan(off, size), "model"};
        if (in.u32(0) != 0x41) throw FormatError(std::format("model 0x{:x}: magic 0x{:x}", off, in.u32(0)));
        const uint32_t nobj = in.u32(8);
        in.need(12, size_t(nobj) * 28);
        // Polygon/cell index space. World models: concatenated object vertex tables (word 3 ignored, 0x40f150).
        // Actor/prop models: vertex word 3 = slot (low 15 bits) in a shared buffer; bit 0x8000 = average with the
        // value already in that slot (GameplayRE, 0x411350/0x411b10; non-blend slots are 0..N-1 in object order).
        // The original averages after projection; averaging in model space here is an approximation (Inferred).
        std::vector<std::array<float, 3>> objv;
        if (actor) actor->objects.assign(nobj, {});
        for (uint32_t o = 0; o < nobj; ++o) {
            const size_t e = 12 + size_t(o) * 28, vt = 12 + size_t(in.u32(e)), nv = in.u32(e + 4);
            in.need(vt, nv * 16);
            for (size_t i = 0; i < nv; ++i) {
                const std::array<float, 3> p{in.f32(vt + i * 16), in.f32(vt + i * 16 + 4), in.f32(vt + i * 16 + 8)};
                if (world) {
                    objv.push_back(p);
                    continue;
                }
                const uint32_t w = in.u32(vt + i * 16 + 12), slot = w & 0x7fff;
                if (actor) actor->objects[o].push_back({{p[0], p[1], p[2]}, uint16_t(slot), bool(w & 0x8000)});
                if (slot >= objv.size()) objv.resize(slot + 1, {0, 0, 0});
                if (w & 0x8000)
                    for (int k = 0; k < 3; ++k) objv[slot][size_t(k)] = (objv[slot][size_t(k)] + p[size_t(k)]) * 0.5f;
                else
                    objv[slot] = p;
            }
        }
        if (actor) actor->slotCount = uint32_t(objv.size());
        std::vector<std::array<float, 3>> block;
        std::string cellName = gname;
        for (uint32_t o = 0; o < nobj; ++o) {
            if (actor) group = o;
            const size_t e = 12 + size_t(o) * 28;
            size_t p = 12 + size_t(in.u32(e + 16));
            int64_t nprim = in.u32(e + 20);
            const std::vector<std::array<float, 3>>* cur = &objv;
            while (nprim > 0) {
                const uint8_t t = in.u8(p + 3);
                const uint16_t cnt = in.u16(p);
                if (t == 0x00 || t == 0x04 || t == 0x08 || t == 0x0c || t == 0x10) {  // cell / hull / portal
                    if ((t == 0x00 || t == 0x04) && world) {
                        ++group;
                        ++s.stats.cells;
                        cellName = std::format("{}/cell{}", gname, s.stats.cells);
                    }
                    p += t == 0x08 || t == 0x0c ? 56 : t == 0x10 ? 20 : 16;
                    nprim -= 1;
                    continue;
                }
                if (t == 0x44) {  // vertex block: replaces the polygon vertex base (§6)
                    if (actor) throw FormatError(std::format("actor model 0x{:x}: vertex block unsupported", off));
                    in.need(p + 4, size_t(cnt) * 12);
                    block.resize(cnt);
                    for (size_t i = 0; i < cnt; ++i) block[i] = {in.f32(p + 4 + i * 12), in.f32(p + 8 + i * 12), in.f32(p + 12 + i * 12)};
                    cur = &block;
                    p += 4 + size_t(cnt) * 12;
                    nprim -= 1;
                    continue;
                }
                const Poly* poly = nullptr;
                for (const Poly& k : kPolys)
                    if (k.type == t) poly = &k;
                const size_t stride = poly ? poly->stride : otherGroupStride(t);
                if (!stride) throw FormatError(std::format("model 0x{:x}: unknown primitive type 0x{:02x} at 0x{:x}", off, t, p));
                if (cnt == 0) throw FormatError(std::format("model 0x{:x}: empty primitive group at 0x{:x}", off, p));
                in.need(p, cnt * stride);
                if (poly) {
                    const bool twoSided = in.u8(p + 2) & 2;  // group's first record decides culling [BF]
                    for (size_t r = 0; r < cnt; ++r) polygon(in, p + r * stride, *poly, *cur, xf, group, cellName, twoSided);
                }
                p += cnt * stride;
                nprim -= cnt;
            }
        }
    }

    void polygon(const In& in, size_t q, const Poly& k, const std::vector<std::array<float, 3>>& verts, const Xform& xf,
                 uint32_t group, const std::string& gname, bool twoSided) {
        const uint8_t flags = in.u8(q + 2);
        int32_t key = -1, ti = -1;
        Blend blend = Blend::Opaque;
        uint8_t alpha = 255;
        if (k.tex) {
            key = resolve(in.u16(q + k.tex));
            if (key == -1) return;  // alias 0: not drawn (TEXTURE_GAP.md §4)
            ti = fallback(key);
            if (flags & 8) {  // semi-transparent: ABR from tpage bits 5-6
                switch (in.u8(q + k.tpage) >> 5 & 3) {
                case 0: blend = Blend::Alpha, alpha = 128; break;
                case 1: blend = Blend::Additive; break;
                case 2: blend = Blend::Subtract; break;
                case 3: blend = Blend::Additive, alpha = 64; break;
                }
            } else if (hasAlpha(key)) blend = Blend::AlphaTest;
        }
        const float tw = ti >= 0 ? float(s.textures[size_t(ti)].width) : 1.0f;
        const float th = ti >= 0 ? float(s.textures[size_t(ti)].height) : 1.0f;
        Mesh& m = mesh(group, gname, key, blend, twoSided);
        const uint32_t base = uint32_t(m.vertices.size());
        for (int i = 0; i < k.n; ++i) {
            const uint16_t vi = in.u16(q + k.idx[i]);
            if (vi >= verts.size())
                throw FormatError(std::format("{}: vertex index {} >= {} in primitive at 0x{:x}", gname, vi, verts.size(), q));
            Vertex v{};
            const auto p = xf.apply(verts[vi][0], verts[vi][1], verts[vi][2]);
            std::copy(p.begin(), p.end(), v.pos);
            float u = kImplicitUv[i][0], w = kImplicitUv[i][1];
            if (k.uv[0]) u = in.u8(q + k.uv[i]) / 255.0f, w = in.u8(q + k.uv[i] + 1) / 255.0f;
            // texel = uv * (size - 1) (MODELS.md §5); +0.5 addresses the texel centre in GL. v is measured from
            // the BOTTOM image row: the PC port uploads TIM rows last-first (FUN_00420d10 resample loop counts
            // rows down from the height) and samples t = v/255. Evidence: L03A ACME crate (model 0x10fbc) has
            // v = 0 on its bottom edge and reads upright only with this flip.
            v.uv[0] = (u * (tw - 1.0f) + 0.5f) / tw;
            v.uv[1] = ((1.0f - w) * (th - 1.0f) + 0.5f) / th;
            v.rgba[0] = in.u8(q + k.col[i]);
            v.rgba[1] = in.u8(q + k.col[i] + 1);
            v.rgba[2] = in.u8(q + k.col[i] + 2);
            v.rgba[3] = alpha;
            m.vertices.push_back(v);
            if (actor) slots->push_back(vi);
        }
        // Quads use PS1 "Z" order: outline v0 v1 v3 v2 -> triangles (0,1,2) + (1,3,2). In engine space the data's
        // visible side has that order clockwise (checked visually on L01a: with this order reversed, plateau tops
        // are visible from above and cliff faces from the canyon), so emit reversed to keep Mesh's CCW-front rule.
        m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
        if (k.n == 4) m.indices.insert(m.indices.end(), {base + 1, base + 2, base + 3});
        if (actor) return;
        ++s.stats.faces;
        s.stats.triangles += k.n - 2;
    }
};

} // namespace

std::vector<SpriteRecord> spriteRecords(std::span<const uint8_t> sec4, uint32_t off, uint32_t size) {
    if (off > sec4.size() || sec4.size() - off < size) throw FormatError(std::format("sprite model 0x{:x} outside section 4", off));
    const In in{sec4.subspan(off, size), "sprite model"};
    std::vector<SpriteRecord> recs;
    const uint32_t nobj = in.u32(8);
    for (uint32_t o = 0; o < nobj; ++o) {
        const size_t e = 12 + size_t(o) * 28;
        size_t p = 12 + size_t(in.u32(e + 16));
        for (int64_t nprim = in.u32(e + 20); nprim > 0;) {
            const uint8_t t = in.u8(p + 3);
            const uint16_t cnt = in.u16(p);
            if (t == 0x00 || t == 0x04 || t == 0x08 || t == 0x0c || t == 0x10) {
                p += t == 0x08 || t == 0x0c ? 56 : t == 0x10 ? 20 : 16;
                nprim -= 1;
            } else if (t == 0x44) {
                p += 4 + size_t(cnt) * 12;
                nprim -= 1;
            } else {
                size_t stride = otherGroupStride(t);
                for (const Poly& k : kPolys)
                    if (k.type == t) stride = k.stride;
                if (!stride || !cnt) throw FormatError(std::format("sprite model 0x{:x}: bad group 0x{:02x}", off, t));
                if (t == 0x64)
                    for (size_t r = 0; r < cnt; ++r) {
                        const size_t q = p + r * 16;
                        recs.push_back({in.u8(q + 2), in.u16(q + 6), in.u8(q + 8), in.u8(q + 9), in.u16(q + 10),
                                        int16_t(in.u16(q + 12)), int16_t(in.u16(q + 14))});
                    }
                p += cnt * stride;
                nprim -= cnt;
            }
        }
    }
    return recs;
}

Scene buildLitScene(const BzeFile& bze, const std::string& level) {
    if (bze.variant != BzeVariant::LostInTime) throw FormatError("buildLitScene: not a Lost in Time BZE");
    const int i1 = bze.find(1), i3 = bze.find(3), i4 = bze.find(4);
    if (i1 < 0) throw FormatError("buildLitScene: no section 1");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const std::vector<uint8_t> s3 = i3 >= 0 ? bze.payload(size_t(i3)) : std::vector<uint8_t>{};
    const std::vector<uint8_t> s4 = i4 >= 0 ? bze.payload(size_t(i4)) : std::vector<uint8_t>{};
    const LevelScript script = parseLevelScript(s1);

    Scene s;
    s.game = GameId::LostInTime;
    s.level = level;
    s.colorScale = 255.0f / 128.0f;  // PS1 texel * colour / 128 (MODELS.md §5); the PC port uses /255
    // bugs.exe OpenGL renderer (0x4ac094 = 2, 0x422a40): CLUT colours (<< 3, 0x420d10) pass through table 0x4b5b40 =
    // trunc(256·(i/256)^(5/6)) (0x45ca08) before upload while 0x4ac034 = 1 (runtime: both set, RUNTIME_REFERENCE §7).
    s.pcTexGamma = 5.0f / 6.0f;
    Builder b{.s = s};

    // Textures (chunk 0x29 tag 0x2a: u32 section-3 offset, u32 id).
    std::vector<std::string> texErrors;
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || it.code != 0x29) continue;
        for (const ScriptTag& t : it.tags) {
            if (t.id != 0x2a) continue;
            const uint32_t off = le32(t.data, 0), id = le32(t.data, 4);
            ++s.stats.textureRefs;
            try {
                Texture tx = decodeTim(s3, off, std::format("{}/tex/{}", level, id));
                if (b.tex.try_emplace(id, int32_t(s.textures.size())).second) s.textures.push_back(std::move(tx));
            } catch (const FormatError& e) {
                ++s.stats.textureErrors;
                texErrors.push_back(std::format("texture {}: {}", id, e.what()));
            }
        }
    }

    // Chunk 0x22 assets: tag 0x27 {u16 role, u16 id} + tag 0x24/0x40 model or 0x25 TOD {u32 off, u32 size}.
    struct Asset { uint16_t role = 0; uint8_t tag = 0; uint32_t off = 0, size = 0; };
    std::map<uint16_t, Asset> all;
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> assets;  // models only (props)
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || it.code != 0x22) continue;
        const auto id = tagData(it, 0x27);
        if (id.empty()) continue;
        const uint16_t aid = uint16_t(id[2] | id[3] << 8);
        for (uint8_t tg : {0x24, 0x40, 0x25})
            if (const auto d = tagData(it, tg); !d.empty()) all[aid] = {uint16_t(id[0] | id[1] << 8), tg, le32(d, 0), le32(d, 4)};
        auto mdl = tagData(it, 0x24);
        if (mdl.empty()) mdl = tagData(it, 0x40);
        if (!mdl.empty()) assets[aid] = {le32(mdl, 0), le32(mdl, 4)};
    }

    // Texture links (TEXTURE_GAP.md): entity tag 0x0b {u16 link id, 0} writes the current frame of a sprite sequence
    // (tag-0x40 asset, tex id @+10 per record) into alias[link id] at run time (FUN_004247a0 at load, FUN_00438830 per
    // frame). Type 20 (FUN_00445500): the first listed asset of class 8, start frame +0x50 and hold byte +0x54 (box
    // tags 0x1c/0x42, last writer wins), +1 frame every `hold` ticks (0 = 256: u8 countdown wrap), wrap after the last
    // record. Type 2 and others: the latest sprite asset listed before tag 0x0b, frames from the script animation
    // (0x438970): the start step's animation id is the role of a listed asset whose tag 0x3f flipbook list
    // {u16 n, u16 0, u32 off, u32 size} -> n x {u8 record, u8 ticks} picks the record and its duration; looped.
    // Runtime-verified on L02A1 links 16/63 (alias table traces, GAMEPLAY_STATUS.md). Without such a list the
    // records are cycled in order, 2 ticks per frame (Inferred).
    std::map<uint16_t, std::pair<uint16_t, std::vector<uint16_t>>> sprites;  // asset id -> (class, frame tex ids)
    for (const auto& [aid, a] : all) {
        if (a.tag != 0x40) continue;
        auto& [cls, ids] = sprites[aid];
        cls = a.role;
        // Scene::sprites: the type-4 path (0x44c170 -> GL 0x40a6c0) draws record k's whole TIM; ABR as world faces.
        std::vector<SpriteFrame>& frames = s.sprites[aid];
        for (const SpriteRecord& r : spriteRecords(s4, a.off, a.size)) {
            ids.push_back(r.tex);
            SpriteFrame& f = frames.emplace_back();
            if (const auto x = b.tex.find(r.tex); x != b.tex.end()) f.texture = x->second;
            if (r.flags & 8) {
                const uint8_t abr = r.tpage >> 5 & 3;
                f.blend = abr == 0 ? Blend::Alpha : abr == 2 ? Blend::Subtract : Blend::Additive;
                f.alpha = abr == 0 ? 128 : abr == 3 ? 64 : 255;
            } else if (f.texture >= 0 && s.textures[size_t(f.texture)].hasAlpha) {
                f.blend = Blend::AlphaTest;
            }
        }
    }
    // Overlay font: the first class-8 sprite asset listed (tag 0x0f) by template entity 68 (0x44ccd0 / 0x44e310 take
    // the actor's first asset with +0x4e == 8). Every record is a whole TIM at u, v = 0 (63/63 levels, Inferred
    // from the data: w, h = TIM size).
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || it.code != 0x08 || !it.entityId || *it.entityId != 68) continue;
        for (const ScriptTag& t : it.tags)
            if (auto sp = t.id == 0x0f ? sprites.find(uint16_t(t.data[0] | t.data[1] << 8)) : sprites.end();
                sp != sprites.end() && sp->second.first == 8 && s.overlayFont.empty())
                for (const uint16_t id : sp->second.second) {
                    const auto tx = b.tex.find(id);
                    s.overlayFont.push_back(tx == b.tex.end() ? -1 : tx->second);
                }
        break;
    }
    std::map<uint16_t, std::pair<uint16_t, std::span<const uint8_t>>> flipbooks;  // asset id -> (role, {record, ticks} pairs)
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || it.code != 0x22) continue;
        const auto id = tagData(it, 0x27), fl = tagData(it, 0x3f);
        if (id.empty() || fl.size() < 8) continue;
        const uint32_t n = uint32_t(fl[0] | fl[1] << 8), off = le32(fl, 4);
        if (off > s4.size() || s4.size() - off < size_t(n) * 2) throw FormatError(std::format("flipbook list 0x{:x} outside section 4", off));
        flipbooks[uint16_t(id[2] | id[3] << 8)] = {uint16_t(id[0] | id[1] << 8), std::span(s4).subspan(off, size_t(n) * 2)};
    }
    // The entity's start animation (state id 2 else 1 -> first step -> tag-0x30 anim id) as a flipbook list.
    auto startFlipbook = [&](const ScriptItem& it) -> std::span<const uint8_t> {
        std::optional<uint16_t> step;
        for (uint16_t want : {2, 1})
            for (const ScriptTag& t : it.tags)
                if (!step && t.id == 0x32 && (t.data[0] | t.data[1] << 8) == want) step = uint16_t(t.data[2] | t.data[3] << 8);
        for (const ScriptTag& t : it.tags) {
            if (!step || t.id != 0x30 || (t.data[0] | t.data[1] << 8) != *step) continue;
            const uint16_t anim = uint16_t(t.data[2] | t.data[3] << 8);
            for (const ScriptTag& r : it.tags)
                if (auto f = r.id == 0x0f ? flipbooks.find(uint16_t(r.data[0] | r.data[1] << 8)) : flipbooks.end();
                    f != flipbooks.end() && f->second.first == anim)
                    return f->second.second;
        }
        return {};
    };
    uint32_t linkFrameErrors = 0;
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || (it.code != 0x07 && it.code != 0x08 && it.code != 0x0a)) continue;
        const auto type = tagData(it, 0x13);
        const std::vector<uint16_t>* last = nullptr;
        std::span<const uint8_t> box;  // +0x50/+0x54: last of tags 0x1c / 0x42 (may follow tag 0x0b)
        for (const ScriptTag& t : it.tags)
            if (t.id == 0x1c || t.id == 0x42) box = t.data;
        for (const ScriptTag& t : it.tags) {
            if (t.id == 0x0f) {
                if (auto sp = sprites.find(uint16_t(t.data[0] | t.data[1] << 8)); sp != sprites.end()) last = &sp->second.second;
                continue;
            }
            if (t.id != 0x0b || !last) continue;
            const bool flip = !type.empty() && type[0] == 20;
            const std::vector<uint16_t>* src = flip ? nullptr : last;
            for (const ScriptTag& a : it.tags) {
                if (!flip || src || a.id != 0x0f) continue;
                if (auto sp = sprites.find(uint16_t(a.data[0] | a.data[1] << 8)); sp != sprites.end() && sp->second.first == 8)
                    src = &sp->second.second;
            }
            if (!src) continue;
            TextureAnim an;
            an.linkId = uint16_t(t.data[0] | t.data[1] << 8);
            for (uint16_t fid : *src)
                if (auto x = b.tex.find(fid); x != b.tex.end()) an.frames.push_back(x->second);
                else ++linkFrameErrors;
            if (an.frames.empty()) continue;
            if (flip) {
                an.startFrame = box.empty() ? 0 : uint32_t(box[0] | box[1] << 8) % an.frames.size();
                an.holdTicks = box.empty() || !box[4] ? 256 : box[4];
            } else if (const auto fl = startFlipbook(it); !fl.empty()) {
                std::vector<int32_t> seq;
                for (size_t k = 0; k + 1 < fl.size(); k += 2)
                    seq.insert(seq.end(), std::max<size_t>(fl[k + 1], 1), an.frames[fl[k] % an.frames.size()]);
                an.frames = std::move(seq);
                an.holdTicks = 1;
            } else {
                an.holdTicks = 2;
                an.timingInferred = true;
            }
            if (auto [x, fresh] = b.links.try_emplace(an.linkId, int32_t(s.textureAnims.size())); fresh) s.textureAnims.push_back(std::move(an));
            else s.textureAnims[size_t(x->second)] = std::move(an);  // several writers: the last entity wins (Inferred)
        }
    }
    std::map<std::vector<uint16_t>, uint32_t> modelOf;  // tag 0x0f list -> Scene::actorModels index
    std::vector<uint16_t> idleOf;                       // per actor model: initial animation id

    uint32_t group = 0;
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk) continue;
        if (it.code == 0x20) {  // world model: v * scale.x + translation (MODELS.md §1); rotation identity in all data
            const auto mdl = tagData(it, 0x24);
            if (mdl.empty()) continue;
            if (it.rotation && std::ranges::any_of(*it.rotation, [](int16_t a) { return a % 0x1000 != 0; }))
                throw FormatError(std::format("world chunk at 0x{:x}: non-identity rotation unsupported", it.offset));
            Xform xf;
            if (it.scale) xf.s[0] = xf.s[1] = xf.s[2] = float((*it.scale)[0]) / 4096.0f;  // X word for all axes
            if (it.position) for (int k = 0; k < 3; ++k) xf.t[k] = float((*it.position)[size_t(k)]);
            ++s.stats.worldModels;
            b.model(s4, le32(mdl, 0), le32(mdl, 4), xf, group, true, std::format("world{}", s.stats.worldModels));
        } else if (it.code == 0x07 || it.code == 0x08 || it.code == 0x0a) {
            // Entity positions are in world units (not 20.12): L01a player (23700,-640,8830) lies inside the world bounds.
            // No position tag = origin (templates, and e.g. L01A's type-2 ambient actors whose TOD clips hold frame 0).
            const std::array<int32_t, 3> pos = it.position ? *it.position : std::array<int32_t, 3>{};
            const float yaw = it.rotation ? angle((*it.rotation)[1]) : 0.0f;
            if (it.player && !s.spawn) {
                s.spawn = std::array<float, 3>{float(pos[0]), -float(pos[1]), -float(pos[2])};
                s.spawnYaw = -yaw;  // PS1 Ry forward (sin, 0, cos) -> engine Camera yaw = -angle (Inferred)
            }
            // An entity lists its assets in repeated tag 0x0f (TODs plus models), in list order (0x448950 appends).
            std::vector<uint16_t> refs;
            for (const ScriptTag& t : it.tags)
                if (t.id == 0x0f) refs.push_back(uint16_t(t.data[0] | t.data[1] << 8));
            // TOD actor (0x4393e0): first role-7 model + first role-4 skeleton of the list; flipbook actors (tag 0x16
            // flags8 0x200000) play role-8 sprites instead and are not handled here.
            const auto flags8 = tagData(it, 0x16);
            const bool flipbook = !flags8.empty() && (le32(flags8, 0) & 0x200000);
            auto first = [&](uint16_t role, bool tod) -> const uint16_t* {
                for (const uint16_t& r : refs)
                    if (auto a = all.find(r); a != all.end() && a->second.role == role && (a->second.tag == 0x25) == tod) return &r;
                return nullptr;
            };
            const uint16_t *mref = first(7, false), *sref = first(4, true);
            if (mref && sref && !flipbook) {
                auto [mo, fresh] = modelOf.try_emplace(refs, uint32_t(s.actorModels.size()));
                if (fresh) {
                    ActorModel& am = s.actorModels.emplace_back();
                    am.modelAsset = *mref, am.skeletonAsset = *sref;
                    am.skeleton = parseTod(s4, all[*sref].off);
                    for (uint16_t r : refs)
                        if (const Asset& a = all[r]; a.tag == 0x25 && r != *sref) am.anims.push_back({a.role, parseTod(s4, a.off)});
                    b.actor = &am;
                    uint32_t objGroup = 0;
                    b.model(s4, all[*mref].off, all[*mref].size, Xform{}, objGroup, false, std::format("actor{}", *mref));
                    b.actor = nullptr;
                    // Initial animation (Inferred; the real one comes from the behaviour's start state): the first
                    // TOD in list order whose first frame shows any geometry (many lists start with a hide-all anim).
                    idleOf.push_back(am.anims.empty() ? 0 : am.anims[0].id);
                    for (const ActorAnim& an : am.anims) {
                        AnimState st;
                        animTick(am, st, an.id, AnimLoop);
                        if (std::ranges::any_of(poseSlots(am, st), [](const auto& v) { return !std::isnan(v[0]); })) {
                            idleOf.back() = an.id;
                            break;
                        }
                    }
                }
                ActorInstance& ai = s.actors.emplace_back();
                ai.model = mo->second;
                ai.pos = pos;
                if (it.rotation) ai.rot = *it.rotation;
                if (it.scale) ai.scale = *it.scale;
                // Start state (LOGIC.md §2, 0x4455e0): tag-0x32 state id 2, else 1 -> its first step id -> the
                // tag-0x30 step / 0x34 player move with that id -> anim id (both payloads: u16 id, u16 anim).
                // Player L01a: state 2 -> move 4 -> anim 35. Fallback: first visible TOD (Inferred).
                ai.anim = idleOf[mo->second];
                std::optional<uint16_t> step;
                for (uint16_t want : {2, 1})
                    for (const ScriptTag& t : it.tags)
                        if (!step && t.id == 0x32 && t.data.size() >= 4 && (t.data[0] | t.data[1] << 8) == want)
                            step = uint16_t(t.data[2] | t.data[3] << 8);
                for (const ScriptTag& t : it.tags)
                    if (step && (t.id == 0x30 || t.id == 0x34) && t.data.size() >= 4 && (t.data[0] | t.data[1] << 8) == *step) {
                        const uint16_t anim = uint16_t(t.data[2] | t.data[3] << 8);
                        if (findAnim(s.actorModels[mo->second], anim) >= 0) ai.anim = anim;
                        break;
                    }
                ai.entityId = it.entityId.value_or(0);
                ai.chunk = it.code;
                ai.player = it.player;
                ai.scriptOffset = it.offset;
                continue;
            }
            // Otherwise placed as a static prop (Inferred): exactly one model with a single object, rest pose;
            // scale tag 0x12 per axis (20.12), rotation tag 0x11 Y only.
            const std::pair<uint32_t, uint32_t>* model = nullptr;
            int statics = 0;
            for (const ScriptTag& t : it.tags)
                if (t.id == 0x0f)
                    if (auto a = assets.find(uint16_t(t.data[0] | t.data[1] << 8)); a != assets.end()) ++statics, model = &a->second;
            if (statics != 1 || !it.position) continue;
            const uint32_t nobj = In{s4, "prop model"}.u32(size_t(model->first) + 8);
            if (nobj != 1) {
                s.stats.skippedActors += nobj > 1;  // nobj 0 = no geometry
                continue;
            }
            Xform xf;
            for (int k = 0; k < 3; ++k) {
                xf.t[k] = float(pos[size_t(k)]);
                if (it.scale) xf.s[k] = float((*it.scale)[size_t(k)]) / 4096.0f;
            }
            xf.cy = std::cos(yaw), xf.sy = std::sin(yaw);
            ++s.stats.props;
            uint32_t propGroup = 0x80000000u | s.stats.props;
            b.model(s4, model->first, model->second, xf, propGroup, false, std::format("prop{}", s.stats.props));
        }
    }

    // One-object template models, built as one-object actor models (posed with a single identity part by the app):
    // shadows (0x44bb50) take the template's first role-6 model; HUD templates (0x44b370, the actor's +0x74 model)
    // take their first static model (tag 0x24; role 7 in the data: digits list role-7 model + two TODs).
    constexpr size_t kHudTemplates = 28;
    std::vector<uint16_t> templates = {472, 473, 474, 475, 476, 477, 478, 479, 480, 481, 483, 485, 486, 487, 488,
                                       489, 490, 491, 492, 493, 494, 495, 496, 923, 924, 925, 961, 962};
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || (it.code != 0x07 && it.code != 0x08 && it.code != 0x0a)) continue;
        const auto t46 = tagData(it, 0x46);
        if (t46.size() >= 2) templates.push_back(uint16_t(t46[0] | t46[1] << 8));
    }
    for (size_t ti = 0; ti < templates.size(); ++ti) {
        const uint16_t sid = templates[ti];
        if (!sid || s.templateModels.contains(sid)) continue;
        for (const ScriptItem& e : script.items) {
            if (e.kind != ScriptItem::Kind::Chunk || e.code != 0x08 || e.entityId.value_or(0) != sid)  // template list 0x4b2390
                continue;
            for (const ScriptTag& t : e.tags) {
                const auto a = t.id == 0x0f ? all.find(uint16_t(t.data[0] | t.data[1] << 8)) : all.end();
                if (a == all.end() || !(ti < kHudTemplates ? a->second.tag == 0x24 : a->second.role == 6 && a->second.tag != 0x25))
                    continue;
                s.templateModels[sid] = uint32_t(s.actorModels.size());
                ActorModel& am = s.actorModels.emplace_back();
                am.modelAsset = a->first;
                b.actor = &am;
                uint32_t objGroup = 0;
                b.model(s4, a->second.off, a->second.size, Xform{}, objGroup, false, std::format("template{}", sid));
                b.actor = nullptr;
                break;
            }
            break;
        }
    }

    s.stats.missingTextureIds.assign(b.missing.begin(), b.missing.end());
    s.notes.push_back(std::format("{} world models, {} cells, {} rigid props (placement Inferred), {} TOD actors ({} models), "
                                  "{} multi-object entities skipped", s.stats.worldModels, s.stats.cells, s.stats.props,
                                  s.actors.size(), s.actorModels.size(), s.stats.skippedActors));
    if (!s.textureAnims.empty())
        s.notes.push_back(std::format("{} texture links (animated, TEXTURE_GAP.md) drive {} faces{}", s.textureAnims.size(),
                                      s.stats.linkedFaces,
                                      linkFrameErrors ? std::format("; {} link frames have no TIM", linkFrameErrors) : ""));
    if (s.stats.missingTextureFaces)
        s.notes.push_back(std::format("{} faces reference {} texture ids that are neither TIMs nor links: not drawn (as the "
                                      "original's alias 0)", s.stats.missingTextureFaces, s.stats.missingTextureIds.size()));
    for (const std::string& e : texErrors) s.notes.push_back(e);
    return s;
}

} // namespace bb
