// Mods: actor models, animations and world meshes as glTF 2.0 (docs/MODDING.md "Models and animations").
// Actor glTF: root node "bbtime" (180° about X, scale 0.001: PS1 axes/units -> glTF), children = TOD parts "part<N>"
// (1-based part list position, hierarchy and rest pose of the skeleton + first frame of the initial animation), each
// carrying the mesh "object<o>" of the TMD object it shows. Node TRS are raw part rot/scale/trans (PS1 space), so an
// animation channel maps 1:1 to TOD coord packets.
#include "bb/actor.h"
#include "bb/bze.h"
#include "bb/mods.h"
#include "mods_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <optional>
#include <print>
#include <regex>
#include <span>
#include <charconv>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace bb::mods {
namespace {

using json = nlohmann::json;
using M4 = std::array<float, 16>;  // column major (glTF)
constexpr float kPi = std::numbers::pi_v<float>;
constexpr size_t kTriWarn = 20000;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// ---- base64 -------------------------------------------------------------------------------------------------------
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string b64(const std::vector<uint8_t>& d) {
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const size_t n = std::min<size_t>(3, d.size() - i);
        const uint32_t v = uint32_t(d[i]) << 16 | (n > 1 ? uint32_t(d[i + 1]) << 8 : 0) | (n > 2 ? d[i + 2] : 0);
        for (size_t k = 0; k < 4; ++k) o += k <= n ? kB64[v >> (18 - 6 * k) & 63] : '=';
    }
    return o;
}
std::vector<uint8_t> unb64(std::string_view s) {
    std::vector<uint8_t> o;
    uint32_t v = 0;
    int n = 0;
    for (const char c : s) {
        const char* p = std::find(kB64, kB64 + 64, c);
        if (p == kB64 + 64) continue;
        v = v << 6 | uint32_t(p - kB64);
        if ((n += 6) >= 8) o.push_back(uint8_t(v >> (n -= 8)));
    }
    return o;
}

// ---- math ---------------------------------------------------------------------------------------------------------
M4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr)
            for (int k = 0; k < 4; ++k) r[size_t(c * 4 + rr)] += a[size_t(k * 4 + rr)] * b[size_t(c * 4 + k)];
    return r;
}
std::array<float, 3> xform(const M4& m, const float* p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
// Quaternion (x, y, z, w) -> row-major 3x3.
std::array<float, 9> quatMat(const std::array<float, 4>& q0) {
    const float n = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    const float x = q0[0] / n, y = q0[1] / n, z = q0[2] / n, w = q0[3] / n;
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
}
// RotMatrix (Rx·Ry·Rz, 4.12) <-> quaternion.
std::array<float, 4> rotQuat(const std::array<int32_t, 3>& r) {
    auto axis = [](int i, int32_t a) {
        const float h = float(a) * kPi / 4096.0f;  // half angle
        std::array<float, 4> q{0, 0, 0, std::cos(h)};
        q[size_t(i)] = std::sin(h);
        return q;
    };
    auto qm = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    return qm(axis(0, r[0]), qm(axis(1, r[1]), axis(2, r[2])));
}
std::array<int32_t, 3> quatRot(const std::array<float, 4>& q) {
    const auto m = quatMat(q);
    // M = Rx(a)·Ry(b)·Rz(c): m02 = sin b, m00/m01 = cos b·(cos c, -sin c), m12/m22 = cos b·(-sin a, cos a).
    const float cb = std::hypot(m[0], m[1]), b = std::atan2(m[2], cb);
    float a, c = 0;
    if (cb > 1e-4f) a = std::atan2(-m[5], m[8]), c = std::atan2(-m[1], m[0]);
    else a = std::atan2(m[2] > 0 ? m[3] : -m[3], m[4]);  // b = ±90°: only a ± c is defined, take c = 0
    auto u = [](float r) { return int32_t(std::lround(r * 4096.0f / (2.0f * kPi))); };
    return {u(a), u(b), u(c)};
}

// ---- glTF reading ---------------------------------------------------------------------------------------------------
struct Gltf {
    json j;
    std::vector<std::vector<uint8_t>> buffers;
    fs::path dir;

    static Gltf load(const fs::path& file) {
        Gltf g;
        g.dir = file.parent_path();
        const std::vector<uint8_t> raw = readFile(file);
        std::vector<uint8_t> glbBin;
        if (raw.size() >= 20 && std::string_view(reinterpret_cast<const char*>(raw.data()), 4) == "glTF") {
            auto u32 = [&](size_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
            size_t o = 12;
            while (o + 8 <= raw.size()) {
                const uint32_t len = u32(o), type = u32(o + 4);
                if (o + 8 + len > raw.size()) throw FormatError("glb: truncated chunk");
                if (type == 0x4e4f534a) g.j = json::parse(raw.begin() + long(o + 8), raw.begin() + long(o + 8 + len));
                else if (type == 0x004e4942) glbBin.assign(raw.begin() + long(o + 8), raw.begin() + long(o + 8 + len));
                o += 8 + len;
            }
        } else {
            g.j = json::parse(raw.begin(), raw.end());
        }
        for (const json& b : g.j.value("buffers", json::array())) {
            const std::string uri = b.value("uri", "");
            if (uri.empty()) g.buffers.push_back(glbBin);
            else if (uri.starts_with("data:")) g.buffers.push_back(unb64(std::string_view(uri).substr(uri.find(',') + 1)));
            else g.buffers.push_back(readFile(g.dir / uri));
        }
        return g;
    }
    const json& at(const char* arr, size_t i) const {
        const json& a = j.at(arr);
        if (i >= a.size()) throw FormatError(std::format("glTF: {} index {} out of range", arr, i));
        return a[i];
    }
    // Accessor as floats (normalized integers scaled), `comps` per element.
    std::vector<float> floats(size_t acc, int& comps) const {
        const json& a = at("accessors", acc);
        static const std::map<std::string, int> kComps{{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
        comps = kComps.at(a.at("type").get<std::string>());
        const size_t count = a.at("count"), ct = a.at("componentType");
        const bool norm = a.value("normalized", false);
        std::vector<float> out(count * size_t(comps), 0.0f);
        if (!a.contains("bufferView")) return out;
        const json& v = at("bufferViews", a["bufferView"]);
        const std::vector<uint8_t>& buf = buffers.at(v.at("buffer"));
        const size_t csize = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : 1;
        const size_t stride = v.value("byteStride", size_t(0)) ? v.value("byteStride", size_t(0)) : csize * size_t(comps);
        const size_t base = v.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
        if (count && base + (count - 1) * stride + csize * size_t(comps) > buf.size()) throw FormatError("glTF: accessor past its buffer");
        for (size_t i = 0; i < count; ++i)
            for (int c = 0; c < comps; ++c) {
                const uint8_t* p = buf.data() + base + i * stride + size_t(c) * csize;
                float f = 0;
                switch (ct) {
                case 5126: std::memcpy(&f, p, 4); break;
                case 5125: { uint32_t x; std::memcpy(&x, p, 4); f = float(x); break; }
                case 5123: { uint16_t x; std::memcpy(&x, p, 2); f = norm ? float(x) / 65535.0f : float(x); break; }
                case 5122: { int16_t x; std::memcpy(&x, p, 2); f = norm ? std::max(float(x) / 32767.0f, -1.0f) : float(x); break; }
                case 5121: f = norm ? float(*p) / 255.0f : float(*p); break;
                case 5120: f = norm ? std::max(float(int8_t(*p)) / 127.0f, -1.0f) : float(int8_t(*p)); break;
                default: throw FormatError("glTF: unsupported component type");
                }
                out[i * size_t(comps) + size_t(c)] = f;
            }
        return out;
    }
    M4 local(const json& n) const {
        if (n.contains("matrix")) {
            M4 m;
            for (size_t i = 0; i < 16; ++i) m[i] = n["matrix"][i];
            return m;
        }
        const auto t = n.value("translation", std::vector<float>{0, 0, 0});
        const auto r = n.value("rotation", std::vector<float>{0, 0, 0, 1});
        const auto s = n.value("scale", std::vector<float>{1, 1, 1});
        const auto q = quatMat({r[0], r[1], r[2], r[3]});
        M4 m{};
        for (int c = 0; c < 3; ++c)
            for (int rr = 0; rr < 3; ++rr) m[size_t(c * 4 + rr)] = q[size_t(rr * 3 + c)] * s[size_t(c)];
        m[12] = t[0], m[13] = t[1], m[14] = t[2], m[15] = 1;
        return m;
    }
    std::vector<int> parents() const {
        std::vector<int> p(j.value("nodes", json::array()).size(), -1);
        for (size_t i = 0; i < p.size(); ++i)
            for (const json& c : j["nodes"][i].value("children", json::array()))
                if (size_t(c) < p.size()) p[size_t(c)] = int(i);
        return p;
    }
    // node index -> part number (1-based) for nodes named part<N>.
    std::map<int, int> parts() const {
        std::map<int, int> out;
        static const std::regex re(R"(part(\d+)(\.\d+)?)");
        const json& nodes = j.value("nodes", json::array());
        for (size_t i = 0; i < nodes.size(); ++i) {
            std::smatch mm;
            const std::string name = nodes[i].value("name", "");
            if (std::regex_match(name, mm, re)) out[int(i)] = std::stoi(mm[1]);
        }
        return out;
    }
};

// ---- glTF writing ---------------------------------------------------------------------------------------------------
struct Out {
    json j = {{"asset", {{"version", "2.0"}, {"generator", "bbtime-modkit"}}},
              {"scene", 0},
              {"scenes", json::array()},
              {"nodes", json::array()},
              {"meshes", json::array()},
              {"materials", json::array()},
              {"accessors", json::array()},
              {"bufferViews", json::array()}};
    std::vector<uint8_t> bin;
    int accessor(const void* p, size_t count, int comp, const char* type, size_t bytes, int target = 0, bool norm = false,
                 json mn = nullptr, json mx = nullptr) {
        while (bin.size() % 4) bin.push_back(0);
        json v{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
        if (target) v["target"] = target;
        bin.insert(bin.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + bytes);
        j["bufferViews"].push_back(v);
        json a{{"bufferView", j["bufferViews"].size() - 1}, {"componentType", comp}, {"count", count}, {"type", type}};
        if (norm) a["normalized"] = true;
        if (!mn.is_null()) a["min"] = mn, a["max"] = mx;
        j["accessors"].push_back(a);
        return int(j["accessors"].size() - 1);
    }
    bool save(const fs::path& file) {
        j["buffers"] = json::array({{{"byteLength", bin.size()}, {"uri", "data:application/octet-stream;base64," + b64(bin)}}});
        for (const char* k : {"materials", "meshes", "images", "textures", "samplers"})
            if (j.contains(k) && j[k].empty()) j.erase(k);
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << j.dump();
        return bool(f);
    }
};

std::string texIdOf(const std::string& name) {
    const size_t p = name.find("/tex/");
    return p == name.npos ? std::string() : name.substr(p + 5);
}

// Material "m<ordinal>_tex<id>" with the exported PNG as image (path relative to the glTF).
struct Materials {
    Out& o;
    const Scene& s;
    fs::path gltfDir, root;
    std::map<std::tuple<int32_t, Blend, bool>, int> made;
    std::map<int32_t, int> ordinal, image;
    int get(const Mesh& m) {
        const auto key = std::tuple{m.texture, m.blend, m.twoSided};
        if (auto it = made.find(key); it != made.end()) return it->second;
        const int k = ordinal.try_emplace(m.texture, int(ordinal.size())).first->second;
        json mat{{"name", m.texture < 0 ? std::format("m{}_untextured", k) : std::format("m{}_tex{}", k, texIdOf(s.textures[size_t(m.texture)].name))},
                 {"doubleSided", m.twoSided},
                 {"pbrMetallicRoughness", {{"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                 {"extras", {{"bbtime_blend", int(m.blend)}}}};
        if (m.blend == Blend::AlphaTest) mat["alphaMode"] = "MASK";
        else if (m.blend != Blend::Opaque) mat["alphaMode"] = "BLEND";
        if (m.texture >= 0) {
            const std::string key = textureKey(s, s.textures[size_t(m.texture)]);
            if (!key.empty()) {
                auto [it, fresh] = image.try_emplace(m.texture, 0);
                if (fresh) {
                    if (!o.j.contains("images")) o.j["images"] = json::array(), o.j["textures"] = json::array(),
                                                 o.j["samplers"] = json::array({{{"magFilter", 9728}, {"minFilter", 9728}}});
                    o.j["images"].push_back({{"uri", (root / (key + ".png")).lexically_relative(gltfDir).generic_string()}});
                    o.j["textures"].push_back({{"sampler", 0}, {"source", o.j["images"].size() - 1}});
                    it->second = int(o.j["textures"].size() - 1);
                }
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", it->second}};
            }
        }
        o.j["materials"].push_back(mat);
        return made[key] = int(o.j["materials"].size() - 1);
    }
};

// One glTF primitive from a mesh with explicit positions.
json primitive(Out& o, const Mesh& m, const std::vector<std::array<float, 3>>& pos, int material) {
    std::vector<float> uv;
    std::vector<uint8_t> col;
    std::array<float, 3> mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < m.vertices.size(); ++i) {
        uv.insert(uv.end(), {m.vertices[i].uv[0], m.vertices[i].uv[1]});
        col.insert(col.end(), m.vertices[i].rgba, m.vertices[i].rgba + 4);
        for (int k = 0; k < 3; ++k) mn[size_t(k)] = std::min(mn[size_t(k)], pos[i][size_t(k)]), mx[size_t(k)] = std::max(mx[size_t(k)], pos[i][size_t(k)]);
    }
    const size_t n = m.vertices.size();
    json attr{{"POSITION", o.accessor(pos.data(), n, 5126, "VEC3", n * 12, 34962, false, mn, mx)},
              {"TEXCOORD_0", o.accessor(uv.data(), n, 5126, "VEC2", n * 8, 34962)},
              {"COLOR_0", o.accessor(col.data(), n, 5121, "VEC4", n * 4, 34962, true)}};
    return {{"attributes", attr}, {"indices", o.accessor(m.indices.data(), m.indices.size(), 5125, "SCALAR", m.indices.size() * 4, 34963)},
            {"material", material}};
}

// ---- actor model helpers ---------------------------------------------------------------------------------------------
std::string modelHash(const ActorModel& m) {
    uint32_t h = 2166136261u;
    for (const auto& o : m.objects) {
        for (const ActorVertex& v : o) {
            for (const float f : v.pos) h = (h ^ uint32_t(int32_t(f))) * 16777619u;
            h = (h ^ v.slot) * 16777619u;
        }
        h = (h ^ 0xffu) * 16777619u;
    }
    // Look too (vertex colours, UVs; not texture ids, which are per level): the gold and the blue TB coin share geometry.
    for (const ActorMesh& am : m.meshes)
        for (const Vertex& v : am.mesh.vertices) {
            for (const uint8_t c : v.rgba) h = (h ^ c) * 16777619u;
            for (const float f : v.uv) h = (h ^ uint32_t(std::lround(f * 4096.0f))) * 16777619u;
        }
    return std::format("m{:08x}", h);
}

// Ids a model answers to, most specific first: "<level>/<asset>", friendly name (bugs / taz), geometry hash.
std::vector<std::string> modelIds(const Scene& s, size_t k) {
    std::vector<std::string> ids{std::format("{}/{}", lower(s.level), s.actorModels[k].modelAsset)};
    for (const ActorInstance& a : s.actors)
        if (a.model == k && a.chunk != 8) {
            const char* n = s.game == GameId::LostInTime ? (a.player ? "bugs" : nullptr) : a.entityId == 2 ? "bugs" : a.entityId == 3 ? "taz" : nullptr;
            if (n && std::ranges::find(ids, n) == ids.end()) ids.push_back(n);
        }
    ids.push_back(modelHash(s.actorModels[k]));
    return ids;
}

uint16_t initialAnim(const Scene& s, size_t k) {
    for (const ActorInstance& a : s.actors)
        if (a.model == k && findAnim(s.actorModels[k], a.anim) >= 0) return a.anim;
    return s.actorModels[k].anims.empty() ? 0 : s.actorModels[k].anims[0].id;
}

AnimState restPose(const ActorModel& m, uint16_t initial) {
    AnimState st;
    animTick(m, st, initial, AnimLoop);
    return st;
}

struct Work { std::array<float, 9> m; std::array<float, 3> t; };
std::vector<Work> worldOf(const std::vector<TodPart>& parts) {
    std::vector<Work> w(parts.size());
    std::vector<uint8_t> done(parts.size(), 0);  // 0 todo, 1 in progress (cycle -> unattached), 2 done
    auto get = [&](auto&& self, size_t i) -> const Work& {
        if (done[i]) return w[i];
        done[i] = 1;
        const TodPart& p = parts[i];
        const std::array<float, 3> t{float(p.trans[0]), float(p.trans[1]), float(p.trans[2])};
        if (p.parent >= 0 && size_t(p.parent) < parts.size() && done[size_t(p.parent)] != 1) {
            const Work& q = self(self, size_t(p.parent));
            Work r{};
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b)
                    for (int c = 0; c < 3; ++c) r.m[size_t(a * 3 + b)] += q.m[size_t(a * 3 + c)] * p.m[size_t(c * 3 + b)];
                r.t[size_t(a)] = q.m[size_t(a * 3)] * t[0] + q.m[size_t(a * 3 + 1)] * t[1] + q.m[size_t(a * 3 + 2)] * t[2] + q.t[size_t(a)];
            }
            w[i] = r;
        } else {
            w[i] = {p.m, t};
        }
        done[i] = 2;
        return w[i];
    };
    for (size_t i = 0; i < parts.size(); ++i) get(get, i);
    return w;
}

// Local position of every mesh vertex of object o (stitch slots written by other objects: rest pose, inverse part).
std::vector<std::array<float, 3>> objectPositions(const ActorModel& m, const ActorMesh& am, const AnimState& st,
                                                  const std::vector<std::array<float, 3>>& slots, const std::vector<Work>& w) {
    std::map<uint16_t, std::array<float, 3>> own;
    for (const ActorVertex& v : m.objects[am.object]) own.try_emplace(v.slot, std::array<float, 3>{v.pos[0], v.pos[1], v.pos[2]});
    int part = -1;
    for (size_t i = 0; i < st.parts.size(); ++i)
        if (st.parts[i].object == int32_t(am.object)) part = int(i);
    std::vector<std::array<float, 3>> out;
    for (const uint16_t sl : am.slots) {
        if (auto it = own.find(sl); it != own.end()) {
            out.push_back(it->second);
            continue;
        }
        std::array<float, 3> p{};
        if (part >= 0 && sl < slots.size() && !std::isnan(slots[sl][0])) {  // inverse(M)·(world - t), M = R·S
            const auto& M = w[size_t(part)].m;
            const float d[3] = {slots[sl][0] - w[size_t(part)].t[0], slots[sl][1] - w[size_t(part)].t[1], slots[sl][2] - w[size_t(part)].t[2]};
            const float det = M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]);
            if (std::abs(det) > 1e-9f) {
                const float inv[9] = {(M[4] * M[8] - M[5] * M[7]) / det, (M[2] * M[7] - M[1] * M[8]) / det, (M[1] * M[5] - M[2] * M[4]) / det,
                                      (M[5] * M[6] - M[3] * M[8]) / det, (M[0] * M[8] - M[2] * M[6]) / det, (M[2] * M[3] - M[0] * M[5]) / det,
                                      (M[3] * M[7] - M[4] * M[6]) / det, (M[1] * M[6] - M[0] * M[7]) / det, (M[0] * M[4] - M[1] * M[3]) / det};
                for (int a = 0; a < 3; ++a) p[size_t(a)] = inv[a * 3] * d[0] + inv[a * 3 + 1] * d[1] + inv[a * 3 + 2] * d[2];
            }
        }
        out.push_back(p);
    }
    return out;
}

// Writes the model's part hierarchy + meshes into o; returns node index per part.
std::vector<int> writeActor(Out& o, const ActorModel& m, const AnimState& st, Materials& mats) {
    const std::vector<std::array<float, 3>> slots = poseSlots(m, st);
    const std::vector<Work> w = worldOf(st.parts);
    std::map<int32_t, int> meshOf;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const ActorMesh& am = m.meshes[i];
        auto [it, fresh] = meshOf.try_emplace(int32_t(am.object), int(o.j["meshes"].size()));
        if (fresh) o.j["meshes"].push_back({{"name", std::format("object{}", am.object)}, {"primitives", json::array()}});
        o.j["meshes"][size_t(it->second)]["primitives"].push_back(primitive(o, am.mesh, objectPositions(m, am, st, slots, w), mats.get(am.mesh)));
    }
    const int root = int(o.j["nodes"].size());
    o.j["nodes"].push_back({{"name", "bbtime"}, {"rotation", {1, 0, 0, 0}}, {"scale", {0.001, 0.001, 0.001}}, {"children", json::array()}});
    o.j["scenes"] = json::array({{{"nodes", {root}}}});
    std::vector<int> node(st.parts.size());
    for (size_t i = 0; i < st.parts.size(); ++i) {
        const TodPart& p = st.parts[i];
        const auto q = rotQuat(p.rot);
        json n{{"name", std::format("part{}", i + 1)},
               {"translation", {p.trans[0], p.trans[1], p.trans[2]}},
               {"rotation", {q[0], q[1], q[2], q[3]}},
               {"scale", {p.scale[0] / 4096.0, p.scale[1] / 4096.0, p.scale[2] / 4096.0}}};
        if (auto it = meshOf.find(p.object); it != meshOf.end()) n["mesh"] = it->second;
        node[i] = int(o.j["nodes"].size());
        o.j["nodes"].push_back(n);
    }
    for (size_t i = 0; i < st.parts.size(); ++i) {
        const int par = st.parts[i].parent;
        json& host = par >= 0 && size_t(par) < st.parts.size() && size_t(par) != i ? o.j["nodes"][size_t(node[size_t(par)])] : o.j["nodes"][size_t(root)];
        if (!host.contains("children")) host["children"] = json::array();
        host["children"].push_back(node[i]);
    }
    if (st.parts.empty())  // rigid one-part templates (HUD, shadows): meshes straight under the root
        for (const auto& [obj, mesh] : meshOf) {
            o.j["nodes"].push_back({{"name", std::format("object{}", obj)}, {"mesh", mesh}});
            o.j["nodes"][size_t(root)]["children"].push_back(o.j["nodes"].size() - 1);
        }
    return node;
}

// Per frame per part {rot, scale, trans} of a clip played from the rest skeleton (the skeleton's parts only).
std::vector<std::vector<TodPart>> sampleClip(const ActorModel& m, const Tod& tod) {
    AnimState st;
    if (m.skeleton.frames()) applyTodFrame(st, m.skeleton, 0);
    if (!st.parts.empty()) st.parts[0].parent = -1;
    const size_t n = st.parts.size();
    std::vector<std::vector<TodPart>> out;
    for (size_t f = 0; f < tod.frames(); ++f) {
        applyTodFrame(st, tod, f);
        out.emplace_back(st.parts.begin(), st.parts.begin() + long(std::min(n, st.parts.size())));
    }
    return out;
}

// Unpacked copy of a TOD: per frame a full coord packet for every skeleton part, object changes (type 2) and box
// changes (type 9). Packed (TB) clips become editable this way.
Tod bake(const ActorModel& m, const Tod& tod) {
    Tod out{.resolution = tod.resolution, .frameStart = {0}, .packets = {}, .packed = false};
    AnimState st;
    if (m.skeleton.frames()) applyTodFrame(st, m.skeleton, 0);
    if (!st.parts.empty()) st.parts[0].parent = -1;
    std::vector<int32_t> obj;
    for (const TodPart& p : st.parts) obj.push_back(p.object);
    std::array<int16_t, 6> box{};
    bool boxSet = false;
    for (size_t f = 0; f < tod.frames(); ++f) {
        applyTodFrame(st, tod, f);
        for (size_t i = 0; i < std::min(obj.size(), st.parts.size()); ++i) {
            const TodPart& p = st.parts[i];
            if (p.object != obj[i]) out.packets.push_back({uint16_t(i + 1), 2, 0, {p.object + 1}}), obj[i] = p.object;
            out.packets.push_back({uint16_t(i + 1), 1, 0xe, {p.rot[0], p.rot[1], p.rot[2], p.scale[0], p.scale[1], p.scale[2], p.trans[0], p.trans[1], p.trans[2]}});
        }
        if (st.boxSet && (!boxSet || st.box != box)) {
            TodPacket b{0, 9, 0, {}};
            for (int i = 0; i < 6; ++i) b.v[size_t(i)] = st.box[size_t(i)];
            b.v[6] = st.radius;
            out.packets.push_back(b);
            box = st.box, boxSet = true;
        }
        out.frameStart.push_back(uint32_t(out.packets.size()));
    }
    return out;
}

const Tod& editable(const ActorModel& m, const Tod& t, Tod& tmp) { return t.packed ? (tmp = bake(m, t)) : t; }

// Replaces box packets (type 9) of `tod` by those of `boxes` (same frame index), keeping gameplay boxes.
void keepBoxes(Tod& tod, const Tod& boxes) {
    Tod out{.resolution = tod.resolution, .frameStart = {0}, .packets = {}, .packed = false};
    for (size_t f = 0; f < tod.frames(); ++f) {
        for (uint32_t k = tod.frameStart[f]; k < tod.frameStart[f + 1]; ++k)
            if (tod.packets[k].type != 9) out.packets.push_back(tod.packets[k]);
        if (f < boxes.frames())
            for (uint32_t k = boxes.frameStart[f]; k < boxes.frameStart[f + 1]; ++k)
                if (boxes.packets[k].type == 9) out.packets.push_back(boxes.packets[k]);
        out.frameStart.push_back(uint32_t(out.packets.size()));
    }
    tod = std::move(out);
}

// ---- import ---------------------------------------------------------------------------------------------------------
float sampleAt(const std::vector<float>& t, const std::vector<float>& v, int comps, bool step, bool cubic, float time, int c) {
    const size_t n = t.size(), stride = cubic ? 3 : 1, off = cubic ? 1 : 0;
    auto val = [&](size_t i) { return v[(i * stride + off) * size_t(comps) + size_t(c)]; };
    if (n == 0) return 0;
    if (time <= t[0]) return val(0);
    if (time >= t[n - 1]) return val(n - 1);
    const size_t i = size_t(std::upper_bound(t.begin(), t.end(), time) - t.begin()) - 1;
    if (step) return val(i);
    const float a = (time - t[i]) / std::max(t[i + 1] - t[i], 1e-9f);
    return val(i) + (val(i + 1) - val(i)) * a;
}
std::array<float, 4> slerpAt(const std::vector<float>& t, const std::vector<float>& v, bool step, bool cubic, float time) {
    std::array<float, 4> q{};
    // Normalized lerp with hemisphere fix (keys are dense in practice: exporters bake per frame).
    const size_t n = t.size(), stride = cubic ? 3 : 1, off = cubic ? 1 : 0;
    auto key = [&](size_t i) { return std::array<float, 4>{v[(i * stride + off) * 4], v[(i * stride + off) * 4 + 1], v[(i * stride + off) * 4 + 2], v[(i * stride + off) * 4 + 3]}; };
    if (n == 0) return {0, 0, 0, 1};
    if (time <= t[0]) return key(0);
    if (time >= t[n - 1]) return key(n - 1);
    const size_t i = size_t(std::upper_bound(t.begin(), t.end(), time) - t.begin()) - 1;
    if (step) return key(i);
    const float a = (time - t[i]) / std::max(t[i + 1] - t[i], 1e-9f);
    auto q0 = key(i), q1 = key(i + 1);
    const float d = q0[0] * q1[0] + q0[1] * q1[1] + q0[2] * q1[2] + q0[3] * q1[3];
    for (int k = 0; k < 4; ++k) q[size_t(k)] = q0[size_t(k)] + ((d < 0 ? -q1[size_t(k)] : q1[size_t(k)]) - q0[size_t(k)]) * a;
    return q;
}

// glTF clip -> TOD. base: the clip it replaces (unpacked), whose other packets (objects, boxes, attach points, parts
// the file does not animate) are kept; nullptr for a new clip.
Tod importClip(const Gltf& g, const Tod* base, std::string& err) {
    const json& anims = g.j.value("animations", json::array());
    if (anims.empty()) {
        err = "no animation in the file";
        return {};
    }
    const std::map<int, int> parts = g.parts();
    struct Chan { int part; std::string path; std::vector<float> t, v; int comps; bool step, cubic; };
    std::vector<Chan> chans;
    float end = 0;
    for (const json& a : anims)  // one clip per file; exporters may split it into one animation per node
    for (const json& c : a.value("channels", json::array())) {
        const json& tg = c.at("target");
        if (!tg.contains("node")) continue;
        const auto p = parts.find(tg["node"].get<int>());
        if (p == parts.end()) continue;
        const json& smp = a.at("samplers").at(c.at("sampler").get<size_t>());
        Chan ch{p->second, tg.at("path"), {}, {}, 0, false, false};
        if (ch.path != "translation" && ch.path != "rotation" && ch.path != "scale") continue;
        int one = 0;
        ch.t = g.floats(smp.at("input"), one);
        ch.v = g.floats(smp.at("output"), ch.comps);
        const std::string ip = smp.value("interpolation", "LINEAR");
        ch.step = ip == "STEP", ch.cubic = ip == "CUBICSPLINE";
        if (!ch.t.empty()) end = std::max(end, ch.t.back());
        chans.push_back(std::move(ch));
    }
    if (chans.empty()) {
        err = "the animation drives no part<N> node";
        return {};
    }
    const uint16_t res = base && base->resolution >= 2 ? base->resolution : 2;
    const float tpf = float(std::max(res >> 1, 1));
    const size_t frames = std::max<size_t>(2, size_t(std::lround(end * 30.0f / tpf)) + 1);
    std::map<int, uint8_t> animated;  // part -> coord flag bits
    for (const Chan& c : chans) animated[c.part] |= c.path == "rotation" ? 2 : c.path == "scale" ? 4 : 8;
    Tod out{.resolution = res, .frameStart = {0}, .packets = {}, .packed = false};
    for (size_t f = 0; f < frames; ++f) {
        if (base && f < base->frames())
            for (uint32_t k = base->frameStart[f]; k < base->frameStart[f + 1]; ++k) {
                TodPacket pk = base->packets[k];
                if (pk.type == 1 && animated.contains(pk.id)) {
                    pk.flag &= uint8_t(~animated[pk.id]);
                    if (!(pk.flag & 0xe)) continue;
                }
                out.packets.push_back(pk);
            }
        const float time = float(f) * tpf / 30.0f;
        for (const auto& [part, bits] : animated) {
            TodPacket pk{uint16_t(part), 1, bits, {}};
            for (const Chan& c : chans) {
                if (c.part != part) continue;
                if (c.path == "rotation") {
                    const auto r = quatRot(slerpAt(c.t, c.v, c.step, c.cubic, time));
                    pk.v[0] = r[0], pk.v[1] = r[1], pk.v[2] = r[2];
                } else {
                    for (int k = 0; k < 3; ++k) {
                        const float x = sampleAt(c.t, c.v, c.comps, c.step, c.cubic, time, k);
                        pk.v[size_t(k + (c.path == "scale" ? 3 : 6))] = int32_t(std::lround(c.path == "scale" ? x * 4096.0f : x));
                    }
                }
            }
            out.packets.push_back(pk);
        }
        out.frameStart.push_back(uint32_t(out.packets.size()));
    }
    return out;
}

// Texture + blend for an imported material. Originals: name m<k>_tex<id> whose image is <id>.png (or missing) ->
// original(k, id); otherwise the image is loaded as a new texture.
struct MatResult { int32_t texture = -1, textureAnim = -1; Blend blend = Blend::Opaque; bool twoSided = false; uint8_t tint[4]{255, 255, 255, 255}; };
MatResult material(const Gltf& g, const json* prim, Scene* sMut, const Scene& s, std::map<std::string, int32_t>& loaded,
                   const std::function<std::optional<MatResult>(int k, const std::string& id)>& original, std::vector<std::string>& notes) {
    MatResult r;
    if (!prim || !prim->contains("material")) return r;
    const json& m = g.at("materials", (*prim)["material"]);
    r.twoSided = m.value("doubleSided", false);
    const std::string mode = m.value("alphaMode", "OPAQUE");
    r.blend = mode == "MASK" ? Blend::AlphaTest : mode == "BLEND" ? Blend::Alpha : Blend::Opaque;
    const json pbr = m.value("pbrMetallicRoughness", json::object());
    if (pbr.contains("baseColorFactor"))
        for (int k = 0; k < 4; ++k) r.tint[k] = uint8_t(std::clamp(pbr["baseColorFactor"][size_t(k)].get<float>(), 0.0f, 1.0f) * 255.0f);
    std::string uri;
    std::optional<size_t> view;
    if (pbr.contains("baseColorTexture")) {
        const json& tex = g.at("textures", pbr["baseColorTexture"].at("index"));
        if (tex.contains("source")) {
            const json& im = g.at("images", tex["source"]);
            uri = im.value("uri", "");
            if (im.contains("bufferView")) view = im["bufferView"].get<size_t>();
        }
    }
    static const std::regex re(R"(m(\d+)_tex(\d+).*)");
    std::smatch mm;
    const std::string name = m.value("name", "");
    const bool named = std::regex_match(name, mm, re);
    const fs::path file = uri.empty() || uri.starts_with("data:") ? fs::path() : g.dir / uri;
    if (named && !view && (uri.empty() || file.filename() == mm[2].str() + ".png"))
        if (auto o = original(std::stoi(mm[1]), mm[2])) {
            o->twoSided = r.twoSided;
            return *o;
        }
    if (uri.empty() && !view) return r;
    const std::string cacheKey = view ? std::format("{}#view{}", g.dir.string(), *view) : uri.starts_with("data:") ? uri.substr(0, 256) + std::to_string(uri.size()) : file.string();
    if (auto it = loaded.find(cacheKey); it != loaded.end()) {
        r.texture = it->second;
        return r;
    }
    for (size_t i = 0; i < s.textures.size(); ++i)  // registered earlier (apply() preloads images for simulation-built models)
        if (s.textures[i].name == "mod:" + cacheKey) {
            if (r.blend == Blend::Opaque && s.textures[i].hasAlpha) r.blend = Blend::AlphaTest;
            r.texture = loaded[cacheKey] = int32_t(i);
            return r;
        }
    if (!sMut) {
        notes.push_back(std::format("mods: material {}: image {} not registered with the level; untextured", name, uri.empty() ? "(embedded)" : uri));
        return r;
    }
    Texture t;
    std::string err;
    if (view || uri.starts_with("data:")) {
        std::vector<uint8_t> bytes;
        if (view) {
            const json& v = g.at("bufferViews", *view);
            const auto& b = g.buffers.at(v.at("buffer"));
            const size_t off = v.value("byteOffset", size_t(0)), len = v.at("byteLength");
            bytes.assign(b.begin() + long(off), b.begin() + long(off + len));
        } else {
            bytes = unb64(std::string_view(uri).substr(uri.find(',') + 1));
        }
        const fs::path tmp = fs::temp_directory_path() / std::format("bbtime-mod-{}.png", std::hash<std::string>{}(cacheKey));
        std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        t = loadPng(tmp, &err);
        std::error_code ec;
        fs::remove(tmp, ec);
    } else {
        t = loadPng(file, &err);
    }
    if (t.rgba.empty()) {
        notes.push_back(std::format("mods: material {}: image {}: {}", name, uri.empty() ? "(embedded)" : uri, err));
        return r;
    }
    t.name = "mod:" + cacheKey;
    if (r.blend == Blend::Opaque && t.hasAlpha) r.blend = Blend::AlphaTest;
    r.texture = loaded[cacheKey] = int32_t(sMut->textures.size());
    sMut->textures.push_back(std::move(t));
    return r;
}

// Every triangle primitive under a node, positions in the space of `space` (an ancestor node or the root).
template <class F> void eachPrimitive(const Gltf& g, int nodeIdx, const M4& toSpace, F&& f) {
    const json& n = g.at("nodes", size_t(nodeIdx));
    if (n.contains("mesh"))
        for (const json& p : g.at("meshes", n["mesh"]).value("primitives", json::array()))
            if (p.value("mode", 4) == 4) f(n, p, toSpace);
}

// Replaces the geometry of model k by the meshes of a model glTF (see header). Returns triangles imported.
// sMut: the scene new textures go to (nullptr: only textures registered in s, see preloadTextures).
size_t importModel(Scene* sMut, const Scene& s, ActorModel& m, uint16_t initial, const Gltf& g, std::vector<std::string>& notes,
                   const std::string& what) {
    const AnimState rest = restPose(m, initial);
    const std::map<int, int> parts = g.parts();
    const std::vector<int> par = g.parents();
    // Vertices at an original position of their object (own or stitched from a neighbour part, rest pose) keep that
    // slot, so unedited seams skin exactly as before; moved / new vertices get fresh slots (rigid on their part).
    using Key = std::array<int32_t, 3>;
    auto keyOf = [](const std::array<float, 3>& p) { return Key{int32_t(std::lround(p[0] * 2)), int32_t(std::lround(p[1] * 2)), int32_t(std::lround(p[2] * 2))}; };
    std::map<uint32_t, std::map<Key, uint16_t>> weld;
    const std::vector<std::array<float, 3>> restSlots = poseSlots(m, rest);
    const std::vector<Work> restWork = worldOf(rest.parts);
    // Original texture of the model by first-appearance ordinal (the exporter's material numbering).
    std::vector<const ActorMesh*> ord;
    for (const ActorMesh& am : m.meshes)
        if (std::ranges::none_of(ord, [&](const ActorMesh* o) { return o->mesh.texture == am.mesh.texture; })) ord.push_back(&am);
    auto original = [&](int ordinal, const std::string&) -> std::optional<MatResult> {
        if (ordinal < 0 || size_t(ordinal) >= ord.size()) return std::nullopt;
        const Mesh& o = ord[size_t(ordinal)]->mesh;
        return MatResult{o.texture, o.textureAnim, o.blend, o.twoSided, {255, 255, 255, 255}};
    };
    std::map<std::string, int32_t> loaded;
    std::set<uint32_t> cleared;
    std::vector<ActorMesh> added;
    size_t tris = 0;
    static const std::regex objRe(R"(object(\d+)(\.\d+)?)");
    const json& nodes = g.j.value("nodes", json::array());
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        if (!nodes[ni].contains("mesh")) continue;
        // Nearest part<N> ancestor (or self); transform from it down to this node.
        M4 rel = identity();
        int cur = int(ni), part = -1;
        while (cur >= 0) {
            if (auto it = parts.find(cur); it != parts.end()) {
                part = it->second;
                break;
            }
            if (par[size_t(cur)] < 0 && size_t(cur) != ni) break;  // the root ("bbtime": PS1 axes -> glTF) is not geometry
            rel = mul(g.local(nodes[size_t(cur)]), rel);
            cur = par[size_t(cur)];
        }
        std::smatch mm;
        const std::string meshName = g.at("meshes", nodes[ni]["mesh"]).value("name", "");
        int obj = std::regex_match(meshName, mm, objRe) ? std::stoi(mm[1]) : -1;
        if (obj < 0 && part >= 1 && size_t(part) <= rest.parts.size()) obj = rest.parts[size_t(part - 1)].object;
        if (obj < 0 || size_t(obj) >= m.objects.size()) {
            notes.push_back(std::format("mods: {}: mesh '{}' is not under a part node showing an object; skipped", what, meshName));
            continue;
        }
        // Neutral vertex colour when the file has none: the replaced object's first colour (PS1 128 = texel as is).
        uint8_t neutral[4] = {128, 128, 128, 255};
        for (const ActorMesh& am : m.meshes)
            if (am.object == uint32_t(obj) && !am.mesh.vertices.empty()) {
                std::copy_n(am.mesh.vertices[0].rgba, 4, neutral);
                break;
            }
        if (cleared.insert(uint32_t(obj)).second) {
            std::map<Key, uint16_t>& wm = weld[uint32_t(obj)];
            for (const ActorMesh& am : m.meshes)
                if (am.object == uint32_t(obj)) {
                    const auto pos = objectPositions(m, am, rest, restSlots, restWork);
                    for (size_t i = 0; i < pos.size(); ++i) wm.try_emplace(keyOf(pos[i]), am.slots[i]);
                }
        }
        eachPrimitive(g, int(ni), rel, [&](const json&, const json& p, const M4& x) {
            const json& at = p.at("attributes");
            int c = 0;
            const std::vector<float> pos = g.floats(at.at("POSITION"), c);
            const size_t n = pos.size() / 3;
            std::vector<float> uv, col;
            int cu = 0, cc = 0;
            if (at.contains("TEXCOORD_0")) uv = g.floats(at["TEXCOORD_0"], cu);
            if (at.contains("COLOR_0")) col = g.floats(at["COLOR_0"], cc);
            std::vector<uint32_t> idx;
            if (p.contains("indices")) {
                int one = 0;
                for (const float f : g.floats(p["indices"], one)) idx.push_back(uint32_t(f));
            } else {
                for (uint32_t i = 0; i < n; ++i) idx.push_back(i);
            }
            const MatResult mr = material(g, &p, sMut, s, loaded, original, notes);
            ActorMesh am;
            am.object = uint32_t(obj);
            am.mesh.texture = mr.texture, am.mesh.textureAnim = mr.textureAnim, am.mesh.blend = mr.blend, am.mesh.twoSided = mr.twoSided;
            am.mesh.name = std::format("mod/{}/object{}", what, obj);
            for (size_t i = 0; i < n; ++i) {
                const auto q = xform(x, &pos[i * 3]);
                uint16_t slot;
                if (auto w = weld[uint32_t(obj)].find(keyOf(q)); w != weld[uint32_t(obj)].end()) slot = w->second;
                else {
                    slot = uint16_t(m.slotCount++);
                    m.objects[size_t(obj)].push_back({{q[0], q[1], q[2]}, slot, false});
                }
                Vertex v{};
                if (cu >= 2) v.uv[0] = uv[i * size_t(cu)], v.uv[1] = uv[i * size_t(cu) + 1];
                for (int k2 = 0; k2 < 4; ++k2)
                    v.rgba[k2] = uint8_t(std::clamp((cc >= 3 && (k2 < 3 || cc == 4) ? col[i * size_t(cc) + size_t(k2)] * 255.0f : float(neutral[k2])) *
                                                        float(mr.tint[k2]) / 255.0f,
                                                    0.0f, 255.0f));
                am.mesh.vertices.push_back(v);
                am.slots.push_back(slot);
            }
            for (const uint32_t i : idx)
                if (i < n) am.mesh.indices.push_back(i);
            am.mesh.indices.resize(am.mesh.indices.size() / 3 * 3);
            tris += am.mesh.indices.size() / 3;
            added.push_back(std::move(am));
        });
    }
    if (m.slotCount > 0xffff) notes.push_back(std::format("mods: {}: more than 65535 vertices; the model is not drawn correctly", what));
    std::erase_if(m.meshes, [&](const ActorMesh& am) { return cleared.contains(am.object); });
    for (ActorMesh& am : added) m.meshes.push_back(std::move(am));
    return tris;
}

// World meshes: nodes "<mesh index>#<name>" replace Scene::meshes[index] (engine space, under the 0.001 root).
void importWorld(Scene& s, const Gltf& g, std::vector<std::string>& notes) {
    const std::vector<int> par = g.parents();
    const json& nodes = g.j.value("nodes", json::array());
    std::map<std::string, int32_t> loaded;
    std::map<size_t, std::vector<Mesh>> repl;
    static const std::regex re(R"((\d+)#.*)");
    for (size_t ni = 0; ni < nodes.size(); ++ni) {
        std::smatch mm;
        const std::string name = nodes[ni].value("name", "");
        if (!std::regex_match(name, mm, re)) continue;
        const size_t mi = std::stoul(mm[1]);
        if (mi >= s.meshes.size()) continue;
        repl[mi];  // a node without mesh deletes the original
        M4 x = identity();
        for (int cur = int(ni); cur >= 0 && par[size_t(cur)] >= 0; cur = par[size_t(cur)]) x = mul(g.local(nodes[size_t(cur)]), x);
        const Mesh& orig = s.meshes[mi];
        auto original = [&](int, const std::string& id) -> std::optional<MatResult> {
            for (size_t t = 0; t < s.textures.size(); ++t)
                if (s.textures[t].name == std::format("{}/tex/{}", s.level, id))
                    return MatResult{int32_t(t), int32_t(t) == orig.texture ? orig.textureAnim : -1, orig.blend, orig.twoSided, {255, 255, 255, 255}};
            return MatResult{orig.texture, orig.textureAnim, orig.blend, orig.twoSided, {255, 255, 255, 255}};
        };
        eachPrimitive(g, int(ni), x, [&](const json&, const json& p, const M4& xf) {
            const json& at = p.at("attributes");
            int c = 0, cu = 0, cc = 0;
            const std::vector<float> pos = g.floats(at.at("POSITION"), c);
            std::vector<float> uv, col;
            if (at.contains("TEXCOORD_0")) uv = g.floats(at["TEXCOORD_0"], cu);
            if (at.contains("COLOR_0")) col = g.floats(at["COLOR_0"], cc);
            const MatResult mr = material(g, &p, &s, s, loaded, original, notes);
            Mesh me{orig.name, mr.texture, mr.blend, {}, {}, mr.twoSided, mr.textureAnim};
            const size_t n = pos.size() / 3;
            for (size_t i = 0; i < n; ++i) {
                Vertex v{};
                const auto q = xform(xf, &pos[i * 3]);
                std::copy_n(q.data(), 3, v.pos);
                if (cu >= 2) v.uv[0] = uv[i * size_t(cu)], v.uv[1] = uv[i * size_t(cu) + 1];
                for (int k = 0; k < 4; ++k)
                    v.rgba[k] = uint8_t(std::clamp(cc >= 3 && (k < 3 || cc == 4) ? col[i * size_t(cc) + size_t(k)] * 255.0f : k < 3 ? 128.0f : 255.0f, 0.0f, 255.0f));
                me.vertices.push_back(v);
            }
            if (p.contains("indices")) {
                int one = 0;
                for (const float f : g.floats(p["indices"], one))
                    if (size_t(f) < n) me.indices.push_back(uint32_t(f));
            } else {
                for (uint32_t i = 0; i < n; ++i) me.indices.push_back(i);
            }
            me.indices.resize(me.indices.size() / 3 * 3);
            repl[mi].push_back(std::move(me));
        });
    }
    // Replace in place (first primitive) and append the rest, keeping indices of untouched meshes stable.
    for (auto& [mi, ms] : repl) {
        if (ms.empty()) {
            s.meshes[mi].indices.clear();
            continue;
        }
        s.meshes[mi] = std::move(ms[0]);
        for (size_t i = 1; i < ms.size(); ++i) s.meshes.push_back(std::move(ms[i]));
    }
}

std::vector<size_t> modelsMatching(const Scene& s, const std::string& id) {
    std::vector<size_t> out;
    for (size_t k = 0; k < s.actorModels.size(); ++k)
        if (std::ranges::contains(modelIds(s, k), id)) out.push_back(k);
    return out;
}

} // namespace

namespace {

// Ids of a model the simulation builds itself (TB pickups / props spawned from templates): no friendly name.
std::vector<std::string> idsOf(const Scene& s, const ActorModel& m) {
    return {std::format("{}/{}", lower(s.level), m.modelAsset), modelHash(m)};
}

// a takes b's skeleton, geometry and clips; a's gameplay boxes stay (clips b lacks play b's first clip).
void swapModel(ActorModel& a, const ActorModel& b) {
    std::vector<ActorAnim> anims;
    for (const ActorAnim& tb : b.anims) {
        Tod tmp, tmp2;
        ActorAnim na{tb.id, editable(b, tb.tod, tmp)};
        if (const int ai = findAnim(a, tb.id); ai >= 0) keepBoxes(na.tod, editable(a, a.anims[size_t(ai)].tod, tmp2));
        anims.push_back(std::move(na));
    }
    for (const ActorAnim& ta : a.anims)  // so actions waiting for a clip end still finish
        if (findAnim(b, ta.id) < 0 && !b.anims.empty()) {
            Tod tmp, tmp2;
            ActorAnim na{ta.id, editable(b, b.anims[0].tod, tmp)};
            keepBoxes(na.tod, editable(a, ta.tod, tmp2));
            anims.push_back(std::move(na));
        }
    const uint16_t ma = a.modelAsset, sa = a.skeletonAsset;
    a = b;
    a.anims = std::move(anims);
    a.modelAsset = ma, a.skeletonAsset = sa;
}

// Swaps, model file, clip files and clip aliases for one model answering to `ids` (most specific first).
void applyOne(Scene* sMut, const Scene& s, ActorModel& am, const std::vector<std::string>& ids, uint16_t initial, const ModSet& m,
              std::vector<std::string>& notes) {
    const std::string g = gameKey(s.game);
    auto note = [&](std::string msg) { notes.push_back(std::move(msg)); };
    // 1. mod.json swaps: the target's skeleton, geometry and clips, with the original's gameplay boxes.
    for (const Info& mod : m.mods())
        for (const auto& [from, to] : mod.swap) {
            if (!from.starts_with(g + ":") || !std::ranges::contains(ids, from.substr(g.size() + 1))) continue;
            const std::string target = to.starts_with(g + ":") ? to.substr(g.size() + 1) : to;
            const std::vector<size_t> dst = modelsMatching(s, target);
            const fs::path* file = m.find(std::format("models/{}/{}", g, target));
            if (!dst.empty() && &s.actorModels[dst[0]] != &am) {
                swapModel(am, s.actorModels[dst[0]]);
                note(std::format("mods: {} swapped {} -> {}", mod.id, from, to));
            } else if (file) {
                try {
                    importModel(sMut, s, am, initial, Gltf::load(*file), notes, target);
                    note(std::format("mods: {} swapped {} -> model {}", mod.id, from, target));
                } catch (const std::exception& e) {
                    note(std::format("mods: {}: {}", file->string(), e.what()));
                }
            } else {
                note(std::format("mods: {}: swap target '{}' is neither a model of {} nor models/{}/{}.gltf", mod.id, to, s.level, g, target));
            }
            m.applied(std::format("swap/{}/{}", from, to));
        }
    // 2. Geometry, most specific id first.
    for (const std::string& id : ids) {
        const std::string key = std::format("models/{}/{}", g, id);
        if (const fs::path* f = m.find(key)) {
            try {
                importModel(sMut, s, am, initial, Gltf::load(*f), notes, id);
                m.applied(key);
            } catch (const std::exception& e) {
                note(std::format("mods: {}: {}", f->string(), e.what()));
            }
            break;
        }
    }
    // 3. Clips.
    std::set<uint16_t> done;
    for (const std::string& id : ids)
        for (const std::string& key : m.keys(std::format("animations/{}/{}/", g, id))) {
            const std::string rest = key.substr(key.rfind('/') + 1);
            uint16_t animId = 0;
            if (std::from_chars(rest.data(), rest.data() + rest.size(), animId).ec != std::errc{} || !done.insert(animId).second) continue;
            const fs::path& f = *m.find(key);
            try {
                const int ai = findAnim(am, animId);
                Tod tmp;
                const Tod* base = ai >= 0 ? &editable(am, am.anims[size_t(ai)].tod, tmp) : nullptr;
                std::string err;
                Tod t = importClip(Gltf::load(f), base, err);
                if (!err.empty()) {
                    note(std::format("mods: {}: {}", f.string(), err));
                    continue;
                }
                if (ai >= 0) am.anims[size_t(ai)].tod = std::move(t);
                else am.anims.push_back({animId, std::move(t)});
                m.applied(key);
            } catch (const std::exception& e) {
                note(std::format("mods: {}: {}", f.string(), e.what()));
            }
        }
    // 4. mod.json "anims": {"<game>:<model id>:<anim id>": <clip id>}.
    for (const Info& mod : m.mods())
        for (const auto& [from, clip] : mod.anims)
            for (const std::string& id : ids) {
                const std::string pre = std::format("{}:{}:", g, id);
                if (!from.starts_with(pre)) continue;
                uint16_t animId = 0;
                const std::string rest = from.substr(pre.size());
                if (std::from_chars(rest.data(), rest.data() + rest.size(), animId).ec != std::errc{}) continue;
                const int ci = findAnim(am, clip);
                if (ci < 0) {
                    note(std::format("mods: {}: anim alias {} -> {}: no clip {}", mod.id, from, clip, clip));
                    continue;
                }
                const Tod t = am.anims[size_t(ci)].tod;
                if (const int ai = findAnim(am, animId); ai >= 0) am.anims[size_t(ai)].tod = t;
                else am.anims.push_back({animId, t});
                m.applied("anims/" + from);
            }
}

// Registers the images of every mod model file of the game as scene textures ("mod:<file>"), so models the
// simulation builds later (applyToModel, const scene) find them uploaded with the level.
void preloadTextures(Scene& s, const ModSet& m) {
    const std::string pre = std::format("models/{}/", gameKey(s.game));
    std::vector<std::string> notes;
    for (const std::string& key : m.keys(pre)) {
        if (key.ends_with("/world")) continue;
        try {
            const Gltf g = Gltf::load(*m.find(key));
            std::map<std::string, int32_t> loaded;
            auto skip = [](int, const std::string&) -> std::optional<MatResult> { return MatResult{}; };  // originals stay
            for (const json& me : g.j.value("meshes", json::array()))
                for (const json& p : me.value("primitives", json::array())) material(g, &p, &s, s, loaded, skip, notes);
        } catch (const std::exception&) {}  // reported when the file is applied
    }
}

} // namespace

void applyModels(Scene& s, const ModSet& m) {
    for (size_t k = 0; k < s.actorModels.size(); ++k) applyOne(&s, s, s.actorModels[k], modelIds(s, k), initialAnim(s, k), m, s.notes);
    if (s.game == GameId::TimeBusters) preloadTextures(s, m);
    const std::string wkey = std::format("models/{}/{}/world", gameKey(s.game), lower(s.level));
    if (const fs::path* f = m.find(wkey)) {
        try {
            importWorld(s, Gltf::load(*f), s.notes);
            m.applied(wkey);
        } catch (const std::exception& e) {
            s.notes.push_back(std::format("mods: {}: {}", f->string(), e.what()));
        }
    }
}

void applyToModel(const Scene& s, ActorModel& model) {
    const ModSet& m = active();
    if (m.empty()) return;
    std::vector<std::string> notes;
    applyOne(nullptr, s, model, idsOf(s, model), model.anims.empty() ? 0 : model.anims[0].id, m, notes);
    for (const std::string& n : notes) std::println(stderr, "bbtime: {}", n);
}

size_t exportModels(const Scene& s, const fs::path& out, std::span<const ActorModel* const> extra) {
    const std::string g = gameKey(s.game);
    size_t files = 0;
    std::string index = std::format("# {} {}: level asset id, model id (models/{}/<model id>.gltf), built by scene|simulation, parts, anim ids\n", g, s.level, g);
    std::set<std::string> written;
    auto one = [&](const ActorModel& m, const std::vector<std::string>& ids, uint16_t initial, const char* kind) {
        const std::string id = ids.size() > 2 ? ids[1] : ids.back();  // friendly name, else the hash
        const AnimState rest = restPose(m, initial);
        index += std::format("{} {} {} objects={} parts={} anims=", m.modelAsset, id, kind, m.objects.size(), rest.parts.size());
        for (const ActorAnim& a : m.anims) index += std::format("{},", a.id);
        index += '\n';
        if (m.meshes.empty() || !written.insert(id).second) return;
        {
            const fs::path file = out / "models" / g / (id + ".gltf");
            Out o;
            Materials mats{o, s, file.parent_path(), out, {}, {}, {}};
            writeActor(o, m, rest, mats);
            files += o.save(file);
        }
        for (const ActorAnim& a : m.anims) {
            const fs::path file = out / "animations" / g / id / std::format("{}.gltf", a.id);
            Out o;
            Materials mats{o, s, file.parent_path(), out, {}, {}, {}};
            const std::vector<int> node = writeActor(o, m, rest, mats);
            const std::vector<std::vector<TodPart>> frames = sampleClip(m, a.tod);
            const float tpf = float(std::max(a.tod.resolution >> 1, 1));
            std::vector<float> times;
            for (size_t f = 0; f < frames.size(); ++f) times.push_back(float(f) * tpf / 30.0f);
            if (times.empty()) continue;
            json anim{{"name", std::format("anim{}", a.id)}, {"channels", json::array()}, {"samplers", json::array()}};
            const int tacc = o.accessor(times.data(), times.size(), 5126, "SCALAR", times.size() * 4, 0, false, {times.front()}, {times.back()});
            for (size_t p = 0; p < node.size(); ++p) {
                std::vector<float> tr, rot, sc;
                for (const auto& fr : frames) {
                    const TodPart q = p < fr.size() ? fr[p] : rest.parts[p];
                    const auto qq = rotQuat(q.rot);
                    tr.insert(tr.end(), {float(q.trans[0]), float(q.trans[1]), float(q.trans[2])});
                    rot.insert(rot.end(), qq.begin(), qq.end());
                    sc.insert(sc.end(), {q.scale[0] / 4096.0f, q.scale[1] / 4096.0f, q.scale[2] / 4096.0f});
                }
                for (const auto& [path, data, type] : {std::tuple{"translation", &tr, "VEC3"}, {"rotation", &rot, "VEC4"}, {"scale", &sc, "VEC3"}}) {
                    anim["samplers"].push_back({{"input", tacc}, {"interpolation", "LINEAR"},
                                                {"output", o.accessor(data->data(), times.size(), 5126, type, data->size() * 4)}});
                    anim["channels"].push_back({{"sampler", anim["samplers"].size() - 1}, {"target", {{"node", node[p]}, {"path", path}}}});
                }
            }
            o.j["animations"] = json::array({anim});
            files += o.save(file);
        }
    };
    // Models sharing a geometry hash share a file: write the animated (parts) version first.
    for (const bool parts : {true, false}) {
        for (size_t k = 0; k < s.actorModels.size(); ++k)
            if ((s.actorModels[k].skeleton.frames() > 0) == parts) one(s.actorModels[k], modelIds(s, k), initialAnim(s, k), "scene");
        for (const ActorModel* m : extra)
            if ((m->skeleton.frames() > 0) == parts) one(*m, idsOf(s, *m), m->anims.empty() ? 0 : m->anims[0].id, "simulation");
    }
    // Static scene meshes (world cells, props): node "<index>#<name>", engine space.
    if (!s.meshes.empty()) {
        const fs::path file = out / "models" / g / lower(s.level) / "world.gltf";
        Out o;
        Materials mats{o, s, file.parent_path(), out, {}, {}, {}};
        json root{{"name", "bbtime"}, {"scale", {0.001, 0.001, 0.001}}, {"children", json::array()}};
        o.j["nodes"].push_back(root);
        for (size_t i = 0; i < s.meshes.size(); ++i) {
            const Mesh& me = s.meshes[i];
            if (me.indices.empty()) continue;
            std::vector<std::array<float, 3>> pos;
            for (const Vertex& v : me.vertices) pos.push_back({v.pos[0], v.pos[1], v.pos[2]});
            o.j["meshes"].push_back({{"name", me.name}, {"primitives", json::array({primitive(o, me, pos, mats.get(me))})}});
            o.j["nodes"].push_back({{"name", std::format("{}#{}", i, me.name)}, {"mesh", o.j["meshes"].size() - 1}});
            o.j["nodes"][0]["children"].push_back(o.j["nodes"].size() - 1);
        }
        o.j["scenes"] = json::array({{{"nodes", {0}}}});
        files += o.save(file);
    }
    std::error_code ec;
    fs::create_directories(out / "models" / g, ec);
    std::ofstream(out / "models" / g / (lower(s.level) + "-ids.txt"), std::ios::binary) << index;
    return files;
}

void validateGltf(const fs::path& file, const std::vector<std::string>& p, const std::string& rel, const Info& mod,
                  const std::function<const Scene*(const std::string&, const std::string&)>& level, std::vector<Issue>& out) {
    (void)mod;
    Gltf g;
    try {
        g = Gltf::load(file);
    } catch (const std::exception& e) {
        out.push_back({rel, std::format("cannot read glTF: {}", e.what()), true});
        return;
    }
    const bool anim = p[0] == "animations";
    const bool world = !anim && p.size() == 4 && p[3] == "world";
    const std::map<int, int> parts = g.parts();
    if (world) {
        const Scene* sc = level(p[1], p[2]);
        size_t named = 0;
        for (const json& n : g.j.value("nodes", json::array())) {
            const std::string name = n.value("name", "");
            const size_t hash = name.find('#');
            if (hash == 0 || hash == name.npos || !std::ranges::all_of(name.substr(0, hash), [](char c) { return c >= '0' && c <= '9'; })) continue;
            ++named;
            if (sc && std::stoul(name.substr(0, hash)) >= sc->meshes.size())
                out.push_back({rel, std::format("node {}: {} has only {} meshes", name, p[2], sc->meshes.size()), false});
        }
        if (!named) out.push_back({rel, "no '<index>#<name>' nodes: keep the exported world node names", true});
        return;
    }
    const bool objectMeshes = std::ranges::any_of(g.j.value("meshes", json::array()),
                                                  [](const json& me) { return me.value("name", "").starts_with("object"); });
    if (parts.empty() && (anim || !objectMeshes)) {
        out.push_back({rel, "no part<N> nodes: animations cannot drive this file (keep the exported node names)", true});
        return;
    }
    int maxPart = 0;
    std::set<int> have;
    for (const auto& [n, part] : parts) have.insert(part), maxPart = std::max(maxPart, part);
    if (int(have.size()) != maxPart) {
        std::string missing;
        for (int i = 1; i <= maxPart; ++i)
            if (!have.contains(i)) missing += std::format(" part{}", i);
        out.push_back({rel, "missing part nodes:" + missing, false});
    }
    if (anim) {
        if (g.j.value("animations", json::array()).empty()) out.push_back({rel, "no animation in the file", true});
        if (p.size() < 4 || std::ranges::any_of(p.back(), [](char c) { return c < '0' || c > '9'; }))
            out.push_back({rel, "expected animations/<game>/<model id>/<anim id>.gltf (anim id = number)", true});
        return;
    }
    // Triangle count and scale (exported models are PS1 units under a 0.001 root: a character is ~100..2000 units).
    size_t tris = 0;
    float lo = 1e30f, hi = -1e30f;
    for (const json& n : g.j.value("nodes", json::array()))
        if (n.contains("mesh"))
            for (const json& pr : g.at("meshes", n["mesh"]).value("primitives", json::array())) {
                int c = 0;
                const auto pos = g.floats(pr.at("attributes").at("POSITION"), c);
                tris += pr.contains("indices") ? size_t(g.at("accessors", pr["indices"]).at("count")) / 3 : pos.size() / 9;
                for (const float f : pos) lo = std::min(lo, f), hi = std::max(hi, f);
            }
    if (tris > kTriWarn) out.push_back({rel, std::format("{} triangles: more than {} may slow the game down", tris, kTriWarn), false});
    if (hi > lo && (hi - lo > 50000.0f || hi - lo < 5.0f))
        out.push_back({rel, std::format("scale looks off: mesh extent {:.1f} units (originals are PS1 units, ~100..2000 for a character)", hi - lo), false});
}

} // namespace bb::mods
