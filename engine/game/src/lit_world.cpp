// LiT player controller (0x43a880), MoveActor (0x437390 = 0x436ff0 + 0x4371f0 + 0x436de0), jump arc (0x43a250),
// camera follow (0x4318f0) and collectibles. Section numbers refer to docs/re/GAMEPLAY.md.
#include "bb/game/lit_world.h"

#include "bb/level.h"
#include "bb/levelscript.h"

#include <algorithm>
#include <numeric>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <span>

namespace bb::game {
namespace {

template <class T> T le(const std::vector<uint8_t>& d, size_t o) {
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}
const ScriptTag* tag(const ScriptItem& it, uint8_t id) {
    for (const ScriptTag& t : it.tags)
        if (t.id == id) return &t;
    return nullptr;
}
V3 v3(const ScriptTag* t) { return t ? V3{le<int32_t>(t->data, 0), le<int32_t>(t->data, 4), le<int32_t>(t->data, 8)} : V3{}; }
LogicItem item(const std::vector<uint8_t>& d, size_t o) {
    return {d[o], d[o + 1], d[o + 2], d[o + 3], le<uint16_t>(d, o + 4), le<uint16_t>(d, o + 6)};
}

// Exe level table 0x4ad380 (`..\BZE\<NAME>.BZE;1`, ids 0..110; 111/112 are the XA files).
constexpr const char* kLevels[] = {
    "TITLE", "LOADING", "LANGUAGE", "LS01", "L01A", "L01B", "L01C", "L01D1", "L01D2", "L02A1", "L02A2", "L02A3",
    "L02A4", "L02A5", "L02A6", "L02B1", "L02B2", "L02C1", "L02C2", "L02C3", "L02C4", "L03A", "L03ACOM", "L03A2",
    "L03B", "L03C", "L03C1", "L03C2", "L03D1", "L04A1", "L04A2", "L04A3", "L04B1", "L04B2", "L04B3", "L04C1", "L04C2",
    "L04C3", "L04D1", "L04D2", "L04E", "L04E2", "L05A1", "L05A2", "L05A3A", "L05A3B", "L05A3C", "L05A4", "L05A5",
    "L05B1", "L05C", "MERLIN", "LB01", "LB02", "LB04", "LB05", "CCINTRO", "CC1A", "CC2A", "CC2B", "CC2C", "CC3A",
    "CC3B", "CC3C", "CC3D", "CC4A", "CC4B", "CC4C", "CC4D", "CC5A", "CCMERLIN", "CCEND", "CREDITS", "DEMO2A2",
    "DEMO3C1", "DEMO4A1", "DEMO5C", "DEMOB02", "SCREEN1", "SCREEN2", "SCREEN3", "L_30BANK", "L_30CF", "L_30ESCA",
    "L_30LOAD", "L_30RIDE", "L_BCORRI", "L_BDUCK", "L_BHUNT", "L_FLOAD", "L_FLOAD2", "L_FRIDE", "L_FXFILE",
    "L_FXROOM", "L_MCOOK", "L_MLOAD", "L_MLOAD2", "L_MWITCH", "L_PCANON", "L_PDOCK", "L_PLOAD", "L_PMINE", "L_PRED",
    "L_SAKICK", "L_SALOAD", "L_SANIGH", "L_SARUN", "L_MERLIN", "L_THALL", "SCREEN4", "SCREEN5"};

constexpr int16_t kStay = -13;            // §3.1 nextState "stay"
constexpr uint16_t kNoButtons = 0xfff1;   // §3.1 authoring value "no button"
constexpr int32_t kDt = 2;                // §1.1 0x4b2384
constexpr int32_t kDeadZone = 50;         // §2.2 / §3.2
constexpr int16_t kCamTable[8][2] = {{0, 400}, {400, 400}, {400, 0}, {400, -400},
                                     {0, -400}, {-400, -400}, {-400, 0}, {-400, 400}};  // 0x4adad8
constexpr int16_t kYawProbe[26] = {0, 341, -341, 682, -682, 1023, -1023, 1364, -1364, 1705, -1705, 2046, -2046,
                                   2387, -2387, 2728, -2728, 3069, -3069, 3410, -3410, 3751, -3751, 4092, 0, 400};  // 0x4adaa8

} // namespace

std::string LitWorld::levelName(int id) {
    return id >= 0 && size_t(id) < std::size(kLevels) ? kLevels[id] : std::string();
}

LitWorld LitWorld::load(const BzeFile& bze, const Scene* scene, uint8_t language) {
    LitWorld w;
    w.language = language > 5 ? 0 : language;  // 0x42e7a0: G[0x10] > 5 -> 0
    w.scene_ = scene;
    const int i1 = bze.find(1), i4 = bze.find(4);
    if (i1 < 0 || i4 < 0) throw FormatError("LitWorld: needs sections 1 and 4");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1)), s4 = bze.payload(size_t(i4));
    const LevelScript ls = parseLevelScript(s1);
    std::map<uint32_t, const ActorInstance*> at;  // ScriptItem::offset -> scene actor (TOD model)
    if (scene)
        for (const ActorInstance& a : scene->actors) at[a.scriptOffset] = &a;
    bool player = false;
    std::map<uint16_t, std::pair<uint16_t, std::vector<uint8_t>>> flipbooks;  // chunk 0x22 tag 0x3f: asset -> (role, pairs)
    std::vector<std::vector<uint16_t>> refs;  // per def: tag-0x0f asset list
    std::vector<uint16_t> playerRefs;
    std::map<uint16_t, AnimFx> animFx;        // chunk 0x22 animation assets with frame sounds / logic events
    std::map<uint16_t, uint16_t> roleOf;      // chunk 0x22 asset id -> class (+0x4e)
    std::map<uint16_t, std::vector<SpriteRecord>> spriteRecs;  // tag-0x40 sprite-sequence assets
    std::set<uint16_t> spriteKind;  // assets whose kind +0x0a is 0x100 (last of tags 0x24 / 0x25 / 0x3f / 0x40 is 0x40)
    const int i3 = bze.find(3);
    const std::vector<uint8_t> s3 = i3 >= 0 ? bze.payload(size_t(i3)) : std::vector<uint8_t>{};
    // Speech: command 0x4b loads section 6, 5, 7, 8, 9, 10 for language 0..5 (0x42e7a0 -> 0x42e3a0); tags 0x4c / 0x4d
    // (and the text tag 0x4a in section 3) hold 6 {u32 off, u32 size} pairs, the language's one is used.
    static constexpr uint32_t kLangSection[6] = {6, 5, 7, 8, 9, 10};
    const int is = bze.find(kLangSection[w.language]);
    const std::vector<uint8_t> sp = is >= 0 ? bze.payload(size_t(is)) : std::vector<uint8_t>{};
    auto region = [&](std::span<const uint8_t> sec, const auto& ref, bool perLanguage = false) {  // tag {u32 off, u32 size}
        const size_t at = perLanguage && ref.size() >= 48 ? size_t(w.language) * 8 : 0;
        const uint32_t off = le<uint32_t>(ref, at), size = le<uint32_t>(ref, at + 4);
        return size_t(off) + size <= sec.size() ? sec.subspan(off, size) : std::span<const uint8_t>{};
    };
    for (const ScriptItem& it : ls.items) {
        if (it.kind != ScriptItem::Kind::Chunk) continue;
        if (it.code == 0x00) {  // 0x42f6c0: 0x01 programs (0x41e030), 0x02 bank (0x41e0e0), 0x04 sound (0x41de40),
                                // 0x4c / 0x4d per-language speech programs (0x41e070) / bank (base 0x4b1c80)
            for (const ScriptTag& t : it.tags) {
                if (t.id == 0x04) w.sfx.addEntry(t.data);
                if (t.id == 0x01 && t.data.size() >= 8) w.sfx.programs = Sfx::programTable(region(s4, t.data));
                if (t.id == 0x02 && t.data.size() >= 8) w.sfx.bank = Sfx::loadBank(region(s3, t.data), BzeVariant::LostInTime);
                if (t.id == 0x4c && t.data.size() >= 8) w.sfx.speechPrograms = Sfx::programTable(region(sp, t.data, true));
                if (t.id == 0x4d && t.data.size() >= 8) w.sfx.speech = Sfx::loadBank(region(sp, t.data, true), BzeVariant::LostInTime);
            }
        } else if (it.code == 0x22) {  // asset: tag 0x27 {u16 role, u16 id}; tag 0x3f flipbook {u16 n, u16, u32 off, u32 size}
            const ScriptTag *id = tag(it, 0x27), *fl = tag(it, 0x3f);
            if (id) {
                roleOf[le<uint16_t>(id->data, 2)] = le<uint16_t>(id->data, 0);
                if (const ScriptTag* sm = tag(it, 0x40))
                    spriteRecs[le<uint16_t>(id->data, 2)] = spriteRecords(s4, le<uint32_t>(sm->data, 0), le<uint32_t>(sm->data, 4));
                uint8_t kind = 0;
                for (const ScriptTag& t : it.tags)
                    if (t.id == 0x24 || t.id == 0x25 || t.id == 0x3f || t.id == 0x40) kind = t.id;
                if (kind == 0x40) spriteKind.insert(le<uint16_t>(id->data, 2));
                AnimFx fx;
                fx.anim = le<uint16_t>(id->data, 0);
                for (const ScriptTag& t : it.tags) {
                    if (t.id == 0x28 && t.data.size() >= 3) fx.sounds.push_back({t.data[0], t.data[1], t.data[2], 0});
                    if (t.id == 0x0d && t.data.size() >= 12)  // {u16 id, u8, u8, u16 frame, cond, A, B, act, u8 X, u8 Y}
                        fx.events.push_back({le<uint16_t>(t.data, 0), t.data[2], t.data[3], le<uint16_t>(t.data, 4),
                                             {t.data[6], t.data[7], t.data[8], t.data[9], t.data[10], t.data[11]}});
                }
                if (!fx.sounds.empty() || !fx.events.empty()) animFx[le<uint16_t>(id->data, 2)] = std::move(fx);
            }
            if (id && fl && fl->data.size() >= 8) {
                const uint32_t n = le<uint16_t>(fl->data, 0), off = le<uint32_t>(fl->data, 4);
                if (off > s4.size() || s4.size() - off < size_t(n) * 2) throw FormatError("LitWorld: flipbook list outside section 4");
                flipbooks[le<uint16_t>(id->data, 2)] = {le<uint16_t>(id->data, 0), {s4.begin() + off, s4.begin() + off + n * 2}};
            }
        } else if (it.code == 0x36) {
            if (const ScriptTag* t = tag(it, 0x37)) {
                const uint32_t off = le<uint32_t>(t->data, 0), size = le<uint32_t>(t->data, 4);
                if (size_t(off) + size > s4.size()) throw FormatError("LitWorld: chunk 0x36 block outside section 4");
                w.hf = Heightfield::parse(std::span(s4).subspan(off, size));
            }
        } else if (it.code == 0x49) {  // overlay text: tag 0x4a = 6 x {u32 offset, u32 size} in section 3, by language
            if (const ScriptTag* t = tag(it, 0x4a); t && bze.find(3) >= 0) {
                const size_t at = t->data.size() >= 48 ? size_t(w.language) * 8 : 0;  // the language's pair (0x42e7a0 'I')
                const uint32_t off = le<uint32_t>(t->data, at), size = le<uint32_t>(t->data, at + 4);
                const std::vector<uint8_t> s3 = bze.payload(size_t(bze.find(3)));
                if (size_t(off) + size > s3.size()) throw FormatError("LitWorld: chunk 0x49 text outside section 3");
                w.overlayText.assign(s3.begin() + off, s3.begin() + off + size);
            }
        } else if (it.code == 0x47) {  // demo stream (0x42e7a0 case 'G' tag 'H': section 3 + offset; G[0x0f] = 1)
            if (const ScriptTag* t = tag(it, 0x48); t && t->data.size() >= 8) {
                const std::span<const uint8_t> b = region(s3, t->data);
                for (size_t k = 0; k + 1 < b.size(); k += 2) w.demo.push_back(uint16_t(b[k] | b[k + 1] << 8));
            }
        } else if (it.code == 0x05) {  // music zones (0x42e7a0 case 5 -> 0x41d8b0; 0x28 entries at most)
            for (const ScriptTag& t : it.tags)
                if (t.id == 0x06 && t.data.size() >= 3 && w.musicZones.size() < 0x28) w.musicZones.push_back({t.data[0], t.data[1], t.data[2]});
        } else if (it.code == 0x3b) {
            if (const ScriptTag* t = tag(it, 0x2b)) w.levelEye_ = v3(t);  // §5 initial eye
            if (const ScriptTag* t = tag(it, 0x3c)) w.shadowMaxY = le<int16_t>(t->data, 0);  // 0x4b2378 (movsx)
        } else if (it.code == 0x09) {  // zone trigger volume (§4.4)
            Trigger tr;
            tr.pos = v3(tag(it, 0x10));
            if (const ScriptTag* t = tag(it, 0x11)) tr.rotY = le<int16_t>(t->data, 2);
            if (const ScriptTag* t = tag(it, 0x16)) tr.flags = le<uint32_t>(t->data, 0);
            if (const ScriptTag* t = tag(it, 0x1c)) {
                tr.sizeX = le<int16_t>(t->data, 0);
                tr.minY = le<int16_t>(t->data, 2);
                tr.sizeZ = le<int16_t>(t->data, 4);
            }
            if (const ScriptTag* t = tag(it, 0x1f)) tr.zone = t->data[0];
            for (const ScriptTag& t : it.tags)
                if (t.id == 0x33 && t.data.size() >= 32)
                    tr.entries.push_back({le<uint32_t>(t.data, 0), le<uint32_t>(t.data, 4), item(t.data, 8),
                                          le<uint32_t>(t.data, 16),
                                          {le<int32_t>(t.data, 20), le<int32_t>(t.data, 24), le<int32_t>(t.data, 28)}});
            w.triggers.push_back(std::move(tr));
        } else if (it.code == 0x07 || it.code == 0x08 || it.code == 0x0a) {
            const ScriptTag* type = tag(it, 0x13);
            if (type && type->data[0] == 1 && it.code != 0x08) {
                if (player) continue;
                player = true;  // §3.1: tables of the player entity (tag 0x13 == 1)
                w.spawn_ = v3(tag(it, 0x10));
                if (const ScriptTag* t = tag(it, 0x11)) w.spawnYaw_ = le<int16_t>(t->data, 2);  // raw (L01A: 4096, Wine +0xde / 0x4b246c)
                if (const ScriptTag* t = tag(it, 0x27)) w.p.entityId = le<uint16_t>(t->data, 0);
                if (const ScriptTag* t = tag(it, 0x1f)) w.p.zone = t->data[0];
                if (const ScriptTag* t = tag(it, 0x46)) w.playerShadow_ = le<uint16_t>(t->data, 0);
                if (const ScriptTag* t = tag(it, 0x38)) w.playerInflate_ = le<int16_t>(t->data, 0);
                if (auto m = at.find(it.offset); m != at.end()) w.model_ = &scene->actorModels[m->second->model];
                for (const ScriptTag& t : it.tags) {
                    const auto& d = t.data;
                    if (t.id == 0x0f) playerRefs.push_back(le<uint16_t>(d, 0));
                    if (t.id == 0x1c || t.id == 0x42) w.playerBoxTop_ = le<int16_t>(d, 2);  // +0x52 until a TOD packet 9
                    if (t.id == 0x32) {
                        PlayerState s{le<uint16_t>(d, 0), {}};
                        for (size_t k = 0; k < 5; ++k) s.moves[k] = le<int16_t>(d, 2 + k * 2);
                        w.states.push_back(s);
                    } else if (t.id == 0x34) {
                        w.moves.push_back({le<uint16_t>(d, 0), le<uint16_t>(d, 2), le<int16_t>(d, 4), le<uint32_t>(d, 8),
                                           le<uint32_t>(d, 12), le<uint16_t>(d, 16), le<int16_t>(d, 18), le<int16_t>(d, 20),
                                           le<int16_t>(d, 22)});
                        // In memory the node's u16 +6 holds the link (file +16; the 0xfc lookup key) and +16 the file's
                        // +6 (runtime: LB02 nodes vs tag 0x34, local/w13/lit/lb02_moves.txt).
                        std::array<uint8_t, 28> node{};
                        std::copy_n(d.begin(), std::min<size_t>(24, d.size()), node.begin() + 4);
                        std::swap_ranges(node.begin() + 10, node.begin() + 12, node.begin() + 20);
                        w.moveImage.insert(w.moveImage.end(), node.begin(), node.end());
                    } else if (t.id == 0x35) {
                        w.rules.push_back({le<uint16_t>(d, 0), le<uint16_t>(d, 2), le<uint16_t>(d, 4), le<uint16_t>(d, 6),
                                           le<int16_t>(d, 8), item(d, 10), le<uint16_t>(d, 20), le<uint32_t>(d, 24),
                                           le<uint32_t>(d, 28), le<uint32_t>(d, 32), le<uint32_t>(d, 36)});
                    }
                }
                continue;
            }
            ActorDef d;  // §7.1 tag map
            d.chunk = it.code;
            if (type) d.type = type->data[0];
            if (auto m = at.find(it.offset); m != at.end()) d.model = int32_t(m->second->model), d.idleAnim = m->second->anim;
            std::vector<uint16_t>& r = refs.emplace_back();
            for (const ScriptTag& t : it.tags) {
                const auto& x = t.data;
                switch (t.id) {
                case 0x10: d.pos = v3(&t); break;
                case 0x11: for (size_t k = 0; k < 3; ++k) d.rot[k] = le<int16_t>(x, k * 2); break;
                case 0x12: for (size_t k = 0; k < 3; ++k) d.scale[k] = le<int32_t>(x, k * 4); break;
                case 0x15: d.cls = x[0]; break;
                case 0x16:  // 0x42e7a0: the first listed asset a sprite (kind 0x100) -> flipbook actor flags8 0x200000
                    d.flags8 = le<uint32_t>(x, 0), d.flagsC = le<uint32_t>(x, 4);
                    if (!r.empty() && spriteKind.contains(r.front())) d.flags8 |= 0x200000;
                    break;
                case 0x1b: for (size_t k = 0; k < 3; ++k) d.box[k] = le<int16_t>(x, k * 2); break;
                case 0x1c: case 0x42:
                    for (size_t k = 0; k < 3; ++k) d.box[3 + k] = le<int16_t>(x, k * 2);
                    if (x.size() >= 8) d.radius = le<uint16_t>(x, 6);
                    break;
                case 0x1d: d.param = le<int16_t>(x, 0); break;
                case 0x1e: d.reg = le<int16_t>(x, 0); break;
                case 0x1f: d.zone = x[0]; break;
                case 0x0b: d.link = le<uint16_t>(x, 0); break;
                case 0x27: d.entityId = le<uint16_t>(x, 0); break;
                case 0x38: d.inflate = le<int16_t>(x, 0); break;
                case 0x3a: d.act3c = le<uint16_t>(x, 0), d.act3e = le<uint16_t>(x, 2); break;
                case 0x46: d.shadow = le<uint16_t>(x, 0); break;
                case 0x0f: r.push_back(le<uint16_t>(x, 0)); break;
                case 0x32: {
                    PlayerState s{le<uint16_t>(x, 0), {}};
                    for (size_t k = 0; k < 5; ++k) s.moves[k] = le<int16_t>(x, 2 + k * 2);
                    d.states.push_back(s);
                    break;
                }
                case 0x30:
                    d.steps.push_back({le<uint16_t>(x, 0), le<uint16_t>(x, 2), le<int16_t>(x, 4), le<uint16_t>(x, 8),
                                       le<uint32_t>(x, 12), le<uint32_t>(x, 16),
                                       {le<int16_t>(x, 20), le<int16_t>(x, 22), le<int16_t>(x, 24), le<int16_t>(x, 26),
                                        le<int16_t>(x, 28)}});
                    break;
                case 0x31:
                    d.items.push_back({le<uint16_t>(x, 0), le<uint16_t>(x, 2), le<uint32_t>(x, 4), le<uint32_t>(x, 8),
                                       item(x, 12), le<uint32_t>(x, 20), le<int32_t>(x, 24), le<int32_t>(x, 28)});
                    break;
                default: break;
                }
            }
            w.defs.push_back(std::move(d));
        }
    }
    for (size_t i = 0; i < w.defs.size(); ++i) {
        for (const uint16_t a : refs[i]) {
            if (auto f = flipbooks.find(a); f != flipbooks.end()) w.defs[i].flipbooks.push_back(f->second);
            if (auto f = animFx.find(a); f != animFx.end()) w.defs[i].animFx.push_back(f->second);
        }
        // 0x4452a0: the first listed asset of class 8 (a sprite sequence; its records are the frames).
        const auto s = std::ranges::find_if(refs[i], [&](uint16_t a) { return roleOf.contains(a) && roleOf[a] == 8; });
        if (s == refs[i].end()) continue;
        if (auto r = spriteRecs.find(*s); r != spriteRecs.end() && !r->second.empty()) {
            ActorDef& d = w.defs[i];
            d.sprite = *s, d.spriteFrames = uint16_t(r->second.size());
            d.spriteW = r->second[0].w, d.spriteH = r->second[0].h;
        }
    }
    for (const uint16_t a : playerRefs)
        if (auto f = animFx.find(a); f != animFx.end()) w.playerFx_.push_back(f->second);
    if (!player) throw FormatError("LitWorld: no player entity (tag 0x13 == 1)");
    if (w.hf.empty()) throw FormatError("LitWorld: no chunk 0x36 collision heightfield");
    if (!w.findState(1) || !w.findMove(uint16_t(w.findState(1)->moves[0])))
        throw FormatError("LitWorld: player tables lack state 1");
    w.start(w.spawn_, w.spawnYaw_, w.p.zone);
    return w;
}

void LitWorld::start(V3 spawn, int16_t yaw, int16_t zone) {
    // New-game defaults (§8.1): health G[1] = max G[2] = 6, lives G[0] = 3.
    G[0] = 3;
    G[1] = G[2] = 6;
    spawn_ = spawn;
    spawnYaw_ = yaw;
    const uint16_t id = p.entityId;
    p = Player{};
    p.entityId = id;
    p.pos = spawn;
    p.yaw = yaw;
    p.zone = zone;
    // 0x4b2460 / 0x4b246c (respawn point, yaw) have no writer at a level load: only trigger entries and actor items with
    // 0x100000 set them (50 levels do so on their first tick, e.g. L01a (23700, −640, 9655) yaw 4096 as the oracle).
    // Before that they hold the previous level's values (0 after boot): the app carries them (carryCheckpoint).
    cam = Camera{};
    cam.eye = cam.eyeCopy = levelEye_;
    actors.clear();  // live list 0x4b3740: chunks 0x07 / 0x0a in file order (§7.1)
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].chunk != 0x08) actors.push_back(instantiate(i));
    order_.resize(actors.size());
    std::iota(order_.begin(), order_.end(), 0u);
    collide_.clear();  // rebuilt by each actorsTick (0x447d40)
}

const PlayerState* LitWorld::findState(uint16_t id) const {
    for (const PlayerState& s : states)
        if (s.id == id) return &s;
    return nullptr;
}
const PlayerMove* LitWorld::findMove(uint16_t id) const {
    for (const PlayerMove& m : moves)
        if (m.id == id) return &m;
    return nullptr;
}
// 0x434840: point h inside Bugs' box — xz distance² within (inflation + radius)², then in his rotated frame inside
// the TOD box grown by the inflation horizontally, strictly between the top +0x52 and +0x4a.
bool LitWorld::handHitsPlayer(const V3& h) const {
    const int32_t dx = int16_t(h.x - p.pos.x), dy = int16_t(h.y - p.pos.y), dz = int16_t(h.z - p.pos.z);
    const int32_t r = playerInflate_ + p.animState.radius;
    if (uint32_t(dz * dz + dx * dx) > uint32_t(r * r)) return false;
    const V3 l = rotY(int16_t(0x1000 - p.yaw), dx, dy, dz);
    const auto& b = p.animState.box;
    return b[0] - playerInflate_ < l.x && l.x < b[3] + playerInflate_ && b[2] - playerInflate_ < l.z &&
           l.z < b[5] + playerInflate_ && l.y < b[1] && b[4] < l.y;
}

int16_t LitWorld::boxTop() const { return p.animState.boxSet ? p.animState.box[4] : playerBoxTop_; }  // +0x52

// 0x41deb0 for self (nullptr = Bugs): mode 1 start, mode 2 release (sfx.h).
void LitWorld::playSound(const LitActor* self, uint32_t id, uint16_t mode) {
    sfx.request(emitter(self), uint16_t(id), mode, rand_);
}

// 0x437ab0 (from 0x43a880 after the state machine, move = current move id): Bugs' looping glide sound 0xd while
// flags14 0x10000, dig sound 0x1b in move 0x8b underground (flags18 4) while a direction is held (GD+0x0b & 0xf0).
void LitWorld::bugsLoopSounds(uint16_t move) {
    const bool glide = p.flags14 & 0x10000, dig = (p.flags18 & 4) && (pad.pressed & 0xf000) && move == 0x8b;
    if (glide != bool(loopSounds_ & 8)) loopSounds_ ^= 8, playSound(nullptr, 0xd, glide ? 1 : 2);
    if (dig != bool(loopSounds_ & 0x10)) loopSounds_ ^= 0x10, playSound(nullptr, 0x1b, dig ? 1 : 2);
}

// ---- tick ----

void LitWorld::tick(const PadInput& in) {
    ++ticks;
    // F1 / Esc (window messages before the tick body, 0x40e4a1), not while a menu page locks them (0x4b3fc0, A120).
    // F1: request the help screen, or leave a pause state. Esc (also not during speech S[0x17] or the cutscene lock
    // G[0x36]): request the pause menu, leave a pause state, or cancel a pending help request.
    if (in.help && !menuLock && !pauseRequest_) {
        if (S[12] == 0) helpRequest_ = true;
        else S[12] = 2;
    }
    if (in.esc && !menuLock && !S[0x17] && !G[0x36]) {
        if (helpRequest_) {
            if (S[12]) S[12] = 2;
            helpRequest_ = false;
        } else if (S[12] == 0) {
            pauseRequest_ = true;
        } else {
            S[12] = 2;
        }
    }
    keys_ = in.esc ? 2 : 0;
    key_ = in.key, joyButton_ = in.joyButton;
    // §2.2 (0x4491c0): all words zero while the player is scripted/dead (flags14 0x1000000) or a level change is
    // pending, unless paused; paused: no ✕/□/L1 window, no remap; the demo stream (G[0x0f]) replaces the pad.
    const std::span<const uint16_t> all(demo);
    std::span<const uint16_t> left = all.subspan(std::min(demoAt_, all.size()));
    padTick(pad, in, S[12] != 0, (p.flags14 & 0x1000000) || nextLevel >= 0, G[0x0f] ? &left : nullptr,
            S[0x17] != 0 && S[0x17] != 4);
    demoAt_ = all.size() - left.size();
    if (jumpBuffer) {  // Enhanced jump buffer (lit_world.h): edge of the physical button(s) the preset makes jump
        static constexpr uint16_t kPhys[8] = {PadSquare, PadCircle, PadTriangle, PadCross, PadL1, PadL2, PadR1, PadR2};
        uint16_t phys = 0;
        for (size_t i = 0; i < 8; ++i)
            if (pad.remap[i] == PadCross) phys |= kPhys[i];
        const bool held = (in.buttons & phys) != 0;
        if (S[12] || G[0x0f]) jumpLeft = 0;
        else if (held && !jumpWasHeld) jumpLeft = uint8_t(jumpBuffer + 1);
        else if (jumpLeft && --jumpLeft && (p.flags14 & 3) == 2) pad.pressed |= PadCross, pad.held |= PadCross;  // grounded
        jumpWasHeld = held;
    } else {
        jumpLeft = 0;
    }
    const uint16_t stateBefore = p.state;
    if (S[12] == 0) cameraTick();  // §1.1 step 3a: camera first, from the previous tick's player position
    actorsTick();   // §7.1: activation, then behaviours in active-list order (player among them)
    if (jumpLeft && p.state != stateBefore && (p.state == 7 || p.state == 8 || p.state == 9 || p.state == 26 || p.state == 72 || p.state == 78))
        jumpLeft = 0;  // the jump took it
    sfx.mix(cam.eye, cam.target, [&](int32_t e, uint16_t flags, V3& pos) {  // 0x41edb0 right after 0x447d40
        if (e < 2) return true;  // Bugs (0x4b324c): fixed levels
        const LitActor& a = actors[size_t(e - 2)];
        pos = a.pos;
        // 0x41f620: entry flag 0x100 adds the first part's translation (its coordinate param, +0x78 -> +0x84..+0x8c).
        if ((flags & 0x100) && !a.animState.parts.empty()) {
            const auto& t = a.animState.parts.front().trans;
            pos = {pos.x + t[0], pos.y + t[1], pos.z + t[2]};
        }
        return false;
    });
    cameraNodes();  // §1.1 step 9: the render pass poses the actors (camera nodes set the view)
    hudTick();      // HUD actor 0x44acc0 (lit_hud.cpp; §3.6 health below 1 -> death state)
    speechTick();   // §1.1 step 5
    // §1.1 step 7 (0x4065e0): pause states. A pending pause (Esc) or help (F1) request opens unless a fade, a scripted
    // camera (0x300 without 1), the cutscene lock G[0x36] or speech blocks it: the HUD is hidden (0x449d60), the pause
    // menu shows the totals (0x449d80) and resets its pause-menu bytes; 2 (resuming) returns to 0 unless a menu page
    // locks it (0x4b3fc0), hiding the HUD again.
    if (S[0] == 0) {
        if (S[12] == 0) {
            if (!(fade.mode || ((cam.flags & 0x300) && !(cam.flags & 1)) || G[0x36] || S[0x17]) && (pauseRequest_ || helpRequest_)) {
                for (HudItem& h : hud) h.shown = false;
                if (pauseRequest_) {
                    hudTotals();
                    S[12] = 1, S[13] = 0, S[14] = 1, S[15] = 0, S[16] = 1;
                } else {
                    S[12] = 3;
                }
            }
        } else if (S[12] == 2 && !menuLock) {
            S[12] = 0;
            for (HudItem& h : hud) h.shown = false;
        }
    }
    if (nextLevel >= 0) {  // level change request (0x4b39bc = 1; e.g. the pause menu's Quit): unpaused, HUD hidden, speech off
        S[12] = 0, S[0x17] = 0;
        for (HudItem& h : hud) h.shown = false;
    }
    musicTick();
    helpRequest_ = pauseRequest_ = false;
    // §1.1 step 9, 0x44c4b0: the cut-scene lock G[0x36] draws black bars (0x40c5e0) and grows / shrinks by 2 per pass.
    letterbox = int8_t(G[0x36]);
    if ((letterbox > 0 && G[0x36] < 0x1e) || letterbox < 0) G[0x36] = uint8_t(G[0x36] + 2);
    fadeTick();     // §1.1 step 9: the render pass advances the fade (0x44c4b0 / 0x44bef0)
}

// Music, once per tick after the pause states (0x4065e0 reaches 0x41d900 unless a pause state paused the stream with
// 0x41d5c0; S[12] 2 resumes it with 0x41d5e0). 0x41d900: no entry for the player's zone -> stop (Track 0); a zone
// change picks its entry unless both of its tracks equal the current one (tracks above 0x28 -> 0x27) and plays
// `track`. 0x41edb0 (every tick, before): a stopped stream reopens the entry's track2. 0x41d630 stops instead of
// playing while the fade 0x4b3232 or the speech status S[0x17] is set, so music restarts after fades and speech.
// Config music volume +0x34 below 50: 0x41d900 (and PlayMusic 0x41d630 / 0x41edb0) do nothing, the stream stays.
void LitWorld::musicTick() {
    musicPaused = S[0] == 0 && (S[12] == 1 || S[12] == 3);
    if (musicPaused || volume34 < 0x32) return;
    bool none = true;
    if (S[0x17] == 0) {
        const uint16_t zone = uint16_t(p.zone);
        const auto it = std::ranges::find_if(musicZones, [&](const auto& e) { return e[0] == zone; });
        if (it == musicZones.end()) {
            musicTrack_ = 0;
        } else {
            none = false;
            if (zone != musicZone_) {
                musicZone_ = zone;
                if (musicTrack_ != (*it)[1] || musicTrack_ != (*it)[2]) {
                    for (uint8_t& t : std::span(it->data() + 1, 2)) t = t > 0x28 ? 0x27 : t;  // 0x4abf78 = 0x28
                    musicEntry_ = int(it - musicZones.begin());
                    music = musicTrack_ = (*it)[1];
                }
            }
        }
    }
    musicNext = musicEntry_ >= 0 ? musicZones[size_t(musicEntry_)][2] : 0;
    if (none || fade.mode || S[0x17]) music = 0;
    else if (!music) music = musicTrack_ = musicNext;  // 0x41edb0 reopens track2
}

void LitWorld::soundKey() {  // 0x40e270 VK_F2
    if (volume30 < 1) {
        volume30 = volume30Kept < 1 ? 100 : volume30Kept;
        sfx.resumeAll();  // 0x41f510
    } else {
        volume30Kept = volume30;
        volume30 = 0;
        sfx.pauseAll();  // 0x41f450
    }
}

// F3: 0x41d5c0 stops the stream (bbtime: Track 0, which musicTick keeps while the volume is below 50); 0x41d5e0 plays
// the track that was playing (0x41d630(0x4abf94): from its start; nothing below volume 50, during a fade or speech).
void LitWorld::musicKey() {  // 0x40e270 VK_F3
    if (volume34 < 1) {
        volume34 = volume34Kept < 1 ? 100 : volume34Kept;
        if (volume34 >= 0x32 && !fade.mode && !S[0x17]) music = musicStopped_;
    } else {
        volume34Kept = volume34;
        volume34 = 0;
        if (music) musicStopped_ = music;
        music = 0;
    }
}

// Speech clip lengths per language (bytes of speeches/Speech0N.xa = stereo frames at 44100 Hz, 0x45c968): row N =
// table 0x45c340 + N·0x100, entry k = clip id k + 2; a clip starts at the sum of the entries before it (each row sums
// to its file's size: Speech00.xa 36102560 B).
static constexpr uint32_t kSpeechClip[6][64] = {
        {77197, 90406, 88204, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230665, 346839, 318239, 247099,
         189021, 199949, 191106, 187893, 272622, 1502372, 4183086, 623438, 1076876, 92746, 99267, 286484, 204502,
         298573, 187581, 196015, 197146, 430963, 339479, 212750, 126887, 186525, 112542, 275479, 196248, 369126,
         325794, 318488, 664293, 237626, 389965, 354743, 378819, 390353, 411233, 431813, 346088, 310430},
        {77176, 90406, 88201, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230645, 339441, 318228, 247095,
         189014, 199908, 191087, 187869, 272586, 1502372, 4183086, 623404, 1076877, 92744, 99226, 286476, 204493,
         298560, 187559, 196015, 197146, 432270, 339439, 212740, 126888, 186526, 112501, 275452, 196248, 369126,
         325769, 318449, 664282, 237614, 389929, 354745, 378824, 390342, 411235, 431785, 346054, 310422},
        {77180, 90406, 88201, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230644, 339440, 318227, 247094,
         189014, 199908, 191087, 187868, 272584, 1502372, 4183086, 623400, 1076877, 92744, 99226, 286476, 204494,
         298560, 187559, 196015, 197146, 432270, 339440, 212797, 126888, 186526, 112500, 275452, 196248, 369126,
         325768, 318449, 664283, 237613, 389890, 354742, 378820, 390333, 411235, 431788, 346054, 310423},
        {77177, 90407, 88202, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230666, 346805, 318227, 247096,
         189014, 199908, 191087, 187869, 272583, 1502372, 4183086, 623401, 1076877, 92744, 99227, 286475, 204493,
         298560, 187560, 196015, 197146, 432272, 339442, 212751, 126888, 186526, 112501, 275452, 196248, 369126,
         325770, 318450, 664283, 237613, 389932, 354745, 378823, 390359, 411235, 431786, 346057, 310422},
        {77176, 90420, 88202, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230666, 339440, 318228, 247095,
         189014, 199908, 191088, 187868, 272583, 1502372, 4183086, 623404, 1075473, 92744, 99227, 286476, 204493,
         298558, 191978, 196015, 197146, 432272, 339442, 212797, 126888, 186526, 112500, 275451, 196248, 369126,
         325770, 318449, 664283, 237614, 389928, 354742, 378823, 390360, 411236, 431785, 346055, 310423},
        {77177, 90407, 88201, 805166, 1102502, 1716648, 971506, 1878373, 1240976, 383501, 753510, 1006069, 447489,
         1232039, 353492, 1130107, 884945, 581247, 1421654, 936905, 606247, 453214, 230666, 339442, 318227, 247094,
         189014, 199908, 191088, 187868, 272583, 1502372, 4183086, 623404, 1076877, 92744, 99227, 286476, 204494,
         298558, 187560, 196015, 197146, 432273, 339442, 212741, 126888, 186526, 112501, 275450, 196248, 369126,
         325768, 318450, 664281, 237613, 389895, 354745, 378821, 390334, 411237, 431786, 346055, 310423},
};

static uint32_t speechClipLen(uint8_t language, uint8_t id) { return id >= 2 && id < 66 ? kSpeechClip[language][id - 2] : 0; }

// XA speech status S[0x17] (0x41e2b0): A81 start 8 -> 2 -> 4 playing (clock started) -> 0 once the clip length (table
// above / 44100 Hz) has elapsed on the 30 Hz tick clock. That is the exe's path without audio (the Wine oracle's);
// with audio the clip itself plays (speechClip) and the exe waits for the player, which ends after the same length.
void LitWorld::speechTick() {
    uint8_t& st = S[0x17];
    if (st == 8) {
        st = 2;
    } else if (st == 2) {
        speechMs_ = uint32_t(speechClipLen(language, speech_) * 1000ull / 44100);
        speechStart_ = ticks;
        st = 4;
    } else if (st == 4 && uint64_t(ticks - speechStart_) * 1000 >= uint64_t(speechMs_) * 30) {
        st = 0;
    }
}

// 0x41e2b0 state 2 with audio (config volume +0x34 >= 50): seek Speech0N.xa to the clip, play its length on the music
// player (the music track restarts from its start afterwards, state 0 -> 0x41d630).
std::optional<LitWorld::SpeechClip> LitWorld::speechClip() const {
    if (S[0x17] != 4 || volume34 < 0x32 || !speechClipLen(language, speech_)) return std::nullopt;
    uint64_t at = 0;
    for (uint8_t k = 2; k < speech_; ++k) at += speechClipLen(language, k);
    return SpeechClip{at, speechClipLen(language, speech_)};
}

void LitWorld::initPlayer() {  // 0x43a880 first call (part list not built yet)
    p.initialised = true;
    p.lean = 0;
    invuln_ = 0;
    p.state = findState(2) ? 2 : 1;  // §3.5 state 2 = level-start idle
    p.seq = 0;
    p.anim = 0;
    p.flags14 = 0;
    vsX_ = vsZ_ = momX_ = momZ_ = 0;
    swimDrop_ = 0;  // 0x4b3c40
    accelK_ = 0x800;
    safe_ = p.pos;
    turnErr_ = -1;
    cameraReset();
    p.flags18 = 0;
}

// 0x43a250: jump arc phase step (§3.3).
void LitWorld::arcTick(Mover& m) {
    const int16_t ph = m.phase;
    m.flags14 = ph < 0x800 ? m.flags14 & ~8u : m.flags14 | 8;
    if (ph < 0xc01 && (!(m.flags14 & 4) || (m.flags14 & 0x10))) {
        const int32_t s = rsin(ph) * m.jumpH;
        m.vy = (m.flags14 & 8) ? int16_t(uint16_t((-s) >> 11) & 0xfffe) : int16_t((-s) >> 12);  // down twice as fast
        m.phase = int16_t(m.phase + m.jumpStep);
        return;
    }
    m.vy = 0;
    const uint32_t f = m.flags14;
    m.flags14 = f & ~9u;
    if (!(f & 2)) m.flags14 |= 0x200;  // arc ended airborne -> free fall
}

// MoveActor 0x437390 (§4.3): horizontal trace with diagonal wall-slide retry (0x436ff0), vertical target
// (0x4371f0), fall/landing/smoothing (0x436de0). delta is in/out (world units this tick).
void LitWorld::moveActor(Mover& m, V3& d, Ground& r, int16_t top) {
    r.zone = m.zone;
    // --- 0x436ff0 ---
    const V3 save = m.pos;
    if (m.vy < 0) m.pos.y += m.vy;  // rising: trace from the raised height
    const int32_t yTrace = m.pos.y;
    const int16_t origDx = int16_t(d.x), origDz = int16_t(d.z);
    int32_t dx = d.x, dz = d.z;
    hf.trace(m.pos, dx, dz, r);
    int32_t accX = dx, accZ = dz;
    m.pos.x += dx;
    m.pos.z += dz;
    if (r.kind & KindDiagonal) {  // wall slide: retry the remaining X, then the remaining Z
        int32_t rx = int32_t(origDx) - dx, rz = 0;
        const int16_t remZ = int16_t(origDz - int16_t(dz));
        m.pos.y = yTrace;
        hf.trace(m.pos, rx, rz, r);
        if (rx != 0) { accX += rx; m.pos.x += rx; }
        rx = 0;
        rz = remZ;
        m.pos.y = yTrace;
        hf.trace(m.pos, rx, rz, r);
        if (rz != 0) { accZ += rz; m.pos.z += rz; }
    } else if (r.kind & KindBlocked) {
        m.flags14 |= 0x100000;
    }
    if (r.kind & KindOpen) {  // open sub-cell: floor of the zone below (§4.2)
        m.pos.y += 1;
        const int16_t keep = r.zone;
        hf.groundAt(m.pos, r);
        r.zone = keep;
    }
    m.groundBase = m.pos.y;
    m.hoff = r.hoff;
    m.pos = save;
    d.x = accX;
    d.z = accZ;
    // --- 0x437390 ---
    if (r.kind & KindBlocked) m.flags14 |= 0x100000;
    if (r.zone != -1) m.zone = r.zone;
    // --- 0x4371f0: vertical target ---
    const int32_t ground = m.hoff + m.groundBase;
    const uint32_t f = m.flags14;
    if (!(f & 1)) {
        if (!(f & 4)) d.y = (r.kind & KindNoZone) ? 200 : ground - m.pos.y;  // no zone: dy = 200 (§4.3)
        else if (ground < m.pos.y + d.y) d.y = ground - m.pos.y;
    } else {
        int32_t target = m.vy + m.pos.y;
        bool land = false;
        if (m.phase < 0x801) {
            if (target + top < r.ceiling) {  // head above the zone ceiling
                if (target <= ground) {
                    V3 q{m.pos.x + d.x, r.ceiling - 1, m.pos.z + d.z};
                    Ground above;
                    above.zone = -1;
                    hf.groundAt(q, above);
                    if (!(above.kind & KindOpen)) target = std::min(r.ceiling - int32_t(top), ground);
                    else if (!(target < ground)) land = true;
                } else {
                    land = true;
                }
            } else if (!(target < ground)) {
                land = true;
            }
        } else if (r.kind == KindOpen || target <= ground) {
            if (r.kind == KindOpen || r.kind == KindNoZone) m.groundBase += 1;
            if (!(target < ground)) land = true;
        } else {
            land = true;
        }
        if (land) {
            m.flags14 = f & ~9u;
            target = ground;
        }
        d.y = target - m.pos.y;
    }
    // --- 0x436de0: smoothing, fall, landing (§3.3) ---
    const int32_t dy0 = d.y;
    const int32_t to = m.pos.y + dy0;
    uint32_t keep = 0, cleared = 0;
    bool done = false;
    if (to < m.pos.y + 200 || (m.flags14 & 1)) {
        const uint32_t g = m.flags14;
        if (!(g & 1)) {
            if (!(g & 2)) {
                if (!(g & 4)) d.y = std::clamp(d.y, -70, 70);  // airborne snap clamp ±70
            } else if (d.y < 1) {
                if (-0x46 < d.y && (d.y & ~1) != 0) d.y >>= 1;  // grounded step smoothing
            } else if ((d.y & ~1) != 0) {
                d.y >>= 1;
            }
        }
        if (d.y + m.pos.y == m.hoff + m.groundBase && r.kind != KindOpen && r.kind != KindNoZone) {
            m.phase = 0x800;  // landing
            m.flags14 = (m.flags14 & 0xfffefdff) | 2;
            done = true;
        } else if (m.flags14 & 2) {
            done = true;
        } else {
            cleared = m.flags14 & ~2u;
            keep = m.flags14 & 5;
            m.flags14 = cleared;
        }
    } else {  // more than 200 below: free fall vy = 70 + 70·cos (§3.3)
        if (m.phase > 0xbff) m.phase = 0xc00;
        const int32_t s = rsin(m.phase + 0x400);
        m.phase = int16_t(m.phase + 0x44);
        const int16_t vy = int16_t(0x46 - int16_t((s * -0x46) >> 12));
        const uint32_t g = m.flags14;
        const int32_t next = (g & 0x10000) ? m.pos.y + 12 : vy + m.pos.y;  // glide: 12
        if (to <= next) {
            if (r.kind == KindOpen || r.kind == KindNoZone) {
                m.flags14 = g & ~2u;
                if (!(g & 4)) m.flags14 = (g & ~2u) | 0x200;
            } else {
                m.phase = 0x800;
                m.flags14 = (g & 0xfffefdff) | 2;
            }
            done = true;
        } else {
            d.y = (g & 0x10000) ? 12 : vy;
            cleared = m.flags14 & ~2u;
            keep = m.flags14 & 4;
            m.flags14 = cleared;
        }
    }
    if (!done && keep == 0) m.flags14 = cleared | 0x200;
    if (m.flags14 & 4) d.y = dy0;
}

// 0x437b90(state, NULL): off his carrier / platform / rope (+0x70), the carried actor let go where it is (a type-34
// held item freed), then the death state.
void LitWorld::kill(int16_t deathState) {
    invuln_ = 0;
    p.seq = 0;
    p.state = uint16_t(deathState);
    // +0x158 is left: a death move with the clip already playing continues it.
    p.flags14 = (p.flags14 | 0x1000004) & 0xfffeffff;
    if (p.rope >= 0 || p.platform >= 0 || p.carrier >= 0) p.flags14 &= 0xffffffeb;
    if (p.carrier >= 0 && actors[size_t(p.carrier)].rider == -2) actors[size_t(p.carrier)].rider = -1;
    p.rope = p.platform = p.carrier = -1;
    p.carrierPart = -1;
    if (p.rider >= 0) {
        if (actors[size_t(p.rider)].type == 0x22) actors[size_t(p.rider)].flags14 |= 0x100;
        dropCarried();
    }
    turnErr_ = -1;
}

void LitWorld::dropCarried() {
    LitActor& c = actors[size_t(p.rider)];
    c.parent = -1;
    c.parentPart = -1;
    c.flags14 = (c.flags14 & ~0x800u) | 0x1000;
    p.rider = -1;
}

void LitWorld::respawn() {  // end of the death clip (0x43a880 tail): checkpoint, health 6, 30 ticks invulnerable
    p.flags14 = 0;
    p.rope = -1;
    p.pos = checkpoint_;
    cam.yaw = int16_t(checkpointYaw_);
    p.yaw = cam.yaw;
    cameraSnap(checkpoint_);
    invuln_ = 0x1e;
    G[1] = 6;
}

// 0x4377a0: force Bugs into state `state` (move index 0, animation restarted).
void LitWorld::forceState(uint16_t state) {
    if (p.flags14 & 0x1000000) return;
    const PlayerState* st = findState(state);
    if (!st) { unsupported.insert(std::format("state {}", state)); return; }
    p.state = state;
    p.seq = 0;
    p.anim = 0;
    p.lean = 0;
    if (const PlayerMove* mv = findMove(uint16_t(st->moves[0])); mv && (mv->flagsB & 0x800)) p.flags14 &= 0xfffffdf6;
    turnErr_ = -1;
}

// 0x437d90: enemy hit on Bugs. Damage only while his move is vulnerable (flagsA 0x4000), no invulnerability,
// no level change, S[3] == 0 and not scripted: 1, or the attacker step +0x16 clamped to 2 (0 = none); 30 ticks
// invulnerable. Returns whether the hit counted (the caller then forces the knock-back state).
bool LitWorld::damagePlayer(LitActor& a, const ActorStep* st) {
    if (invuln_ > 0 || nextLevel >= 0 || S[3] || (p.flags14 & 0x81000000)) return false;
    a.flags14 |= 0x8000;
    const PlayerState* ps = findState(p.state);
    const PlayerMove* mv = ps && p.seq <= 4 ? findMove(uint16_t(ps->moves[p.seq])) : nullptr;
    if (!mv || !(mv->flagsA & 0x4000)) return false;
    p.contact = a.entityId;
    a.contact = 1;
    int dmg = 1;
    if (st && !((a.flags8 & 0x10000000) && (a.flags14 & 1)) && !(st->flagsA & 0x200000)) dmg = std::min<int>(st->w[1], 2);
    if (dmg > 0) {
        G[1] = uint8_t(G[1] - dmg);
        invuln_ = 0x1e;
        p.flags14 |= 0x2000000;
    }
    a.flags18 |= 0x4000;
    if (int8_t(G[1]) < 1) a.flags18 |= 0x2000;
    return true;
}

// 0x4384a0 + 0x434b00 for actor m (self, nullptr = Bugs): the first trigger volume containing it runs the entries
// addressed to its entity id (§7.5; the logic item goes through the VM).
void LitWorld::zoneTriggers(Mover& m, LitActor* self) {
    if (nextLevel >= 0) return;
    const int16_t top = self ? self->box[4] : boxTop();
    for (const Trigger& t : triggers) {
        if (!(t.flags & 2) && t.zone != m.zone) continue;
        const int16_t lx0 = int16_t(int16_t(m.pos.x) - int16_t(t.pos.x)), ly = int16_t(int16_t(m.pos.y) - int16_t(t.pos.y)),
                      lz0 = int16_t(int16_t(m.pos.z) - int16_t(t.pos.z));
        if (!(top + ly < 1 && t.minY <= ly)) continue;
        int32_t lx = lx0, lz = lz0;
        if (!(t.flags & 1)) {  // local frame: RotMatrixZYX(0x1000 - rot); only the yaw is modelled
            const V3 l = rotY(0x1000 - t.rotY, lx0, 0, lz0);
            lx = int16_t(l.x);
            lz = int16_t(l.z);
        }
        if (lx < 0 || lx > t.sizeX || lz < 0 || lz > t.sizeZ) continue;
        for (const TriggerEntry& e : t.entries) {
            if ((m.entityId == 6 || e.actor == 6) && self && (self->flagsC & 2)) { self->flags14 |= 0x100; return; }
            if (e.actor != m.entityId) continue;
            if (e.mask14 && !(m.flags14 & e.mask14)) continue;
            if (e.item.cond && !cond(e.item, self)) continue;
            if (e.item.act) act(e.item, self);
            const V3 arg{e.arg[0], e.arg[1], e.arg[2]};
            if (e.flags & 0x80000000) forceState(uint16_t(e.arg[0]));
            if (e.flags & 0x800) { p.pos = arg; p.platform = p.rope = -1; p.flags14 &= ~0x14u; cameraSnap(arg); }  // 0x4379b0
            if (e.flags & 0x40000000) {  // teleport self
                m.pos = arg;
                if (!self && (p.platform >= 0 || p.rope >= 0)) { p.platform = p.rope = -1; p.flags14 = (p.flags14 & ~0x14u) | 0x202; }
            }
            if (e.flags & 0x80000) nextLevel = e.arg[0];  // level exit: GD+0 = arg, 0x4b39bc = 1
            if (e.flags & 0x200000) { kill(int16_t(e.arg[0])); G[1] = 0; }  // kill, death state arg0
            if (e.flags & 0x400000) defaultDeathState_ = e.arg[0];
            if (e.flags & 0x100000) { checkpoint_ = arg; checkpointYaw_ = p.yaw; }
            // 0x10000 leaves the entry loop into 0x4384a0's tail: flags14 |= 0x100 (freed; runtime: L04B3's floor
            // volume (0, 0, 0) y 0..−400 frees ball 100 falling into it at counter 2067, w9lit8/o_back.csv).
            if (e.flags & 0x10000) { m.flags14 |= 0x100; return; }
            if (e.flags & 0x200) playSound(self, uint32_t(e.arg[0]));  // 0x4386ee
            if (e.flags & 0x2000) {  // 0x437510: the actor in the volume starts state arg0
                if (self) {
                    startScript(*self, uint16_t(e.arg[0])), self->flags14 &= 0xf9fffdf6;
                } else if (const PlayerState* st = findState(uint16_t(e.arg[0]))) {
                    // Bugs (only LB02's zone-50 volume, state 1): 0x437510 sets +0x184 = move-list head + the
                    // state's first move id, a misaligned "move" (Wine read, local/w13/lit/lb02_moves.txt: head + 2,
                    // u16 +6 = move 2's mode 2, byte +8 = its +6 low byte 2). It keeps the clip only when that byte
                    // has 0x40 and the u16 equals the current move's anim; otherwise +0x158 / +0x146 / +0x14c (clip)
                    // and +0x17c (his state id) become 0. His tick resolves the state from +0x17c, so with no state
                    // 0 (none in LiT) the exe dereferences null and crashes; the LB02 volume (y 27140) is never
                    // reached by the ski cut-scene Bugs, who stays at the origin. bbtime's missing-state fallback
                    // (state 1) stands in for the crash.
                    const auto img = [&](size_t o) { return o < moveImage.size() ? moveImage[o] : uint8_t(0); };
                    const size_t o = uint16_t(st->moves[0]);
                    const uint16_t g6 = uint16_t(img(o + 6) | img(o + 7) << 8);
                    const PlayerState* cur = findState(p.state);
                    const PlayerMove* cm = cur && p.seq <= 4 ? findMove(uint16_t(cur->moves[p.seq])) : nullptr;
                    if (!(img(o + 8) & 0x40) || !cm || g6 != cm->anim) p.state = 0, p.anim = 0;
                    p.flags14 &= 0xf9fffdf6;
                }
            }
            if (e.flags & 0x40) cameraSnap(arg);
            if (e.flags & 0x800000) unsupported.insert("trigger flag 0x800000");  // respawn point 0x4b2180 (no reader found)
            if (!(e.flags & 0x8000)) return;
        }
        return;
    }
}

// ---- ropes (0x43a310 / 0x43a7b0) and floating (0x43a880 LAB_0043d6d3) ----

// 0x43a310 (mask 0x400004): the first active collision-class-2 rope (flags8 0x400000 pole / 4 chain) in Bugs' zone,
// not in an arc of its own, within its radius² (xz), whose frame (Ry(-yaw) only) holds his hands (feet - 300):
// pole |x|, |z| <= inflation and 0..seg; chain: the node origins carried up to the top node's frame by each lower
// node's matrix (the top node's own matrix is skipped, as the exe), then the root pitch; node j spans its origin to
// the one below (the bottom node: seg down), the hands strictly inside in y and within inflation of the span in x/z;
// that node becomes +0x13c (straight on the next swing). ly = his local height (feet).
int LitWorld::ropeGrab(int32_t& ly) {
    for (size_t i = 0; i < actors.size(); ++i) {
        LitActor& o = actors[i];
        if (!o.active || o.dead || o.def < 0 || defs[size_t(o.def)].cls != 2 || !(o.flags8 & 0x400004) || (o.flags14 & 1) ||
            o.zone != p.zone || o.animState.parts.size() < 2)
            continue;
        const V3 rel{int16_t(int16_t(p.pos.x) - int16_t(o.pos.x)), int16_t(int16_t(p.pos.y) - int16_t(o.pos.y)),
                     int16_t(int16_t(p.pos.z) - int16_t(o.pos.z))};
        if (uint32_t(rel.z * rel.z + rel.x * rel.x) > o.radius2) continue;
        Mat inv;
        rotMatrixZYX(inv, 0, 0x1000 - o.yaw, 0);
        const V3 l = apply(inv, rel);
        const int32_t r = o.inflate, h = l.y - 300;
        if (l.x < -r || l.x > r) continue;
        if (o.flags8 & 0x400000) {
            if (-r <= l.z && l.z <= r && h >= 0 && h <= o.ropeSeg) { ly = int16_t(l.y); return int(i); }
            continue;
        }
        const auto& parts = o.animState.parts;
        struct E { V3 v; int16_t dy = 0, dz = 0; size_t part = 0; };
        std::vector<E> e;
        for (size_t q = parts.size() - 1; q >= 1 && e.size() < 20; --q) {
            e.push_back({{int16_t(parts[q].trans[0]), int16_t(parts[q].trans[1]), int16_t(parts[q].trans[2])}, 0, 0, q});
            const size_t s = e.size() - 1;
            if (s < 2) continue;
            const Mat M = ropePartMatrix(parts[e[s - 1].part]);
            const V3 a = e[s - 1].v;
            for (size_t j = 0; j + 1 < s; ++j) {
                const V3 v = apply16(M, e[j].v);
                e[j].v = {int16_t(v.x + a.x), int16_t(v.y + a.y), int16_t(v.z + a.z)};
            }
        }
        Mat pitch;
        rotMatrixZYX(pitch, o.ropePitch, 0, 0);
        for (E& x : e) x.v = apply16(pitch, x.v);
        e[0].dy = o.ropeSeg;
        for (size_t j = 0; j + 1 < e.size(); ++j)
            e[j + 1].dy = int16_t(e[j].v.y - e[j + 1].v.y), e[j + 1].dz = int16_t(e[j].v.z - e[j + 1].v.z);
        for (const E& x : e) {
            const bool inZ = x.dz < 0 ? x.dz + x.v.z - r <= l.z && l.z <= x.v.z + r : l.z <= x.dz + x.v.z + r && x.v.z - r <= l.z;
            if (inZ && x.v.y < h && h < x.dy + x.v.y) {
                o.ropeGrab = int16_t(x.part);
                ly = int16_t(l.y);
                return int(i);
            }
        }
    }
    return -1;
}

// 0x43a7b0: v = the int read at the move vector +4 (its forward s16 + 0 above it); hands h = v - 300 + local y.
int LitWorld::ropeClimb(int32_t v) {
    if (p.rope < 0) return 1;
    LitActor& o = actors[size_t(p.rope)];
    const int32_t h = v - 300 + p.local.y;
    if (o.flags8 & 0x400000) return h < 1 ? 2 : o.ropeSeg <= h ? 1 : 0;
    const size_t n = o.animState.parts.size();
    int16_t sum = 0;
    size_t q = 1;
    for (; q < n; ++q) {
        sum = int16_t(sum + o.ropeSeg);
        if (h < sum) break;
    }
    if (q < n && h > 0) { o.ropeGrab = int16_t(q); return 0; }
    o.ropeGrab = 1;
    return h < 1 ? 2 : 1;
}

// Bugs' coordinate chain: his local trans (origin of his own matrix) through the rope root +0x80.
V3 LitWorld::ropeWorld() const {
    const Mat& m = actors[size_t(p.rope)].ropeM;
    const V3 v = apply(m, p.local);
    return {v.x + m.t.x, v.y + m.t.y, v.z + m.t.z};
}

// Riding (+0x70 = platform, +0xc4 = its +0x7c): world = RotMatrix(platform rx, yaw, roll) · s16 local + its position.
V3 LitWorld::platformWorld() const {
    const LitActor& a = actors[size_t(p.platform)];
    Mat m;
    rotMatrixXYZ(m, a.rot[0], a.yaw, a.rot[2]);
    const V3 v = apply(m, p.local);
    return {v.x + a.pos.x, v.y + a.pos.y, v.z + a.pos.z};
}

// 0x43a880 attached (+0xc4): (0, 0, len) turned by Bugs' local yaw (+0x19e), then by each coordinate up the chain
// (0x4081c0: the rope root / the platform's RotMatrix / the carrier's part chain), its heading.
int16_t LitWorld::attachedHeading(int32_t len) const {
    Mat local, m;
    rotMatrixXYZ(local, 0, p.localYaw, 0);
    if (p.carrier >= 0) {  // carried: the carrier's (hand part) coordinate chain
        const V3 o = frameWorld(p.carrier, p.carrierPart, {});
        const V3 d = frameWorld(p.carrier, p.carrierPart, apply16(local, {0, 0, len}));
        return int16_t(ratan2(d.x - o.x, d.z - o.z));
    }
    if (p.rope >= 0) {
        m = actors[size_t(p.rope)].ropeM;
    } else {
        const LitActor& a = actors[size_t(p.platform)];
        rotMatrixXYZ(m, a.rot[0], a.yaw, a.rot[2]);
    }
    const V3 d = apply16(m, apply16(local, {0, 0, len}));
    return int16_t(ratan2(d.x, d.z));
}

// 0x434230: the rider's box corners (its rotation) and centre, offset by its x/z distance to the platform only (no y),
// turned into the platform frame (0x407b90 of 0x1000 − rot); any inside its x/z box ± inflation keeps it on. The
// faces tested follow the sector of its full-delta centre around the box centre (runtime: L01A seesaw, off at local
// x 1240 with roll 363 = 1005 − 40 > 960; RUNTIME_REFERENCE §18).
bool LitWorld::overPlatform(const LitActor& pl, const Mover& m, std::array<int16_t, 3> rot, const std::array<int16_t, 6>& q) const {
    const V3 dw{int16_t(m.pos.x - pl.pos.x), int16_t(m.pos.y - pl.pos.y), int16_t(m.pos.z - pl.pos.z)};
    Mat inv, r;
    rotMatrixZYX(inv, 0x1000 - pl.rot[0], 0x1000 - pl.yaw, 0x1000 - pl.rot[2]);
    rotMatrixXYZ(r, rot[0], rot[1], rot[2]);
    const auto& b = pl.box;
    const int32_t i = pl.inflate, cx = (b[3] >> 1) + (b[0] >> 1), cz = (b[2] >> 1) + (b[5] >> 1);
    const V3 c = apply16(inv, dw);
    const int32_t ang = ratan2(c.x - cx, c.z - cz);
    const int32_t a1 = ratan2((ang < 0 ? b[0] : b[3]) - cx + i, b[5] - cz + i), a2 = ratan2((ang < 0 ? b[0] : b[3]) - cx + i, b[2] - cz - i);
    const V3 pts[5] = {{q[0], 0, q[5]}, {q[3], 0, q[5]}, {q[0], 0, q[2]}, {q[3], 0, q[2]}, {}};
    for (const V3& pt : pts) {
        V3 v = apply16(r, pt);
        v = apply16(inv, {int16_t(v.x + dw.x), v.y, int16_t(v.z + dw.z)});
        const bool x0 = b[0] - i <= v.x, x1 = v.x <= b[3] + i, z0 = b[2] - i <= v.z, z1 = v.z <= b[5] + i;
        if (ang < 0 ? (a1 < ang ? z1 && x0 : a2 < ang ? x0 && z1 && z0 : z0 && x0) : (ang < a1 ? z1 && x1 : ang < a2 ? x1 && z1 && z0 : z0 && x1))
            return true;
    }
    return false;
}

void LitWorld::dropRope() {
    p.rope = -1;
    p.flags14 &= ~4u;
}

// Floating (LAB_0043d6d3) with the 0x433500 contact c: a buoyant actor (flags8 0x100000 round: horizontal distance r
// within its radius +0x2e; 0x800000: r = 0) holds Bugs unless he rises in an arc. Entering: flags14 0x404, plunge =
// -dy/16 (0 below 5 or for 0x800000), bob amplitude 7 + rand & 7, phases 0x400. While plunging: w.y += sin(ramp)·
// plunge, ramp += 0x40 up to 0x800. Then the lift w.y = -sin(ramp)·(base - k·base) with k = dy/top + r/radius (base
// 0x50 round / 0x78), ramp 0 -> 0x400, then + sin(bob)·amp (bob += 100, new amp 9 + rand & 7 each 0x1000), applied
// while dy + vy stays below top + 10. Leaving (no contact, outside, rising): flags14 0x400 -> free fall 0x200.
// Runtime: L01D1 geyser (21040, -2000, 7280) after S[45] = 1 (RUNTIME_REFERENCE §13). The drop of a held actor on
// entry is not modelled (carrying is not simulated).
void LitWorld::swim(int c, V3& w) {
    auto leave = [&] {
        if (p.flags14 & 0x400) p.flags14 = (p.flags14 & 0xfffffbfb) | 0x200;
    };
    if (c < 0 || !(actors[size_t(c)].flags8 & 0x900000) || ((p.flags14 & 1) && !(p.flags14 & 8))) return leave();
    LitActor& o = actors[size_t(c)];
    o.flags14 |= 0x8000;
    if (p.flags14 & 0x10) {
        p.lean = 0;
        p.platform = -1;
        p.flags14 = (p.flags14 & 0xfffffdeb) | 2;
    }
    const int32_t dx = int16_t(p.pos.x - o.pos.x), dy = int16_t(p.pos.y - o.pos.y), dz = int16_t(p.pos.z - o.pos.z);
    const int32_t r = (o.flags8 & 0x100000) ? int16_t(isqrt(dx * dx + dz * dz)) : 0;
    if (int32_t(o.radius) < r) return leave();
    if (!(p.flags14 & 0x400)) {
        swimAmp_ = (rand_() & 7) + 7;
        swimDrop_ = -dy >> 4;
        if (swimDrop_ < 5 || (o.flags8 & 0x800000)) swimDrop_ = 0;
        swimPh_ = swimRamp_ = 0x400;
        p.flags14 = (p.flags14 & 0xfffffdf6) | 0x404;
        if (p.rider >= 0) dropCarried();  // 0x43a880: the carried actor is let go where it is
    }
    if (swimDrop_ == 0) {
        const int32_t base = (o.flags8 & 0x100000) ? 0x50 : 0x78, top = o.box[4], rad = o.radius;
        // Exe crash path: 0x43a880 divides by the box top and the radius unguarded, so a float actor whose top or radius
        // is 0 raises a divide error and the game dies (runtime: L01D1's geyser forced by a memory poke into contact in
        // its idle state 2, RUNTIME_REFERENCE §13; unpoked play never got there). bbtime takes a zero term instead.
        const int32_t k = (top ? (dy << 12) / top : 0) + (rad ? (r << 12) / rad : 0);
        int16_t vy = int16_t((-(rsin(swimRamp_) * (base - ((k * base) >> 12)))) >> 12);
        if (swimRamp_ < 0x400) swimRamp_ += 0x40;
        else vy = int16_t(vy + int16_t((rsin(swimPh_) * swimAmp_) >> 12));
        if (top + 10 < dy + vy) w.y += vy;
        swimPh_ += 100;
        if (swimPh_ > 0x1000) {
            swimAmp_ = (rand_() & 7) + 9;
            swimPh_ -= 0x1000;
        }
    } else if (swimRamp_ < 0x800) {
        w.y += (rsin(swimRamp_) * swimDrop_) >> 12;
        swimRamp_ += 0x40;
    } else {
        swimDrop_ = swimRamp_ = 0;
    }
}

// ---- player 0x43a880 ----

void LitWorld::playerTick() {
    const uint32_t prevFlags = p.flags14;  // 0x4b3c24
    p.flags14 &= 0xfff9bfff;
    if (!G[0x0f] && ((cam.flags & 0x100) || (prevFlags & 0x80000000))) {  // a demo (G[0x0f]) keeps steering
        vsX_ = vsZ_ = momX_ = momZ_ = 0;
        accelK_ = 0x800;
        if (!(cam.flags & 0x200)) pad.pressed &= 0x800;
    }
    if (!p.initialised) initPlayer();

    // 1. Resolve state and move (§3.1 step 1).
    const PlayerState* st = findState(p.state);
    int16_t moveId = st && p.seq <= 4 ? st->moves[p.seq] : int16_t(-16);
    if (!st || moveId == -16 || p.seq > 4 || !findMove(uint16_t(moveId))) {
        st = findState(1);
        p.flags14 &= 0xfdffffdf;
        p.state = 1;
        p.seq = 0;
        moveId = st->moves[0];
        p.anim = 0;
        if (p.rope >= 0 && (p.flags14 & 2)) {  // parent flags8 4 (a rope) and grounded: let go, yaw = local yaw
            p.yaw = p.localYaw;
            dropRope();
        }
    }
    const PlayerMove* mv = findMove(uint16_t(moveId));
    p.flags18 = (mv->flagsB & 0x1000) ? p.flags18 | 2 : p.flags18 & ~2u;
    p.flags18 = (mv->flagsB & 0x2000) ? p.flags18 | 4 : p.flags18 & ~4u;

    // 2. Steering (§3.2). Local velocity s = (sx, sy, sz-forward).
    int16_t lx = 0, ly = 0, lz = 0;
    const bool frozen = (p.flags14 & 0x80000000) && !G[0x0f];
    if (!frozen) {
        if (!(mv->flagsA & 2)) {
            if (mv->flagsA & 0x100000) lz = int16_t(lz + mv->speed * kDt);  // constant forward
            const int16_t s = int16_t(p.lean >> 2);
            p.lean = s == 0 ? 0 : int16_t(-s);
        } else {
            if ((p.flags14 & 0x209) == 0)  // camera yaw refreshed only while grounded
                camYawRef_ = int16_t(ratan2(cam.target.x - cam.eye.x, cam.target.z - cam.eye.z));
            if (pad.sx != 0 || pad.sy != 0) {
                const int32_t mag = isqrt(pad.sx * pad.sx + pad.sy * pad.sy);
                int16_t pw = int16_t((mag - kDeadZone) * int16_t(0x80 / kDeadZone));  // p = (mag - 50)·2
                if (pw > 0x80) pw = 0x80;
                if (mv->flagsB & 0x4000000) p.seq = uint16_t(pw > 0x77);  // run at p >= 0x78
                if (mv->flagsB & 0x8000000) {  // animation pace from the stick
                    int32_t a = pw;
                    if ((mv->flagsB & 0x4000000) && a < 0x78) a = a * 0x1111 >> 12;
                    paceAcc_ += a;
                    p.frameSkip = uint8_t(5 - paceAcc_ / 32);
                    paceAcc_ -= paceAcc_ / 32 * 32;
                }
                int16_t v = int16_t((mv->speed * pw * kDt) >> 7);  // walk 40 / run 50 (§3.7)
                if (v == 0) v = 1;
                if (p.rope < 0 && p.platform < 0 && p.carrier < 0) {
                    int16_t t = int16_t(ratan2(pad.sx, pad.sy));
                    if (!(mv->flagsA & 0x8000)) t = int16_t(t + camYawRef_);
                    if (t > 0x800) t = int16_t(t - 0x1000);
                    if (t < -0x800) t = int16_t(t + 0x1000);
                    if (p.yaw > 0x800) p.yaw = int16_t(p.yaw - 0x1000);
                    if (p.yaw < -0x800) p.yaw = int16_t(p.yaw + 0x1000);
                    turnErr_ = int16_t(t - p.yaw);
                    if (turnErr_ > 0x800) turnErr_ = int16_t(turnErr_ - 0x1000);
                    if (turnErr_ < -0x800) turnErr_ = int16_t(turnErr_ + 0x1000);
                    p.lean = int16_t(turnErr_ >> 3);
                    const int16_t ad = turnErr_ < 0 ? int16_t(-turnErr_) : turnErr_;
                    lz = int16_t(lz + int16_t((int32_t(int16_t(0x800 - ad)) * v) >> 11));  // forward·(0x800-|Δ|)>>11
                } else {  // attached (rope / platform / carrier): the chain heading, always camera-relative, no lean
                    p.yaw = attachedHeading(1000);
                    int32_t d = p.yaw - wrapSigned(ratan2(pad.sx, pad.sy) + camYawRef_);
                    if (d > 0x800) d -= 0x1000;
                    if (d < -0x800) d += 0x1000;
                    turnErr_ = int16_t(-d);
                    lz = int16_t(lz + int16_t((int32_t(v) * (0x800 - std::abs(d))) >> 11));
                }
            }
            if (mv->flagsA & 0x100000) lz = int16_t(lz + mv->speed * kDt);
        }
        if (turnErr_ != -1) {  // turn Δ>>2 per tick, final snap
            int16_t s = turnErr_;
            const int16_t q = int16_t(turnErr_ >> 2);
            if (q == 0) turnErr_ = -1;
            else { turnErr_ = int16_t(turnErr_ - q); s = q; }
            // *(+0xc0) + 0x12: the local yaw while attached
            int16_t& yaw = p.rope >= 0 || p.platform >= 0 || p.carrier >= 0 ? p.localYaw : p.yaw;
            yaw = int16_t(yaw + s);
        }
    }
    if (mv->flagsB & 0x10) {  // face camera (attached: the local yaw takes the difference)
        const int16_t t = int16_t(ratan2(cam.eye.x - p.pos.x, cam.eye.z - p.pos.z));
        if (p.rope < 0 && p.platform < 0 && p.carrier < 0) {
            p.yaw = t;
        } else {
            int32_t d = p.yaw - wrapSigned(t);
            if (d > 0x800) d -= 0x1000;
            if (d < -0x800) d += 0x1000;
            p.localYaw = int16_t(p.localYaw - d);
        }
    }
    if (!(p.flags14 & 2)) p.lean = 0;
    // Variable jump height (§3.3): ✕ released in 0x600 < ph < 0x800 -> apex now.
    if ((mv->flagsA & 0x10) && !(mv->flagsB & 0x40000000) && !(pad.held & PadCross) && !(p.flags14 & 8) &&
        p.phase > 0x600 && p.phase < 0x800)
        p.phase = 0x800;
    if ((mv->flagsA & 0xc00) && (pad.sx != 0 || pad.sy != 0)) {  // climbing: stick -> vertical
        int16_t m = int16_t(std::min(isqrt(pad.sx * pad.sx + pad.sy * pad.sy), 0x7f));
        if (mv->flagsA & 0x400) m = int16_t(-m);
        ly = int16_t(ly + int16_t((mv->speed * m * kDt) >> 7));
    }
    // Climbing on the rope (move flagsA 0x200): local x/z reset, 0x43a7b0 limits (top: no climbing up; past the
    // bottom: let go, yaw = local yaw, next move).
    if ((mv->flagsA & 0x200) && p.rope >= 0) {
        p.local.x = p.local.z = 0;
        const int lim = ropeClimb(int32_t(uint16_t(lz)));
        if (lim == 2 && (mv->flagsA & 0x400)) ly = 0;
        if (lim == 1) {
            p.yaw = p.localYaw;
            dropRope();
            ++p.seq;
        }
    }
    V3 w = rotY(p.yaw, lx, ly, lz);

    // 3. Slide surfaces and acceleration (§3.2).
    if (!frozen && (p.flags14 & 2)) {
        V3 q{p.pos.x + w.x, p.pos.y + w.y, p.pos.z + w.z};
        Ground g;
        g.zone = p.zone;
        hf.groundAt(q, g);
        if (!(g.kind & KindSlide)) {
            p.flags14 &= ~0x10000000u;
        } else {
            if (!(p.flags14 & 0x10000000)) { vsX_ = w.x >> 2; vsZ_ = w.z >> 2; p.flags14 |= 0x10000000; }
            int32_t slX = 0, slZ = 0;
            const int16_t y16 = int16_t(p.pos.y);
            auto probe = [&](int32_t ox, int32_t oz, int32_t& acc, int sign) {
                V3 pt{p.pos.x + ox, p.pos.y, p.pos.z + oz};
                Ground r;
                r.zone = -1;
                hf.groundAt(pt, r);
                if ((r.kind & 0xc) == 0 && (r.kind & KindSlide)) {
                    const int32_t dh = r.groundY - p.pos.y;
                    if (-0x65 < dh && dh < 200) acc += (sign * (r.groundY - int32_t(y16)) * 3000) >> 12;
                }
            };
            probe(0xa0, 0, slX, 1);
            probe(-0xa0, 0, slX, -1);
            probe(0, 0xa0, slZ, 1);
            probe(0, -0xa0, slZ, -1);
            int16_t d = int16_t((slX - momX_) >> 4);
            momX_ = d != 0 ? momX_ + d : slX;
            d = int16_t((slZ - momZ_) >> 4);
            momZ_ = d != 0 ? momZ_ + d : slZ;
        }
    }
    if ((p.flags14 & 0x201) == 0) {
        if (!(p.flags14 & 0x10000000)) {
            int16_t d = int16_t(momX_ >> 2);
            momX_ = d == 0 ? 0 : momX_ - d;  // momentum ×0.75
            d = int16_t(momZ_ >> 2);
            momZ_ = d == 0 ? 0 : momZ_ - d;
            d = int16_t(((w.x - vsX_) * accelK_) >> 12);  // ½ error per tick
            vsX_ = (d != 0 || accelK_ != 0x800) ? vsX_ + d : w.x;
            d = int16_t(((w.z - vsZ_) * accelK_) >> 12);
            vsZ_ = (d != 0 || accelK_ != 0x800) ? vsZ_ + d : w.z;
        } else {  // ice: 1 unit per tick toward the target
            if (int16_t(w.x) != int16_t(vsX_)) vsX_ += int16_t(int16_t(w.x) - int16_t(vsX_)) < 0 ? -1 : 1;
            else vsX_ = w.x;
            if (int16_t(w.z) != int16_t(vsZ_)) vsZ_ += int16_t(int16_t(w.z) - int16_t(vsZ_)) < 0 ? -1 : 1;
            else vsZ_ = w.z;
        }
        w.x = vsX_ + momX_;
        w.z = vsZ_ + momZ_;
    } else {  // airborne: no smoothing, horizontal velocity carried into the momentum term
        momX_ = w.x;
        momZ_ = w.z;
    }
    accelK_ = accelK_ < 0x800 ? accelK_ + 0x10 : 0x800;

    // 4. Jump start and arc (§3.3).
    if ((mv->flagsA & 0x10) && ((p.flags14 & 0x209) == 0 || (p.flags14 & 0x400))) {
        p.jumpH = mv->jumpH;
        p.jumpStep = mv->jumpStep;
        p.phase = 0x400;
        p.vy = 0;
        p.flags14 = (p.flags14 & 0xeffff9f1) | 1;
        if (p.rope >= 0 || p.platform >= 0) {  // jumping off: world yaw = local yaw + support yaw (+0x19e + its +0xde)
            p.yaw = int16_t(p.localYaw + actors[size_t(p.rope >= 0 ? p.rope : p.platform)].yaw);
            if (p.rope >= 0) dropRope();  // a platform is let go by playerContacts (rising)
        }
    }
    if ((p.flags14 & 1) && (!(p.flags14 & 0x200) || (p.flags14 & 0x400)) && !frozen) arcTick(p);
    p.flags14 = (mv->flagsA & 0x810000) ? p.flags14 | 0x40 : p.flags14 & ~0x40u;  // attack active
    playerAttacks(*mv);
    // Put down (move flagsA 0x40) / throw (flagsB 0x40000) the carried actor once the move has a hand point: at the
    // hand height, its x/z offset pushed against the solids / mask-0x100 actors (0x433500 mask 0x20100) and traced
    // against the walls from his position (0x434cd0), his heading, flags14 0x1000 put down / 0x80000 thrown; his box
    // is the fixed one again.
    if (((mv->flagsA & 0x40) || (mv->flagsB & 0x40000)) && p.handSet && p.rider >= 0) {
        LitActor& c = actors[size_t(p.rider)];
        c.flags14 = (c.flags14 & 0xfffff7f9) | ((mv->flagsB & 0x40000) ? 0x80000u : 0x1000u);
        V3 d{int16_t(p.hand.x - p.pos.x), int16_t(p.hand.y - p.pos.y), int16_t(p.hand.z - p.pos.z)};
        contactPush(p, kParentBugs, playerInflate_, p.animState.radius, p.animState.box, 0x20100, d);
        V3 at = p.pos;
        int32_t dx = int16_t(d.x), dz = int16_t(d.z);
        Ground g;
        g.zone = p.zone;
        hf.trace(at, dx, dz, g);
        c.pos = {p.pos.x + dx, p.hand.y, p.pos.z + dz};
        c.zone = p.zone;
        c.yaw = p.yaw;
        c.parent = -1;
        c.parentPart = -1;
        p.rider = -1;
        auto& b = p.animState.box;
        b[0] = -70, b[2] = -40, b[3] = 70, b[5] = 40;
        p.animState.radius = 80;
    }
    // Pumping the swing: climbing move + R1 adds 4 to a chain's amplitude (+0x144, before its own update).
    if ((mv->flagsA & 0x200) && (pad.pressed & PadR1) && p.rope >= 0 && !(actors[size_t(p.rope)].flags8 & 0x400000))
        actors[size_t(p.rope)].ropeAmp += 4;
    // Rope grab (move flagsA 0x100, falling / descending / gliding, no support): attached at his local height,
    // local rotation 0, world position from the rope's current matrix, move cleared, flags14 4.
    if ((mv->flagsA & 0x100) && (p.flags14 & 0x10208) && p.rope < 0 && p.platform < 0) {
        int32_t ly0 = 0;
        if (const int r0 = ropeGrab(ly0); r0 >= 0) {
            p.rope = r0;
            p.local = {0, ly0, 0};
            p.localYaw = 0;
            p.pos = ropeWorld();
            w = {};
            p.flags14 = (p.flags14 & 0xfffffdf6) | 4;
        }
    }
    V3 wMove = w;  // psVar6 + 0x14
    if (p.rope >= 0 || p.platform >= 0) {  // attached: world yaw from the chain, the step to the chain's origin clamped by the trace
        const V3 o = p.rope >= 0 ? ropeWorld() : platformWorld();
        int32_t dx = o.x - p.pos.x, dz = o.z - p.pos.z;
        const int16_t dx0 = int16_t(dx), dz0 = int16_t(dz);
        Ground g;
        g.zone = p.zone;
        V3 a = p.pos;
        hf.trace(a, dx, dz, g);
        w.x += dx - dx0;
        w.z += dz - dz0;
        p.yaw = attachedHeading(0x1000);
    }
    // Carrying a big actor (flags8 0x2000, or a type-34 held item) outside a put-down / throw move: the x and z extremes
    // of his box corners turned by his rotation (0x408cf0..0x408d20), each plus the move, are traced from 100 above
    // him; what a wall takes off comes off the move. The exe's min / max loop overwrites the last two corners' z with
    // its running min / max (only the first two corners' z and 0 count), kept here.
    if (p.rider >= 0) {
        const LitActor& c = actors[size_t(p.rider)];
        if (((c.flags8 & 0x2000) || c.type == 0x22) && !(mv->flagsA & 0x40) && !(mv->flagsB & 0x40000)) {
            const auto& b = p.animState.box;
            std::array<int16_t, 12> q{b[0], 0, b[5], b[3], 0, b[5], b[0], 0, b[2], b[3], 0, b[2]};  // 0x4b3be0
            Mat m;
            rotMatrixXYZ(m, 0, p.yaw, p.lean);
            for (size_t k = 0; k < 12; k += 3) {
                const V3 v = apply16(m, {q[k], q[k + 1], q[k + 2]});
                q[k] = int16_t(v.x), q[k + 1] = int16_t(v.y), q[k + 2] = int16_t(v.z);
            }
            q[11] = 0;
            int32_t minX = 0, maxX = 0;
            int16_t minZ = 0;
            for (size_t k = 0; k < 12; k += 3) {
                const int16_t x = q[k], z = q[k + 2];
                if (minX <= x) maxX = std::max<int32_t>(maxX, x);
                else minX = x;
                q[8] = z;
                if (minZ <= z) {
                    q[8] = minZ;
                    if (q[11] < z) q[11] = z;
                }
                minZ = q[8];
            }
            const int16_t ext[4][2] = {{int16_t(minX + int16_t(w.x)), 0}, {int16_t(maxX + int16_t(w.x)), 0},
                                       {0, int16_t(q[8] + int16_t(w.z))}, {0, int16_t(q[11] + int16_t(w.z))}};
            for (const auto& e : ext) {
                V3 at{p.pos.x, p.pos.y - 100, p.pos.z};
                int32_t dx = e[0], dz = e[1];
                Ground g;
                g.zone = p.zone;
                hf.trace(at, dx, dz, g);
                w.x += dx - e[0], w.z += dz - e[1];
                wMove.x += dx - e[0], wMove.z += dz - e[1];
            }
        }
    }

    // 5. Collision move (§4.4 reimplementation notes): X-only, Z-only, then MoveActor with the clamped delta.
    const int32_t sum0 = w.z + w.x;
    Ground r;
    {
        V3 a = p.pos;
        int32_t dx = w.x, dz = 0;
        r.zone = p.zone;
        hf.trace(a, dx, dz, r);
        const int16_t cx = int16_t(dx);
        a = p.pos;
        dx = 0;
        dz = w.z;
        r.zone = p.zone;
        hf.trace(a, dx, dz, r);
        w.x = cx;
        w.z = dz;
    }
    if ((p.flags14 & 0x81000000) == 0 || G[0x0f]) moveActor(p, w, r, boxTop());
    if (sum0 != w.z + w.x) p.flags14 |= 0x100000;  // blocked by a wall this tick
    if (mv->flagsA & 0x200) {  // climbing moves keep the pre-trace move; blocked or out of every zone: let go, fall
        w = wMove;
        // Let go also when carrying while attached to a platform / carrier (+0x6c set, +0x70 any).
        const bool loose = p.rope >= 0 || (p.rider >= 0 && (p.platform >= 0 || p.carrier >= 0));
        if (loose && ((p.flags14 & 0x100000) || r.zone == -1)) {
            if (p.carrier >= 0 && actors[size_t(p.carrier)].rider == -2) actors[size_t(p.carrier)].rider = -1;
            p.rope = p.platform = p.carrier = -1;
            p.carrierPart = -1;
            p.flags14 = (p.flags14 & 0xffffffe9) | 0x200;
        }
    }
    const int ci = playerContacts(w, mv->flagsA, lz);  // 0x433500 mask 0x9a690a: solids, platforms, pushables, pickups
    swim(ci, w);
    if (mv->flagsA & 0x200) {  // climbing: only the local height moves (unattached: his own coordinate, no move)
        if (p.rope >= 0) {
            p.local.y += w.y;
            p.pos = ropeWorld();
        }
    } else if (p.rope >= 0 || p.platform >= 0) {  // attached / riding, other moves: the move in the support's yaw frame
        Mat inv;
        rotMatrixXYZ(inv, 0, -actors[size_t(p.rope >= 0 ? p.rope : p.platform)].yaw, 0);
        const V3 d = applyLV(inv, w);
        p.local = {p.local.x + d.x, p.local.y + d.y, p.local.z + d.z};
        p.pos = p.rope >= 0 ? ropeWorld() : platformWorld();
    } else {
        p.pos.x += w.x;
        p.pos.y += w.y;
        p.pos.z += w.z;
    }
    if (p.platform >= 0) {  // 0x434230 after the move: off the platform (or on the ground) -> his own coordinate again
        LitActor& pl = actors[size_t(p.platform)];
        if (overPlatform(pl, p, {0, p.yaw, p.lean}, p.animState.box) && !(p.flags14 & 2)) {
            pl.flags14 |= 0x8000;  // touched by Bugs (its items' gate14 0x8000)
        } else {
            // 0x434cd0 from the platform's x/z at his height, in his zone: the step off slides along the walls.
            V3 q{pl.pos.x, p.pos.y, pl.pos.z};
            int32_t dx = p.pos.x - pl.pos.x, dz = p.pos.z - pl.pos.z;
            Ground g;
            g.zone = p.zone;
            hf.trace(q, dx, dz, g);
            p.pos.x = pl.pos.x + dx;
            p.pos.z = pl.pos.z + dz;
            p.platform = -1;
            p.lean = 0;
            p.flags14 = (p.flags14 & 0xfffffdeb) | 2;
        }
    }
    // No contact and no move: 0x433380 (mask 0x2802, sensors excluded) at (0, −50, 100) in his frame -> 0x4b3c00,
    // flags14 0x40000, its 0x8000 (also the actor he carries; runtime: L01a crate 234 held at rest, counters 227-237).
    if (ci < 0 && w.x == 0 && w.y == 0 && w.z == 0) {
        Mat m;
        rotMatrixXYZ(m, 0, p.yaw, p.lean);
        const V3 o = apply(m, {0, -50, 100});
        if (const int k = pointHit({p.pos.x + o.x, p.pos.y + o.y, p.pos.z + o.z}, 0x2802, 0x2000000, kParentBugs); k >= 0) {
            pickup_ = k;
            p.flags14 |= 0x40000;
            actors[size_t(k)].flags14 |= 0x8000;
        }
    }
    if (!(prevFlags & 0x200) && (prevFlags & 0x12) && (p.flags14 & 0x200)) p.flags18 |= 0x200;  // walked off a ledge

    // 6. Transitions (§3.1 step 3): the first passing rule of the run of rules of group move.link (0x43e2fd: held
    // actor mask +0x1c on the carried actor's flags8, parent mask +0x20 on +0x70's flags8 — with 0x8000 also a
    // type-14 parent whose step has flagsB 0x10000000, or none and Bugs' flags18 0x40 —, flags14 / flags18 masks,
    // buttons, condition). Then, carrying, the rule's +0x0c command goes to the carried actor: 0xd3 frees it, else
    // it starts state +0x0c (+0x17e; seq and clip restart). With no rule passing that is the rule that ended the run
    // (the next group's first), as in the exe. A state change clears his hand point (+0x68).
    if (mv->link != 0) {
        const auto first = std::ranges::find(rules, mv->link, &PlayerRule::group);
        int16_t next = kStay;
        auto ru = first;
        const int support = p.carrier >= 0 ? p.carrier : p.rope >= 0 ? p.rope : p.platform;  // +0x70
        for (; ru != rules.end() && ru->group == mv->link; ++ru) {
            if (ru->heldMask && !(p.rider >= 0 && (actors[size_t(p.rider)].flags8 & ru->heldMask))) continue;
            if (ru->parentMask && !(support >= 0 && (actors[size_t(support)].flags8 & ru->parentMask))) {
                if (!(ru->parentMask & 0x8000)) continue;
                const LitActor* s = support >= 0 ? &actors[size_t(support)] : nullptr;
                if (!s || s->type != 14 || s->step < 0) {
                    if (!(p.flags18 & 0x40)) continue;
                } else if (!(defs[size_t(s->def)].steps[size_t(s->step)].flagsB & 0x10000000)) {
                    continue;
                }
            }
            if (ru->flags14Mask && (((p.flags14 & ru->flags14Mask) != 0) == bool(ru->invert & 2))) continue;
            if (ru->flags18Mask && (((p.flags18 & ru->flags18Mask) != 0) == bool(ru->invert & 1))) continue;
            if (ru->any && !(pad.pressed & ru->any)) continue;
            if (ru->all != (pad.pressed & ru->all) && ru->all != kNoButtons) continue;
            if (!cond(ru->item, nullptr)) continue;
            act(ru->item, nullptr);
            next = ru->next;
            break;
        }
        if (first != rules.end() && ru != rules.end() && ru->heldCmd && p.rider >= 0) {
            LitActor& c = actors[size_t(p.rider)];
            if (ru->heldCmd == 0xd3) {
                c.flags14 |= 0x100;
                c.parent = -1;
                p.rider = -1;
            } else {
                const auto& sts = defs[size_t(c.def)].states;
                if (c.type != 0x22) c.p17e = int16_t(ru->heldCmd);
                else if (const auto s = std::ranges::find(sts, uint16_t(ru->heldCmd), &PlayerState::id); s != sts.end())
                    c.state = int16_t(s - sts.begin());
                c.anim = 0;
                c.seq = 0;
            }
        }
        if (next != kStay) {
            p.seq = 0;
            if ((p.state == 5 || p.state == 0x12) && (p.flags14 & 0x200)) {  // coyote jump
                p.flags14 &= ~0x200u;
                next = 7;
            }
            const PlayerState* ns = findState(uint16_t(next));
            if (!ns || !findMove(uint16_t(ns->moves[0]))) {
                unsupported.insert(std::format("state {}", next));
                ns = findState(1);
            }
            p.state = ns->id;
            mv = findMove(uint16_t(ns->moves[0]));
            p.anim = 0;
            p.handSet = false;
            if (mv->flagsB & 0x800) p.flags14 &= 0xfffffdf6;
        }
    }
    // Pick-up (move flagsA 0x20, nothing carried, a hand point): the last contact touched outside a pick-up move
    // (0x4b3c00) when it is a pickable (flags8 0x2802) other than his +0x70; a big one (0x2000) is looked for again
    // with 0x433380 (mask 0x2802, sensors excluded) at (0, −50, 100), then (∓100, −50, 100) in his frame. It hangs at
    // his hand part (local identity), flags14 0x800, its old carrier's +0x6c cleared.
    if (p.rider < 0 && (mv->flagsA & 0x20) && p.handSet && pickup_ >= 0 && !actors[size_t(pickup_)].dead &&
        (actors[size_t(pickup_)].flags8 & 0x2802)) {
        const int support = p.carrier >= 0 ? p.carrier : p.rope >= 0 ? p.rope : p.platform;
        int k = pickup_ != support ? pickup_ : -1;
        if (k >= 0 && (actors[size_t(k)].flags8 & 0x2000)) {
            Mat m;
            rotMatrixXYZ(m, 0, p.yaw, p.lean);
            k = -1;
            for (const int16_t x : {0, -100, 100}) {
                const V3 o = apply(m, {x, -50, 100});
                if ((k = pointHit({p.pos.x + o.x, p.pos.y + o.y, p.pos.z + o.z}, 0x2802, 0x2000000, kParentBugs)) >= 0) break;
            }
        }
        if (k >= 0) {  // the exe links it (+0x70, +0xc4 = his +0x68, identity local) without placing it: its own
            LitActor& c = actors[size_t(k)];  // update puts it at the hand (runtime: L01a crate 234 at counter 227)
            if (c.parent >= 0 && actors[size_t(c.parent)].rider == k) actors[size_t(c.parent)].rider = -1, c.parent = -1;
            pickup_ = -1;
            p.rider = k;
            c.flags14 = (c.flags14 & 0xfff7efed) | 0x800;
            c.parent = kParentBugs;
            c.parentPart = attachPart(kParentBugs, true);
            c.local = {};
            c.localYaw = c.matYaw = 0;
            c.rot[0] = c.rot[2] = 0;
        }
    }
    p.flags14 = (mv->flagsA & 0x2000) ? (p.flags14 & 0xfffffdf6) | 0x10000 : p.flags14 & ~0x10000u;  // glide

    // 7. Animation (§3.1 step 4, §6).
    if (p.anim == mv->anim) {
        if (!(mv->mode & 0x8000) || lz != 0) {
            const uint32_t res = model_ ? todTick(*model_, p.animState, 0, 0, playerFx_, nullptr) : AnimMissing;
            if (res == AnimFinished) {
                if (p.state == 1) p.seq = uint16_t(rand_() & 3);  // random idle fidget
                if (level == 3) playSound(nullptr, 0x4a, 2);       // LS01 (0x43ea74)
                ++p.seq;
                p.anim = 0;
                if (p.flags14 & 0x1000000) respawn();
            } else if ((mv->flagsA & 0x80) && (p.flags14 & 0x12)) {  // advance on landing
                if (level == 3) playSound(nullptr, 0x4a, 2);         // 0x43eb31
                ++p.seq;
                p.anim = 0;
            }
        }
    } else {
        if (model_) todTick(*model_, p.animState, mv->anim, uint32_t(uint16_t(mv->mode)), playerFx_, nullptr);
        p.anim = mv->anim;
    }
    if (model_ && p.animState.attachNew) {  // packet 10 -> +0x58 (0x44e400), as for actors (lit_actors animate)
        p.animState.attachNew = false;
        p.handSet = false;
        if (p.animState.attachPart >= 0)
            if (const auto o = partOriginQ12(p.animState, size_t(p.animState.attachPart), p.pos, {0, p.yaw, p.lean}, {4096, 4096, 4096}))
                p.hand = *o, p.handSet = true;
    }
    bugsLoopSounds(mv->id);  // 0x43eb57
    // 0x43a880 tail: unless the move keeps the animated box (flagsB 0x2000), Bugs' box x/z and radius are fixed;
    // only the packet-9 heights follow the animation (runtime: L01A fight capture, +0x48..+0x54 = ±70 / ±40, +0x2e 80).
    // Carrying a big actor (flags8 0x2000) outside a put-down move his z1 reaches past it (its z1 + inflation + 90,
    // radius √(z1² + 4900) in s16, at least √0x1964); with flagsB 0x2000 a carried actor lends him its box.
    const LitActor* held = p.rider >= 0 ? &actors[size_t(p.rider)] : nullptr;
    auto& b = p.animState.box;
    if (!(mv->flagsB & 0x2000)) {
        b[0] = -70, b[2] = -40, b[3] = 70, b[5] = 40;
        p.animState.radius = 80;
        if (held && (held->flags8 & 0x2000) && !(mv->flagsA & 0x40)) {
            b[5] = int16_t(held->inflate + held->box[5] + 0x5a);
            const int16_t r2 = std::max<int16_t>(int16_t(b[5] * b[5] + 0x1324), 0x1964);
            p.animState.radius = uint16_t(isqrt(r2));
        }
    } else if (held) {
        b = held->box;
        p.animState.boxSet = true;
        p.animState.radius = held->radius;
    }

    // 8. End of tick: inside a wall -> restore the last safe position (§4.3).
    {
        V3 q = p.pos;
        Ground g;
        g.zone = p.zone;
        hf.groundAt(q, g);
        if (!(g.kind & KindBlocked)) {
            safe_ = p.pos;
        } else {
            V3 z{};
            p.pos = safe_;
            moveActor(p, z, g, boxTop());
            safe_.y = p.pos.y + z.y;
            p.pos.y = safe_.y;
        }
    }
    p.flags14 &= 0x7fedefff;
    p.flags18 &= 0xfffffdbe;
    // Not falling while supported (0x16) or attached (+0x70: carrier, platform, rope; runtime: L03C1 carried flags14 0).
    if ((p.flags14 & 0x16) || p.carrier >= 0 || p.platform >= 0 || p.rope >= 0) p.flags14 &= ~0x200u;
    if (invuln_ != 0) {
        if (invuln_ < 0x1e) p.flags14 &= 0xfdffffdf;
        --invuln_;
    }
    if (!(p.flags14 & 0x1000000)) zoneTriggers(p, nullptr);
    p.lastMove = w.x | w.y | w.z;
}

// ---- camera 0x4318f0 (normal follow; look mode, directors, scripted modes and shake not ported) ----

void LitWorld::cameraSnap(const V3& at) {  // 0x4373f0
    const V3 off = rotY(p.yaw, 0, 0, -100);
    cam.target = cam.targetCopy = {at.x, at.y - 400, at.z};
    cam.eye = cam.eyeCopy = cam.cand = cam.candCopy = {at.x + off.x, at.y - 400, at.z + off.z};
    cam.recentreRequest = 0;
    cam.cachedYaw = int16_t(p.yaw + 0x800);
    cam.flags |= 0x40;
    cam.yaw = cam.cachedYaw;
}

void LitWorld::cameraReset() {  // 0x431160 (camera reset, also A77)
    const V3 eye = cam.eye, target = cam.target;
    cameraSnap(p.pos);
    if (target.x != 0) cam.target = cam.targetCopy = target;
    if (eye.x != 0) cam.eye = cam.eyeCopy = cam.cand = cam.candCopy = eye;
    cam.lastPlayer = p.pos;
    cam.flags = 0x40;
    cam.recentreRequest = 0;
    cam.heightOffset = -200;  // §5 level defaults -200 / 600 / 1100
    cam.eyeHeight = 600;
    cam.distance = 0x44c;
    cam.recentreTimer = 0;
    cam.cachedDist = 0x44c;
    cam.cachedYaw = 0;
    focus_ = -1;
    G[0x36] = 0;  // the cut-scene lock (runtime: L02A1 left mid-intro with GD+0x76 = 31 -> 0 at L02A2's start)
}

// Draw pass 0x44c4b0 (full frames; skipped frames run 0x40a530, which bbtime never does): flags &= ~0x300, then for
// each drawn actor (+4 & 2, not type 7) a part that is not a mesh (flags & 6 clear, 0x11 only if hidden 0x8000) with flags
// 0x40 and no 0x20, on an actor whose box is not all 0, is a camera node: the actor's flags18 |= 0x1000, 0x200 if its
// flagsC has 0x100, and 0x44c070 sets the eye to the part's world origin, the look point to the eye plus its matrix
// applied to (0, 0, 1000), flags |= 0x100, the yaw 0x4b3e1a / 0x4b3e2c = atan2(eye − look), the distance 0x4b3da8 =
// that x/z length + 1; flagsC 0x10000000 also seeds the candidate and the unshaken copies. Runtime: the title camera
// actor (0, 146, 0) holds eye (1, 146, 0), look (1, 146, 1000), flags 0x342 (RUNTIME_REFERENCE.md §14).
// bbtime does not gate on its `visible` (+4 & 2 stand-in): CCINTRO's camera actor (zone 3, act3c 0x61) is zone-culled
// in bbtime from the first ticks on (Bugs falls from the origin, the follow camera leaves zone 3's view), so the node
// never held the view (0.12.0: the intro showed a field from below). Runtime (/b56): flags 0x342, Bugs at (0, 0, 0)
// and the eye on the node's path throughout; why the exe's first ticks keep the node is not traced (Inferred).
void LitWorld::cameraNodes() {
    cam.flags &= ~0x300u;
    if (!scene_) return;
    for (const uint32_t k : order_) {
        LitActor& a = actors[k];
        if (!a.active || a.dead || a.type == 7 || a.def < 0 || defs[size_t(a.def)].model < 0) continue;
        if (std::ranges::all_of(a.box, [](int16_t v) { return v == 0; })) continue;
        const ActorModel& m = scene_->actorModels[size_t(defs[size_t(a.def)].model)];
        const std::vector<TodPart>& parts = a.animState.parts;
        for (size_t i = 0; i < parts.size(); ++i) {
            const uint16_t f = parts[i].flags;
            if (parts[i].dead || (f & 6) || ((f & 0x11) && !(f & 0x8000)) || (f & 0x60) != 0x40) continue;
            a.flags18 |= 0x1000;
            if (a.flagsC & 0x100) cam.flags |= 0x200;
            const std::array<int32_t, 3> at{a.pos.x, a.pos.y, a.pos.z};
            const std::array<int16_t, 3> rot{a.rot[0], a.yaw, a.rot[2]};
            const auto eye = partOrigin(m, a.animState, i, at, rot, a.scale, false);
            const auto look = partOrigin(m, a.animState, i, at, rot, a.scale, false, {0, 0, 1000});
            cam.eye = {(*eye)[0], (*eye)[1], (*eye)[2]};
            cam.target = {(*look)[0], (*look)[1], (*look)[2]};
            cam.flags |= 0x100;
            const int32_t dx = cam.eye.x - cam.target.x, dz = cam.eye.z - cam.target.z;
            cam.yaw = cam.cachedYaw = int16_t(ratan2(dx, dz));
            cam.cachedDist = isqrt(dx * dx + dz * dz) + 1;
            if (a.flagsC & 0x10000000) cam.cand = cam.eyeCopy = cam.candCopy = cam.eye, cam.targetCopy = cam.target;
        }
    }
}

// 0x4318f0 is called with the camera target actor 0x4b39b8: Bugs, or for one tick the actor that ran A85 (0x42d840;
// 0x447d40 resets the target to Bugs after the call). Its zone is written to Bugs' +0x26; every exit past the target
// checks runs the shake 0x4312c0.
void LitWorld::cameraTick() {
    const int ti = camTarget_;
    camTarget_ = -1;
    Mover& T = ti >= 0 && size_t(ti) < actors.size() && !actors[size_t(ti)].dead ? static_cast<Mover&>(actors[size_t(ti)]) : p;
    const Zone* pz = hf.findZone(T.pos, T.zone);
    if (pz) p.zone = int16_t(pz->id);
    // Camera pseudo-actor zone (0x4b39c8): the (previous tick's) eye, else the target's record; kept when neither.
    const Zone* cz = hf.findZone(cam.eye, -1);
    if (!cz) cz = pz;
    if (cz) camZone = int16_t(cz->id);
    if (!p.initialised) return;
    cameraFollow(T, ti >= 0 && &T != &p && actors[size_t(ti)].type == 14);
    cameraShake();
}

// A59 / A68 shake 0x4312c0: the look-at point moves by sin(phase)·amp >> 11 in x and cos(phase)·amp >> 12 in y; the
// s16 phase advances by the speed and the amplitude (also the duration) decays by 3 per tick while the phase is at
// or above 0x800 (s16 compare: after the 16-bit wrap it climbs back through the negatives first).
void LitWorld::cameraShake() {
    if (shakeAmp_ == 0) return;
    cam.target.x += int16_t(rsin(shakePhase_) * shakeAmp_ >> 11);
    cam.target.y += int16_t(rcos(shakePhase_) * shakeAmp_ >> 12);
    shakePhase_ = int16_t(shakePhase_ + shakeSpeed_);
    if (shakePhase_ < 0x800) return;
    shakeAmp_ = shakeAmp_ < 4 ? 0 : int16_t(shakeAmp_ - 3);
}

void LitWorld::cameraFollow(Mover& target, bool type14) {
    bool moved = p.lastMove != 0 || type14;  // param_2 = Bugs' +0x1b4; a type-14 target counts as moving
    if (cam.lastPlayer != target.pos) moved = true;
    cam.lastPlayer = target.pos;
    if ((cam.flags & 0x40) && !(cam.flags & 0x300)) {
        if (cam.recentreRequest < 10) { ++cam.recentreRequest; moved = true; }
        else cam.flags &= ~0x40u;
    }
    if (camHold_ > 0) {  // A41 hold 0x4b3dac: camera frozen, the target frozen (flags14 0x80000000, cleared each tick)
        --camHold_;
        target.flags14 |= 0x80000000;
        focus_ = -1;
        return;
    }
    if (focus_ >= 0 && actors[size_t(focus_)].dead) focus_ = -1;
    LitActor* F = focus_ >= 0 ? &actors[size_t(focus_)] : nullptr;
    // A focus actor whose step has flagsB 0x200 becomes the followed target (frozen, flags14 0x80000000).
    const bool follow = F && F->step >= 0 && (defs[size_t(F->def)].steps[size_t(F->step)].flagsB & 0x200);
    if (follow) F->flags14 |= 0x80000000;
    Mover& T = follow ? static_cast<Mover&>(*F) : target;
    if (cam.flags & 0x300) {  // a camera node (0x40a1f0) owns the view: the target frozen, no follow (0x4318f0)
        T.flags14 |= 0x80000000;
        cam.flags |= 0x40;
        cam.recentreRequest = 0;
        focus_ = -1;
        return;
    }
    int32_t dist = (T.flags14 & 0x1000000) ? 500 : cam.distance;
    if (cam.flags & 0xc00) {  // scripted points (type-36 directors, 0x4318f0 0xc00 branch): no follow this tick
        cam.flags &= ~0x10u;
        const V3 P = T.pos;
        const V3 to = (cam.flags & 0x800) ? cam.lookPoint : V3{P.x, P.y - 400, P.z};
        cam.target.x -= (cam.target.x - to.x) >> 2;
        cam.target.y -= (cam.target.y - to.y) >> 2;
        cam.target.z -= (cam.target.z - to.z) >> 2;
        if (cam.flags & 0x1000) {  // eye point kept at fixedDist from the target (s16 deltas as the exe)
            const int32_t dx = int16_t(cam.point.x - cam.target.x), dy = int16_t(cam.point.y - cam.target.y),
                          dz = int16_t(cam.point.z - cam.target.z);
            const int32_t len = std::max(isqrt(dy * dy + dz * dz + dx * dx), 1);
            cam.point.x = (((dx << 12) / len) * cam.fixedDist >> 12) + cam.target.x;
            cam.point.y = (((dy << 12) / len) * cam.fixedDist >> 12) + cam.target.y;
            cam.point.z = (((dz << 12) / len) * cam.fixedDist >> 12) + cam.target.z;
        }
        if (cam.flags & 0x400) {
            cam.eye.x -= (cam.eye.x - cam.point.x) >> 2;
            cam.eye.y -= (cam.eye.y - cam.point.y) >> 2;
            cam.eye.z -= (cam.eye.z - cam.point.z) >> 2;
        }
        if (cam.flags & 0x4000) {
            cam.cand = cam.eyeCopy = cam.candCopy = cam.eye;
            cam.targetCopy = cam.target;
        }
        if (cam.flags & 8) cam.flags &= 0xffffa3f7;
        const int32_t dx = int16_t(cam.eye.x - cam.target.x), dz = int16_t(cam.eye.z - cam.target.z);
        cam.yaw = cam.cachedYaw = int16_t(ratan2(dx, dz));
        cam.cachedDist = isqrt(dz * dz + dx * dx) + 1;
        cam.flags |= 0x40;
        cam.recentreTimer = 0x3c;
        cam.recentreRequest = 0;
        return;
    }
    // Manual rotation (§5 step 4): L2 +0x26, R2 -0x26 per tick; holds the yaw (flags |= 0x82). With a focus actor
    // (0x4b3950) instead: the yaw turns 1/16 of the way to the focus -> target direction a tick, distance >= 1500.
    const uint32_t f = cam.flags & ~0x10u;
    cam.flags = f & 0xffffa3e7;
    cam.lastStep = 0;
    if (!(pad.pressed & PadL1) && !(T.flags14 & 0x1000000)) {
        const bool l2 = pad.pressed & PadL2, r2 = pad.pressed & PadR2;
        if (F) {
            cam.flags |= 2;
            cam.yaw = int16_t(cam.yaw + (angleDiff(ratan2(T.pos.x - F->pos.x, T.pos.z - F->pos.z), cam.yaw) >> 4));
            dist = std::max(cam.distance, 0x5dc);
            focus_ = -1;
        } else if (l2 && !r2 && !(f & 0x8000)) { cam.flags |= 0x82; cam.yaw = int16_t(cam.yaw + 0x26); cam.lastStep = 0x26; }
        else if (r2 && !l2 && !(f & 0x8000)) { cam.flags |= 0x82; cam.yaw = int16_t(cam.yaw - 0x26); cam.lastStep = -0x26; }
        else if (pad.camStep && !l2 && !r2 && !(f & 0x8000)) {  // Enhanced right stick (pad.h): the L2 / R2 path
            cam.flags |= 0x82;
            cam.yaw = int16_t(cam.yaw + pad.camStep);
            cam.lastStep = pad.camStep;
        } else if (moved) cam.flags = f & 0xffffa3e5;
    }
    cam.yaw = int16_t(wrapSigned(cam.yaw));
    cam.cand = cam.candCopy;
    cam.eye = cam.eyeCopy;
    cam.target = cam.targetCopy;

    const V3 P = T.pos;
    auto rel = [&](const V3& c, int32_t& r, int16_t& a, bool fallback) {
        const int32_t dx = int16_t(int16_t(c.x) - int16_t(P.x)), dz = int16_t(int16_t(c.z) - int16_t(P.z));
        const int32_t r2 = dz * dz + dx * dx;
        if (fallback && r2 == 0) { r = 100; a = int16_t(T.yaw - 0x800); }
        else { r = isqrt(r2) + 1; a = int16_t(ratan2(dx, dz)); }
    };
    int32_t r;
    int16_t a;
    rel(cam.cand, r, a, true);
    if (moved) cam.recentreTimer = 0x3c;
    if (cam.recentreTimer > 0) --cam.recentreTimer;
    // Auto-recentre (§5 step 6): toward player.yaw + 0x800 by diff·30 >> 11.
    if (!(cam.flags & 2) && (cam.recentreTimer == 0 || moved)) {
        const int32_t step = angleDiff(T.yaw + 0x800, a) * 0x1e >> 0xb;
        if (step == 0) cam.flags |= 2;
        else a = int16_t(a + step);
    }
    if (!(cam.flags & 2)) cam.yaw = a;
    else a = cam.yaw;

    V3 off = rotY(a, 0, 0, r);
    int32_t probeY = P.y - 500;
    if (!(T.flags14 & 0x16)) {  // airborne: probe from just under the ceiling of the zone above
        V3 q = P;
        Ground g;
        g.zone = -1;
        hf.groundAt(q, g);
        const Zone* z0 = g.rec;
        q = {P.x, P.y - 500, P.z};
        hf.groundAt(q, g);
        if (!(g.rec == z0 || (g.kind & 6) == 0 || g.zone == -1)) probeY = std::min(g.rec->yBottom + 500, P.y);
    }
    Ground t;
    auto trace = [&](V3 s, int32_t& ox, int32_t& oz) {
        t = Ground{};
        hf.trace(s, ox, oz, t);
    };
    int32_t ox = off.x, oz = off.z;
    trace({P.x, probeY, P.z}, ox, oz);
    cam.cand.x = P.x + ox;
    cam.cand.z = P.z + oz;
    r = isqrt(ox * ox + oz * oz) | 1;
    a = int16_t(ratan2(ox, oz));
    if (!(cam.flags & 2)) {  // 0x431770: swing the yaw out of walls with up to 25 rotated probes
        const V3 s{P.x, cam.eye.y, P.z};
        std::array<int16_t, 24> dists{};
        int found = 25;
        for (int i = 0; i < 25; ++i) {
            const V3 v = rotY(kYawProbe[i] + a, 0, 0, r);
            int32_t px = v.x, pz = v.z;
            trace(s, px, pz);
            const int16_t d = int16_t(isqrt(px * px + pz * pz));
            if (i < 24) dists[size_t(i)] = d;
            else cam.recentreRequest = d;  // the exe's 25th store lands on 0x4b3df0 (faithful overrun)
            if (!(t.kind & KindBlocked) && v.z + v.x == px + pz) { found = i; break; }
        }
        int16_t pick = kYawProbe[found];
        if (found > 0x17) {  // exe quirk: the best index survives only if it is the last new maximum
            int16_t best = 0, idx = 0;
            for (int i = 0; i < 24; ++i) {
                idx = 0;
                if (best < dists[size_t(i)]) { idx = int16_t(i); best = dists[size_t(i)]; }
            }
            pick = kYawProbe[idx];
        }
        a = int16_t(a + (pick >> 2));
    }
    // Distance band (§5 step 7): toward 1100 ± 100, >> 3 when closer, >> 1 when farther.
    if (r < dist - 100) r += (dist - r - 100) >> 3;
    if (dist + 100 < r) r += (dist - r + 100) >> 1;
    off = rotY(a, 0, 0, r);
    ox = off.x;
    oz = off.z;
    trace({P.x, probeY, P.z}, ox, oz);
    const int32_t L = isqrt(ox * ox + oz * oz);
    const V3 step = rotY(a, 0, 0, 200);
    int32_t sx = 0, sz = 0;
    for (int32_t i = L / 200; i >= 0; --i) {  // 0x4313a0 actor boxes in 200-unit steps: no camera solids
        const V3 s = i == 0 ? rotY(a, 0, 0, L % 200) : step;
        sx += s.x;
        sz += s.z;
    }
    cam.cand.x = P.x + sx;
    cam.cand.z = P.z + sz;
    // Eight ±400 wall probes around the candidate push it away from walls.
    int32_t ax = 0, az = 0;
    for (const auto& d : kCamTable) {
        int32_t px = (d[0] >> 1) + (d[0] >> 1), pz = (d[1] >> 1) + (d[1] >> 1);
        trace({cam.cand.x, probeY, cam.cand.z}, px, pz);
        ax += px;
        az += pz;
    }
    if (!moved && (az != 0 || ax != 0)) {
        cam.flags |= 2;
        if (500 < isqrt(ax * ax + az * az)) cam.yaw = int16_t(cam.yaw - cam.lastStep);
    }
    cam.cand.x += ax >> 2;
    cam.cand.z += az >> 2;
    rel(cam.cand, r, a, false);
    if (!moved && (cam.flags & 0xc0) == 0) { r = cam.cachedDist; a = cam.cachedYaw; }
    else { cam.cachedDist = r; cam.cachedYaw = a; }
    off = rotY(a, 0, 0, r);
    cam.cand.x = P.x + off.x;
    cam.cand.z = P.z + off.z;
    const V3 far = rotY(a, 0, 0, r + 400);
    // Target follows the player >> 2 (y = player + heightOffset); eye 400 beyond the candidate, >> 2.
    cam.target.x += (P.x - cam.target.x) >> 2;
    cam.target.y += (cam.heightOffset - cam.target.y + P.y) >> 2;
    cam.target.z += (P.z - cam.target.z) >> 2;
    cam.eye.x += (far.x - cam.eye.x + P.x) >> 2;
    cam.eye.z += (far.z - cam.eye.z + P.z) >> 2;
    // Eye height: pulled >> 3 into ±100 of min(ground, player.y) - 600.
    {
        V3 q{cam.cand.x, probeY, cam.cand.z};
        Ground g;
        g.zone = -1;
        hf.groundAt(q, g);
        if ((g.kind & KindOpen) && g.groundY < P.y) {
            g.zone = -1;
            q.y += 1;
            hf.groundAt(q, g);
        }
        const int32_t gy = (!(g.kind & KindBlocked) && g.zone != -1 ? std::min(g.groundY, P.y) : P.y) - cam.eyeHeight;
        if (gy + 100 < cam.eye.y) cam.eye.y += (gy - cam.eye.y + 100) >> 3;
        else if (cam.eye.y < gy - 100) cam.eye.y += (gy - cam.eye.y - 100) >> 3;
    }
    cam.targetCopy = cam.target;
    cam.eyeCopy = cam.eye;
    cam.cand.y = cam.eye.y;
    // Ceiling: 600-unit probe above the eye.
    {
        V3 q = cam.eye;
        Ground g;
        g.zone = -1;
        hf.groundAt(q, g);
        const int32_t gy = g.groundY;
        const Zone* z0 = g.rec;
        q.y = cam.eye.y - 600;
        g.zone = -1;
        hf.groundAt(q, g);
        if (g.rec != z0 && (g.kind & 6) && g.zone != -1) {
            const int32_t yb = g.rec->yBottom;
            cam.eye.y += (yb < gy - 0x262 ? yb - cam.eye.y + 600 : gy - 10 - cam.eye.y) >> 3;
            cam.cand.y = cam.eyeCopy.y = cam.eye.y;
        }
    }
    cam.candCopy = cam.cand;
    cam.flags &= ~0x80u;
}

// ---- shadows: 0x44c4b0 registration, 0x44bb50 draw, 0x44ba00 position, 0x44b4f0 scale / tilt ----

std::vector<LitWorld::Shadow> LitWorld::shadows() const {
    std::vector<Shadow> out;
    if (!scene_) return out;
    // at / rot: the drawn root (0x4375d0 walks the coordinate chain, so a carried Bugs' shadow follows his carrier's
    // part of this tick like his model); the y clamp, zone and tilt probes use the stored position +0xe4.
    auto one = [&](uint16_t entity, const Mover& m, const V3& at, const ActorModel* model, const AnimState& st,
                   std::array<int16_t, 3> rot, std::array<int32_t, 3> scale, const std::array<int16_t, 6>& box,
                   const LitActor* support, int32_t owner) {
        if (!entity || !model || st.parts.empty() || out.size() >= 40 || !scene_->templateModels.contains(entity)) return;
        // 0x44ba00: one part -> the actor position, else the origin of the second part of the list; skipped when that
        // part is not drawn (+0xb4).
        const auto o = partOrigin(*model, st, st.parts.size() > 1 ? 1 : 0, {at.x, at.y, at.z}, rot, scale);
        if (!o) return;
        Shadow s{entity, st.parts.size() > 1 ? V3{(*o)[0], (*o)[1], (*o)[2]} : at, {0, rot[1], 0}, 0x1000,
                 &m == &p ? p.shadowBias : int16_t(0), owner};
        if (support && ((support->flags8 & 8) || (support->flagsC & 0x2000))) {  // riding: at the feet, platform tilt
            s.pos.y = m.pos.y;
            s.rot = {support->rot[0], support->yaw, support->rot[2]};
            out.push_back(s);
            return;
        }
        Ground g;
        g.zone = m.zone;
        hf.groundAt(s.pos, g);  // overwrites y with the zone bottom
        while ((g.kind & KindOpen) && g.zone != -1) {  // open floor: drop into the zone below
            ++s.pos.y;
            hf.groundAt(s.pos, g);
        }
        s.pos.y += g.hoff;
        if (s.pos.y > shadowMaxY) s.pos.y = shadowMaxY;
        if (!(cam.flags & 0x100) && s.pos.y < m.pos.y) s.pos.y = m.pos.y;
        // 0x44b4f0 first pass (0x44bb50 mask 8): the first collision-list actor (0x4b39ac order) that is a platform
        // (flags8 8) or flagsC 0x2000, not flags8 1, class 2, not the owner, whose top (+0x52, + 40; + 80 from 0x47 on)
        // lies between the owner's feet and the shadow, with the owner inside its box ± inflation in its frame
        // (Rz·Ry·Rx of 0x1000 − rot): the shadow goes onto its top plane (its RotMatrix·Scale applied to (0, top, 0) /
        // (0, 0x1000, 0) / (0, scale y, 0)), scale (feet − y)·4 + 0x1000 ≥ 0x800, its rotation.
        for (const size_t k : collide_) {
            const LitActor& t = actors[k];
            const int32_t top = t.box[4], lim = t.pos.y + top + (top < 0x47 ? 0x28 : 0x50);
            if (m.pos.y > lim || lim > s.pos.y || !((t.flags8 & 8) || (t.flagsC & 0x2000)) || (t.flags8 & 1) || t.def < 0 ||
                defs[size_t(t.def)].cls != 2 || static_cast<const Mover*>(&t) == &m)
                continue;
            Mat inv;
            rotMatrixZYX(inv, 0x1000 - t.rot[0], 0x1000 - t.yaw, 0x1000 - t.rot[2]);
            const V3 d{int16_t(m.pos.x - t.pos.x), int16_t(m.pos.y - t.pos.y), int16_t(m.pos.z - t.pos.z)};
            const V3 l = apply16(inv, d);
            const int32_t inf = t.inflate;
            if (l.x < t.box[0] - inf || l.x > t.box[3] + inf || l.z < t.box[2] - inf || l.z > t.box[5] + inf) continue;
            Mat M;
            rotMatrixXYZ(M, t.rot[0], t.yaw, t.rot[2]);
            for (size_t j = 0; j < 9; ++j) M.m[j] = int16_t((M.m[j] * t.scale[j % 3]) >> 12);  // ScaleMatrix 0x408060
            const V3 q = apply16(M, {0, top, 0}), n = apply16(M, {0, 0x1000, 0}), sy = apply16(M, {0, int16_t(t.scale[1]), 0});
            const int32_t dot = q.y * n.y + q.z * n.z + n.x * q.x;
            s.pos.y = (dot - d.x * n.x - d.z * n.z) / int32_t(int32_t(int16_t(sy.y)) | 1) + t.pos.y;
            s.scale = std::max((m.pos.y - s.pos.y) * 4 + 0x1000, 0x800);
            s.rot = {t.rot[0], t.yaw, t.rot[2]};
            out.push_back(s);
            return;
        }
        // Second pass: ground at three box corners of the actor position, each kept within ±0x22 of the shadow height.
        const int32_t ref = s.pos.y;
        Ground h;
        h.zone = m.zone;
        auto probe = [&](int32_t dx, int32_t dz) {
            V3 q{m.pos.x + dx, m.pos.y, m.pos.z + dz};
            hf.groundAt(q, h);
            if (h.kind & KindBlocked) return ref;
            const int32_t y = q.y + h.hoff;
            return y >= ref + 0x23 || y <= ref - 0x23 ? ref : y;
        };
        const V3 a{box[0], probe(box[0], box[2]), box[2]}, b{box[3], probe(box[3], box[2]), box[2]},
            c{box[0], probe(box[0], box[5]), box[5]};
        s.scale = ref < m.pos.y ? 0x1000 : std::max((m.pos.y - ref) * 4 + 0x1000, 0x800);
        if (a.y != b.y || a.y != c.y) {  // tilt to the plane of the three probes (yaw dropped)
            const V3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
            const int32_t nx = u.y * v.z - u.z * v.y, ny = v.x * u.z - u.x * v.z, nz = u.x * v.y - v.x * u.y;
            s.rot = {int16_t(ratan2(nz, ny)), 0, int16_t(ratan2(nx, ny))};
        }
        out.push_back(s);
    };
    // Bugs first (the registration list follows the draw list), skipped in first person (camera flags 0x20).
    if (!(cam.flags & 0x20)) {
        const auto [at, yaw] = playerDraw();
        one(playerShadow_, p, at, model_, p.animState, {0, yaw, p.lean}, {4096, 4096, 4096}, p.animState.box,
            p.platform >= 0 ? &actors[size_t(p.platform)] : nullptr, -1);
    }
    for (const LitActor& a : actors) {
        if (!a.active || !a.visible || a.dead || a.type == 7) continue;
        const ActorDef& d = defs[size_t(a.def)];
        const bool tod = a.animState.box != std::array<int16_t, 6>{};  // TOD packet 9 overrides the tag box
        one(d.shadow, a, a.pos, d.model >= 0 ? &scene_->actorModels[size_t(d.model)] : nullptr, a.animState,
            {a.rot[0], a.yaw, a.rot[2]}, a.scale, tod ? a.animState.box : a.box, a.parent >= 0 ? &actors[size_t(a.parent)] : nullptr,
            int32_t(&a - actors.data()));
    }
    return out;
}

} // namespace bb::game
