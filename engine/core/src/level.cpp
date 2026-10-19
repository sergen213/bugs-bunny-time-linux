#include "bb/level.h"
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
    std::map<uint32_t, int32_t> tex;  // texture id -> Scene::textures index
    std::set<uint32_t> missing;
    std::map<std::tuple<uint32_t, int32_t, Blend, bool>, size_t> meshOf;

    Mesh& mesh(uint32_t group, const std::string& gname, int32_t t, Blend b, bool twoSided) {
        auto [it, fresh] = meshOf.try_emplace({group, t, b, twoSided}, s.meshes.size());
        if (fresh) {
            Mesh& m = s.meshes.emplace_back();
            m.name = std::format("{}/tex{}{}", gname, t, twoSided ? "/2s" : "");
            m.texture = t;
            m.blend = b;
            m.twoSided = twoSided;
        }
        return s.meshes[it->second];
    }

    // One model ("PC TMD", MODELS.md §2-6) at sec4[off, off+size). `group` names the mesh group; world models
    // advance it per cell header, props pass a unique value.
    void model(std::span<const uint8_t> sec4, uint32_t off, uint32_t size, const Xform& xf, uint32_t& group,
               bool world, const std::string& gname) {
        if (off > sec4.size() || sec4.size() - off < size)
            throw FormatError(std::format("model 0x{:x}+0x{:x} outside section 4 (0x{:x})", off, size, sec4.size()));
        const In in{sec4.subspan(off, size), "model"};
        if (in.u32(0) != 0x41) throw FormatError(std::format("model 0x{:x}: magic 0x{:x}", off, in.u32(0)));
        const uint32_t nobj = in.u32(8);
        in.need(12, size_t(nobj) * 28);
        std::vector<std::array<float, 3>> objv;  // concatenated object vertex tables
        for (uint32_t o = 0; o < nobj; ++o) {
            const size_t e = 12 + size_t(o) * 28, vt = 12 + size_t(in.u32(e)), nv = in.u32(e + 4);
            in.need(vt, nv * 16);
            for (size_t i = 0; i < nv; ++i) objv.push_back({in.f32(vt + i * 16), in.f32(vt + i * 16 + 4), in.f32(vt + i * 16 + 8)});
        }
        std::vector<std::array<float, 3>> block;
        std::string cellName = gname;
        for (uint32_t o = 0; o < nobj; ++o) {
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
        int32_t ti = -1;
        Blend blend = Blend::Opaque;
        uint8_t alpha = 255;
        if (k.tex) {
            const uint16_t id = in.u16(q + k.tex);
            if (auto it = tex.find(id); it != tex.end()) ti = it->second;
            else {
                ++s.stats.missingTextureFaces;
                missing.insert(id);
            }
            if (flags & 8) {  // semi-transparent: ABR from tpage bits 5-6
                switch (in.u8(q + k.tpage) >> 5 & 3) {
                case 0: blend = Blend::Alpha, alpha = 128; break;
                case 1: blend = Blend::Additive; break;
                case 2: blend = Blend::Subtract; break;
                case 3: blend = Blend::Additive, alpha = 64; break;
                }
            } else if (ti >= 0 && s.textures[size_t(ti)].hasAlpha) blend = Blend::AlphaTest;
        }
        const float tw = ti >= 0 ? float(s.textures[size_t(ti)].width) : 1.0f;
        const float th = ti >= 0 ? float(s.textures[size_t(ti)].height) : 1.0f;
        Mesh& m = mesh(group, gname, ti, blend, twoSided);
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
        }
        // Quads use PS1 "Z" order: outline v0 v1 v3 v2 -> triangles (0,1,2) + (1,3,2). In engine space the data's
        // visible side has that order clockwise (checked visually on L01a: with this order reversed, plateau tops
        // are visible from above and cliff faces from the canyon), so emit reversed to keep Mesh's CCW-front rule.
        m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
        if (k.n == 4) m.indices.insert(m.indices.end(), {base + 1, base + 2, base + 3});
        ++s.stats.faces;
        s.stats.triangles += k.n - 2;
    }
};

} // namespace

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
    Builder b{s, {}, {}, {}};

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

    // Static prop assets: chunk 0x22, tag 0x27 (u16 asset id at +2) + tag 0x24/0x40 model.
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> assets;
    for (const ScriptItem& it : script.items) {
        if (it.kind != ScriptItem::Kind::Chunk || it.code != 0x22) continue;
        const auto id = tagData(it, 0x27);
        auto mdl = tagData(it, 0x24);
        if (mdl.empty()) mdl = tagData(it, 0x40);
        if (!id.empty() && !mdl.empty()) assets[uint16_t(id[2] | id[3] << 8)] = {le32(mdl, 0), le32(mdl, 4)};
    }

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
        } else if ((it.code == 0x07 || it.code == 0x08 || it.code == 0x0a) && it.position) {
            // Entity positions are in world units (not 20.12): L01a player (23700,-640,8830) lies inside the world bounds.
            const auto& pos = *it.position;
            const float yaw = it.rotation ? angle((*it.rotation)[1]) : 0.0f;
            if (it.player && !s.spawn) {
                s.spawn = std::array<float, 3>{float(pos[0]), -float(pos[1]), -float(pos[2])};
                s.spawnYaw = -yaw;  // PS1 Ry forward (sin, 0, cos) -> engine Camera yaw = -angle (Inferred)
            }
            // An entity lists its assets in repeated tag 0x0f (mostly 0x25 animation blocks plus one static model).
            // Placed here (Inferred): entities with exactly one static model that has a single object, drawn in
            // rest pose; scale tag 0x12 per axis (20.12), rotation tag 0x11 Y only. Multi-object (skinned) actors
            // need the 0x25 data: Research Required.
            const std::pair<uint32_t, uint32_t>* model = nullptr;
            int statics = 0;
            for (const ScriptTag& t : it.tags)
                if (t.id == 0x0f)
                    if (auto a = assets.find(uint16_t(t.data[0] | t.data[1] << 8)); a != assets.end()) ++statics, model = &a->second;
            if (statics != 1) continue;
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

    s.stats.missingTextureIds.assign(b.missing.begin(), b.missing.end());
    s.notes.push_back(std::format("{} world models, {} cells, {} rigid props (placement Inferred), {} skinned actors skipped "
                                  "(Research Required)", s.stats.worldModels, s.stats.cells, s.stats.props, s.stats.skippedActors));
    if (s.stats.missingTextureFaces)
        s.notes.push_back(std::format("{} faces reference {} texture ids absent from this level: drawn untextured",
                                      s.stats.missingTextureFaces, s.stats.missingTextureIds.size()));
    for (const std::string& e : texErrors) s.notes.push_back(e);
    return s;
}

std::vector<std::pair<uint32_t, uint32_t>> tbTextureRefs(std::span<const uint8_t> s1) {
    // ponytail: TB tag tables are not known yet (BZE.md); locate the single texture chunk `2c 28 (29 <8>)* 2d`
    // by pattern (present exactly once in all 58 TB levels). Replace with the TB level-script walker.
    static constexpr uint8_t kPat[] = {0x2c, 0x28, 0x29};
    const auto hit = std::ranges::search(s1, kPat);
    std::vector<std::pair<uint32_t, uint32_t>> refs;
    if (hit.empty()) return refs;
    size_t p = size_t(hit.begin() - s1.begin()) + 2;
    const In in{s1, "tb texture list"};
    while (in.u8(p) == 0x29) {
        refs.emplace_back(in.u32(p + 1), in.u32(p + 5) & 0xfff);
        p += 9;
    }
    if (s1[p] != 0x2d) throw FormatError(std::format("tb texture list: expected chunk end 0x2d at 0x{:x}", p));
    return refs;
}

std::vector<Texture> decodeTbTextures(const BzeFile& bze, const std::string& level, SceneStats* stats) {
    if (bze.variant != BzeVariant::TimeBusters) throw FormatError("decodeTbTextures: not a Time Busters BZE");
    const int i1 = bze.find(1), i3 = bze.find(3);
    if (i1 < 0) throw FormatError("decodeTbTextures: no section 1");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const std::vector<uint8_t> s3 = i3 >= 0 ? bze.payload(size_t(i3)) : std::vector<uint8_t>{};
    std::vector<Texture> out;
    for (auto [off, id] : tbTextureRefs(s1)) {
        if (stats) ++stats->textureRefs;
        try {
            out.push_back(decodeTim(s3, off, std::format("{}/tex/{}", level, id)));
        } catch (const FormatError&) {
            if (stats) ++stats->textureErrors;
        }
    }
    return out;
}

} // namespace bb
