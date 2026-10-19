#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/tb_actor.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

namespace bb {
namespace {

using M3 = std::array<float, 9>;  // row major
using V3 = std::array<float, 3>;

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[size_t(i * 3 + j)] = a[size_t(i * 3)] * b[size_t(j)] + a[size_t(i * 3 + 1)] * b[size_t(3 + j)] +
                                   a[size_t(i * 3 + 2)] * b[size_t(6 + j)];
    return r;
}

V3 apply(const M3& m, const V3& t, const V3& v) {
    return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + t[0], m[3] * v[0] + m[4] * v[1] + m[5] * v[2] + t[1],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2] + t[2]};
}

// RotMatrix 0x407a50 (M = Rx * Ry * Rz, 4.12 angles) followed by ScaleMatrix 0x408060 (column j *= s[j] / 4096).
template <class R, class S> M3 coordMatrix(const R& rot, const S& scale) {
    auto cs = [](int32_t a) {
        const float r = float(a) * 2.0f * std::numbers::pi_v<float> / 4096.0f;
        return std::pair{std::cos(r), std::sin(r)};
    };
    const auto [cx, sx] = cs(rot[0]);
    const auto [cy, sy] = cs(rot[1]);
    const auto [cz, sz] = cs(rot[2]);
    const M3 x{1, 0, 0, 0, cx, -sx, 0, sx, cx}, y{cy, 0, sy, 0, 1, 0, -sy, 0, cy}, z{cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    M3 m = mul(x, mul(y, z));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[size_t(i * 3 + j)] *= float(scale[size_t(j)]) / 4096.0f;
    return m;
}

int findPart(const std::vector<TodPart>& parts, uint32_t id) {
    for (size_t i = 0; i < parts.size(); ++i)
        if (!parts[i].dead && parts[i].id == id) return int(i);
    return -1;
}

// World matrices of every part (GsGetLw chain 0x40a050 / CompMatrixLV 0x408690): workm = parent.workm (x) coord.
struct Work { M3 m; V3 t; };
void workMatrices(const std::vector<TodPart>& parts, const Work& root, std::vector<Work>& out) {
    out.assign(parts.size(), {});
    std::vector<uint8_t> state(parts.size(), 0);  // 0 todo, 1 in progress, 2 done
    auto get = [&](auto&& self, size_t i) -> const Work& {
        if (state[i] == 2) return out[i];
        state[i] = 1;
        const TodPart& p = parts[i];
        const V3 t{float(p.trans[0]), float(p.trans[1]), float(p.trans[2])};
        const int par = p.parent;
        const bool chained = par >= 0 && size_t(par) < parts.size() && state[size_t(par)] != 1;  // cycles: unattached
        const Work& w = par == -1 ? root : chained ? self(self, size_t(par)) : Work{M3{1, 0, 0, 0, 1, 0, 0, 0, 1}, {}};
        out[i] = par == -2 || (par >= 0 && !chained) ? Work{p.m, t} : Work{mul(w.m, p.m), apply(w.m, w.t, t)};
        state[i] = 2;
        return out[i];
    };
    for (size_t i = 0; i < parts.size(); ++i) get(get, i);
}

// 0x40a050 skips parts whose world matrix is zero (m00..m21 == 0 and the high byte of m22 == 0, Q12).
bool zeroMatrix(const M3& m) {
    for (int i = 0; i < 8; ++i)
        if (std::lround(m[size_t(i)] * 4096.0f) != 0) return false;
    const long m22 = std::lround(m[8] * 4096.0f);
    return m22 >= 0 && m22 < 256;
}

bool drawn(const ActorModel& model, const TodPart& p, const Work& w) {
    return !p.dead && (p.flags & 6) && !(p.flags & 0x8000) && p.object >= 0 && size_t(p.object) < model.objects.size() &&
           !zeroMatrix(w.m);
}

// Shared slot buffer (0x411350): parts in list order write their object's vertices; blend vertices average with
// an already written slot (world space here, after projection in the original). NaN = unwritten.
void skin(const ActorModel& model, const AnimState& st, const Work& root, std::vector<V3>& slots, std::vector<Work>& work) {
    workMatrices(st.parts, root, work);
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    slots.assign(model.slotCount, V3{nan, nan, nan});
    for (size_t i = 0; i < st.parts.size(); ++i) {
        const TodPart& p = st.parts[i];
        if (!drawn(model, p, work[i])) continue;
        for (const ActorVertex& v : model.objects[size_t(p.object)]) {
            if (v.slot >= slots.size()) continue;
            const V3 w = apply(work[i].m, work[i].t, {v.pos[0], v.pos[1], v.pos[2]});
            V3& s = slots[v.slot];
            if (v.blend && !std::isnan(s[0])) s = {(s[0] + w[0]) * 0.5f, (s[1] + w[1]) * 0.5f, (s[2] + w[2]) * 0.5f};
            else s = w;
        }
    }
}

uint32_t finishedResult(uint32_t flags) { return flags & AnimHold ? AnimPlaying : AnimFinished; }

} // namespace

Tod parseTod(std::span<const uint8_t> d, size_t off) {
    auto need = [&](size_t p, size_t n) {
        if (p > d.size() || d.size() - p < n)
            throw FormatError(std::format("TOD at 0x{:x}: {} bytes at 0x{:x} past end (0x{:x})", off, n, p, d.size()));
    };
    auto u16 = [&](size_t p) { need(p, 2); return uint16_t(d[p] | d[p + 1] << 8); };
    auto u32 = [&](size_t p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; };
    auto s16 = [&](size_t p) { return int32_t(int16_t(u16(p))); };
    need(off, 8);
    if (d[off] != 0x50 || d[off + 1] != 0) throw FormatError(std::format("TOD at 0x{:x}: bad id {:02x} {:02x}", off, d[off], d[off + 1]));
    Tod t;
    t.resolution = u16(off + 2);
    const uint32_t nframes = u32(off + 4);
    size_t p = off + 8;
    for (uint32_t f = 0; f < nframes; ++f) {
        const size_t words = u16(p), npk = u16(p + 2);
        t.frameStart.push_back(uint32_t(t.packets.size()));
        size_t q = p + 8;
        for (size_t k = 0; k < npk; ++k) {
            const uint32_t w = u32(q);
            const size_t len = w >> 24;
            if (!len) throw FormatError(std::format("TOD at 0x{:x}: zero-length packet at 0x{:x}", off, q));
            need(q, len * 4);
            TodPacket pk{uint16_t(w), uint8_t(w >> 16 & 15), uint8_t(w >> 20 & 15), {}};
            const size_t pl = q + 4, end = q + len * 4;
            auto have = [&](size_t n) {
                if (pl + n > end) throw FormatError(std::format("TOD at 0x{:x}: short type-{} packet at 0x{:x}", off, pk.type, q));
            };
            switch (pk.type) {
            case 1: {
                size_t r = pl;
                if (pk.flag & 2) have(r - pl + 8), pk.v[0] = s16(r), pk.v[1] = s16(r + 2), pk.v[2] = s16(r + 4), r += 8;
                if (pk.flag & 4) have(r - pl + 8), pk.v[3] = s16(r), pk.v[4] = s16(r + 2), pk.v[5] = s16(r + 4), r += 8;
                if (pk.flag & 8) have(r - pl + 12), pk.v[6] = int32_t(u32(r)), pk.v[7] = int32_t(u32(r + 4)), pk.v[8] = int32_t(u32(r + 8));
                break;
            }
            case 2: have(4), pk.v[0] = int32_t(u32(pl) & 0xffff); break;
            case 3: have(4), pk.v[0] = int32_t(u32(pl)); break;
            case 8: break;
            case 9:
                have(14);
                for (int i = 0; i < 6; ++i) pk.v[size_t(i)] = s16(pl + size_t(i) * 2);
                pk.v[6] = u16(pl + 12);
                break;
            default: q = end; continue;  // 0 attr, 6 light, 0xa attach point: not applied by this port
            }
            t.packets.push_back(pk);
            q = end;
        }
        if (q - p != words * 4) throw FormatError(std::format("TOD at 0x{:x}: frame {} size mismatch", off, f));
        p = q;
    }
    t.frameStart.push_back(uint32_t(t.packets.size()));
    return t;
}

void applyTodFrame(AnimState& st, const Tod& tod, size_t frame) {
    if (tod.packed) return applyTbFrame(st, tod, frame);
    std::vector<TodPart>& parts = st.parts;
    // Packets address the k-th part of the list as it was at frame start; an empty list is searched by id.
    std::vector<uint32_t> snap;
    for (size_t i = 0; i < parts.size(); ++i)
        if (!parts[i].dead) snap.push_back(uint32_t(i));
    for (uint32_t k = tod.frameStart[frame]; k < tod.frameStart[frame + 1]; ++k) {
        const TodPacket& pk = tod.packets[k];
        const int pi = snap.empty() ? findPart(parts, pk.id) : pk.id && pk.id <= snap.size() ? int(snap[pk.id - 1u]) : -1;
        TodPart* p = pi >= 0 ? &parts[size_t(pi)] : nullptr;
        switch (pk.type) {
        case 1:
            if (!p) break;
            p->flags &= 0x7fff;
            if (pk.flag & 2) p->rot = {pk.v[0], pk.v[1], pk.v[2]};
            if (pk.flag & 4) p->scale = {pk.v[3], pk.v[4], pk.v[5]};
            if (pk.flag & 6) p->m = coordMatrix(p->rot, p->scale);
            if (pk.flag & 8) p->trans = {pk.v[6], pk.v[7], pk.v[8]};
            break;
        case 2:
            if (!p) break;
            p->flags = pk.v[0] ? 4 : 8;
            p->object = pk.v[0] - 1;
            break;
        case 3:
            if (!p) break;
            p->parent = pk.v[0] == 0 || pk.v[0] == 0xffff ? -1 : findPart(parts, uint32_t(pk.v[0]));
            if (p->parent < 0 && pk.v[0] != 0 && pk.v[0] != 0xffff) p->parent = -2;
            break;
        case 8:
            if (!(pk.flag & 1)) {
                constexpr uint16_t kCreate[5] = {0x8008, 0x40, 0x80, 0x100, 0x200};  // 1..4: light/flag nodes
                const unsigned sub = pk.flag >> 1 & 7;
                if (sub < 5) {
                    TodPart np;
                    np.id = pk.id;
                    np.flags = kCreate[sub];
                    parts.push_back(np);
                } else if (sub == 5 && p) {
                    p->flags &= 0x7fff;
                }
            } else if ((pk.flag & 0xe) == 0xa) {  // 0xb: hide until a coord packet carries scale again
                if (!p) break;
                p->scale = {0, 0, 0};
                p->m[0] = p->m[1] = p->m[4] = p->m[5] = p->m[8] = 0;  // the original clears only these five
                p->flags |= 0x8000;
            } else if (const int dead = findPart(parts, pk.id); dead >= 0) {
                parts[size_t(dead)].dead = true;
            }
            break;
        case 9:
            for (int i = 0; i < 6; ++i) st.box[size_t(i)] = int16_t(pk.v[size_t(i)]);
            st.radius = uint16_t(pk.v[6]);
            break;
        }
    }
}

int findAnim(const ActorModel& model, uint16_t animId) {
    for (size_t i = 0; i < model.anims.size(); ++i)
        if (model.anims[i].id == animId) return int(i);
    return -1;
}

uint32_t animTick(const ActorModel& model, AnimState& st, uint16_t animId, uint32_t mode) {
    mode &= 0xffff;  // u16 parameter
    if (!mode) {
        if (st.flags & AnimPause) return AnimPlaying;
        if (st.flags & 0x100) return finishedResult(st.flags);
    } else if (mode & AnimStop) {
        st.flags = 0x100;
        return AnimStopped;
    } else if ((mode & AnimResume) && (st.flags & AnimPause)) {
        st.flags &= ~AnimPause;
    } else if (mode & AnimPause) {
        st.flags |= AnimPause;
        return AnimPlaying;
    }
    st.flags &= ~0x3000u;
    if (st.parts.empty()) {  // first call: build the part list from the role-4 skeleton, first part on the root
        if (model.skeleton.frames()) applyTodFrame(st, model.skeleton, 0);
        if (!st.parts.empty()) st.parts[0].parent = -1;
    }
    if (mode & 0xb) {
        if (!(mode & 8) || !st.tod) {
            const int a = findAnim(model, animId);
            if (a < 0) {
                st.flags = 0x100;
                return AnimMissing;
            }
            st.tod = &model.anims[size_t(a)].tod;
            st.anim = animId;
        }
        st.flags = mode | 0x1200;
        st.next = 0;
        st.last = uint16_t(st.tod->frames() - 1);
        st.ticksPerFrame = uint8_t(st.tod->resolution >> 1);
        st.frame = 0;
        st.countdown = 0;
    }
    if (!st.tod) return AnimPlaying;
    if (st.countdown == 0) {
        if (st.frame == st.last) {
            if (st.flags & AnimOnce) {
                st.flags = (st.flags & ~0x200u) | 0x100;
                return finishedResult(st.flags);
            }
            if (st.flags & AnimLoop) st.flags |= 0x80;
            st.next = 0;
            st.last = uint16_t(st.tod->frames() - 1);
            st.flags |= 0x1000;
            st.frame = 0;
        }
        if (st.next < st.tod->frames()) applyTodFrame(st, *st.tod, st.next);  // port guard: 1-frame TODs
        ++st.next;
        ++st.frame;
        st.countdown = st.ticksPerFrame;
    }
    --st.countdown;
    if (uint32_t(st.frame) == uint32_t(st.last) - 1u) st.flags |= 0x2000;
    if (st.countdown != 0 || st.frame != st.last || !(st.flags & AnimOnce)) return AnimPlaying;
    st.flags = (st.flags & ~0x200u) | 0x100;
    return finishedResult(st.flags);
}

std::vector<std::array<float, 3>> poseSlots(const ActorModel& model, const AnimState& st) {
    std::vector<V3> slots;
    std::vector<Work> work;
    skin(model, st, Work{M3{1, 0, 0, 0, 1, 0, 0, 0, 1}, {}}, slots, work);
    return slots;
}

void poseActor(const ActorModel& model, const AnimState& st, std::array<int32_t, 3> pos, std::array<int16_t, 3> rot,
               std::array<int32_t, 3> scale, std::vector<Mesh>& out) {
    std::vector<V3> slots;
    std::vector<Work> work;
    skin(model, st, Work{coordMatrix(rot, scale), {float(pos[0]), float(pos[1]), float(pos[2])}}, slots, work);
    for (size_t i = 0; i < st.parts.size(); ++i) {
        if (!drawn(model, st.parts[i], work[i])) continue;
        for (const ActorMesh& am : model.meshes) {
            if (am.object != uint32_t(st.parts[i].object)) continue;
            Mesh& m = out.emplace_back();
            m.texture = am.mesh.texture;
            m.textureAnim = am.mesh.textureAnim;
            m.blend = am.mesh.blend;
            m.twoSided = am.mesh.twoSided;
            m.vertices = am.mesh.vertices;
            for (size_t v = 0; v < m.vertices.size(); ++v) {
                const V3& s = slots[am.slots[v]];
                m.vertices[v].pos[0] = s[0], m.vertices[v].pos[1] = -s[1], m.vertices[v].pos[2] = -s[2];  // PS1 -> engine
            }
            for (size_t t = 0; t + 2 < am.mesh.indices.size(); t += 3) {
                const uint32_t* tri = &am.mesh.indices[t];
                if (std::isnan(m.vertices[tri[0]].pos[0]) || std::isnan(m.vertices[tri[1]].pos[0]) ||
                    std::isnan(m.vertices[tri[2]].pos[0]))
                    continue;
                m.indices.insert(m.indices.end(), tri, tri + 3);
            }
            if (m.indices.empty()) {
                out.pop_back();
                continue;
            }
            // Unwritten (NaN) vertices are never indexed; give them a real position so bounds stay finite.
            const Vertex& ok = m.vertices[m.indices[0]];
            for (Vertex& v : m.vertices)
                if (std::isnan(v.pos[0])) std::copy_n(ok.pos, 3, v.pos);
        }
    }
}

} // namespace bb
