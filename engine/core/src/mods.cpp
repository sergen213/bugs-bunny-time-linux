// Mods: mod.json, the ModSet resolver, PNG textures, validation. Models / animations / world meshes: mods_gltf.cpp.
#include "bb/mods.h"
#include "bb/tb_level.h"
#include "mods_internal.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <set>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace bb::mods {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool isNumber(std::string_view s) { return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; }); }

std::vector<std::string> split(const std::string& key) {
    std::vector<std::string> out;
    for (size_t a = 0; a <= key.size();) {
        size_t b = key.find('/', a);
        if (b == key.npos) b = key.size();
        out.push_back(key.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// "L01A/tex/12" -> {"l01a", "12"}.
std::pair<std::string, std::string> texId(const std::string& name) {
    const size_t p = name.find("/tex/");
    if (p == name.npos) return {};
    return {lower(name.substr(0, p)), name.substr(p + 5)};
}

} // namespace

fs::path defaultRoot() {
    const char* x = std::getenv("XDG_DATA_HOME");
    if (x && *x) return fs::path(x) / "bbtime" / "mods";
    const char* h = std::getenv("HOME");
    return fs::path(h ? h : ".") / ".local" / "share" / "bbtime" / "mods";
}

Info readInfo(const fs::path& dir) {
    Info i;
    i.dir = dir;
    i.id = dir.filename().string();
    std::ifstream f(dir / "mod.json", std::ios::binary);
    if (!f) {
        i.error = "no mod.json";
        return i;
    }
    try {
        const nlohmann::json j = nlohmann::json::parse(f);
        auto str = [&](const char* k, std::string& out) {
            if (j.contains(k) && j[k].is_string()) out = j[k].get<std::string>();
        };
        str("name", i.name);
        str("version", i.version);
        str("author", i.author);
        str("description", i.description);
        str("game", i.game);
        if (i.game != "lit" && i.game != "tb" && i.game != "both") i.error = std::format("game must be lit, tb or both (got '{}')", i.game);
        if (i.name.empty()) i.name = i.id;
        if (j.contains("swap") && j["swap"].is_object())
            for (const auto& [k, v] : j["swap"].items())
                if (v.is_string()) i.swap[lower(k)] = lower(v.get<std::string>());
        if (j.contains("anims") && j["anims"].is_object())
            for (const auto& [k, v] : j["anims"].items())
                if (v.is_number_unsigned()) i.anims[lower(k)] = v.get<uint16_t>();
    } catch (const std::exception& e) {
        i.error = std::format("mod.json: {}", e.what());
    }
    return i;
}

std::string writeInfo(const Info& i) {
    std::error_code ec;
    fs::create_directories(i.dir, ec);
    if (ec) return std::format("cannot create {}: {}", i.dir.string(), ec.message());
    nlohmann::ordered_json j{{"id", i.id},           {"name", i.name},   {"version", i.version}, {"author", i.author},
                             {"description", i.description}, {"game", i.game}};
    if (!i.swap.empty()) j["swap"] = i.swap;
    if (!i.anims.empty()) j["anims"] = i.anims;
    std::ofstream f(i.dir / "mod.json", std::ios::binary | std::ios::trunc);
    f << j.dump(2) << '\n';
    return f ? std::string() : std::format("cannot write {}", (i.dir / "mod.json").string());
}

std::vector<Info> scan(const fs::path& root) {
    std::vector<Info> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(root, ec))
        if (e.is_directory() && fs::exists(e.path() / "mod.json")) out.push_back(readInfo(e.path()));
    std::ranges::sort(out, {}, &Info::id);
    return out;
}

std::string keyOf(const fs::path& relative) {
    const std::string ext = lower(relative.extension().string());
    std::string key = lower((relative.parent_path() / relative.stem()).generic_string());
    const std::string kind = key.substr(0, key.find('/'));
    const bool ok = (kind == "textures" && ext == ".png") || ((kind == "models" || kind == "animations") && (ext == ".gltf" || ext == ".glb")) ||
                    (kind == "sounds" && ext == ".wav") || (kind == "music" && (ext == ".wav" || ext == ".ogg"));
    return ok && key.find('/') != key.npos ? key : std::string();
}

ModSet ModSet::load(const fs::path& root, std::span<const std::string> enabled) {
    ModSet m;
    for (const std::string& id : enabled) {
        Info i = readInfo(root / id);
        if (!i.error.empty()) {
            m.problems_.push_back(std::format("mod {}: {}", id, i.error));
            continue;
        }
        const size_t idx = m.mods_.size();
        std::error_code ec;
        for (fs::recursive_directory_iterator it(i.dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file())
                if (const std::string k = keyOf(fs::relative(it->path(), i.dir)); !k.empty()) m.files_[k].emplace_back(idx, it->path());
        m.mods_.push_back(std::move(i));
    }
    return m;
}

const fs::path* ModSet::find(const std::string& key) const {
    const auto it = files_.find(key);
    return it == files_.end() ? nullptr : &it->second.back().second;
}

std::vector<std::string> ModSet::keys(const std::string& prefix) const {
    std::vector<std::string> out;
    for (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix); ++it) out.push_back(it->first);
    return out;
}

std::map<std::string, std::vector<std::string>> ModSet::conflicts() const {
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& [k, v] : files_) {
        std::vector<std::string> ids;
        for (const auto& [idx, p] : v)
            if (ids.empty() || ids.back() != mods_[idx].id) ids.push_back(mods_[idx].id);
        if (ids.size() > 1) out[k] = std::move(ids);
    }
    return out;
}

void ModSet::applied(const std::string& key) const {
    if (applied_.insert(key).second)
        if (const fs::path* p = find(key)) std::println(stderr, "bbtime: mod override {} <- {}", key, p->string());
}

namespace {
ModSet& activeSet() {
    static ModSet m;
    return m;
}
} // namespace

void setActive(ModSet m) {
    for (const std::string& p : m.problems()) std::println(stderr, "bbtime: {}", p);
    activeSet() = std::move(m);
}
const ModSet& active() { return activeSet(); }

Texture loadPng(const fs::path& file, std::string* err) {
    Texture t;
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load(file.string().c_str(), &w, &h, &n, 4);
    if (!px) {
        if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "cannot read";
        return t;
    }
    t.name = file.stem().string();
    t.width = uint32_t(w), t.height = uint32_t(h);
    t.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    for (size_t i = 3; i < t.rgba.size(); i += 4) t.hasAlpha |= t.rgba[i] < 255;
    return t;
}

bool savePng(const fs::path& file, const Texture& t) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    return t.width && t.height &&
           stbi_write_png(file.string().c_str(), int(t.width), int(t.height), 4, t.rgba.data(), int(t.width) * 4) != 0;
}

std::string textureKey(const Scene& s, const Texture& t) {
    const auto [level, id] = texId(t.name);
    return id.empty() ? std::string() : std::format("textures/{}/{}/{}", gameKey(s.game), level, id);
}

void apply(Scene& s, const ModSet& m, double tbBrightness) {
    if (m.empty()) return;
    for (const std::string& p : m.problems()) s.notes.push_back("mods: " + p);
    // 2-D uses size quads and crop rectangles by texel size: those keep the original size.
    std::set<int32_t> fixedSize(s.overlayFont.begin(), s.overlayFont.end());
    fixedSize.insert(s.hudSheet.begin(), s.hudSheet.end());
    for (const auto& [id, frames] : s.sprites)
        for (const SpriteFrame& f : frames) fixedSize.insert(f.texture);
    std::set<int32_t> alpha;  // replaced textures that gained transparency
    for (size_t i = 0; i < s.textures.size(); ++i) {
        Texture& tex = s.textures[i];
        std::string key = textureKey(s, tex);
        if (key.empty()) continue;
        const fs::path* file = m.find(key);
        if (!file) {
            key = std::format("textures/{}/common/{}", gameKey(s.game), texId(tex.name).second);
            file = m.find(key);
        }
        if (!file) continue;
        std::string err;
        Texture png = loadPng(*file, &err);
        if (png.rgba.empty()) {
            s.notes.push_back(std::format("mods: {}: bad PNG ({})", file->string(), err));
            continue;
        }
        if (fixedSize.contains(int32_t(i)) && (png.width != tex.width || png.height != tex.height)) {  // nearest resample
            Texture r{.name = {}, .width = tex.width, .height = tex.height, .rgba = std::vector<uint8_t>(size_t(tex.width) * tex.height * 4)};
            for (uint32_t y = 0; y < r.height; ++y)
                for (uint32_t x = 0; x < r.width; ++x)
                    std::copy_n(&png.rgba[(size_t(y * png.height / r.height) * png.width + x * png.width / r.width) * 4], 4,
                                &r.rgba[(size_t(y) * r.width + x) * 4]);
            png.width = r.width, png.height = r.height, png.rgba = std::move(r.rgba);
        }
        if (s.game == GameId::TimeBusters && tbBrightness != 1.0)
            for (size_t p = 0; p < png.rgba.size(); p += 4) tbAdjustColor(&png.rgba[p], tbBrightness);
        png.hasAlpha = std::ranges::any_of(std::views::iota(size_t(0), png.rgba.size() / 4), [&](size_t p) { return png.rgba[p * 4 + 3] < 255; });
        if (png.hasAlpha && !tex.hasAlpha) alpha.insert(int32_t(i));
        png.name = tex.name;
        tex = std::move(png);
        m.applied(key);
    }
    // Faces built opaque from an opaque original alpha-test the new transparency (the TIM colour-key rule).
    auto fix = [&](Mesh& me) {
        if (me.blend == Blend::Opaque && alpha.contains(me.texture)) me.blend = Blend::AlphaTest;
    };
    for (Mesh& me : s.meshes) fix(me);
    for (ActorModel& am : s.actorModels)
        for (ActorMesh& me : am.meshes) fix(me.mesh);
    applyModels(s, m);
}

size_t exportTextures(const Scene& s, const fs::path& out) {
    size_t n = 0;
    for (const Texture& t : s.textures)
        if (const std::string k = textureKey(s, t); !k.empty() && !t.rgba.empty()) n += savePng(out / (k + ".png"), t);
    return n;
}

std::vector<Issue> validate(const Info& mod, SceneSource scene, void* ctx) {
    std::vector<Issue> out;
    if (!mod.error.empty()) out.push_back({"mod.json", mod.error, true});
    try {  // the folder name is the id settings.ini and load order use
        std::ifstream f(mod.dir / "mod.json", std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(f);
        if (j.contains("id") && j["id"].is_string() && j["id"].get<std::string>() != mod.id)
            out.push_back({"mod.json", std::format("id '{}' differs from the folder name '{}' (the folder name is used)", j["id"].get<std::string>(), mod.id), false});
    } catch (const std::exception&) {}
    if (mod.id.empty() || !std::ranges::all_of(mod.id, [](char c) { return std::isalnum(uint8_t(c)) || c == '-' || c == '_' || c == '.'; }))
        out.push_back({"mod.json", "id should only use letters, digits, '-', '_' and '.'", false});
    auto level = [&](const std::string& g, const std::string& l) -> const Scene* {
        return scene ? scene(ctx, g == "lit" ? GameId::LostInTime : GameId::TimeBusters, l) : nullptr;
    };
    std::error_code ec;
    for (fs::recursive_directory_iterator it(mod.dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const fs::path rel = fs::relative(it->path(), mod.dir);
        const std::string f = rel.generic_string(), key = keyOf(rel);
        const std::string top = lower(f.substr(0, f.find('/')));
        if (f == "mod.json" || lower(rel.stem().string()) == "readme" || lower(rel.stem().string()) == "license") continue;
        if (key.empty()) {
            if (top != "models" && top != "animations")  // glTF .bin / texture files sit next to their .gltf
                out.push_back({f, "ignored: not a known override path (see docs/MODDING.md)", false});
            continue;
        }
        const std::vector<std::string> p = split(key);
        if (p[1] != "lit" && p[1] != "tb") {
            out.push_back({f, "second folder must be lit or tb", true});
            continue;
        }
        if (mod.game != "both" && p[1] != mod.game) out.push_back({f, std::format("file for {} in a {} mod", p[1], mod.game), false});
        if (top == "textures") {
            if (p.size() != 4 || !isNumber(p[3])) {
                out.push_back({f, "expected textures/<game>/<level|common>/<texture id>.png", true});
                continue;
            }
            std::string err;
            if (loadPng(it->path(), &err).rgba.empty()) out.push_back({f, "bad PNG: " + err, true});
            else if (p[2] != "common")
                if (const Scene* sc = level(p[1], p[2])) {
                    const bool known = std::ranges::any_of(sc->textures, [&](const Texture& t) { return textureKey(*sc, t) == key; });
                    if (!known) out.push_back({f, std::format("unknown texture id {} in {}", p[3], p[2]), false});
                }
        } else if (top == "sounds") {
            if (p.size() != 4 || !isNumber(p[3])) out.push_back({f, "expected sounds/<game>/<level>/<index>.wav", true});
            std::ifstream w(it->path(), std::ios::binary);
            char h[12]{};
            w.read(h, 12);
            if (std::string_view(h, 4) != "RIFF" || std::string_view(h + 8, 4) != "WAVE") out.push_back({f, "not a RIFF/WAVE file", true});
        } else if (top == "music") {
            if (p.size() != 3) out.push_back({f, "expected music/<game>/<track>.ogg|wav", true});
        } else {
            validateGltf(it->path(), p, f, mod, level, out);
        }
    }
    for (const auto& [from, to] : mod.swap)
        if (!from.starts_with("lit:") && !from.starts_with("tb:")) out.push_back({"mod.json", std::format("swap key '{}' must be lit:<id> or tb:<id>", from), true});
    return out;
}

} // namespace bb::mods
