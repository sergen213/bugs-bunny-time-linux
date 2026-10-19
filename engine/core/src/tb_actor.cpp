#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <tuple>

namespace bb {
namespace {

uint16_t le16(std::span<const uint8_t> b, size_t p) {
    if (p + 2 > b.size()) throw FormatError(std::format("tb actor: read at 0x{:x} past end 0x{:x}", p, b.size()));
    return uint16_t(b[p] | b[p + 1] << 8);
}
uint32_t le32(std::span<const uint8_t> b, size_t p) { return le16(b, p) | uint32_t(le16(b, p + 2)) << 16; }

// Payload words per packet header (tb_anim.py plen).
size_t payloadWords(uint16_t w) {
    const unsigned t = w & 0xf00, f = w & 0xf000;
    switch (t) {
    case 0x100: case 0xd00: return (f & 0x2000 ? 3 : 0) + (f & 0x4000 ? 3 : 0) + (f & 0x8000 ? 3 : f & 0x1000 ? 2 : 0);
    case 0xb00: return (3 * size_t(std::popcount(f & 0xe000u)) + 1) >> 1;
    case 0x900: return f & 0x1000 ? 4 : 7;
    case 0x200: case 0x300: case 0xc00: return 1;
    case 0x600: case 0xe00: return 2;
    case 0x400: case 0x700: return 4;
    default: return 0;
    }
}

// RotMatrix (Rx * Ry * Rz, 4.12) then column ScaleMatrix (tb_anim.py _rot); float like actor.cpp coordMatrix.
void rebuild(TodPart& p) {
    auto cs = [](int32_t a) {
        const float r = float(a) * 2.0f * std::numbers::pi_v<float> / 4096.0f;
        return std::pair{std::cos(r), std::sin(r)};
    };
    const auto [cx, sx] = cs(p.rot[0]);
    const auto [cy, sy] = cs(p.rot[1]);
    const auto [cz, sz] = cs(p.rot[2]);
    // Rx*Ry*Rz expanded.
    const float m[9] = {cy * cz, -cy * sz, sy,
                        sx * sy * cz + cx * sz, -sx * sy * sz + cx * cz, -sx * cy,
                        -cx * sy * cz + sx * sz, cx * sy * sz + sx * cz, cx * cy};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.m[size_t(i * 3 + j)] = m[i * 3 + j] * float(p.scale[size_t(j)]) / 4096.0f;
}

int32_t s8(uint32_t x) { return int32_t(int8_t(uint8_t(x))); }
int32_t s16(uint32_t x) { return int32_t(int16_t(uint16_t(x))); }

void packedRot(TodPart& p, uint32_t w, uint32_t a, uint32_t b) {
    p.rot = {int32_t(a & 0xfff), int32_t(b & 0xfff), int32_t((w >> 4 & 0xf00) | (a >> 8 & 0xf0) | b >> 12)};
    rebuild(p);
}
void packedTrans(TodPart& p, uint32_t a, uint32_t b) { p.trans = {s8(a & 0xff), s16(b), s8(a >> 8)}; }

struct Chunks {
    TbScript script;
    std::vector<uint8_t> s1;
    std::map<uint32_t, std::vector<uint8_t>> secs;
    const BzeFile& bze;
    std::span<const uint8_t> region(const TbChunk& ch, std::span<const uint8_t> offSize) {
        auto [it, fresh] = secs.try_emplace(ch.section);
        if (fresh) {
            const int i = bze.find(ch.section);
            if (i < 0) throw FormatError(std::format("tb actors: section {} absent", ch.section));
            it->second = bze.payload(size_t(i));
        }
        const uint32_t off = le32(offSize, 0), size = le32(offSize, 4);
        if (off > it->second.size() || it->second.size() - off < size)
            throw FormatError(std::format("tb actors: block 0x{:x}+0x{:x} outside section {}", off, size, ch.section));
        return std::span<const uint8_t>(it->second).subspan(off, size);
    }
};

} // namespace

Tod parseTbTod(std::span<const uint8_t> d) {
    if (d.size() < 8 || d[0] != 0x50 || d[1] != 0 || d[3] != 0) throw FormatError("packed TOD: bad header");
    Tod t;
    t.packed = true;
    t.resolution = d[2];
    const uint32_t nf = le32(d, 4);
    size_t p = 8;
    for (uint32_t f = 0; f < nf; ++f) {
        const size_t npk = le16(d, p + 2);
        p += 8;
        t.frameStart.push_back(uint32_t(t.packets.size()));
        for (size_t k = 0; k < npk; ++k) {
            const uint16_t w = le16(d, p);
            const size_t n = payloadWords(w);
            if (n > 9) throw FormatError(std::format("packed TOD: packet 0x{:04x} at 0x{:x} too long", w, p));
            TodPacket pk{uint16_t(w & 0xff), uint8_t(w >> 8 & 0xf), uint8_t(w >> 12), {}};
            for (size_t i = 0; i < n; ++i) pk.v[i] = le16(d, p + 2 + 2 * i);
            t.packets.push_back(pk);
            p += 2 + 2 * n;
        }
    }
    if (p != d.size()) throw FormatError(std::format("packed TOD: parsed 0x{:x} of 0x{:x} bytes", p, d.size()));
    t.frameStart.push_back(uint32_t(t.packets.size()));
    return t;
}

void applyTbFrame(AnimState& st, const Tod& tod, size_t frame) {
    std::vector<TodPart>& parts = st.parts;
    auto at = [&](uint32_t pid) -> TodPart* { return pid && pid <= parts.size() ? &parts[pid - 1] : nullptr; };
    for (uint32_t k = tod.frameStart[frame]; k < tod.frameStart[frame + 1]; ++k) {
        const TodPacket& pk = tod.packets[k];
        const uint32_t t = uint32_t(pk.type) << 8, f = uint32_t(pk.flag) << 12, w = f | t | pk.id;
        if (t == 0x800 || t == 0xc00) {
            if ((f & 0x1000) && (f & 0xe000) == 0xa000) {  // hide: scale 0, five matrix entries cleared
                if (TodPart* p = at(pk.id)) {
                    p->scale = {0, 0, 0};
                    p->m[0] = p->m[1] = p->m[4] = p->m[5] = p->m[8] = 0;
                    p->flags |= 0x8000;
                }
            } else if (f & 0x1000) {
                if (at(pk.id)) parts.erase(parts.begin() + (pk.id - 1));
            } else if (f >> 13 == 5) {
                if (TodPart* p = at(pk.id)) p->flags &= 0x7fff;
            } else {
                constexpr uint16_t kCreate[5] = {0x8008, 0x8040, 0x8080, 0x8100, 0x8200};
                TodPart np;
                np.flags = kCreate[f >> 13 < 5 ? f >> 13 : 0];
                np.parent = -1;  // new parts hang on the actor root (parent 0)
                parts.push_back(np);
            }
            if (t == 0xc00) {
                TodPart* p = f & 0x1000 ? at(pk.id) : parts.empty() ? nullptr : &parts.back();
                if (!p) continue;
                const uint32_t o = pk.v[0] & 0xff, par = uint32_t(pk.v[0]) >> 8;
                p->object = int32_t(o) - 1;
                p->flags = o ? 4 : 8;
                p->parent = par == 0 || par == 0xff ? -1 : int32_t(par) - 1;
            }
            continue;
        }
        if (t == 0x900) {
            if (f & 0x1000)
                for (size_t i = 0; i < 6; ++i) st.box[i] = int16_t(st.box[i] + s8(uint32_t(pk.v[i / 2]) >> (8 * (i & 1))));
            else
                for (size_t i = 0; i < 6; ++i) st.box[i] = int16_t(pk.v[i]);
            st.radius = uint16_t(pk.v[f & 0x1000 ? 3 : 6]);
            continue;
        }
        if (t == 0xa00) continue;
        TodPart* p = at(pk.id);
        if (!p) continue;
        const auto& v = pk.v;
        switch (t) {
        case 0x300: p->parent = v[0] == 0 || v[0] == 0xffff ? -1 : v[0] - 1; break;
        case 0x100: case 0xd00: {
            p->flags &= 0x7fff;
            size_t q = 0;
            if (f & 0x2000) p->rot = {s16(v[0]), s16(v[1]), s16(v[2])}, q = 3;
            else if (t == 0xd00) p->rot = {0, 0, 0};
            if (f & 0x4000) p->scale = {s16(v[q]), s16(v[q + 1]), s16(v[q + 2])}, q += 3;
            else if (t == 0xd00) p->scale = {4096, 4096, 4096};
            if ((f & 0x6000) || t == 0xd00) rebuild(*p);
            if (f & 0x8000) p->trans = {s16(v[q]), s16(v[q + 1]), s16(v[q + 2])};
            else if (f & 0x1000) packedTrans(*p, uint32_t(v[q]), uint32_t(v[q + 1]));
            else if (t == 0xd00) p->trans = {0, 0, 0};
            break;
        }
        case 0x400: case 0x700:
            if (t == 0x400) p->flags &= 0x7fff, p->scale = {4096, 4096, 4096};
            packedRot(*p, w, uint32_t(v[0]), uint32_t(v[1]));
            packedTrans(*p, uint32_t(v[2]), uint32_t(v[3]));
            break;
        case 0xe00: packedRot(*p, w, uint32_t(v[0]), uint32_t(v[1])); break;
        case 0xb00: {  // s8 deltas read sequentially (lo byte, hi byte of each word)
            p->flags &= 0x7fff;
            size_t q = 0;
            auto byte = [&](size_t i) { return s8(uint32_t(v[i / 2]) >> (8 * (i & 1))); };
            for (auto [bit, arr] : {std::pair{0x2000u, &p->rot}, std::pair{0x4000u, &p->scale}})
                if (f & bit) {
                    for (size_t a = 0; a < 3; ++a) (*arr)[a] += byte(q + a);
                    q += 3;
                }
            if (f & 0x6000) rebuild(*p);
            if (f & 0x8000)
                for (size_t a = 0; a < 3; ++a) p->trans[a] += byte(q + a);
            break;
        }
        default: break;  // 0x200 object / 0x600 param: absent from the data
        }
    }
}

ActorModel buildTbActorModel(const Scene& s, std::span<const uint8_t> m) {
    ActorModel am;
    const size_t nobj = le16(m, 2), ph = le32(m, 4);
    std::vector<std::pair<size_t, size_t>> objs;  // (vertex table offset, count)
    size_t flat = 0;
    bool anySlot = false;
    for (size_t i = 0; i < nobj; ++i) {
        objs.emplace_back(8 + le32(m, 8 + 8 * i), le32(m, 12 + 8 * i));
        for (size_t k = 0; k < objs.back().second; ++k) anySlot |= le16(m, objs.back().first + 8 * k + 6) != 0;
        flat += objs.back().second;
    }
    // Vertex word 3 = slot (& 0x7fff), 0x8000 = seam averaged with the slot; 37 models have word 3 = 0 everywhere:
    // their primitives index the concatenated tables (§5.3). Later objects with word 3 all 0 are unslotted.
    std::vector<int32_t> owner;  // slot -> object writing it first (non-seam)
    size_t base = 0;
    for (size_t i = 0; i < nobj; ++i) {
        auto& ov = am.objects.emplace_back();
        bool any = false;
        for (size_t k = 0; k < objs[i].second; ++k) any |= le16(m, objs[i].first + 8 * k + 6) != 0;
        for (size_t k = 0; k < objs[i].second; ++k) {
            const size_t q = objs[i].first + 8 * k;
            const uint16_t w3 = le16(m, q + 6);
            ActorVertex v{{float(int16_t(le16(m, q))), float(int16_t(le16(m, q + 2))), float(int16_t(le16(m, q + 4)))},
                          anySlot ? uint16_t(w3 & 0x7fff) : uint16_t(base + k), anySlot && (w3 & 0x8000)};
            if (anySlot && i && !any) v.slot = 0xffff;  // unslotted object: no primitive uses it
            if (v.slot != 0xffff && !v.blend) {
                if (owner.size() <= v.slot) owner.resize(size_t(v.slot) + 1, -1);
                if (owner[v.slot] < 0) owner[v.slot] = int32_t(i);
            }
            ov.push_back(v);
        }
        base += objs[i].second;
    }
    am.slotCount = uint32_t(anySlot ? owner.size() : flat);
    if (nobj == 0) return am;

    std::map<std::string, int32_t> texByName;
    for (size_t i = 0; i < s.textures.size(); ++i) texByName.emplace(s.textures[i].name, int32_t(i));
    std::map<std::tuple<uint32_t, int32_t, Blend, bool>, size_t> meshOf;
    const size_t nC = le16(m, ph), nD = le16(m, ph + 2), nA = m[ph + 4], nB = m[ph + 5];
    const size_t oB = le16(m, ph + 6), oC = le16(m, ph + 8), oD = le16(m, ph + 10);
    const std::tuple<size_t, size_t, size_t, int> groups[] = {{0xc, nA, 0x24, 3}, {oB, nB, 0x24, 4}, {oC, nC, 0x28, 3}, {oD, nD, 0x28, 4}};
    for (auto [off, cnt, size, n] : groups)
        for (size_t i = 0; i < cnt; ++i) {
            const size_t r = ph + off + i * size;
            le16(m, r + size - 2);  // bounds
            const uint8_t b0 = m[r], b1 = m[r + 1];
            const uint32_t id = le16(m, r + 2) & 0xfff;
            int32_t tex = -1;
            if (auto it = texByName.find(std::format("{}/tex/{}", s.level, id)); it != texByName.end()) tex = it->second;
            else
                for (const TextureAnim& a : s.textureAnims)
                    if (a.linkId == id) tex = a.frames[a.startFrame];
            if (tex < 0) continue;  // alias 0: not drawn (as world prims, TEXTURE_GAP.md §1)
            uint32_t idx[4];
            for (size_t k = 0; k < 4; ++k) idx[k] = b1 & 0x80 ? le16(m, r + 0x20 + 2 * k) : m[r + 0x20 + k];
            const uint32_t limit = am.slotCount;
            bool ok = true;
            for (int k = 0; k < n; ++k) ok &= idx[k] < limit;
            if (!ok) throw FormatError(std::format("tb actor: primitive at 0x{:x} indexes slot >= {}", r, limit));
            Blend blend = Blend::Opaque;
            uint8_t alpha = 255;
            if (b0 & 2) {
                switch (m[r + 0x1a] >> 5 & 3) {
                case 0: blend = Blend::Alpha, alpha = 128; break;
                case 1: blend = Blend::Additive; break;
                case 2: blend = Blend::Subtract; break;
                case 3: blend = Blend::Additive, alpha = 64; break;
                }
            } else if (s.textures[size_t(tex)].hasAlpha) blend = Blend::AlphaTest;
            const uint32_t obj = anySlot && idx[0] < owner.size() && owner[idx[0]] >= 0 ? uint32_t(owner[idx[0]]) : 0;
            auto [it, fresh] = meshOf.try_emplace({obj, tex, blend, bool(b1 & 0x10)}, am.meshes.size());
            if (fresh) {
                ActorMesh& nm = am.meshes.emplace_back();
                nm.object = obj;
                nm.mesh.texture = tex;
                nm.mesh.blend = blend;
                nm.mesh.twoSided = b1 & 0x10;
            }
            ActorMesh& am2 = am.meshes[it->second];
            const Texture& tx = s.textures[size_t(tex)];
            const float tw = float(tx.width), th = float(tx.height);
            constexpr uint8_t kNoUv[4][2] = {{0, 0}, {255, 0}, {255, 255}, {0, 255}};
            const size_t uvAt[4] = {r + 0x14, r + 0x18, r + 0x1c, r + 0x1e};
            const uint32_t vb = uint32_t(am2.mesh.vertices.size());
            for (int k = 0; k < n; ++k) {
                Vertex v{};
                const uint8_t u = b1 & 0x40 ? kNoUv[k][0] : m[uvAt[k]], vv = b1 & 0x40 ? kNoUv[k][1] : m[uvAt[k] + 1];
                v.uv[0] = (float(u) / 255.0f * (tw - 1.0f) + 0.5f) / tw;  // same texel convention as tb_level.cpp
                v.uv[1] = ((1.0f - float(vv) / 255.0f) * (th - 1.0f) + 0.5f) / th;
                const size_t c = r + 4 + (b0 & 0x10 ? size_t(k) * 4 : 0);
                v.rgba[0] = m[c], v.rgba[1] = m[c + 1], v.rgba[2] = m[c + 2], v.rgba[3] = alpha;
                am2.mesh.vertices.push_back(v);
                am2.slots.push_back(uint16_t(idx[k]));
            }
            am2.mesh.indices.insert(am2.mesh.indices.end(), {vb, vb + 2, vb + 1});
            if (n == 4) am2.mesh.indices.insert(am2.mesh.indices.end(), {vb + 1, vb + 2, vb + 3});
        }
    return am;
}

void addTbActors(Scene& s, const BzeFile& bze) {
    const int i1 = bze.find(1);
    if (i1 < 0) throw FormatError("tb actors: no section 1");
    Chunks c{{}, bze.payload(size_t(i1)), {}, bze};
    c.script = parseTbScript(c.s1);
    struct Asset { uint16_t role; std::span<const uint8_t> model, tod; };
    std::map<uint16_t, Asset> assets;
    for (const TbChunk& ch : c.script.chunks) {
        if (ch.type != 0x22) continue;
        const auto id = ch.tag(0x27);
        if (id.empty()) continue;
        Asset a{le16(id, 0), {}, {}};
        if (const auto t = ch.tag(0x24); !t.empty()) a.model = c.region(ch, t);
        if (const auto t = ch.tag(0x25); !t.empty()) a.tod = c.region(ch, t);
        assets[le16(id, 2)] = a;
    }
    std::map<uint16_t, const TbChunk*> templates;
    for (const TbChunk& ch : c.script.chunks)
        if (const auto id = ch.tag(0x27); ch.type == 0x08 && !id.empty()) templates.emplace(le16(id, 0), &ch);
    std::map<std::vector<uint16_t>, uint32_t> modelOf;  // link list -> Scene::actorModels index
    std::map<uint16_t, Tod> todCache;
    for (const TbChunk& ch : c.script.chunks) {
        if (ch.type != 0x07 && ch.type != 0x0a && ch.type != 0x63) continue;
        const auto pos = ch.tag(0x10), id = ch.tag(0x27);
        if (pos.empty()) continue;
        std::vector<uint16_t> links;
        int statics = 0;
        for (const TbTag& t : ch.tags)
            if (t.id == 0x0f) {
                links.push_back(le16(t.data, 0));
                if (auto a = assets.find(links.back()); a != assets.end() && !a->second.model.empty()) ++statics;
            }
        const Asset *model = nullptr, *rig = nullptr;
        uint16_t modelId = 0, rigId = 0;
        for (uint16_t l : links) {
            auto a = assets.find(l);
            if (a == assets.end()) continue;
            if (!model && a->second.role == 13 && !a->second.model.empty()) model = &a->second, modelId = l;
            if (!rig && a->second.role == 1 && !a->second.tod.empty()) rig = &a->second, rigId = l;
        }
        if (!model || !rig || le16(model->model, 2) == 0) continue;
        if (statics == 1 && le16(model->model, 2) == 1) continue;  // buildTbScene draws it as a rigid prop
        auto [mit, fresh] = modelOf.try_emplace(links, uint32_t(s.actorModels.size()));
        if (fresh) {
            ActorModel am = buildTbActorModel(s, model->model);
            am.modelAsset = modelId;
            am.skeletonAsset = rigId;
            am.skeleton = parseTbTod(rig->tod);
            for (uint16_t l : links) {
                auto a = assets.find(l);
                if (a == assets.end() || a->second.tod.empty() || a->second.role == 1) continue;
                if (findAnim(am, a->second.role) >= 0) continue;  // first link with the role wins
                am.anims.push_back({a->second.role, parseTbTod(a->second.tod)});
            }
            s.actorModels.push_back(std::move(am));
        }
        ActorInstance inst;
        inst.model = mit->second;
        inst.pos = {int16_t(le16(pos, 0)), int16_t(le16(pos, 4)), int16_t(le16(pos, 8))};
        if (const auto r = ch.tag(0x11); !r.empty()) inst.rot = {int16_t(le16(r, 0)), int16_t(le16(r, 2)), int16_t(le16(r, 4))};
        if (const auto sc = ch.tag(0x12); !sc.empty())
            inst.scale = {int16_t(le16(sc, 0)), int16_t(le16(sc, 4)), int16_t(le16(sc, 8))};
        inst.entityId = id.empty() ? 0 : le16(id, 0);
        inst.chunk = ch.type;
        inst.player = inst.entityId == 2 || inst.entityId == 3;
        inst.scriptOffset = ch.offset;
        // Initial anim: event 2 (else 1) transition -> first state -> its anim id (own records override template).
        std::map<uint16_t, uint16_t> stateAnim, trans;
        const TbChunk* tmpl = nullptr;
        if (!id.empty())
            if (auto t = templates.find(le16(id, 2)); t != templates.end()) tmpl = t->second;
        for (const TbChunk* src : {tmpl, &ch}) {
            if (!src) continue;
            for (const TbTag& t : src->tags) {
                if (t.id == 0x2f) stateAnim[le16(t.data, 0)] = le16(t.data, 2);
                if (t.id == 0x31) trans[le16(t.data, 0)] = le16(t.data, 2);
            }
        }
        const ActorModel& am = s.actorModels[inst.model];
        auto tr = trans.find(2);
        if (tr == trans.end()) tr = trans.find(1);
        if (tr != trans.end())
            if (auto sa = stateAnim.find(tr->second); sa != stateAnim.end() && findAnim(am, sa->second) >= 0) inst.anim = sa->second;
        if (!inst.anim && !am.anims.empty()) inst.anim = am.anims.front().id;
        s.actors.push_back(inst);
    }
}

} // namespace bb
