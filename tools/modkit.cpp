// bbtime-modkit: exports original assets in the mod override layout and validates mod folders (docs/MODDING.md).
//   bbtime-modkit export --game lit|tb --level L [--what textures|models|sounds|music|all] --out DIR [--root GAMEROOT]
//   bbtime-modkit validate MODDIR [--root-lit DIR] [--root-tb DIR]
//   bbtime-modkit new ID [--name N] [--author A] [--game lit|tb|both] [--description D] [--mods-dir DIR]
// Game roots default to settings.ini lit_root / tb_root, else $BBTIME_DATA/<lit|tb>.
#include "bb/audio.h"
#include "bb/game/lit_world.h"
#include "bb/game/tb_world.h"
#include "bb/level.h"
#include "bb/mods.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <cctype>
#include <vector>

namespace fs = std::filesystem;
using bb::GameId;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// Case-insensitive child lookup (installs mix Datas/bze and Datas/BZE, L01a.bze ...).
fs::path findCi(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == lower(name)) return e.path();
    return {};
}

fs::path gameRoot(GameId g, const std::map<std::string, std::string>& opt) {
    const char* k = g == GameId::LostInTime ? "root-lit" : "root-tb";
    if (auto it = opt.find(k); it != opt.end()) return it->second;
    if (auto it = opt.find("root"); it != opt.end()) return it->second;
    const bbtime::Settings s = bbtime::loadSettings();
    const fs::path r = g == GameId::LostInTime ? s.litRoot : s.tbRoot;
    if (!r.empty()) return r;
    const char* d = std::getenv("BBTIME_DATA");
    return fs::path(d ? d : ".") / (g == GameId::LostInTime ? "lit" : "tb");
}

fs::path levelFile(const fs::path& root, const std::string& level) {
    const fs::path bze = findCi(findCi(root, "Datas"), "bze");
    return bze.empty() ? fs::path() : findCi(bze, level + ".bze");
}

struct Level {
    bb::BzeFile bze;
    bb::Scene scene;
};

std::unique_ptr<Level> loadLevel(GameId g, const fs::path& root, const std::string& level) {
    const fs::path f = levelFile(root, level);
    if (f.empty()) return nullptr;
    auto l = std::make_unique<Level>();
    l->bze = bb::loadBze(f);
    l->scene = g == GameId::LostInTime ? bb::buildLitScene(l->bze, level) : bb::buildTbScene(l->bze, level, 1.0);
    if (g == GameId::TimeBusters) bb::addTbActors(l->scene, l->bze);
    return l;
}

size_t exportSounds(GameId g, Level& l, const std::string& level, const fs::path& out) {
    bb::game::Sfx sfx;
    try {  // the banks exactly as the game loads them
        if (g == GameId::LostInTime) sfx = bb::game::LitWorld::load(l.bze, &l.scene).sfx;
        else sfx = bb::game::TbWorld::load(l.bze, l.scene).sfx;
    } catch (const std::exception&) {  // no playable world (cutscenes, menus): the whole section-3 table
        if (const int i3 = l.bze.find(3); i3 >= 0)
            sfx.bank = bb::game::Sfx::loadBank(l.bze.payload(size_t(i3)),
                                               g == GameId::LostInTime ? bb::BzeVariant::LostInTime : bb::BzeVariant::TimeBusters);
    }
    size_t n = 0;
    const std::string base = std::format("sounds/{}/{}", bb::mods::gameKey(g), lower(level));
    for (const auto& [bank, dir] : {std::pair{sfx.bank, base}, {sfx.speech, base + "_speech"}})
        if (bank)
            for (size_t i = 0; i < bank->size(); ++i) {
                const bb::AiffSound& a = (*bank)[i];
                if (a.channels && !a.pcm.empty())
                    n += bb::writeWav(out / dir / std::format("{}.wav", i), a.channels, a.rate, a.pcm, a.loopStart, a.loopEnd);
            }
    return n;
}

size_t exportMusic(GameId g, const fs::path& root, const fs::path& out) {
    size_t n = 0;
    const fs::path dir = findCi(findCi(root, "Datas"), "track");
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (lower(e.path().extension().string()) != ".xa") continue;
        bb::XaStream xa(e.path());
        std::vector<int16_t> pcm;
        while (xa.read(1 << 20, pcm) > 0) {}
        n += bb::writeWav(out / "music" / bb::mods::gameKey(g) / (lower(e.path().stem().string()) + ".wav"),
                          uint16_t(bb::xaChannels(xa.layout())), bb::kXaRate, pcm);
    }
    return n;
}

int usage() {
    std::println(stderr, "usage: bbtime-modkit export --game lit|tb --level L [--what textures|models|sounds|music|all] --out DIR [--root GAMEROOT]\n"
                         "       bbtime-modkit validate MODDIR [--root-lit DIR] [--root-tb DIR]\n"
                         "       bbtime-modkit new ID [--name N] [--author A] [--version V] [--game lit|tb|both] [--description D] [--mods-dir DIR]\n"
                         "see docs/MODDING.md");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.starts_with("--") && i + 1 < argc) opt[a.substr(2)] = argv[++i];
        else pos.push_back(a);
    }
    try {
        if (cmd == "export") {
            if (!opt.contains("game") || !opt.contains("level") || !opt.contains("out")) return usage();
            const GameId g = opt["game"] == "tb" ? GameId::TimeBusters : GameId::LostInTime;
            const std::string what = opt.contains("what") ? opt["what"] : "all", level = opt["level"];
            const fs::path root = gameRoot(g, opt), out = opt["out"];
            if (what == "music") {
                std::println("music: {} files", exportMusic(g, root, out));
                return 0;
            }
            auto l = loadLevel(g, root, level);
            if (!l) {
                std::println(stderr, "bbtime-modkit: {}.bze not found under {}/Datas/bze", level, root.string());
                return 1;
            }
            if (what == "textures" || what == "models" || what == "all")  // models reference the exported PNGs
                std::println("textures: {} files", bb::mods::exportTextures(l->scene, out));
            if (what == "models" || what == "all") {
                std::vector<const bb::ActorModel*> built;  // TB: pickups / props the simulation builds itself
                std::optional<bb::game::TbWorld> w;
                if (g == GameId::TimeBusters) try {
                    w.emplace(bb::game::TbWorld::load(l->bze, l->scene));
                    built = w->builtModels();
                } catch (const std::exception&) {}
                std::println("models + animations: {} files", bb::mods::exportModels(l->scene, out, built));
            }
            if (what == "sounds" || what == "all") std::println("sounds: {} files", exportSounds(g, *l, level, out));
            std::println("written under {}", out.string());
            return 0;
        }
        if (cmd == "validate") {
            if (pos.size() != 1) return usage();
            const bb::mods::Info info = bb::mods::readInfo(pos[0]);
            struct Ctx {
                std::map<std::string, std::unique_ptr<Level>> cache;
                std::map<std::string, std::string> opt;
            } ctx{{}, opt};
            auto src = [](void* c, GameId g, const std::string& level) -> const bb::Scene* {
                Ctx& x = *static_cast<Ctx*>(c);
                auto [it, fresh] = x.cache.try_emplace(std::format("{}/{}", int(g), level));
                if (fresh) try { it->second = loadLevel(g, gameRoot(g, x.opt), level); } catch (const std::exception&) {}
                return it->second ? &it->second->scene : nullptr;
            };
            const auto issues = bb::mods::validate(info, src, &ctx);
            for (const auto& i : issues) std::println("{}: {}: {}", i.error ? "error" : "warning", i.file, i.message);
            std::println("{}: {} issue(s)", info.id, issues.size());
            return std::ranges::any_of(issues, &bb::mods::Issue::error) ? 1 : 0;
        }
        if (cmd == "new") {
            if (pos.size() != 1) return usage();
            bb::mods::Info i;
            i.id = pos[0];
            i.name = opt.contains("name") ? opt["name"] : i.id;
            i.author = opt["author"], i.description = opt["description"];
            i.version = opt.contains("version") ? opt["version"] : "1.0";
            i.game = opt.contains("game") ? opt["game"] : "both";
            i.dir = (opt.contains("mods-dir") ? fs::path(opt["mods-dir"]) : bb::mods::defaultRoot()) / i.id;
            if (const std::string e = bb::mods::writeInfo(i); !e.empty()) {
                std::println(stderr, "bbtime-modkit: {}", e);
                return 1;
            }
            std::println("{}", i.dir.string());
            return 0;
        }
    } catch (const std::exception& e) {
        std::println(stderr, "bbtime-modkit: {}", e.what());
        return 1;
    }
    return usage();
}
