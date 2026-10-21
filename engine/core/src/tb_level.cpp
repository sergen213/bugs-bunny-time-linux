#include "bb/tb_level.h"
#include "bb/tim.h"

#include <algorithm>
#include <array>
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
    int16_t s16(size_t p) const { return int16_t(u16(p)); }
    uint32_t u32(size_t p) const { need(p, 4); return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
};

// ---------------------------------------------------------------- section 1 (TB_LEVELS.md §3)
struct Sz { uint8_t tag, size; };
using Table = std::array<int8_t, 256>;  // payload size per tag, -1 = no handler

constexpr Sz kLogic[] = {{0x4d, 2}, {0x4e, 4}, {0x4f, 8}, {0x50, 8}, {0x51, 8}, {0x52, 8}, {0x53, 6},
                         {0x54, 12}, {0x55, 12}, {0x56, 16}, {0x57, 20}, {0x58, 8}, {0x59, 4}, {0x5a, 8},
                         {0x5b, 8}, {0x5c, 2}, {0x5d, 8}, {0x5e, 12}, {0x5f, 16}, {0x60, 8}};
constexpr Sz kEnt[] = {{0x0b, 4}, {0x0c, 4}, {0x0f, 2}, {0x10, 12}, {0x11, 6}, {0x12, 12}, {0x13, 1}, {0x15, 1},
                       {0x16, 28}, {0x19, 4}, {0x1a, 4}, {0x1b, 6}, {0x1c, 8}, {0x1d, 4}, {0x1e, 2}, {0x1f, 1},
                       {0x27, 4}, {0x2f, 48}, {0x31, 12}, {0x37, 10}, {0x38, 2}, {0x39, 12}, {0x40, 20},
                       {0x41, 6}, {0x45, 2}, {0x61, 14}, {0x62, 0}, {0x65, 2}};
constexpr Sz kAudio[] = {{0x01, 8}, {0x02, 8}, {0x04, 12}, {0x43, 12}, {0x4b, 56}, {0x4c, 56}};
constexpr Sz kMusic[] = {{0x06, 3}};
constexpr Sz kWorld[] = {{0x10, 12}, {0x11, 6}, {0x12, 12}, {0x21, 4}, {0x24, 8}, {0x38, 2}, {0x42, 1}, {0x62, 0}};
constexpr Sz kAsset[] = {{0x0d, 12}, {0x24, 8}, {0x25, 8}, {0x26, 4}, {0x27, 4}, {0x3e, 12}, {0x3f, 8}};
constexpr Sz kTex[] = {{0x29, 8}};
constexpr Sz k35[] = {{0x36, 8}};
constexpr Sz kParams[] = {{0x2a, 8}, {0x3c, 14}, {0x3d, 36}, {0x66, 3}};
constexpr Sz k46[] = {{0x47, 8}};
constexpr Sz kLang[] = {{0x49, 56}, {0x68, 56}};

constexpr Table table(std::span<const Sz> a, std::span<const Sz> b = {}) {
    Table t{};
    t.fill(-1);
    for (Sz s : a) t[s.tag] = int8_t(s.size);
    for (Sz s : b) t[s.tag] = int8_t(s.size);
    return t;
}
constexpr Table tEnt = table(kEnt, kLogic), tAudio = table(kAudio), tMusic = table(kMusic), tWorld = table(kWorld),
                tAsset = table(kAsset, kLogic), tTex = table(kTex), t35 = table(k35), tParams = table(kParams),
                t46 = table(k46), tLang = table(kLang);

const Table* tagTable(uint8_t type) {
    switch (type) {
    case 0x00: return &tAudio;
    case 0x05: return &tMusic;
    case 0x07: case 0x08: case 0x09: case 0x0a: case 0x63: return &tEnt;
    case 0x20: return &tWorld;
    case 0x22: return &tAsset;
    case 0x28: return &tTex;
    case 0x35: return &t35;
    case 0x3a: return &tParams;
    case 0x46: return &t46;
    case 0x48: return &tLang;
    default: return nullptr;
    }
}

uint32_t le32(std::span<const uint8_t> b, size_t p) { return uint32_t(b[p] | b[p + 1] << 8 | b[p + 2] << 16 | uint32_t(b[p + 3]) << 24); }
uint16_t le16(std::span<const uint8_t> b, size_t p) { return uint16_t(b[p] | b[p + 1] << 8); }
// Entity tags 0x10/0x12: three i32 fields of which the game reads the low s16 halves (TB_LEVELS.md §3.3).
std::array<float, 3> lowHalves(std::span<const uint8_t> b) {
    return {float(int16_t(le16(b, 0))), float(int16_t(le16(b, 4))), float(int16_t(le16(b, 8)))};
}

// ---------------------------------------------------------------- geometry (MODELS.md §10)
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

// FUN_00405c90 CheckPrimValidity.
bool primValid(uint8_t b0, uint8_t b1) {
    return (b0 & 4) && (b0 & 0x20) && !(b0 & 0x40) && !(!(b0 & 8) && (b1 & 0x20)) && !((b1 & 0x40) && (b0 & 2));
}

// b1 bit 6 (no UVs): i0..i3 get (0,0) (1,0) (1,1) (0,1) [GLQ], in UV-byte units.
constexpr uint8_t kNoUv[4][2] = {{0, 0}, {255, 0}, {255, 255}, {0, 255}};

using Vec3 = std::array<float, 3>;

struct Builder {
    Scene& s;
    std::map<uint32_t, int32_t> tex;    // texture id -> Scene::textures index
    std::map<uint32_t, int32_t> links;  // texture link id -> Scene::textureAnims index (TEXTURE_GAP.md)
    std::set<uint32_t> missing;
    std::map<std::tuple<uint32_t, int32_t, Blend, bool>, size_t> meshOf;
    uint32_t nextGroup = 0;
    bool world = false;       // building a chunk-0x20 world model (stats split)
    uint64_t lodFaces = 0;    // tree C (far LOD) faces: decoded, not drawn
    std::vector<Vec3> verts;  // current leaf / object table, transformed

    // Face texture id -> texture index (>= 0), texture link (-2 - anim index), or -1 = alias 0. The original resolves
    // ids through the alias table DAT_004d038c and skips prims whose alias is 0 (TEXTURE_GAP.md §1); counted here.
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

    const Texture& fallback(int32_t key) const {
        return s.textures[size_t(key >= 0 ? key : s.textureAnims[size_t(-2 - key)].frames[s.textureAnims[size_t(-2 - key)].startFrame])];
    }

    // `key` as returned by resolve(); a link mesh binds the link's start frame as its static fallback.
    Mesh& mesh(uint32_t group, const std::string& gname, int32_t key, Blend b, bool twoSided) {
        auto [it, fresh] = meshOf.try_emplace({group, key, b, twoSided}, s.meshes.size());
        if (fresh) {
            Mesh& m = s.meshes.emplace_back();
            const int32_t anim = key >= 0 ? -1 : -2 - key;
            m.name = anim < 0 ? std::format("{}/tex{}{}", gname, key, twoSided ? "/2s" : "")
                              : std::format("{}/link{}{}", gname, s.textureAnims[size_t(anim)].linkId, twoSided ? "/2s" : "");
            m.texture = int32_t(&fallback(key) - s.textures.data());
            m.textureAnim = anim;
            m.blend = b;
            m.twoSided = twoSided;
        }
        return s.meshes[it->second];
    }

    bool hasAlpha(int32_t key) const {
        if (key >= 0) return s.textures[size_t(key)].hasAlpha;
        return std::ranges::any_of(s.textureAnims[size_t(-2 - key)].frames, [&](int32_t f) { return s.textures[size_t(f)].hasAlpha; });
    }

    void checkIndices(const uint32_t* idx, int n, const std::string& gname, size_t r) const {
        for (int k = 0; k < n; ++k)
            if (idx[k] >= verts.size())
                throw FormatError(std::format("{}: vertex index {} >= {} in primitive at 0x{:x}", gname, idx[k], verts.size(), r));
    }

    // One primitive: record at r (b0 @0, b1 @1, tex @2, colours @4..), vertex indices idx[0..n), UV byte pairs
    // at uvAt[k] (nullptr = b1 bit 6 fixed UVs), tpage byte at tpageAt. Opaque meshes share `group`;
    // translucent ones use `sortGroup` so the renderer can sort them by a small centre.
    void prim(const In& in, size_t r, int n, const uint32_t* idx, const size_t* uvAt, size_t tpageAt, uint32_t group,
              uint32_t sortGroup, const std::string& gname) {
        const uint8_t b0 = in.u8(r), b1 = in.u8(r + 1);
        const int32_t key = resolve(in.u16(r + 2) & 0xfff);
        checkIndices(idx, n, gname, r);
        if (key == -1) return;  // alias 0: not drawn
        Blend blend = Blend::Opaque;
        uint8_t alpha = 255;
        if (b0 & 2) {  // semi-transparent: ABR from the tpage byte bits 5-6 (same mapping as LiT, scene.h)
            switch (in.u8(tpageAt) >> 5 & 3) {
            case 0: blend = Blend::Alpha, alpha = 128; break;
            case 1: blend = Blend::Additive; break;
            case 2: blend = Blend::Subtract; break;
            case 3: blend = Blend::Additive, alpha = 64; break;
            }
        } else if (hasAlpha(key)) blend = Blend::AlphaTest;
        const bool translucent = blend != Blend::Opaque && blend != Blend::AlphaTest;
        // b1 bit 4 -> render flag 0x80: the screen-space back-face test is skipped (FUN_0041eee0).
        Mesh& m = mesh(translucent ? sortGroup : group, gname, key, blend, b1 & 0x10);
        const float tw = float(fallback(key).width), th = float(fallback(key).height);
        const bool gouraud = b0 & 0x10;  // else colour 0 for every vertex [COL]
        const uint32_t base = uint32_t(m.vertices.size());
        for (int k = 0; k < n; ++k) {
            Vertex v{};
            std::copy(verts[idx[k]].begin(), verts[idx[k]].end(), v.pos);
            const uint8_t u = uvAt ? in.u8(uvAt[k]) : kNoUv[k][0], w = uvAt ? in.u8(uvAt[k] + 1) : kNoUv[k][1];
            // Same texel convention as LiT (level.cpp): uv/255 over size-1, v measured from the bottom row.
            v.uv[0] = (float(u) / 255.0f * (tw - 1.0f) + 0.5f) / tw;
            v.uv[1] = ((1.0f - float(w) / 255.0f) * (th - 1.0f) + 0.5f) / th;
            const size_t c = r + 4 + (gouraud ? size_t(k) * 4 : 0);
            v.rgba[0] = in.u8(c), v.rgba[1] = in.u8(c + 1), v.rgba[2] = in.u8(c + 2), v.rgba[3] = alpha;
            m.vertices.push_back(v);
        }
        // PS1 Z order quads (outline i0 i1 i3 i2): triangles (0,1,2) + (1,3,2), emitted reversed as in LiT.
        m.indices.insert(m.indices.end(), {base, base + 2, base + 1});
        if (n == 4) m.indices.insert(m.indices.end(), {base + 1, base + 2, base + 3});
        ++s.stats.faces;
        s.stats.triangles += uint32_t(n - 2);
    }

    // Octree model (MODELS.md §10.1): trees A (main), B (alt) and C (detail), 8-byte s16 vertices per leaf and
    // 28-byte primitives with a shared 12-byte UV table.
    void octree(std::span<const uint8_t> m, const Xform& xf, const std::string& gname) {
        const In in{m, "octree model"};
        const uint32_t uvTab = in.u32(8), group = nextGroup++;
        std::vector<bool> seen(m.size());
        const auto walk = [&](this const auto& self, size_t node, size_t root, int depth, bool draw) -> void {
            if (depth > 16) throw FormatError(std::format("{}: octree deeper than 16 at 0x{:x}", gname, node));
            in.need(node, 0x14);
            if (seen[node]) throw FormatError(std::format("{}: octree node 0x{:x} reached twice", gname, node));
            seen[node] = true;
            const uint8_t kind = in.u8(node + 2);
            if (kind == 1) {
                for (size_t k = 0; k < 8; ++k)
                    if (const uint16_t c = in.u16(node + 0x10 + 2 * k)) self(root + size_t(c) * 4, root, depth + 1, draw);
                return;
            }
            if (kind != 0) throw FormatError(std::format("{}: octree node 0x{:x} kind {}", gname, node, kind));
            const uint16_t nv = in.u16(node + 0x10), np = in.u16(node + 0x12);
            in.need(node + 0x14, size_t(nv) * 8 + size_t(np) * 28);
            verts.resize(nv);
            for (size_t i = 0; i < nv; ++i) {
                const size_t q = node + 0x14 + i * 8;
                verts[i] = xf.apply(in.s16(q), in.s16(q + 2), in.s16(q + 4));
            }
            const uint32_t sortGroup = nextGroup++;
            for (size_t i = 0; i < np; ++i) {
                const size_t r = node + 0x14 + size_t(nv) * 8 + i * 28;
                const uint8_t b0 = in.u8(r), b1 = in.u8(r + 1);
                if (!primValid(b0, b1)) {
                    ++s.stats.invalidPrims;
                    continue;
                }
                const int n = b0 & 8 ? 4 : 3;
                const uint32_t idx[4] = {in.u8(r + 0x14), in.u8(r + 0x15), in.u8(r + 0x16), in.u8(r + 0x17)};
                size_t uv[4] = {}, tpage = r;  // tpage only read for semi-transparent prims, which always have UVs
                if (!(b1 & 0x40)) {
                    if (!uvTab) throw FormatError(std::format("{}: primitive at 0x{:x} needs the absent UV table", gname, r));
                    const size_t e = size_t(uvTab) + in.u16(r + 0x1a);
                    in.need(e, 12);
                    uv[0] = e, uv[1] = e + 4, uv[2] = e + 8, uv[3] = e + 10, tpage = e + 6;
                }
                ++s.stats.worldFaces;
                if (draw) prim(in, r, n, idx, b1 & 0x40 ? nullptr : uv, tpage, group, sortGroup, gname);
                else {
                    resolve(in.u16(r + 2) & 0xfff);  // counted like drawn faces (TEXTURE_GAP.md counts all trees)
                    checkIndices(idx, n, gname, r);
                    ++lodFaces;
                }
            }
        };
        // Tree A always; tree B (alt) always; tree C (detail) is a far LOD that FUN_00406840 draws only beyond the
        // near draw distance and FUN_00404f40 builds only when the DETAILS setting is < 2. At DETAILS 2 (FUN_00409840:
        // near = far) tree A covers the whole range, so tree C is decoded and checked here but not drawn.
        walk(0x18, 0x18, 0, true);
        if (const uint32_t h = in.u32(0)) walk(size_t(h) + 0x18, size_t(h) + 0x18, 0, true);
        if (const uint32_t h = in.u32(4)) walk(size_t(h) + 0x18, size_t(h) + 0x18, 0, false);
    }

    // Actor model (MODELS.md §10.2) with exactly one object: rest pose, transformed by xf.
    void actor(std::span<const uint8_t> m, const Xform& xf, const std::string& gname) {
        const In in{m, "actor model"};
        const size_t ph = in.u32(4), vt = 8 + size_t(in.u32(8)), nv = in.u32(12);
        in.need(vt, nv * 8);
        verts.resize(nv);
        for (size_t i = 0; i < nv; ++i) verts[i] = xf.apply(in.s16(vt + i * 8), in.s16(vt + i * 8 + 2), in.s16(vt + i * 8 + 4));
        const size_t nC = in.u16(ph), nD = in.u16(ph + 2), nA = in.u8(ph + 4), nB = in.u8(ph + 5);
        const size_t oB = in.u16(ph + 6), oC = in.u16(ph + 8), oD = in.u16(ph + 10);
        const uint32_t group = nextGroup++;
        const std::tuple<size_t, size_t, size_t, int> groups[] = {{0xc, nA, 0x24, 3}, {oB, nB, 0x24, 4}, {oC, nC, 0x28, 3}, {oD, nD, 0x28, 4}};
        for (auto [off, cnt, size, n] : groups) {
            in.need(ph + off, cnt * size);
            for (size_t i = 0; i < cnt; ++i) {
                const size_t r = ph + off + i * size;
                const uint8_t b1 = in.u8(r + 1);
                if (bool(b1 & 0x80) != (size == 0x28))  // u16 indices exactly in the 0x28-byte records
                    throw FormatError(std::format("{}: primitive at 0x{:x}: index width flag does not match record size", gname, r));
                uint32_t idx[4];
                for (size_t k = 0; k < 4; ++k) idx[k] = b1 & 0x80 ? in.u16(r + 0x20 + 2 * k) : in.u8(r + 0x20 + k);
                const size_t uv[4] = {r + 0x14, r + 0x18, r + 0x1c, r + 0x1e};
                prim(in, r, n, idx, b1 & 0x40 ? nullptr : uv, r + 0x1a, group, group, gname);
            }
        }
    }
};

std::span<const uint8_t> region(std::span<const uint8_t> sec, std::span<const uint8_t> offSize, uint32_t secId) {
    const uint32_t off = le32(offSize, 0), size = le32(offSize, 4);
    if (off > sec.size() || sec.size() - off < size)
        throw FormatError(std::format("block 0x{:x}+0x{:x} outside section {} (0x{:x})", off, size, secId, sec.size()));
    return sec.subspan(off, size);
}

// Chunk 0x22 tag 0x24 dispatch [TL]: (u32 @+0x18 & 0xffffff) == 0x1ffff -> octree, else actor format.
bool isOctree(std::span<const uint8_t> m) { return m.size() >= 0x1c && (le32(m, 0x18) & 0xffffff) == 0x1ffff; }

} // namespace

std::span<const uint8_t> TbChunk::tag(uint8_t id) const {
    for (const TbTag& t : tags)
        if (t.id == id) return t.data;
    return {};
}

TbScript parseTbScript(std::span<const uint8_t> d, uint32_t languageSection) {
    const In in{d, "tb section 1"};
    TbScript out;
    size_t p = 0;
    if (d.size() >= 2 && d[0] == 0x2c && d[1] == 0x44) {  // pseudo-chunk 2c (44 xx)* 2d (never in shipped data)
        for (p = 1; in.u8(p) == 0x44; p += 2) {}
        if (in.u8(p) != 0x2d) throw FormatError(std::format("tb section 1: pseudo-chunk byte 0x{:02x} at 0x{:x}", d[p], p));
        ++p;
    }
    uint32_t section = 3;
    bool pools = false;
    for (;;) {
        const uint8_t c = in.u8(p);
        if (c == 0x2e) {
            out.end = uint32_t(p);
            return out;
        }
        if (c == 0x2b) {
            if (!pools) throw FormatError(std::format("tb section 1: command 0x2b at 0x{:x} before pool sizes (0x3a/0x3d)", p));
            section = 4, ++p;
            continue;
        }
        if (c == 0x4a) {
            section = languageSection, ++p;
            continue;
        }
        if (c != 0x2c) throw FormatError(std::format("tb section 1: top-level byte 0x{:02x} at 0x{:x}", c, p));
        TbChunk& ch = out.chunks.emplace_back();
        ch.offset = uint32_t(p);
        ch.type = in.u8(p + 1);
        ch.section = section;
        const Table* t = tagTable(ch.type);
        if (!t) throw FormatError(std::format("tb section 1: chunk type 0x{:02x} at 0x{:x}", ch.type, p + 1));
        for (p += 2;;) {
            const uint8_t tag = in.u8(p);
            if (tag == 0x2d) break;
            const int n = (*t)[tag];
            if (n < 0) throw FormatError(std::format("tb section 1: tag 0x{:02x} in chunk 0x{:02x} at 0x{:x}", tag, ch.type, p));
            in.need(p + 1, size_t(n));
            ch.tags.push_back({tag, uint32_t(p), d.subspan(p + 1, size_t(n))});
            pools |= ch.type == 0x3a && tag == 0x3d;
            p += 1 + size_t(n);
        }
        ++p;
    }
}

Scene buildTbScene(const BzeFile& bze, const std::string& level) {
    if (bze.variant != BzeVariant::TimeBusters) throw FormatError("buildTbScene: not a Time Busters BZE");
    const int i1 = bze.find(1);
    if (i1 < 0) throw FormatError("buildTbScene: no section 1");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const TbScript script = parseTbScript(s1);
    std::map<uint32_t, std::vector<uint8_t>> secs;
    const auto section = [&](uint32_t id) -> std::span<const uint8_t> {
        auto [it, fresh] = secs.try_emplace(id);
        if (fresh) {
            const int i = bze.find(id);
            if (i < 0) throw FormatError(std::format("buildTbScene: section {} referenced but absent", id));
            it->second = bze.payload(size_t(i));
        }
        return it->second;
    };

    Scene s;
    s.game = GameId::TimeBusters;
    s.level = level;
    // Inferred: PS1 modulation (texel * colour / 128) as LiT; vertex colours centre near 128 in the data (Enhanced).
    // The PC GL path does not double (byte/255; Original Fidelity uses 1.0): measured gain original/bbtime 1.01-1.03
    // on ARCA01 (RUNTIME_REFERENCE.md §7). Its texel tables 0x639de0/0x63ae80 (on: 0x639da4 = 1) are identity at
    // runtime, so pcTexGamma stays 1.
    s.colorScale = 255.0f / 128.0f;
    Builder b{s, {}, {}, {}, {}, 0, false, 0, {}};

    std::vector<std::string> texErrors;
    for (const TbChunk& ch : script.chunks) {
        if (ch.type != 0x28) continue;
        for (const TbTag& t : ch.tags) {
            const uint32_t off = le32(t.data, 0), id = le32(t.data, 4) & 0xfff;  // high 20 bits = byte size
            ++s.stats.textureRefs;
            try {
                Texture tx = decodeTim(section(ch.section), off, std::format("{}/tex/{}", level, id));
                if (b.tex.try_emplace(id, int32_t(s.textures.size())).second) s.textures.push_back(std::move(tx));
            } catch (const FormatError& e) {
                ++s.stats.textureErrors;
                texErrors.push_back(std::format("texture {}: {}", id, e.what()));
            }
        }
    }

    // Chunk 0x22 assets by asset id (tag 0x27: u16 class @0, u16 asset id @2 = key of entity tag 0x0f): tag 0x24
    // models, and tag 0x3f sprite sequences (MODELS.md §10.3: nD 16-byte 0x64 records at block + 0xc, tex id @+10).
    struct Asset { std::span<const uint8_t> model; bool octree; bool placed = false; };
    struct Sprite { uint16_t cls; std::vector<uint32_t> ids; };
    std::vector<Asset> models;
    std::map<uint16_t, size_t> assetModel;
    std::map<uint16_t, Sprite> sprites;
    for (const TbChunk& ch : script.chunks) {
        if (ch.type != 0x22) continue;
        const auto id = ch.tag(0x27);
        if (const auto seq = ch.tag(0x3f); !seq.empty() && !id.empty()) {
            const In in{region(section(ch.section), seq, ch.section), "sprite sequence"};
            const size_t ph = in.u32(4), nd = in.u16(ph + 2);
            Sprite& sp = sprites[le16(id, 2)] = {le16(id, 0), {}};
            for (size_t i = 0; i < nd; ++i) {
                if (in.u8(ph + 0xc + 16 * i + 3) != 0x64)
                    throw FormatError(std::format("sprite sequence record {} at 0x{:x}: type is not 0x64", i, ph + 0xc + 16 * i));
                sp.ids.push_back(in.u16(ph + 0xc + 16 * i + 10) & 0xfff);
            }
        }
        const auto mdl = ch.tag(0x24);
        if (mdl.empty()) continue;
        const auto m = region(section(ch.section), mdl, ch.section);
        models.push_back({m, isOctree(m)});
        s.stats.skyDomes += models.back().octree;
        if (!id.empty()) assetModel[le16(id, 2)] = models.size() - 1;
    }
    // Overlay font (0x4660c0: template 0x18) and HUD sheet (0x462430: template 0x1f): the template's first linked
    // class-0x17 sprite sequence, one whole TIM per record (u, v 0; w, h = TIM size in all 49 levels' data).
    for (const TbChunk& ch : script.chunks) {
        const auto id = ch.tag(0x27);
        if (ch.type != 0x08 || id.empty() || (le16(id, 0) != 0x18 && le16(id, 0) != 0x1f)) continue;
        std::vector<int32_t>& out = le16(id, 0) == 0x18 ? s.overlayFont : s.hudSheet;
        for (const TbTag& t : ch.tags) {
            const auto sp = t.id == 0x0f ? sprites.find(le16(t.data, 0)) : sprites.end();
            if (sp == sprites.end() || sp->second.cls != 0x17 || !out.empty()) continue;
            for (const uint32_t tid : sp->second.ids) {
                const auto tx = b.tex.find(tid);
                out.push_back(tx == b.tex.end() ? -1 : tx->second);
            }
        }
    }

    // Texture links (TEXTURE_GAP.md): entity tag 0x0b {u16 link id, 0} makes the entity write its current sprite
    // frame's texture id into alias[link id] at run time (FUN_00408410 at load, FUN_00465c80 per frame).
    // Frames: type 20 (tag 0x13) = looping flipbook FUN_00453d00 over the first listed asset of class 0x17; start
    // frame u16 +0x68, hold byte +0x6c, both from tag 0x41 (the only writer on link entities in the data); the frame
    // advances when the hold counter runs out, i.e. every hold+1 ticks, never when hold = 0; wraps after the last
    // record. Type 2 (script graph) and others: the latest kind-0x100 asset before tag 0x0b, frame order and timing
    // driven by the logic VM — cycled in record order here, 2 ticks per frame (Inferred).
    uint32_t linkFrameErrors = 0;
    for (const TbChunk& ch : script.chunks) {
        if (ch.type != 0x07 && ch.type != 0x08 && ch.type != 0x0a && ch.type != 0x63) continue;
        const auto cls = ch.tag(0x13);
        const int type = cls.empty() ? -1 : cls[0];
        const Sprite* last = nullptr;
        for (const TbTag& t : ch.tags) {
            if (t.id == 0x0f) {
                if (auto it = sprites.find(le16(t.data, 0)); it != sprites.end()) last = &it->second;
                continue;
            }
            if (t.id != 0x0b || !last) continue;
            const Sprite* src = last;
            if (type == 20) {
                src = nullptr;
                for (const TbTag& a : ch.tags) {
                    if (a.id != 0x0f) continue;
                    if (auto it = sprites.find(le16(a.data, 0)); it != sprites.end() && it->second.cls == 0x17) {
                        src = &it->second;
                        break;
                    }
                }
                if (!src) continue;  // the original dereferences a null asset here
            }
            TextureAnim an;
            an.linkId = le16(t.data, 0);
            for (uint32_t fid : src->ids)
                if (auto it = b.tex.find(fid); it != b.tex.end()) an.frames.push_back(it->second);
                else ++linkFrameErrors;
            if (an.frames.empty()) continue;
            if (const auto p = ch.tag(0x41); type == 20 && !p.empty()) {
                an.startFrame = le16(p, 0) % an.frames.size();
                an.holdTicks = p[4] ? p[4] + 1u : UINT32_MAX;
            } else {
                an.holdTicks = 2;
                an.timingInferred = true;
            }
            if (auto [it, fresh] = b.links.try_emplace(an.linkId, int32_t(s.textureAnims.size())); fresh) s.textureAnims.push_back(std::move(an));
            else s.textureAnims[size_t(it->second)] = std::move(an);  // several writers: the last entity wins (Inferred)
        }
    }

    std::string players;
    for (const TbChunk& ch : script.chunks) {
        if (ch.type == 0x20) {  // world model: v * scale + translation; rotation identity in all data
            const auto mdl = ch.tag(0x24);
            if (mdl.empty()) continue;
            if (const auto r = ch.tag(0x11); !r.empty())
                for (size_t k = 0; k < 3; ++k)
                    if (le16(r, 2 * k) % 0x1000)
                        throw FormatError(std::format("world chunk at 0x{:x}: non-identity rotation unsupported", ch.offset));
            Xform xf;
            if (const auto sc = ch.tag(0x12); !sc.empty()) xf.s[0] = xf.s[1] = xf.s[2] = float(le16(sc, 0)) / 4096.0f;  // u16 X for all axes
            if (const auto t = ch.tag(0x10); !t.empty()) std::ranges::copy(lowHalves(t), xf.t);
            ++s.stats.worldModels;
            b.world = true;
            b.octree(region(section(ch.section), mdl, ch.section), xf, std::format("world{}", s.stats.worldModels));
            b.world = false;
            continue;
        }
        if (ch.type != 0x07 && ch.type != 0x0a && ch.type != 0x63) continue;  // placed instances (08 = templates)
        const auto pos = ch.tag(0x10);
        if (pos.empty()) continue;
        const Vec3 p = lowHalves(pos);
        const auto rot = ch.tag(0x11);
        const float yaw = rot.empty() ? 0.0f : angle(int16_t(le16(rot, 2)));
        if (const auto id = ch.tag(0x27); !id.empty() && (le16(id, 0) == 2 || le16(id, 0) == 3)) {
            const bool bugs = le16(id, 0) == 2;
            if (bugs && !s.spawn) {
                s.spawn = std::array<float, 3>{p[0], -p[1], -p[2]};
                s.spawnYaw = -yaw;  // PS1 Ry forward (sin, 0, cos) -> engine Camera yaw = -angle (Inferred, as LiT)
            }
            players += std::format("{}{} (id {}) at ({}, {}, {})", players.empty() ? "" : ", ", bugs ? "Bugs" : "Taz",
                                   le16(id, 0), p[0], p[1], p[2]);
        }
        // Placement rule as LiT (Inferred): an instance listing exactly one model asset (tag 0x0f) is drawn at its
        // position with rotation Y (tag 0x11) and per-axis scale (tag 0x12, 4.12). Octree models (sky domes) are
        // held by class-9 entities whose init FUN_0044d6f0 builds the octree and forces scale 1 (+0xec..+0xf0 = 0x1000).
        Asset* model = nullptr;
        int statics = 0;
        for (const TbTag& t : ch.tags)
            if (t.id == 0x0f)
                if (auto a = assetModel.find(le16(t.data, 0)); a != assetModel.end()) ++statics, model = &models[a->second];
        if (statics != 1) continue;
        Xform xf;
        std::ranges::copy(p, xf.t);
        xf.cy = std::cos(yaw), xf.sy = std::sin(yaw);
        if (model->octree) {
            model->placed = true;
            b.octree(model->model, xf, std::format("sky{}", ch.offset));
            continue;
        }
        const uint16_t nobj = In{model->model, "actor model"}.u16(2);
        if (nobj != 1) {
            s.stats.skippedActors += nobj > 1;  // nobj 0 = no geometry
            continue;
        }
        if (const auto sc = ch.tag(0x12); !sc.empty())
            for (size_t k = 0; k < 3; ++k) xf.s[k] = float(int16_t(le16(sc, 4 * k))) / 4096.0f;
        ++s.stats.props;
        b.actor(model->model, xf, std::format("prop{}@{:x}", s.stats.props, ch.offset));  // @offset: TbActor::chunk
    }
    uint32_t unplaced = 0;
    for (const Asset& a : models)
        if (a.octree && !a.placed) ++unplaced, b.octree(a.model, Xform{}, std::format("sky_origin{}", unplaced));

    s.stats.missingTextureIds.assign(b.missing.begin(), b.missing.end());
    s.notes.push_back(std::format("{} world models, {} sky domes ({} not held by an entity: drawn at the origin, Inferred), "
                                  "{} rigid props (placement Inferred), {} skinned actors skipped (Research Required)",
                                  s.stats.worldModels, s.stats.skyDomes, unplaced, s.stats.props, s.stats.skippedActors));
    if (!players.empty()) s.notes.push_back("Players (identity Inferred): " + players);
    if (s.stats.invalidPrims) s.notes.push_back(std::format("{} primitives fail CheckPrimValidity: skipped as in the game", s.stats.invalidPrims));
    if (b.lodFaces) s.notes.push_back(std::format("{} far-LOD faces (octree tree C, DETAILS < 2 only) decoded, not drawn", b.lodFaces));
    if (!s.textureAnims.empty())
        s.notes.push_back(std::format("{} texture links (animated, TEXTURE_GAP.md) drive {} faces{}", s.textureAnims.size(),
                                      s.stats.linkedFaces,
                                      linkFrameErrors ? std::format("; {} link frames have no TIM", linkFrameErrors) : ""));
    if (s.stats.missingTextureFaces)
        s.notes.push_back(std::format("{} faces reference {} texture ids that are neither TIMs nor links: not drawn (as the "
                                      "original)", s.stats.missingTextureFaces, s.stats.missingTextureIds.size()));
    for (const std::string& e : texErrors) s.notes.push_back(e);
    return s;
}

} // namespace bb
