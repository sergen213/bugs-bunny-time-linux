// Mods (docs/MODDING.md): $XDG_DATA_HOME/bbtime/mods/<id>/ folders with mod.json and override files keyed by the
// stable ids the exporter (bbtime-modkit) writes. ModSet resolves a key against the enabled mods in load order
// (later wins); apply() swaps textures, actor models, animations and world meshes into a decoded Scene. With no mod
// enabled nothing here runs, so output stays identical to the unmodded game.
#pragma once
#include "bb/scene.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace bb::mods {

namespace fs = std::filesystem;

struct Info {
    std::string id, name, version, author, description;
    std::string game = "both";                  // lit | tb | both
    // Whole-model swaps "lit:<model id>" -> "lit:<model id>" (an original model of the same level) or a mod model
    // name (models/<game>/<name>.gltf of an enabled mod).
    std::map<std::string, std::string> swap;
    // Animation aliases "lit:<model id>:<anim id>" -> anim id: requests for the first play the second clip.
    std::map<std::string, uint16_t> anims;
    fs::path dir;
    std::string error;                          // mod.json missing / malformed (the mod is then not loadable)
};

fs::path defaultRoot();                          // $XDG_DATA_HOME/bbtime/mods (default ~/.local/share/bbtime/mods)
Info readInfo(const fs::path& dir);              // never throws; problems in Info::error
std::string writeInfo(const Info& info);         // creates info.dir and writes mod.json; error message, empty = ok
std::vector<Info> scan(const fs::path& root);    // every subfolder with a mod.json, sorted by id

// Override key of a file inside a mod: lower-case relative path without extension, '/' separated
// ("textures/lit/l01a/12"); empty for mod.json and files outside the known kinds.
std::string keyOf(const fs::path& relative);
// Key prefix for a game: "lit" / "tb".
inline const char* gameKey(GameId g) { return g == GameId::LostInTime ? "lit" : "tb"; }

class ModSet {
public:
    // Loads the enabled ids in order from root; unknown or broken ids are skipped with a warning in problems().
    static ModSet load(const fs::path& root, std::span<const std::string> enabled);
    bool empty() const { return mods_.empty(); }
    const std::vector<Info>& mods() const { return mods_; }
    const std::vector<std::string>& problems() const { return problems_; }
    // Winning file for a key (see keyOf), nullptr if no enabled mod has it.
    const fs::path* find(const std::string& key) const;
    // Keys under a prefix ("animations/lit/12/"), sorted.
    std::vector<std::string> keys(const std::string& prefix) const;
    // Keys provided by two or more enabled mods -> mod ids in load order (the last one wins).
    std::map<std::string, std::vector<std::string>> conflicts() const;
    // Records an applied override (logged to stderr once per key); count = distinct keys applied so far.
    void applied(const std::string& key) const;
    size_t appliedCount() const { return applied_.size(); }

private:
    std::vector<Info> mods_;
    std::vector<std::string> problems_;
    std::map<std::string, std::vector<std::pair<size_t, fs::path>>> files_;  // key -> (mod index, file)
    mutable std::set<std::string> applied_;
};

// The enabled set of this process (set once at startup by the game: settings.ini `mods`, --mods, --no-mods;
// problems are printed). Empty by default = unmodded.
void setActive(ModSet m);
const ModSet& active();

// Applies every override for s.game / s.level: textures (PNG, any size; HUD/font/sprite textures are resampled to the
// original size), actor model geometry (glTF part nodes) and mod.json swaps, animation clips (replace / add by anim
// id) and world meshes (models/<game>/<level>/world.gltf). tbBrightness: the TB palette pass applied to mod PNGs.
// Problems go to s.notes as "mods: ..." lines; never throws.
void apply(Scene& s, const ModSet& m, double tbBrightness = 1.0);
// Models the simulation builds itself from level data (TB pickups and props spawned from templates, entities
// without a scene actor): model file, swaps and clips of the active set, ids "<level>/<asset>" and the geometry
// hash. New images must have been registered by apply() (it preloads every mod model's images for TB). No-op
// without active mods; problems go to stderr.
void applyToModel(const Scene& s, ActorModel& model);

// PNG I/O (stb). loadPng returns an empty texture and sets err on failure.
Texture loadPng(const fs::path& file, std::string* err = nullptr);
bool savePng(const fs::path& file, const Texture& t);

// Exporter (bbtime-modkit export): originals of a decoded level under out/ in the override layout.
// textures/<game>/<level>/<id>.png
size_t exportTextures(const Scene& s, const fs::path& out);
// models/<game>/<model id>.gltf (named part nodes, meshes, rest pose) and animations/<game>/<model id>/<anim id>.gltf
// (the model plus one clip), models/<game>/<level>/world.gltf (static scene meshes). Returns files written.
// extra: models the simulation builds itself (TbWorld::builtModels), listed as "simulation" in <level>-ids.txt.
size_t exportModels(const Scene& s, const fs::path& out, std::span<const ActorModel* const> extra = {});

// Static preview of a glTF / .glb (launcher Mods viewer): the triangles of its default scene with node transforms
// applied (rest / bind pose: skins and animations are ignored), one group per primitive. Throws on unreadable files.
struct Preview {
    struct Group {
        std::vector<float> vertices;      // x y z u v r g b a per vertex (COLOR_0 × base colour factor; 0.5 = neutral)
        fs::path image;                   // base colour image file, or
        std::vector<uint8_t> imageBytes;  // the embedded one (glb buffer view / data: URI)
        bool alphaTest = false;           // alphaMode MASK or BLEND
    };
    std::vector<Group> groups;
    std::vector<std::string> nodes;       // every node name (launcher: part / world node checks before replacing)
};
Preview loadPreview(const fs::path& file);

// Validation of one mod folder (Mods page / bbtime-modkit validate). scene(game, level) optionally decodes a level for
// id checks (return nullptr when the game data is unavailable; then ids are not checked).
struct Issue {
    std::string file;     // relative to the mod folder
    std::string message;
    bool error = false;   // false = warning
};
using SceneSource = const Scene* (*)(void* ctx, GameId game, const std::string& level);
std::vector<Issue> validate(const Info& mod, SceneSource scene = nullptr, void* ctx = nullptr);

} // namespace bb::mods
