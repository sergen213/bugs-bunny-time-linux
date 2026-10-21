#include "bb/game/tb_world.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>

namespace bb::game {
namespace {

uint16_t le16(std::span<const uint8_t> b, size_t p) { return p + 2 <= b.size() ? uint16_t(b[p] | b[p + 1] << 8) : 0; }
uint32_t le32(std::span<const uint8_t> b, size_t p) { return le16(b, p) | uint32_t(le16(b, p + 2)) << 16; }
int16_t s16(std::span<const uint8_t> b, size_t p) { return int16_t(le16(b, p)); }

int32_t wrap(int32_t a) {  // angle into (-0x800, 0x800]
    a &= 0xfff;
    return a > 0x800 ? a - 0x1000 : a;
}
int32_t sgn(int64_t v) { return (v > 0) - (v < 0); }
// fN(x) of the camera (§4.3): |x|*N < 4096 ? x*|x|*N >> 12 : x.
int32_t fN(int32_t x, int32_t n) { return std::abs(x) * n < 4096 ? int32_t((int64_t(x) * std::abs(x) * n) >> 12) : x; }
int32_t len3(int64_t x, int64_t y, int64_t z) { return int32_t(std::lround(std::sqrt(double(x * x + y * y + z * z)))); }

bool isCond(uint8_t t) {
    switch (t) {
    case 0x4f: case 0x50: case 0x51: case 0x52: case 0x54: case 0x57: case 0x58: case 0x5a: case 0x5b: case 0x5c:
    case 0x5d: case 0x5e: case 0x60: return true;
    default: return false;
    }
}
uint32_t itemFlags(const TbLogicItem& it) { return it.tag == 0x5c || it.tag == 0x52 ? le16(it.d, 0) : le32(it.d, 0) & 0xffff; }

// Encoder 0x433900: a statement marker (cond flags 0x08 / 0x10 top level, 0x20 nested) gets a skip slot that the
// next marker fills: pending nested slots by any marker, pending top-level slots by top-level markers.
void linkSkips(TbBlock& b) {
    int32_t top = -1, sub = -1;
    for (size_t i = 0; i < b.size(); ++i) {
        b[i].skip = -1;
        if (!isCond(b[i].tag)) continue;
        const uint32_t f = itemFlags(b[i]);
        if (!(f & 0x38)) continue;
        if (sub >= 0) b[size_t(sub)].skip = int32_t(i);
        if (f & 0x18) {
            if (top >= 0) b[size_t(top)].skip = int32_t(i);
            sub = -1;
        }
        (f & 0x20 ? sub : top) = int32_t(i);
    }
}

TbStateRec parseState(std::span<const uint8_t> p) {
    return {le16(p, 0), le16(p, 2), le16(p, 4), le16(p, 8), le16(p, 0xc), le32(p, 0x10), le32(p, 0x14), le32(p, 0x18),
            s16(p, 0x1c), s16(p, 0x1e), s16(p, 0x20), s16(p, 0x22), s16(p, 0x24), s16(p, 0x26), s16(p, 0x28),
            s16(p, 0x2a), s16(p, 0x2c)};
}

// Logic blocks of one chunk in file order (encoder 0x433ab0 'M'): ids < 10000 go to `own`, others to `global`
// (list 0x4d5984, pushed at the head: the last definition wins).
void parseBlocks(const TbChunk& ch, std::map<uint16_t, TbBlock>* own, std::map<uint16_t, TbBlock>& global) {
    TbBlock* cur = nullptr;
    for (const TbTag& t : ch.tags) {
        if (t.id == 0x4d) {
            const uint16_t id = le16(t.data, 0);
            cur = id < 10000 ? (own ? &((*own)[id] = {}) : nullptr) : &(global[id] = {});
        } else if (t.id >= 0x4e && t.id <= 0x60) {
            if (!cur) continue;
            TbLogicItem it;
            it.tag = t.id;
            std::copy_n(t.data.begin(), std::min(t.data.size(), it.d.size()), it.d.begin());
            cur->push_back(it);
        } else {
            cur = nullptr;
        }
    }
}

// +0x4a of 0x450440: distance of the farthest box corner from the origin.
uint16_t boxRadius(const std::array<int16_t, 6>& b) {
    auto m = [](int32_t lo, int32_t hi) { return std::max(lo * lo, hi * hi); };
    return uint16_t(isqrt(m(b[0], b[3]) + m(b[1], b[4]) + m(b[2], b[5])));
}

// Entity data (loader 0x4347c0 + template resolution 0x433620). chain = own chunk, then its template (tag 0x27 @2:
// the first instance with that id in list order, else the chunk-0x08 template), recursively. The lists +0x10c/+0x110/
// +0x114 are the own records followed by the template's: the first definition of an id wins; class 16 inherits
// blocks only. Every other field comes from the own chunk.
void loadEntity(TbActor& a, const std::vector<const TbChunk*>& chain, std::map<uint16_t, TbBlock>& global) {
    const TbChunk& own = *chain.front();
    if (const auto b = own.tag(0x13); !b.empty()) a.behaviour = b[0];
    for (const TbChunk* src : chain) {
        if (src == &own || a.behaviour != 16)
            for (const TbTag& t : src->tags) {
                if (t.id == 0x2f) {
                    const TbStateRec s = parseState(t.data);
                    if (std::ranges::find(a.states, s.id, &TbStateRec::id) == a.states.end()) a.states.push_back(s);
                } else if (t.id == 0x31) {
                    TbTransition tr{le16(t.data, 0), {le16(t.data, 2), le16(t.data, 4), le16(t.data, 6), le16(t.data, 8), le16(t.data, 10)}};
                    if (std::ranges::find(a.trans, tr.event, &TbTransition::event) == a.trans.end()) a.trans.push_back(tr);
                }
            }
        std::map<uint16_t, TbBlock> blocks;
        parseBlocks(*src, &blocks, global);
        for (const TbTag& t : src->tags)  // the list head = the first own 'M' with an entity id
            if (t.id == 0x4d && a.firstBlock < 0 && le16(t.data, 0) < 10000) a.firstBlock = le16(t.data, 0);
        for (auto& [bid, items] : blocks) a.blocks.try_emplace(bid, std::move(items));
    }
    for (auto& [bid, items] : a.blocks) linkSkips(items);
    for (const TbTag& t : own.tags) {
        const std::span<const uint8_t> d = t.data;
        switch (t.id) {
        case 0x10: a.spawn = a.pos = {s16(d, 0), s16(d, 4), s16(d, 8)}; break;
        case 0x11: a.pitch = s16(d, 0), a.spawnYaw = a.yaw = int16_t(wrap(s16(d, 2))), a.roll = s16(d, 4); break;
        case 0x12: a.scale = {s16(d, 0), s16(d, 4), s16(d, 8)}; break;
        case 0x16:
            a.flags0 = le32(d, 0), a.flags1 = le32(d, 4), a.flags10 = le32(d, 8);
            a.progBlock = le16(d, 12);
            a.progMask = {le32(d, 16), le32(d, 20), le32(d, 24)};
            break;
        case 0x1b: a.box[0] = s16(d, 0), a.box[1] = s16(d, 2), a.box[2] = s16(d, 4); break;
        case 0x1c: a.box[3] = s16(d, 0), a.box[4] = s16(d, 2), a.box[5] = s16(d, 4), a.radius = le16(d, 6); break;
        case 0x1d:
            if (a.behaviour == 0x0e || a.behaviour == 0x1e || a.behaviour == 5 || a.behaviour == 0) a.turnRate = s16(d, 0);
            break;
        case 0x1e: a.hp = s16(d, 0); break;
        case 0x37:  // +0x3e inflation of the box x/z (and of later 0x61 records)
            a.inflate = s16(d, 0), a.weight = s16(d, 2);
            a.box[3] = int16_t(a.box[3] + a.inflate), a.box[5] = int16_t(a.box[5] + a.inflate);
            a.box[0] = int16_t(a.box[0] - a.inflate), a.box[2] = int16_t(a.box[2] - a.inflate);
            a.radius = uint16_t(a.radius + a.inflate);
            break;
        case 0x39: a.view = le16(d, 0), a.range = le16(d, 2), a.actBlock = uint16_t(le32(d, 4)); break;
        case 0x65: a.gdBit = le16(d, 0); break;
        case 0x61:
            a.boxes.push_back({int16_t(s16(d, 2) - a.inflate), s16(d, 4), int16_t(s16(d, 6) - a.inflate),
                               int16_t(s16(d, 8) + a.inflate), s16(d, 10), int16_t(s16(d, 12) + a.inflate)});
            break;
        default: break;
        }
    }
    if (!a.boxes.empty()) a.box = a.boxes[0], a.radius = boxRadius(a.box);
}

constexpr uint32_t kNoEvent = 0x50000000;  // block ended true
constexpr uint32_t kFalse = 0x10000000;    // block ended false

// V action items that create entities (0x45ad70 non-0x80000000 branch, bits 0x810): template id p3.
void spawnTemplates(const TbBlock& b, std::set<uint16_t>& out) {
    for (const TbLogicItem& it : b)
        if (it.tag == 0x56 && !(le32(it.d, 0) & 0x80000000u) && (le32(it.d, 0) & 0x810)) out.insert(le16(it.d, 12));
}

bool isInstance(const TbChunk& ch) { return ch.type == 0x07 || ch.type == 0x0a || ch.type == 0x63; }

} // namespace

void tbPadTick(TbPad& p, const PadInput& in, uint16_t edgeMask) {
    p.prevHeld = p.held;
    p.prevRaw = p.raw;
    p.raw = in.buttons;
    p.held = in.buttons;  // remap preset 0 (identity)
    if (in.analog && !(in.buttons & 0xf000)) {
        auto axis = [](int raw) {
            const int d = raw - 0x80;
            if (std::abs(d) <= 49) return 0;  // dead zone 0x4d..0xb1
            return std::clamp((d - sgn(d) * 49) * 128 / 78, -127, 127);
        };
        p.sx = int8_t(axis(in.lx));
        p.sy = int8_t(-axis(in.ly));
        if (p.sx < 0) p.held |= PadLeft; else if (p.sx > 0) p.held |= PadRight;
        if (p.sy > 0) p.held |= PadUp; else if (p.sy < 0) p.held |= PadDown;
        p.raw = p.held;
    } else {
        p.sx = int8_t(in.buttons & PadLeft ? -0x7f : in.buttons & PadRight ? 0x7f : 0);
        p.sy = int8_t(in.buttons & PadUp ? 0x7f : in.buttons & PadDown ? -0x7f : 0);
    }
    p.pressed = p.held & ~(p.prevHeld & edgeMask);
}

std::string_view TbWorld::levelName(int index) {
    static constexpr std::string_view kNames[96] = {
        "TITLE", "LOADING", "LANGUAGE", "ZOO", "GRANNY", "RETRY", "AZCA01", "AZCASG01", "AZCASG02", "AZCASG03",
        "AZCASG04", "AZCASG05", "AZR01", "AZQ01", "AZQ02", "AZBOSS01", "AZBOSS02", "VICA01", "VICASG01", "VICASG02",
        "VICASG03", "VICASG04", "VICASG05", "VIR01", "VIQ01", "VIQ02", "VIBOSS01", "VIBOSS02", "ARCA01", "ARCA02",
        "ARCA03", "ARCA04", "ARCA05", "ARCA06", "ARCASG01", "ARCASG02", "ARCASG03", "ARCASG04", "ARCASG05", "ARR01",
        "ARQ01", "ARQ02", "ARBOSS01", "ARBOSS02", "FWCA01", "FWCASG01", "FWCASG02", "FWCASG03", "FWCASG04", "FWCASG05",
        "FWR01", "FWQ01", "FWQ02", "FWBOSS01", "FWBOSS02", "TRCA01", "TRCA02", "TRCASG01", "TRCASG02", "TRCASG03",
        "TRCASG04", "TRCASG05", "TRR01", "TRQ01", "TRQ02", "TRBOSS01", "TRBOSS02", "CCERACUT", "TEST01", "TEST02",
        "TEST03", "TEST04", "TEST05", "SCREEN1", "SCREEN2", "SCREEN3", "MKSCR1", "MKSCR2", "MKSCR3", "DEMO1",
        "DEMO2", "DEMO3", "DEMO4", "CCINTRO", "CCAZIN01", "CCAZBO02", "CCVIIN01", "CCVIBO02", "CCARIN01", "CCARBO02",
        "CCTRIN01", "CCTRBO02", "CCEND", "CREDITS", "MUSIC", "SPEECHES"};
    return index >= 0 && index < 96 ? kNames[index] : std::string_view();
}

TbWorld TbWorld::load(const BzeFile& bze, const Scene& scene, const std::array<uint8_t, 0x300>* gd) {
    TbWorld w;
    w.col = TbCollision::fromLevel(bze);
    const int i1 = bze.find(1);
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const TbScript script = parseTbScript(s1);
    std::map<uint16_t, const TbChunk*> templates, firstInstance;
    struct Asset { uint16_t role = 0; const TbChunk* ch = nullptr; std::span<const uint8_t> tod, model; };
    std::map<uint16_t, Asset> assets;
    // Sounds (level loader chunk 0, as LiT): 0x01 programs (section 4), 0x02 bank (0x429e50, section 3), 0x04 sound
    // (0x406ff0, 12 B), 0x4b / 0x4c speech programs / bank of the language section (file entry after section 3;
    // ponytail: language 0, English).
    const int i3 = bze.find(3), i4 = bze.find(4);
    std::vector<uint8_t> s3, s4, sp;
    if (i3 >= 0) s3 = bze.payload(size_t(i3));
    if (i4 >= 0) s4 = bze.payload(size_t(i4));
    if (i3 >= 0 && size_t(i3) + 1 < bze.sections.size()) sp = bze.payload(size_t(i3) + 1);
    auto part = [](std::span<const uint8_t> sec, std::span<const uint8_t> ref) {
        const uint32_t off = le32(ref, 0), size = le32(ref, 4);
        return size_t(off) + size <= sec.size() ? sec.subspan(off, size) : std::span<const uint8_t>{};
    };
    w.sfx.tb = true, w.sfx.volume = 127;
    for (const TbChunk& ch : script.chunks) {
        if (ch.type == 0x00)
            for (const TbTag& t : ch.tags) {
                if (t.id == 0x04) w.sfx.addEntry(t.data);
                if (t.id == 0x01) w.sfx.programs = Sfx::programTable(part(s4, t.data));
                if (t.id == 0x02) w.sfx.bank = Sfx::loadBank(part(s3, t.data), BzeVariant::TimeBusters);
                if (t.id == 0x4b) w.sfx.speechPrograms = Sfx::programTable(part(sp, t.data));
                if (t.id == 0x4c) w.sfx.speech = Sfx::loadBank(part(sp, t.data), BzeVariant::TimeBusters);
            }
        const auto id = ch.tag(0x27);
        if (ch.type == 0x08 && !id.empty()) templates.emplace(le16(id, 0), &ch);
        if (isInstance(ch) && !id.empty()) firstInstance.emplace(le16(id, 0), &ch);
        if (ch.type == 0x22 && !id.empty()) assets[le16(id, 2)] = {le16(id, 0), &ch, ch.tag(0x25), ch.tag(0x24)};
        if (const auto p = ch.tag(0x3c); ch.type == 0x3a && !p.empty()) w.edgeMask = le16(p, 12);  // §4.3
        for (const TbTag& t : ch.tags)  // §6: first action-126 setter = the level-start track (I/H)
            if (t.id == 0x53 && le16(t.data, 0) == 126 && !w.music) w.music = t.data[2];
        if (ch.type != 0x22 && ch.type != 0x08) parseBlocks(ch, nullptr, w.globalBlocks_);  // global blocks only
    }
    // 0x433620: template id -> the first living entity with that id (0x45a120, ids > 0x10), else the 0x08 template.
    auto chainOf = [&](const TbChunk& ch) {
        std::vector<const TbChunk*> chain{&ch};
        for (int guard = 0; guard < 16; ++guard) {
            const auto id = chain.back()->tag(0x27);
            const uint16_t t = id.empty() ? 0 : le16(id, 2);
            if (!t) break;
            auto i = firstInstance.find(t);
            const TbChunk* next = i != firstInstance.end() && t > 0x10 ? i->second : nullptr;
            if (!next)
                if (auto tt = templates.find(t); tt != templates.end()) next = tt->second;
            if (!next || std::ranges::find(chain, next) != chain.end()) break;
            chain.push_back(next);
        }
        return chain;
    };
    std::map<uint32_t, std::vector<uint8_t>> secs;
    auto region = [&](const TbChunk& ch, std::span<const uint8_t> ref) {
        auto [it, fresh] = secs.try_emplace(ch.section);
        if (fresh) {
            const int si = bze.find(ch.section);
            if (si < 0) throw FormatError(std::format("tb world: section {} absent", ch.section));
            it->second = bze.payload(size_t(si));
        }
        const uint32_t off = le32(ref, 0), size = le32(ref, 4);
        if (off > it->second.size() || it->second.size() - off < size) throw FormatError("tb world: asset outside its section");
        return std::span<const uint8_t>(it->second).subspan(off, size);
    };
    // Animation clock (and, for spawn templates, the drawable model) from the entity's own tag-0x0f asset links:
    // first link with a role wins (role 1 rig, 13 model, others anims), as addTbActors.
    std::map<std::pair<std::vector<uint16_t>, bool>, std::shared_ptr<const ActorModel>> models;
    auto modelOf = [&](const TbChunk& ch, bool mesh) -> std::shared_ptr<const ActorModel> {
        std::vector<uint16_t> links;
        for (const TbTag& tg : ch.tags)
            if (tg.id == 0x0f) links.push_back(le16(tg.data, 0));
        auto [it, fresh] = models.try_emplace({links, mesh});
        if (!fresh) return it->second;
        auto m = std::make_shared<ActorModel>();
        bool rig = false, body = false;
        for (uint16_t l : links) {
            auto as = assets.find(l);
            if (as == assets.end()) continue;
            const Asset& a = as->second;
            if (a.role == 13 && !a.model.empty() && mesh && !body) {
                ActorModel built = buildTbActorModel(scene, region(*a.ch, a.model));
                m->objects = std::move(built.objects), m->meshes = std::move(built.meshes), m->slotCount = built.slotCount;
                m->modelAsset = l, body = true;
            }
            if (a.tod.empty()) continue;
            if (a.role == 1) {
                if (!rig) m->skeleton = parseTbTod(region(*a.ch, a.tod)), m->skeletonAsset = l, rig = true;
            } else if (findAnim(*m, a.role) < 0) {
                m->anims.push_back({a.role, parseTbTod(region(*a.ch, a.tod))});
            }
        }
        return it->second = (rig || !m->anims.empty()) ? std::move(m) : nullptr;
    };
    // The asset 'M' blocks (encoder 0x433ab0 order) of each anim of a model: anim role -> the entity's first tag-0x0f
    // link with that role and a TOD (as the anims were built), along the template chain (instances often link none).
    auto addLogic = [&](const ActorModel& m, const std::vector<const TbChunk*>& chain) {
        for (const ActorAnim& an : m.anims)
            for (const TbTag& tg : chain | std::views::transform([](const TbChunk* c) -> const std::vector<TbTag>& {
                                       return c->tags;
                                   }) | std::views::join) {
                if (tg.id != 0x0f) continue;
                const auto as = assets.find(le16(tg.data, 0));
                if (as == assets.end() || as->second.role != an.id || as->second.tod.empty()) continue;
                std::vector<TbBlock> logic;
                bool open = false;
                for (const TbTag& t : as->second.ch->tags) {
                    if (t.id == 0x4d) logic.emplace_back(), open = true;
                    else if (open && t.id >= 0x4e && t.id <= 0x60) {
                        TbLogicItem li;
                        li.tag = t.id;
                        std::copy_n(t.data.begin(), std::min(t.data.size(), li.d.size()), li.d.begin());
                        logic.back().push_back(li);
                    } else open = false;
                }
                for (TbBlock& b : logic) linkSkips(b);
                if (!logic.empty()) w.animLogic_.try_emplace(&an.tod, std::move(logic));
                break;
            }
    };
    // Every instance chunk is an entity of the list 0x4d4fd0 in file order (the loader appends, 0x4609b0).
    bool found[2] = {};
    for (const TbChunk& ch : script.chunks) {
        if (!isInstance(ch)) continue;
        const auto id = ch.tag(0x27);
        const uint16_t eid = id.empty() ? 0 : le16(id, 0);
        const bool player = (eid == 2 || eid == 3) && !ch.tag(0x10).empty() && !found[eid - 2];
        TbActor& a = player ? w.actors[size_t(eid - 2)] : w.entities.emplace_back();
        if (player) found[eid - 2] = true;
        a.id = eid;
        a.uid = player ? uint32_t(eid - 2) : w.nextUid_++;
        a.listPos = w.nextListPos_++;
        a.chunk = ch.offset;
        loadEntity(a, chainOf(ch), w.globalBlocks_);
        for (const ActorInstance& ai : scene.actors)
            if (ai.scriptOffset == ch.offset && ai.model < scene.actorModels.size()) a.model = &scene.actorModels[ai.model];
        if (!a.model && (a.ownModel = modelOf(ch, true))) a.model = a.ownModel.get();
        if (a.model) addLogic(*a.model, chainOf(ch));
    }
    if (!found[0] || !found[1]) throw FormatError("tb world: Bugs (id 2) and Taz (id 3) entities required");
    for (auto& [bid, items] : w.globalBlocks_) linkSkips(items);
    // Spawnable templates (V 0x10/0x800 targets) reachable from any logic: prototype entity (0x460b00 copies it
    // whole) with its model, rig and anims.
    std::set<uint16_t> want{0x18, 0x1d}, done;  // 0x18: overlay text (S30 / R 0x22, 0x4680b0); 0x1d: pause menu (0x4014c0)
    for (TbActor* a : w.order())
        for (const auto& [bid, b] : a->blocks) spawnTemplates(b, want);
    for (const auto& [bid, b] : w.globalBlocks_) spawnTemplates(b, want);
    while (!want.empty()) {
        const uint16_t t = *want.begin();
        want.erase(want.begin());
        if (!done.insert(t).second) continue;
        auto tc = templates.find(t);
        if (tc == templates.end()) continue;
        TbActor proto;
        proto.id = t;
        loadEntity(proto, chainOf(*tc->second), w.globalBlocks_);
        proto.ownModel = modelOf(*tc->second, true);
        if (proto.ownModel) addLogic(*proto.ownModel, chainOf(*tc->second));
        for (const auto& [bid, b] : proto.blocks) spawnTemplates(b, want);
        w.templates_.emplace(t, std::move(proto));
    }
    // Overlay text block (chunk 0x48 tag 0x49/0x68, 7 x {off, size} into section 3; GD+0x93 language 0 = English)
    // copied to 0x4d5b88, and the HUD sheet frame sizes (0x462430).
    for (const TbChunk& ch : script.chunks) {
        if (ch.type != 0x48) continue;
        auto ref = ch.tag(0x49);
        if (ref.empty()) ref = ch.tag(0x68);
        if (ref.size() >= 8) {
            const auto block = region(ch, ref);
            w.textBlock_.assign(block.begin(), block.end());
        }
        break;
    }
    for (const int32_t t : scene.hudSheet)
        w.hudFrames_.push_back(t >= 0 ? std::array<int16_t, 2>{int16_t(scene.textures[size_t(t)].width), int16_t(scene.textures[size_t(t)].height)}
                                      : std::array<int16_t, 2>{1, 1});
    // End of load (0x4347c0): Bugs / Taz health from GAME_DATA +0xa5 / +0xa6 (new game: 6, lives 3).
    if (gd) w.GD = *gd;
    else w.GD[0xa0] = 3, w.GD[0xa5] = w.GD[0xa6] = 6, w.GD[0x99] = 1;  // new game (0x401ad0 with slot -1)
    // Pad records GD+8 / GD+0x30, +0x10..+0x27: button remap words (preset 0 = identity: ← → ↑ ↓ □ ○ △ ✕ L1 L2 R1
    // R2), part of every save (R: local/saves/tb); a save written without them would leave the original deaf.
    for (const size_t rec : {size_t(8), size_t(0x30)})
        for (size_t k = 0; k < 12; ++k) {
            static constexpr uint16_t kRemap[12] = {0x8000, 0x2000, 0x1000, 0x4000, 0x80, 0x20, 0x10, 0x40, 4, 1, 8, 2};
            w.GD[rec + 0x10 + 2 * k] = uint8_t(kRemap[k]), w.GD[rec + 0x11 + 2 * k] = uint8_t(kRemap[k] >> 8);
        }
    w.actors[0].hp = w.GD[0xa5], w.actors[1].hp = w.GD[0xa6];
    // §2.1: run speed S[0x66] (Bugs) / S[0x67] (Taz) = 25 in most level-logic setters (402/450, 150 setters);
    // preset in case the level's setter is never reached (M).
    w.S[0x66] = w.S[0x67] = 25;
    return w;
}

std::vector<TbActor*>& TbWorld::order() {
    if (listOwner_ == this) return list_;
    list_.clear();
    auto it = entities.begin();
    std::array<TbActor*, 2> pl{&actors[0], &actors[1]};
    if (pl[1]->listPos < pl[0]->listPos) std::swap(pl[0], pl[1]);
    size_t p = 0;
    for (; it != entities.end(); ++it) {
        while (p < 2 && pl[p]->listPos < it->listPos) list_.push_back(pl[p++]);
        list_.push_back(&*it);
    }
    while (p < 2) list_.push_back(pl[p++]);
    listOwner_ = this;
    return list_;
}

TbActor* TbWorld::byListPos(uint32_t pos) {
    for (TbActor* a : order())
        if (a->listPos == pos) return a;
    return nullptr;
}

void TbWorld::setTwoPlayer(bool on) {
    A4 = on ? uint8_t(A4 | 0x10) : uint8_t(A4 & ~0x12);
    cam.init = false;  // follow flags are chosen once at camera init (§4.3)
}

bool TbWorld::setState(int actor, uint16_t id) { return setState(actors[size_t(actor & 1)], id); }

bool TbWorld::setState(TbActor& a, uint16_t id) {
    for (size_t i = 0; i < a.states.size(); ++i)
        if (a.states[i].id == id) {
            a.state = int32_t(i);
            a.seq = 0;
            a.curAnim = 0;
            return true;
        }
    return false;
}

TbActor* TbWorld::entity(int32_t uid) {
    if (uid < 0) return nullptr;
    if (uid < 2) return &actors[size_t(uid)];
    for (TbActor* e : order())
        if (int32_t(e->uid) == uid) return e;
    return nullptr;
}

void TbWorld::tick(const PadInput& p1, const PadInput& p2) {
    // 0x42ae60 before the views update: requests of the previous tick start now, heard from last tick's camera.
    sfx.mix(cam.eye, cam.look, [&](int32_t e, V3& pos) {
        if (e <= 2) return true;  // Bugs / Taz (0x4d236c, 0x4d2368): fixed levels
        const TbActor* a = entity(e - 1);
        pos = a ? a->pos : V3{cam.look.x + 0x100000, cam.look.y, cam.look.z};  // freed entity: out of range
        return false;
    });
    effects.clear();
    glyphs.clear(), textBoxes.clear();  // 0x45ee90: the overlay sprite / box lists restart every tick
    tbPadTick(pads[0], p1, edgeMask);
    if (A4 & 0x10) tbPadTick(pads[1], p2, edgeMask);
    else pads[1] = {};  // the P2 report is read only in 2-player mode (GAMEPLAY.md §2.3)
    GD[0x94] = 3;       // 0x4615b0: both pad reads succeed (keyboard / SDL); the pause menu's "no controller" test
    dt = ticks == 0 ? 0 : 2;
    lastCallFriend = 0;
    camFocus = focus();  // 0x4011c0 latch before 0x45eee0: the camera follows the old focus this tick
    for (uint8_t& f : flash)  // 0x4011c0: hit flash counters run to 0x3c
        if ((f & 0xc0) && (f & 0x3c) < 0x3c) ++f;
    if (invuln > 0) --invuln;
    pauseTick();
    if (!S[9]) cameraTick();  // 0x45eee0: the camera 0x439700 stands still while paused
    viewList();
    switchTick();
    // Entity loop 0x460630 over the view list: list order (players at their file position), spawns appended and
    // ticked in the same pass; +0x82 += 1 after each tick unless +0x28 & 0x20; an entity flagged +0x20 & 0x2000
    // is not ticked and is destroyed right after its slot (0x45ecd0).
    // While paused (S[9]) only entities with flags1 0x40000000 tick (the pause menu, its texts); the others still
    // die (0x460630).
    for (size_t i = 0; i < order().size(); ++i) {
        TbActor& a = *order()[i];
        if (!a.active) continue;
        if (S[9] && !(a.flags1 & 0x40000000u)) continue;
        if (!(a.f20 & 0x2000)) {
            classTick(a);
            if (!(a.f28 & 0x20) || S[9]) ++a.stateTicks;
        }
    }
    // HUD: the class 6 entity 0x463170 (template 0x1f holder, spawned at the load: appended after the level's
    // entities; the oracle's counter slides in on the pickup tick, hud/slide.csv).
    hudTick();
    textFlush();  // 0x4681c0: deferred (priority) overlay texts
    if (ticks < 2) cameraNodes();  // load-mode frames: a model camera node places the view (RETRY)
    for (auto it = entities.begin(); it != entities.end();) {
        if (!(it->f20 & 0x2000)) { ++it; continue; }
        TbActor& d = *it;  // 0x45ecd0: dying program (mask 0x2000), detach, release children
        if (d.progMask[0] & 0x2000) runProgram(d, 0, 0, 0);
        const int32_t dead = int32_t(d.uid);
        forEachEntity([&](TbActor& c) {
            if (c.parent != dead) return;
            if (c.behaviour == 0x21) c.f20 |= 0x2000;
            c.f20 &= ~0x208u;
            c.f24 |= 0x80;
            detach(c);
            if (c.progMask[1] & 0x80) runProgram(c, 0, 0x80, 0);
        });
        if (lastSpawn == dead) lastSpawn = -1;
        sfx.releaseOwned(dead + 1);  // 0x407220
        it = entities.erase(it);
        listOwner_ = nullptr;
    }
    for (TbActor* p : {&actors[0], &actors[1]})  // 0x453de0 tail: health mirrors GD+0xa5/+0xa6
        GD[p->id == 2 ? 0xa5 : 0xa6] = uint8_t(p->hp);
    ++ticks;
    fadeTick();  // 0x464ef0, once per rendered tick
}

// ------------------------------------------------------------------------------------------------ pause
// 0x4014c0 head (before the world tick): Start (pad-0 raw 0x800; re-armed only once released after a resume, latch
// 0x4d02d0) pauses outside cinematics (GD+0x9f) and demos (GD+0x8a): S[9] = 1
// and the pause menu entity, template 0x1d (flags1 0x40000000; +0xc |= 0x400 for pad 0's press, else 0x800). Its
// logic sets S[9] = 2 to resume. Not ported: music / voice pause 0x429180 / 0x42b5c0 and PlayMusic(-1) on resume.
void TbWorld::pauseTick() {
    const bool start = pads[0].raw & PadStart;
    if (pauseLatch_) {
        if (!start) pauseLatch_ = false;
        return;
    }
    if (S[0]) return;
    if (S[9] == 2) {
        pauseLatch_ = true, S[9] = 0;
        return;
    }
    // (The camera-node hold 0x4d515c & 0x300 also blocks it, but every render 0x465130 clears those bits, so they are
    // only seen in load-mode frames; bbtime keeps the hold for RETRY's view and leaves it out here.)
    if (S[9] || restart || !start || GD[0x9f] || GD[0x8a]) return;
    S[9] = 1;
    if (TbActor* m = spawn(0x1d, {}, 0)) m->flags1 |= (pads[0].pressed & PadStart) ? 0x400 : 0x800;
}

// ------------------------------------------------------------------------------------------------ switching
void TbWorld::switchTick() {
    TbActor& f = actors[size_t(focus())];
    TbActor& p = actors[size_t(1 - focus())];
    if (!(A4 & 0x10)) {
        f.flags1 = (f.flags1 & ~0x800u) | 0x400;
        p.flags1 = (p.flags1 & ~0x400u) | 0x800;
    }
    airTicks = f.f20 & 0xa ? 0 : uint8_t(std::min(airTicks + 1, 3));
    if (cooldown < 45 && !S[9]) ++cooldown;
    auto r1Rule = [&](uint16_t held) {
        const bool r1 = held & PadR1;
        if (r1 && !(A4 & 8) && !(p.f20 & 0x2000000) && !(S[3] & 0xc) && !GD[0x9f]) {
            A4 |= 8;
            if (!S[9]) {  // paused: only latched
                A4 ^= 4;
                for (TbActor& a : actors) a.f28 |= 0x100;  // "switched" entity programs (not interpreted)
            }
        } else if (!r1) {
            A4 &= ~8;
        }
    };
    auto l1Rule = [&](uint16_t held) {
        if (!(held & PadL1)) A4 &= ~0x20;
        else if (!(A4 & 0x20)) A4 |= 0x20, callFriend(false);
    };
    if (!(A4 & 0x10)) {
        r1Rule(pads[0].held);
        l1Rule(pads[0].held);
    } else {
        A4 = uint8_t((A4 & ~1) | 2);
        if ((f.flags1 & 0x400) && f.id == 3) {
            std::swap(f.flags1, p.flags1);  // only the pad bits differ in practice
            A4 |= 8;
            A4 ^= 4;
        } else {
            r1Rule(padOf(f).held);
            l1Rule(padOf(p).held);
        }
    }
    for (TbActor& a : actors) a.f28 &= ~0x100u;
    S[3] &= 0xf5;
}

bool TbWorld::callFriend(bool force) {
    TbActor& f = actors[size_t(focus())];
    TbActor& p = actors[size_t(1 - focus())];
    lastCallFriend = -1;
    if (!force && (S[9] || GD[0x9f] || (S[3] & 3) || airTicks > 2 || (f.f24 & 0x40) || (p.f20 & 0x2000000) || cooldown < 45)) {
        if (!(S[3] & 0x10) && !S[9] && !GD[0x9f]) playSound(f, 0x5f);  // refusal, muted while paused / in a dialog
        return false;
    }
    // place() 0x44ecd0: from p = focus - (0,200,0), 8 rays of 500 around the camera yaw; a candidate spot c is
    // dropped to the floor below it (query c + (0,400,0)); 4 clearance rays of 60 run 150 above that floor point,
    // which becomes the result.
    const V3 o{f.pos.x, f.pos.y - 200, f.pos.z};
    const uint32_t q = (((p.flags10 & f.flags10) >> 24) & 0xf0) | (force ? 0x702u : 0x703u);
    const int16_t yaw = camYaw();
    int64_t best = 0;
    V3 result;
    for (int i = 0; i < 8; ++i) {
        const int32_t a = yaw + (i & 1 ? -1 : 1) * (2 * (i / 2) + 1) * 0x100;
        V3 d = rotY(a, 0, 0, 500);
        const auto h = col.query(o, d, 20, q);
        if (h) d = {h->p.x - o.x, h->p.y - o.y, h->p.z - o.z};
        auto scale08 = [](const V3& v) { return V3{v.x * 0xccd / 4096, v.y * 0xccd / 4096, v.z * 0xccd / 4096}; };
        const V3 q8 = scale08(d);
        const int64_t qd = h ? int64_t(q8.x) * q8.x + int64_t(q8.z) * q8.z : 200000;
        if (qd <= best) continue;
        // Runtime trace (ARCA01, local/wine/tbplay/D_callfriend.csv): the spot is 0.8 * (d shortened by 50) =
        // (326, 148) from Bugs for camYaw 490, i.e. the 50-unit cut comes before the 0.8 scale (§4.2 order corrected).
        const int32_t l = len3(d.x, d.y, d.z);
        if (l > 0) d = {int32_t(int64_t(d.x) * (l - 50) / l), int32_t(int64_t(d.y) * (l - 50) / l), int32_t(int64_t(d.z) * (l - 50) / l)};
        d = scale08(d);
        const V3 c{o.x + d.x, o.y + d.y, o.z + d.z};
        const auto fl = col.query(c, {0, 400, 0}, 20, q);
        if (!fl || fl->n[1] >= kTbFloorNy) continue;
        const V3 up{fl->p.x, fl->p.y - 150, fl->p.z};
        bool clear = true;
        for (int k = 0; k < 4 && clear; ++k) clear = !col.query(up, rotY(k * 0x400, 0, 0, 60), 20, q);
        if (clear) best = qd, result = fl->p;
    }
    if (best < 0x57e5) {
        playSound(f, 0x5f);
        return false;
    }
    detach(p);
    p.pos = result;
    p.yaw = lookYaw_;
    p.f20 = (p.f20 | 0x10000000) & ~0x4200u;
    p.f24 &= ~0x40u;
    p.attachPart = -1;  // +0x84 = 0
    if (p.progMask[0] & 0x10000000) runProgram(p, 0x10000000);  // arrival: block 4 spawns the carrier (§4.2)
    cooldown = 0;
    lastCallFriend = 1;
    return true;
}

// ------------------------------------------------------------------------------------------------ entities
TbActor* TbWorld::lookup(uint32_t sel, TbActor& self) {
    TbActor& fa = actors[size_t(focus())];
    TbActor& pa = actors[size_t(1 - focus())];
    TbActor* hit = nullptr;
    if (sel & 0xe0008000u) {
        if (sel & 0x40000000u) {  // a child (+0x8c list): id, any (0), a dying one or a player for id 10
            const uint32_t id = sel & 0xbfffffffu;
            forEachEntity([&](TbActor& c) {
                if (!hit && c.parent == int32_t(self.uid) &&
                    (c.id == id || id == 0 || (c.f20 & 0x2000) || (id == 10 && (c.flags0 & 0x4000000))))
                    hit = &c;
            });
            return hit;
        }
        if (sel & 0x80000000u) {  // the parent (+0x90) unless it has another id
            const uint32_t id = sel & 0x7fffffffu;
            TbActor* p = entity(self.parent);
            if (p && p->id != id && id != 0 && !(p->f20 & 0x2000) && (id != 10 || !(p->flags0 & 0x4000000))) return nullptr;
            return p;
        }
        if (!(sel & 0x8000)) {  // 0x20000000: first entity with the id
            forEachEntity([&](TbActor& e) { if (!hit && e.id == (sel & 0xdfffffffu)) hit = &e; });
            return hit;
        }
        uint64_t bestD = ~uint64_t(0);  // 0x8000: nearest living entity with the id (deltas >> 2)
        forEachEntity([&](TbActor& e) {
            if (e.id != (sel & 0xffff7fffu) || (e.f20 & 0x2000)) return;
            const int64_t x = (e.pos.x - self.pos.x) >> 2, y = (e.pos.y - self.pos.y) >> 2, z = (e.pos.z - self.pos.z) >> 2;
            const uint64_t d = uint64_t(x * x + y * y + z * z);
            if (d < bestD) bestD = d, hit = &e;
        });
        return hit;
    }
    if (sel > 0x10) {
        forEachEntity([&](TbActor& e) { if (!hit && e.id == sel && !(e.f20 & 0x2000)) hit = &e; });
        return hit;
    }
    switch (sel) {
    case 0xb: return entity(self.contact);  // +0x54 last contact
    case 0xd: return entity(self.link);     // +0x1ac link
    case 0xc: return &self;
    case 8: case 0xe: return &fa;
    case 9: case 0xf: return &pa;
    case 2: return GD[0xa2] & 1 ? nullptr : &actors[0];
    case 3: return GD[0xa2] & 2 ? nullptr : &actors[1];
    default: break;
    }
    if (GD[0xa2] & 3) {
        if (self.f24 & 0x3000000) self.f24 = (self.f24 & ~0x3000000u) | (fa.id == 2 ? 0x1000000u : 0x2000000u);
        return &fa;
    }
    if (sel == 10) {
        if (self.f24 & 0x1000000) return &actors[0];
        if (self.f24 & 0x2000000) return &actors[1];
    }
    hit = &fa;
    if (!(pa.f20 & 0x80000000u) && sel >= 4 && sel <= 7) {  // 4/6 nearest, 5/7 farthest player (6/7 in xz)
        auto dist = [&](const TbActor& t) {
            const int64_t x = t.pos.x - self.pos.x, y = sel < 6 ? t.pos.y - self.pos.y : 0, z = t.pos.z - self.pos.z;
            return isqrt(int32_t(std::min<int64_t>(x * x + y * y + z * z, 0x7fffffff)));
        };
        const bool tazNearer = dist(actors[1]) < dist(actors[0]);
        const bool nearest = sel == 4 || sel == 6;
        hit = &actors[tazNearer == nearest ? 1 : 0];
    }
    if (sel == 10) hit->f24 |= hit->id == 2 ? 0x1000000u : 0x2000000u;
    return hit;
}

// 0x44e560 coordinate chain: the part's origin through its parents to the actor root (yaw rotation + position).
V3 TbWorld::partWorld(const TbActor& a, int32_t part) const {
    const std::vector<TodPart>& parts = a.anim.parts;
    double v[3] = {0, 0, 0};
    for (int32_t i = part, guard = 0; i >= 0 && size_t(i) < parts.size() && guard < 64; ++guard) {
        const TodPart& t = parts[size_t(i)];
        const double x = v[0], y = v[1], z = v[2];
        for (int r = 0; r < 3; ++r) v[r] = t.m[size_t(r * 3)] * x + t.m[size_t(r * 3 + 1)] * y + t.m[size_t(r * 3 + 2)] * z + t.trans[size_t(r)];
        i = t.parent;
    }
    const V3 r = rotY(a.yaw, int32_t(std::lround(v[0])), int32_t(std::lround(v[1])), int32_t(std::lround(v[2])));
    return {a.pos.x + r.x, a.pos.y + r.y, a.pos.z + r.z};
}

TbActor* TbWorld::spawn(uint16_t tmpl, V3 pos, int16_t yaw) {
    auto t = templates_.find(tmpl);
    if (t == templates_.end()) {
        unsupported.insert(std::format("spawn{}", tmpl));
        return nullptr;
    }
    TbActor& e = entities.emplace_back(t->second);  // 0x460b00: copy of the template entity, appended to the list
    e.uid = nextUid_++;
    e.listPos = nextListPos_++;
    e.active = true;  // and to this tick's view list: it ticks later in the same pass
    e.model = e.ownModel.get();
    e.pos = e.spawn = pos;
    e.yaw = e.spawnYaw = yaw;
    listOwner_ = nullptr;
    return &e;
}

void TbWorld::attach(TbActor& parent, TbActor& child) {
    if (parent.parent == int32_t(child.uid)) detach(parent);
    if (child.parent >= 0) {
        detach(child);
        child.f20 &= ~0x208u;
    }
    child.parent = int32_t(parent.uid);
    child.parentPart = parent.attachPart;  // +0xe8 = the parent's +0x84 part, else its root coordinate
    child.pos = partWorld(parent, child.parentPart);
}

void TbWorld::detach(TbActor& child) {
    child.parent = -1;
    child.parentPart = -1;
}

// ------------------------------------------------------------------------------------------------ logic VM
TbBlock* TbWorld::findBlock(TbActor& a, uint16_t id) {
    auto& m = id < 10000 ? a.blocks : globalBlocks_;
    auto it = m.find(id);
    return it == m.end() ? nullptr : &it->second;
}

// 0x45ea90: program block +0x2c (pad of flags1 0x800), its event dispatched, then the trigger bits cleared.
uint32_t TbWorld::runProgram(TbActor& a, uint32_t c20, uint32_t c24, uint32_t c28) {
    uint32_t r = 0x80000000;
    if (a.progBlock)
        if (TbBlock* b = findBlock(a, a.progBlock)) {
            r = runItems(a, *b);
            if (!(r & 0x90000000)) dispatch(a, r);
        }
    a.f20 &= ~c20, a.f24 &= ~c24, a.f28 &= ~c28;
    return r;
}

// 'W' = VM op 0xc -> 0x45a660 {flags, mode, p8, pc, p10}.
bool TbWorld::condW(TbActor& a, const TbLogicItem& it) {
    const std::span<const uint8_t> d(it.d);
    const uint32_t mode = le32(d, 4), p8 = le32(d, 8), pc = le32(d, 12), p10 = le32(d, 16);
    const AnimState& an = a.anim;
    if (mode & 4) {  // anim frame counter +0x14a == p8 + 1 with the frame hold +0x153 expired
        if (p8 + 1 != an.frame || an.countdown != 0) return false;
    } else if (mode & 8) {  // +0x14e & 0x2000: last frame shown (TbActor::animLast)
        if (!a.animLast) return false;
    } else if ((mode & 0x80) && !(an.flags & 0x1000)) {  // +0x14e & 0x1000: clip (re)started
        return false;
    }
    if (mode & 3) {  // chance: rand() & 0xffff must exceed p8 (mode 1) / pc (mode 2)
        if (uint32_t(rng() & 0xffff) <= (mode & 1 ? p8 : pc)) return false;
    }
    if (mode & 0x20) {  // +0x54 contact entity has id p10
        const TbActor* c = entity(a.contact);
        if (!c || c->id != p10) return false;
    }
    if (mode & 0x80000000u) {
        TbActor* t = lookup(p10, a);
        if (!t) return false;
        const uint32_t sub = mode & 0x8f000000u;
        auto sector = [](uint32_t span, uint32_t v) { return (span & 0xfff) >= (v & 0xfff); };
        if (mode & 0x0f000000u) switch (sub) {
        case 0x81000000u: if (t->anim.frame != p8 + 1) return false; break;
        case 0x83000000u:
            if (!sector(pc - p8, uint32_t(ratan2(t->pos.x - a.pos.x, t->pos.z - a.pos.z)) - uint16_t(a.yaw) - p8)) return false;
            break;
        case 0x85000000u: if (t->tr < 0 || t->trans[size_t(t->tr)].event != p8) return false; break;
        case 0x86000000u: if (!t->rec() || t->rec()->id != p8) return false; break;
        case 0x88000000u: {
            const int64_t x = a.pos.x - t->pos.x, y = a.pos.y - t->pos.y, z = a.pos.z - t->pos.z;
            const uint32_t sum = uint32_t(x * x + z * z + (mode & 0x20000000u ? y * y : 0)), lim = p8 * p8;
            if (!(mode & 0x10000000u) ? lim < sum : sum < lim) return false;
            break;
        }
        case 0x89000000u: if (!sector(pc - p8, uint32_t(uint16_t(t->yaw)) - p8)) return false; break;
        case 0x8a000000u: case 0x8b000000u: {
            const int32_t y = sub == 0x8a000000u ? t->pos.y : t->pos.y - a.pos.y;
            if (int32_t(p8) < y || y < int32_t(pc)) return false;
            break;
        }
        case 0x8d000000u: if (!sector(pc - p8, uint32_t(uint16_t(t->yaw)) - uint16_t(a.yaw) - p8)) return false; break;
        case 0x82000000u: {  // line of sight from 3/4 of the box height (query 9 / 0x209 radius p8)
            auto eye = [](const TbActor& e) {
                const int32_t h = e.box[4] - e.box[1];
                return V3{e.pos.x, e.pos.y - (h >> 2) + e.box[1] + h, e.pos.z};
            };
            const V3 s = eye(a), g = eye(*t);
            // ponytail: world polygons only; the original also stops at other actors (query flag 1).
            if (col.query(s, {g.x - s.x, g.y - s.y, g.z - s.z}, p8 ? int32_t(p8) : 5, p8 ? 0x209 : 9)) return false;
            break;
        }
        case 0x87000000u:  // +0x42 weight in [p8, pc] (pc 0 = no upper bound)
            if (t->weight < int32_t(p8) || (pc && int32_t(pc) < t->weight)) return false;
            break;
        case 0x8c000000u: {  // the +0x1ac link has the target's id
            const TbActor* l = entity(a.link);
            if (!l || l->id != t->id) return false;
            break;
        }
        default: break;
        }
    }
    if (mode & 0x40) {  // side +0x24 & 0x3000000 toward the player p10 (4..7 nearest/farthest, p8 bias)
        uint32_t sel = p10, side = a.f24 & 0x3000000;
        if (sel == 8) sel = actors[size_t(focus())].id;
        else if (sel == 9) sel = actors[size_t(1 - focus())].id;
        if (sel == 2 || sel == 3) {
            side = 0x1000000;
        } else if (sel >= 4 && sel <= 7) {
            if (GD[0xa2] & 3) {
                side = actors[size_t(focus())].id != 2 ? 0x2000000 : 0x1000000;
            } else {
                auto dist = [&](const TbActor& p) {
                    const int64_t x = a.pos.x - p.pos.x, y = sel < 6 ? a.pos.y - p.pos.y : 0, z = a.pos.z - p.pos.z;
                    return isqrt(int32_t(std::min<int64_t>(x * x + y * y + z * z, 0x7fffffff)));
                };
                int32_t db = dist(actors[0]), dz = dist(actors[1]);
                if (sel == 4 || sel == 6) db = -db, dz = -dz;
                if (side == 0x2000000) dz += int32_t(p8);
                else if (side == 0x1000000) db += int32_t(p8);
                side = db < dz ? 0x2000000 : 0x1000000;
            }
        }
        if (side == (a.f24 & 0x3000000)) return false;
        a.f24 = (a.f24 & ~0x3000000u) | side;
    }
    return !(mode & 0x10);
}

bool TbWorld::cond(TbActor& a, TbLogicItem& it, bool cur) {
    const std::span<const uint8_t> d(it.d);
    auto Sv = [&](uint32_t i) -> uint32_t { return i < S.size() ? S[i] : 0; };
    auto gd8 = [&](uint32_t y) -> uint32_t { return 0x58 + y < GD.size() ? GD[0x58 + y] : 0; };
    auto gd16 = [&](uint32_t y) -> uint32_t {  // even offsets below 0x31 are u16 fields
        return (y & 1) == 0 && y < 0x31 ? uint32_t(GD[0x58 + y] | GD[0x59 + y] << 8) : gd8(y);
    };
    switch (it.tag) {
    case 0x4f: return a.f20 & le32(d, 4);
    case 0x50: return a.f24 & le32(d, 4);
    case 0x51: return a.f28 & le32(d, 4);
    case 0x5c: return cur;  // ELSE: no test (op 0x12), NOT inverts
    case 0x57: return condW(a, it);
    case 0x58: {  // pad 'X' (§3.1, VM op 0xe)
        const uint32_t f = le32(d, 0), b = le32(d, 4);
        if ((a.flags0 & 0x4000000) && GD[0x9f]) return false;
        int pad = a.padIndex();
        if (f & 0x800) pad = 0; else if (f & 0x1000) pad = 1; else if (f & 0x6000) return false;  // pads 2/3 absent
        const TbPad& p = pads[size_t(pad)];
        uint16_t bits;
        if (f & 0x100) bits = f & 0x200 ? p.raw : f & 0x400 ? uint16_t(p.raw & ~p.prevRaw) : uint16_t(p.raw & ~(p.prevRaw & edgeMask));
        else bits = f & 0x200 ? p.held : f & 0x400 ? uint16_t(p.held & ~p.prevHeld) : p.pressed;
        if (f & 0x8000) {  // camera-relative stick directions
            bits &= 0xfff;
            const V3 l = rotY(camYaw() - a.yaw, p.sx, 0, p.sy);
            if (l.x < -50) bits |= PadLeft; else if (l.x > 50) bits |= PadRight;
            if (l.z < -50) bits |= PadDown; else if (l.z > 50) bits |= PadUp;
        }
        const uint16_t req = uint16_t(b), any = uint16_t(b >> 16);
        return (any & bits) && (req & bits) == req;
    }
    case 0x52: {  // 'R' {u8 flags, u8, u8 cond, u8, u16 X, u16 Y}: table 0x4cd560[cond](self, {X @+4, Y @+6})
        const uint32_t ci = d[2], y = le16(d, 6), n = y << 8 | le16(d, 4);
        uint32_t x = le16(d, 4);
        switch (ci) {
        case 0x01: return Sv(y) == x;
        case 0x02: return int16_t(gd16(y)) == int16_t(x);
        case 0x03: return x < Sv(y);
        case 0x04: return x < gd16(y);
        case 0x05: return x <= Sv(y);
        case 0x06: return x <= gd16(y);
        case 0x07: return Sv(y) < x;
        case 0x09: return Sv(y) <= x;
        case 0x0d: return int8_t(Sv(y)) == int8_t(Sv(x));
        case 0x0f: return a.stateTicks < n;
        case 0x10: return n < a.stateTicks;
        case 0x11: return a.stateTicks == n;
        case 0x13: return n < a.stateTicks / 30u;
        case 0x12: return a.stateTicks / 30u < n;                 // 0x430500
        case 0x1b: return int16_t(gd16(y)) != int16_t(x);         // 0x4300b0
        case 0x29: return int8_t(Sv(y)) != int8_t(Sv(x));         // 0x42fe20
        case 0x44: return (gd8(y) & x) == x;                      // 0x430a20
        case 0x14: return a.stateTicks % 30 == 0 && a.stateTicks / 30u == n;
        case 0x18: return Sv(y) & (x & 0xff);
        case 0x19: return gd8(y) & (x & 0xff);
        case 0x1a: return Sv(y) != x;
        case 0x22: textStatus = textSpawn(uint16_t(x), 0); return x & textStatus;  // 0x430760: show text X, status & X
        case 0x23: return x & textStatus;                                        // 0x4307a0: last text status & X
        case 0x35: case 0x37:  // 0x4308a0 / 0x430900: sound X on any voice / on own voice == (Y == 1)
            return sfx.playing(uint16_t(x), ci == 0x37 ? int32_t(a.uid) + 1 : 0) == (y == 1);
        case 0x36: {                          // the focus carries an attached entity
            bool any = false;
            const int32_t fu = int32_t(actors[size_t(focus())].uid);
            forEachEntity([&](TbActor& c) { any = any || c.parent == fu; });
            return any;
        }
        case 0x45: {  // bit X of the GD+500 flag set (Y == 0: clear)
            if (x == 0xffff) x = a.gdBit;                         // own +0x48
            if ((x & 0xfff8) > 0x5e0) return false;
            const bool bit = (500 + (x >> 3) < GD.size()) && (GD[500 + (x >> 3)] >> (x & 7) & 1);
            return y == 0 ? !bit : bit;
        }
        case 0x49: return int32_t(x) <= 0 && 0 <= int32_t(y);  // water depth +0x1a2 in [X, Y]: no water simulated
        case 0x4a: return false;                                 // 0x40fc30
        default: break;
        }
        unsupported.insert(std::format("R{:#x}", ci));
        return false;
    }
    case 0x54: {  // 'T' {u8 flags, u8, u8 sub, u8, u32 mask, u32 target}: op 10, table 0x4cd8e4 (sub < 0xc: target)
        const uint32_t sub = d[2], mask = le32(d, 4);
        TbActor* t = sub < 0xc ? lookup(le32(d, 8), a) : &a;
        if (!t) return false;
        switch (sub) {
        case 0: return t->f20 & mask;
        case 1: return t->f24 & mask;
        case 2: return t->f28 & mask;
        case 3: return t->flags0 & mask;
        case 7: return t->groundPoly >= 0 && col.surfaces[col.polys[size_t(t->groundPoly)].surf].id == mask;  // 0x432f40
        default: unsupported.insert(std::format("T{}", sub)); return false;
        }
    }
    case 0x5a: {  // 'Z' op 0x10: the parent's +0x08 & mask
        const TbActor* p = entity(a.parent);
        return p && (p->flags0 & le32(d, 4));
    }
    case 0x5b: {  // '[' op 0x11: any attached child's +0x08 & mask
        bool any = false;
        const uint32_t mask = le32(d, 4);
        forEachEntity([&](TbActor& c) { any = any || (c.parent == int32_t(a.uid) && (c.flags0 & mask)); });
        return any;
    }
    case 0x5e: {  // '^' op 0x14 -> 0x45cc40 {flags, u16 frame, u32 mode, u16 template}: effect at the attach part
        const uint32_t mode = le32(d, 4);
        uint32_t want = uint32_t(le16(d, 2)) + 1;
        if (((mode & 0x40000) && want < a.anim.frame) || ((mode & 0x80000) && a.anim.frame < want)) want = a.anim.frame;
        if (a.anim.frame != want && !(mode & 0x800)) return false;
        if (a.anim.countdown != 0) return false;
        if (a.attachPart < 0 && !(mode & 0x4000)) return false;
        if ((mode & 0x20) && a.attachPart >= 0 && !(mode & 0x4000)) {  // release what hangs on the attach part
            TbActor* c = nullptr;
            forEachEntity([&](TbActor& e) { if (!c && e.parent == int32_t(a.uid) && e.parentPart == a.attachPart) c = &e; });
            if (c) {
                const V3 at = partWorld(a, c->parentPart);
                c->f20 = (c->f20 & 0xffff7fffu) | 0x10000;
                detach(*c);
                c->pos = at, c->yaw = a.yaw;
                if (c->progMask[0] & 0x10000) runProgram(*c, 0x10000);
            }
        }
        if (mode & 0x10000) {  // ground decal under the actor: needs the 0x17-role decal asset path
            unsupported.insert("^10000");
            return false;
        }
        if (mode & 0x2060f) effects.push_back(le16(d, 8));  // spawn bits 1 2 4 8 0x200 0x400 0x20000 (0x45cc40)
        return true;
    }
    case 0x60: {  // '`' op 0x16 {flags, u8 sound @2, u8 frame @4, u8 count @6}: sound on anim frame, every count+1
        if (a.anim.frame != uint32_t(d[4]) + 1 || a.anim.countdown != 0) return false;
        if (it.counter == 0) {
            it.counter = d[6];
            playSound(a, d[2]);
        } else {
            --it.counter;
        }
        return true;
    }
    default:
        unsupported.insert(std::string(1, char(it.tag)));
        return false;
    }
}

// 'V' = VM op 0xb -> 0x45ad70 {u32 op, p1, p2, p3}; returns 0 or the block result (sequence step / side event).
uint32_t TbWorld::actionV(TbActor& a, const TbLogicItem& it) {
    const std::span<const uint8_t> d(it.d);
    const uint32_t u = le32(d, 0), p1 = le32(d, 4), p2 = le32(d, 8), p3 = le32(d, 12);
    auto sameId = [&](TbActor* t, auto&& f) {  // p3 & 0x20000000: every entity with the target's id
        const uint16_t id = t->id;
        forEachEntity([&](TbActor& e) { if (e.id == id) f(e); });
    };
    if (u & 0x80000000u) {  // global branch: damage, checkpoints, view flags, teleports, death overrides, call friend
        if (u & 0x20000) unsupported.insert("V rig swap 0x469320");
        if (u & 0x80000)  // forced damage p2 (transition p1, 0/0x80000000 = none) on the living target p3
            if (TbActor* t = lookup(p3, a); t && !(t->f20 & 0x2000000)) {
                t->dmgIn = int16_t(p2);
                t->f24 |= 0x40000000;
                t->hitTr = p1 == 0x80000000u || p1 == 0 ? int16_t(-1) : int16_t(p1);
            }
        if (u & 0x10) runNear(a, p1, p2);
        if (u & 0xa) checkpoint = {int16_t(p1), int16_t(p2), int16_t(p3), checkpoint[3]};  // 0x4d26e0..e8
        if (u & 0x418f00)
            if (TbActor* t0 = lookup(p3 & ~0x20000000u, a)) {
                auto apply = [&](TbActor& t) {
                    if (u & 0x100) t.view |= uint16_t(p1), t.sleep = 0;
                    if (u & 0x200) t.view &= uint16_t(~p1), t.sleep = 0;
                    if (u & 0x10000) t.progMask[0] |= p1, t.progMask[1] |= p2;
                    if (u & 0x8000) t.progMask[0] &= ~p1, t.progMask[1] &= ~p2;
                    if ((u & 0x400) && p1 < S.size()) S[p1] = uint8_t(t.hp);
                    if ((u & 0x800) && p1 < S.size()) t.hp = S[p1];
                    if (u & 0x400000) a.yaw = int16_t(t.yaw & 0xfff);
                };
                if (p3 & 0x20000000u) sameId(t0, apply); else apply(*t0);
            }
        if (u & 1)  // place the last spawned / probed entity (0x4d5eb8)
            if (TbActor* t = entity(lastSpawn)) teleport(*t, {int32_t(p1), int32_t(p2), int32_t(p3)});
        if (u & 4) teleport(a, {int32_t(p1), int32_t(p2), int32_t(p3)});
        if (u & 0x1000) {
            if (p1) deathEvent2[0] = p1;
            if (p2) deathEvent2[1] = p2;
        }
        if ((u & 0x2000) && ((a.f24 & 1) || p3)) {  // death transitions of a player this entity is killing
            if (p1 && (p3 || ((actors[0].f20 & 0xc000000) && actors[0].contact == int32_t(a.uid)))) deathEvent[0] = p1;
            if (p2 && (p3 || ((actors[1].f20 & 0xc000000) && actors[1].contact == int32_t(a.uid)))) deathEvent[1] = p2;
        }
        if (u & 0x4000) callFriend(false);
        if (u & 0x100000) unsupported.insert("V throw 0x80100000");
        if (u & 0x60)  // S[p1] = distance >> 8 between the targets p2 and p3 (0 = focus), xz / 3D
            if (p1 < S.size()) {
                TbActor* t2 = p2 ? lookup(p2, a) : &actors[size_t(focus())];
                TbActor* t3 = p3 ? lookup(p3, a) : &actors[size_t(focus())];
                uint8_t v = 0;
                if (t2 && t3 && t2 != t3) {
                    const int64_t x = t2->pos.x - t3->pos.x, y = (u & 0x40) ? t2->pos.y - t3->pos.y : 0, z = t2->pos.z - t3->pos.z;
                    v = uint8_t(isqrt(int32_t(std::min<int64_t>(x * x + y * y + z * z, 0x7fffffff))) >> 8);
                }
                S[p1] = v;
            }
        if (u & 0x200000) unsupported.insert("V effect 0x451e10");
        if ((u & 0x200000) && !((p1 & 1) && (a.f24 & 4)) && uint16_t(p3) < fx_.size()) {
            // 0x451e10 -> 0x452ab0 (skipped for p1 & 1 outside the view, +0x24 & 4): emit slot p3's particle count;
            // only the rand() draws are simulated: 2 per particle (direction), +1 with flags 0x80 (+0x0e spread), +1
            // unless 0x800 (+0x07). A full pool (120, 0x4528f0) steals the oldest particle, so every particle draws.
            const FxSlot& f = fx_[uint16_t(p3)];
            for (int k = 0; k < f.count; ++k) {
                rng(), rng();
                if (f.flags & 0x80) rng();
                if (!(f.flags & 0x800)) rng();
            }
        }
        return 0;
    }
    if (u & 0x800000) unsupported.insert("V 0x800000");
    uint32_t res = 0;
    TbActor* made = nullptr;
    if (u & 0x810) {  // spawn template p3 (p2 times, at least once) at the attach point or the actor
        uint32_t n = 0;
        do {
            const V3 at = (u & 0x40000000u) || a.attachPart < 0 ? a.pos : a.attachPos;
            made = spawn(uint16_t(p3), at, a.yaw);
            if (made) {
                if (u & 0x8000000) made->pitch = a.pitch, made->roll = a.roll;
                if (u & 0x10000) made->hp = a.hp;
                if (!(u & 0x40000) && (u & 0x800)) made->yaw = int16_t(wrap(rng() & 0xfff));
            }
        } while (++n < p2);
    }
    if (u & 0x2f64182) {
        const bool one = !(p3 & 0x20000000u);
        TbActor* t = made ? made : lookup(p3, a);
        if (t) {
            if (u & 0x40000) attach(a, *t);  // the target hangs on this actor's attach part
            if (u & 0x200000) lastSpawn = int32_t(t->uid);
            if (u & 0x8000) t->link = a.link;
            if (u & 0x4000) {  // event p1 to the target (0x20000000: to every entity with its id)
                if (one) dispatch(*t, p1);
                else sameId(t, [&](TbActor& e) { dispatch(e, p1); });
            }
            if ((u & 0x100000) && one) {  // the target comes to this actor (its attach point when attached)
                teleport(*t, a.parent >= 0 ? a.attachPos : a.pos);
                if (!(u & 0x1000000)) t->yaw = a.yaw;
            }
            if (u & 0x20000) {  // kill the target(s)
                if (one) t->f20 |= 0x2000;
                else sameId(t, [&](TbActor& e) { e.f20 |= 0x2000; });
            }
            if (u & 0x100) a.link = one ? int32_t(t->uid) : -1;
            if (u & 0x2000000)
                if (TbActor* l = lookup(p2, a)) l->link = one ? int32_t(t->uid) : -1;
            if ((u & 2) && one) {  // which side of this actor's box the target is on: behind p1, in front p2
                const V3 l = rotY(-a.yaw, t->pos.x - a.pos.x, t->pos.y - a.pos.y, t->pos.z - a.pos.z);
                if (l.z < a.box[2]) res = p1;
                else if (a.box[5] < l.z) res = p2;
            }
            if ((u & 0x80) && one) attach(*t, a);  // this actor hangs on the target
            if ((u & 0x400000) && one) t->turnRate = int16_t(p1);
        }
    }
    if ((u & 1) && a.parent >= 0) {  // detach, keeping the world position
        if (const TbActor* par = entity(a.parent); par && !(u & 0x1000000)) a.yaw = par->yaw;
        detach(a);
    }
    if (u & 0x60) playSound(a, uint16_t(p1), (u & 0x20) ? 1 : 2);  // 0x20 start, else 0x40 release (0x407080)
    if (u & 0x80000) res = 0x20000000;               // next in sequence
    if (u & 0x4000000) res = 0x2000000;              // previous
    if (u & 0x10000000) {                             // restart the transition at index p1
        res = 0x1000000;
        a.seq = uint16_t(p1);
    }
    if (u & 8) runNear(a, p1, p2);
    if (u & 0x1000) {  // level end / player out: GD+0 = p1 (2000 keeps it), down players leave, reload
        if (p1 != 2000) nextLevel = int32_t(p1);
        uint8_t& a2 = GD[0xa2];
        if ((a2 & 0x11) && (a2 & 0x22)) {
            if (GD[0xa0]) --GD[0xa0]; else nextLevel = 5;
            a2 &= 0xcc;
        } else if (a2 & 0x10) {
            a2 = uint8_t((a2 & ~0x10) | 1);
        } else if (a2 & 0x20) {
            a2 = uint8_t((a2 & ~0x20) | 2);
        }
        restart = true;
    }
    if (u & 0x2000) checkpoint = {int16_t(a.pos.x), int16_t(a.pos.y), int16_t(a.pos.z), a.yaw};
    if (u & 0x200) {  // release attached children matching +0x08 & p1 or +0x0c & p2
        std::vector<TbActor*> kids;
        forEachEntity([&](TbActor& c) {
            if (c.parent == int32_t(a.uid) && ((p1 & c.flags0) || (p2 & c.flags1))) kids.push_back(&c);
        });
        for (TbActor* c : kids) {
            const V3 at = partWorld(a, c->parentPart);
            c->f20 = (c->f20 & 0xffff7bdeu) | 0x10000;
            detach(*c);
            c->pos = at;
            c->pitch = c->roll = 0;
            if (!(u & 0x1000000)) c->yaw = a.yaw;
            if (c->progMask[0] & 0x10000) runProgram(*c, 0x10000);
        }
    }
    if (u & 0x400) {  // remove this entity
        a.f20 |= 0x2000;
        return 0;
    }
    return res;
}

uint32_t TbWorld::action(TbActor& a, const TbLogicItem& it) {
    const std::span<const uint8_t> d(it.d);
    if (it.tag == 0x56) return actionV(a, it);
    if (it.tag == 0x53) {  // 'S' op 7 {u16 id, X @+2, Y @+4}: table 0x4cd6ac
        const uint16_t id = le16(d, 0), x = le16(d, 2), y = le16(d, 4);
        uint8_t* sv = y < S.size() ? &S[y] : nullptr;
        uint8_t* gv = 0x58u + y < GD.size() ? &GD[0x58u + y] : nullptr;
        switch (id) {
        case 3: if (sv) *sv |= uint8_t(x); return 0;                    // 0x430db0
        case 4: if (gv) *gv |= uint8_t(x); return 0;                    // 0x430dd0
        case 5: if (sv) *sv &= uint8_t(x); return 0;                    // 0x430df0
        case 7: case 8:                                                 // 0x430f70 / 0x430fa0: rand() % X
            if (!x) break;
            if (uint8_t* v = id == 7 ? sv : gv) *v = uint8_t(rng() % int32_t(x)); else rng();
            return 0;
        case 10: if (sv) *sv = uint8_t(x); return 0;                    // 0x430ce0
        case 11:                                                        // 0x430d00: u16 below 0x31 at even offsets
            if ((y & 1) == 0 && y < 0x31) GD[0x58u + y] = uint8_t(x), GD[0x59u + y] = uint8_t(x >> 8);
            else if (gv) *gv = uint8_t(x);
            return 0;
        case 12: if (sv && x < S.size()) *sv = S[x]; return 0;           // 0x430e30
        case 16: a.stateTicks = 0; return 0;                             // 0x430fd0
        case 20: playSound(a, x); return 0;                              // 0x430ff0
        case 21: if (sv) *sv = uint8_t(*sv + int8_t(x)); return 0;      // 0x431010
        case 22:                                                         // 0x431030 GD(Y) += X (u16 at even Y <= 0x30)
            if ((y & 1) == 0 && y <= 0x30) {
                const uint16_t s = uint16_t((GD[0x58u + y] | GD[0x59u + y] << 8) + x);
                GD[0x58u + y] = uint8_t(s), GD[0x59u + y] = uint8_t(s >> 8);
            } else if (gv) {
                *gv = uint8_t(*gv + x);
            }
            return 0;
        case 34: if (sv) a.yaw = int16_t(a.yaw + (int8_t(*sv) << 4)); return 0;  // 0x4311a0 yaw += (s8)S[Y] << 4
        case 76: case 77:                                                // 0x432100 / 0x432170: octant of target X
            if (TbActor* t = lookup(x, a); t && sv) {
                const int32_t ang = id == 76 ? ratan2(t->pos.x - a.pos.x, t->pos.z - a.pos.z) - uint16_t(a.yaw)
                                             : ratan2(a.pos.x - t->pos.x, a.pos.z - t->pos.z) - uint16_t(t->yaw);
                *sv = uint8_t(1u << ((uint32_t(ang) >> 9) & 7));
            }
            return 0;
        case 79:  // 0x432320 forced switch
            if (!(GD[0xa2] & 3) && (x == 0 || x == actors[size_t(1 - focus())].id)) {
                A4 ^= 4;
                for (TbActor& c : actors) c.f28 |= 0x100;
            }
            return 0;
        case 82:  // 0x432530: select collision box record (tag 0x61), arg2 & 2 = no morph
            if (x < a.boxes.size() && int32_t(x) != a.boxIdx) a.boxIdx = x, a.boxInstant = y & 2;
            return 0;
        case 126: music = x & 0xff; return 0;  // 0x432db0
        case 118: {  // 0x432c80 -> 0x452060: field X & 0xff of the effect slot this entity owns (0x451e10, else 0) = Y
            const auto own = std::ranges::find(fx_, int32_t(a.uid), &FxSlot::owner);
            FxSlot& f = own == fx_.end() ? fx_[0] : *own;
            switch (x & 0xff) {
            case 4: f.count = uint8_t(y); break;
            case 10: f.flags = y; break;
            case 7:  // bind slot Y to this entity
                for (FxSlot& s : fx_) if (s.owner == int32_t(a.uid)) s.owner = -1;
                if (int16_t(y) >= 0 && size_t(int16_t(y)) < fx_.size()) fx_[size_t(int16_t(y))].owner = int32_t(a.uid);
                break;
            default: break;  // emitter shape / colour / timing fields: no rand() or gameplay effect
            }
            return 0;
        }
        case 0: case 40: case 42: case 43: case 131: return 0;           // log stub / pad rumble (not ported)
        case 24: if (sv) *sv = uint8_t(*sv - int8_t(x)); return 0;      // 0x431070
        case 30: textStatus = textSpawn(uint16_t(x), int16_t(y)); return 0;   // 0x4312f0: text X, priority Y
        case 51:                                                         // 0x4319f0: X 0 fade in, 1 fade out (holds)
            if (x == 0) fadeIn(1, 0x100);
            else if (x == 1) fadeOut(2, 0x100);
            return 0;
        case 52: fadeIn(0x41, 0x400); return 0;                         // 0x431a30: dark-light-dark blink
        case 31:                                                         // 0x431320: remove class-31 entities
            forEachEntity([&](TbActor& e) { if (e.behaviour == 31 && (x == 0xffff || e.id == x)) e.f20 |= 0x2000; });
            return 0;
        case 63: playSound(a, x, 2); return 0;                           // 0x431c40 release
        case 74: cameraSnap(actors[size_t(focus())]); return 0;          // 0x431fb0 camera reset flag 4 (I: snap)
        case 90: {                                                       // 0x432670 GD(Y) += GD(X)
            auto gdw = [&](uint16_t o) { return (o & 1) == 0 && o < 0x31; };
            const uint32_t v = gdw(x) ? uint32_t(GD[0x58u + x] | GD[0x59u + x] << 8) : GD[0x58u + x];
            if (gdw(y)) {
                const uint16_t s = uint16_t((GD[0x58u + y] | GD[0x59u + y] << 8) + v);
                GD[0x58u + y] = uint8_t(s), GD[0x59u + y] = uint8_t(s >> 8);
            } else if (gv) {
                *gv = uint8_t(*gv + v);
            }
            return 0;
        }
        case 106: {                                                      // 0x432ad0 GAME_DATA +500 bit X (own +0x48)
            const uint16_t b = x == 0xffff ? a.gdBit : x;
            if ((b & 0xfff8) < 0x5e1) {
                uint8_t& m = GD[500 + (b >> 3)];
                m = y ? uint8_t(m | 1 << (b & 7)) : uint8_t(m & ~(1 << (b & 7)));
            }
            return 0;
        }
        default: break;
        }
        unsupported.insert(std::format("S{}", id));
        return 0;
    }
    if (it.tag == 0x55) {  // 'U' op 9 (table 0x4cd91c) {u32 index, A, B}: ops < 26 on target B, else self
        const uint32_t idx = le32(d, 0), A = le32(d, 4), B = le32(d, 8);
        auto op = [&](TbActor& t) {
            switch (idx) {
            case 0: t.f20 |= A; break;
            case 1: t.f24 |= A; break;
            case 2: t.f28 |= A; break;
            case 3: t.f20 &= ~A; break;
            case 4: t.f24 &= ~A; break;
            case 5: t.f28 &= ~A; break;
            case 6: t.flags0 |= A; break;
            case 7: t.flags1 |= A; break;
            case 8: t.flags10 |= A; break;
            case 9: t.flags0 &= ~A; break;
            case 10: t.flags1 &= ~A; break;
            case 11: t.flags10 &= ~A; break;
            case 13: t.yaw = int16_t(A & 0xfff); break;
            case 14: case 15: case 16: t.vel[idx - 14] = int32_t(int16_t(A)) * 16; break;
            case 18: t.id = uint16_t(A); break;
            case 22: break;
            case 23: t.range = uint16_t(A); break;
            case 24: t.progMask[2] |= A; break;
            case 25: t.progMask[2] &= ~A; break;
            default: unsupported.insert(std::format("U{}", idx)); break;  // 12 hover height, 17, 19/21 grips, 20 light
            }
        };
        if (idx < 26) {
            if (TbActor* t = lookup(B & ~0x20000000u, a)) {
                if (!(B & 0x20000000u)) op(*t);
                else forEachEntity([&](TbActor& e) { if (e.id == (B & 0xdfffffffu)) op(e); });
            }
            return 0;
        }
        for (TbSurface& s : col.surfaces)  // surface ops 28/29/30 (§1.3)
            if (B == 0x80000000u || s.id == B) {
                if (idx == 28) s.flags |= A;
                else if (idx == 29) s.flags &= ~A;
                else if (idx == 30) s.friction = uint16_t(A);
            }
        if (idx != 26 && (idx < 28 || idx > 30)) unsupported.insert(std::format("U{}", idx));
        return 0;
    }
    if (it.tag == 0x5f) {  // '_' op 0x15 (0x45c670) {u32 flags, p1..p3}: camera script points
        const uint32_t f = le32(d, 0);
        V3 p{s16(d, 4), s16(d, 8), s16(d, 12)};
        if (f & 0x80000000u) {
            const TbActor* t = lookup(le32(d, 12), a);
            if (!t) return 0;
            p = t->pos;
        }
        CamScript& c = camScript;
        if (f & 0x20000000u) c.flags |= 0x20;
        if (f & 1) {
            c.flags |= 0x400, c.look = p;
            if (f & 0x20) c.flags |= 0x4000;
            if (f & 0x40) c.flags |= 0x10000;
        } else if (f & 4) {
            c.flags &= ~0x4400u;
        }
        if (f & 2) {
            c.flags |= 0x800, c.eye = p;
            if (f & 0x20) c.flags |= 0x8000;
            if (f & 0x40) c.flags |= 8;
        } else if (f & 8) {
            c.flags &= ~0x8800u;
        }
        if (f & 0x40000000u) c.flags = le32(d, 8) ? c.flags | 0x80000 : c.flags & ~0x80000u;
        // ponytail: recorded only; the camera modes these flags select in 0x439700 and the 0x400.. sub-ops
        // (0x43a1b0/0x43a0e0/0x43a110/0x43a0f0/0x43a130) are not ported.
        if (c.flags) unsupported.insert("_ camera script");
        return 0;
    }
    if (it.tag == 0x59) {  // 'Y' op 0xf: event to the first attached child of behaviour 0x21 (5 = drop it)
        const uint32_t ev = le32(d, 0);
        TbActor* c = nullptr;
        forEachEntity([&](TbActor& e) { if (!c && e.parent == int32_t(a.uid) && e.behaviour == 0x21) c = &e; });
        if (c) {
            if (ev == 5) c->f20 |= 0x2000, detach(*c);
            else dispatch(*c, ev);
        }
        return 0;
    }
    unsupported.insert(std::string(1, char(it.tag)));
    return 0;
}

uint32_t TbWorld::runBlock(TbActor& a, uint16_t block) {
    TbBlock* b = findBlock(a, block);
    return b ? runItems(a, *b) : 0x80000000;
}

// VM 0x45d7a0 at tag level: statement markers (0x08 IF: ends the block when the previous statement was true,
// 0x10 IF, 0x20 nested: skips when false), chains (1 AND: skip when false, 2 OR: keep when true, 4 NOT; 0x40 acts as
// AND and forces true for ']' '^' '`'), 'N' returns its event, actions run when true ('V' may end the block with a
// sequence result); a false AND / nested marker jumps to the current skip target, else the block ends false.
uint32_t TbWorld::runItems(TbActor& a, TbBlock& b) {
    static thread_local int depth = 0;
    if (depth > 32) {
        unsupported.insert("]depth");
        return kFalse;
    }
    struct Guard { Guard() { ++depth; } ~Guard() { --depth; } } guard;
    // Entity register +0x126 = the u16 S[0x1fe] (0x4d269e) while this entity's logic runs (0x45d7a0 entry / exit;
    // a nested run first stores the live value into its owner, 0x45e78a).
    auto s16 = [this]() -> uint16_t { return uint16_t(S[0x1fe] | S[0x1ff] << 8); };
    auto set16 = [this](uint16_t v) { S[0x1fe] = uint8_t(v), S[0x1ff] = uint8_t(v >> 8); };
    struct Reg {
        TbWorld& w; TbActor& a; TbActor* prev; uint16_t saved; decltype(s16)& get; decltype(set16)& set;
        ~Reg() { a.reg = get(), w.regOwner_ = prev, set(prev ? prev->reg : saved); }
    } reg{*this, a, regOwner_, s16(), s16, set16};
    if (regOwner_) regOwner_->reg = s16();
    set16(a.reg), regOwner_ = &a;
    bool cur = true, stmt = false;
    int32_t skip = -1;
    for (size_t i = 0; i < b.size();) {
        TbLogicItem& it = b[i];
        auto doSkip = [&]() {
            if (skip < 0) return false;
            i = size_t(skip);
            cur = true, stmt = false, skip = -1;
            return true;
        };
        if (isCond(it.tag)) {
            const uint32_t f = itemFlags(it);
            if (f & 0x38) {
                if (f & 8) {
                    if (stmt && cur) return kNoEvent;
                } else if ((f & 0x20) && !cur) {
                    if (doSkip()) continue;
                    return kFalse;
                }
                cur = true, stmt = true, skip = it.skip;
            }
            const bool force = it.tag == 0x5d || it.tag == 0x5e || it.tag == 0x60;
            if (((f & 1) || (force && (f & 0x40))) && !cur) {
                if (doSkip()) continue;
                return kFalse;
            }
            if (!((f & 2) && cur)) {
                bool v;
                if (it.tag == 0x5d) {  // ']' op 0x13 {flags, u16 block, u32 target}: run a block on an entity
                    const uint32_t t = le32(it.d, 4), sel = (t & 0xffffu) | (t & 0xe0000000u);
                    const uint16_t bid = le16(it.d, 2);
                    v = cur;  // target or block absent: unchanged
                    if (!(sel & 0x20000000u)) {
                        TbActor* tg = sel == 0xc ? &a : lookup(sel, a);
                        if (TbBlock* sb = tg ? findBlock(*tg, bid) : nullptr) {
                            const uint32_t r = runItems(*tg, *sb);
                            if (sel == 0xc) {
                                if (!(r & 0x10000000)) return r;  // the called block's event ends this one
                            } else if (!(r & 0x90000000)) {
                                dispatch(*tg, r);
                            }
                            v = (f & 0x40) || r != kFalse;
                        }
                    } else {
                        std::vector<TbActor*> all;
                        forEachEntity([&](TbActor& e) { if (e.id == (sel & 0xdfffffffu)) all.push_back(&e); });
                        for (TbActor* e : all)
                            if (TbBlock* sb = findBlock(*e, bid)) {
                                const uint32_t r = runItems(*e, *sb);
                                if (!(r & 0x90000000)) dispatch(*e, r);
                                v = v || r;
                            }
                    }
                } else {
                    v = cond(a, it, cur);
                    if (force && (f & 0x40)) v = true;
                }
                cur = v != bool(f & 4);
            }
        } else if (it.tag == 0x4e) {
            if (cur) return le32(it.d, 0);
            if (doSkip()) continue;
            return kFalse;
        } else {
            if (!cur) {
                if (doSkip()) continue;
                return kFalse;
            }
            if (const uint32_t r = action(a, it)) return r;
        }
        ++i;
    }
    return cur ? kNoEvent : kFalse;
}

// ------------------------------------------------------------------------------------------------ states
// 0x44d990: event -> transition sequence -> state entry (§3.1, §2.3). +0x82 (stateTicks) is not touched.
bool TbWorld::dispatch(TbActor& a, uint32_t ev) {
    auto findTr = [&](uint32_t e) -> int32_t {
        for (size_t i = 0; i < a.trans.size(); ++i)
            if (a.trans[i].event == e) return int32_t(i);
        return -1;
    };
    auto findState = [&](uint16_t id) -> int32_t {
        for (size_t i = 0; i < a.states.size(); ++i)
            if (a.states[i].id == id) return int32_t(i);
        return -1;
    };
    const uint16_t oldAnim = a.rec() ? a.rec()->anim : 0;
    int32_t tr = a.tr, st = a.state;
    if ((ev & 0x27000000) && tr >= 0) {
        if (ev & 0x20000000) ++a.seq;
        else if (ev & 0x2000000) a.seq = a.seq ? uint16_t(a.seq - 1) : 0;
        else if (!(ev & 0x1000000)) a.seq = uint16_t(ev);
        const uint16_t id = a.seq <= 4 ? a.trans[size_t(tr)].seq[a.seq] : 0xfff0;
        if (id == 0xfff0 || id == 0) {
            if (a.f20 & 0x2000) return false;
            ev = 1;
        } else if (const int32_t s = findState(id); s >= 0) {
            st = s;
        } else {
            return false;  // missing state: the exe spins forever here
        }
    }
    if (!(ev & 0x27000000) || tr < 0) {
        int32_t t = -1;
        if (ev & 0x8000000) {  // hit: own hurt transition, then the attacker's +0x5e, then 3
            for (int32_t e : {int32_t(a.rec() ? a.rec()->ownHurt : 0), int32_t(a.hitTr), 3})
                if (e > 0 && (t = findTr(uint32_t(e))) >= 0) break;
        } else if (ev & 0x10000000) {  // death: player overrides 0x4d2720/0x4d5948, then 0x4d5944/0x4d273c, else 4
            const int k = &a == &actors[0] ? 0 : &a == &actors[1] ? 1 : -1;
            uint32_t e = 4;
            if (k >= 0 && (deathEvent[size_t(k)] || deathEvent2[size_t(k)])) e = deathEvent[size_t(k)] ? deathEvent[size_t(k)] : deathEvent2[size_t(k)];
            t = findTr(e & 0xffff);
            if (t < 0) return false;
        } else {
            if ((a.flags0 & 0x4000000) && (a.f20 & 0x2000000)) playerDown(a);  // 0x44ea50
            t = findTr(ev & 0xffff);
            if (t < 0 && ev == 2) t = findTr(1);
        }
        if (t < 0) return false;
        tr = t;
        a.seq = 0;
        st = findState(a.trans[size_t(t)].seq[0]);
        if (st >= 0 && (a.states[size_t(st)].flagsB & 0x4000000)) {  // walk/run split by stick magnitude
            const TbPad& p = padOf(a);
            a.seq = isqrt(p.sx * p.sx + p.sy * p.sy) >= 0x78;
            st = findState(a.trans[size_t(t)].seq[a.seq]);
        }
        if (st < 0) return false;
    }
    a.tr = tr;
    a.state = st;
    a.attachPart = -1;  // 0x44d990 LAB_0044e53e: every state entry clears +0x84 (the next 0xa00 packet sets it)
    const TbStateRec& r = a.states[size_t(st)];
    if (!(r.mode & 0x40) || r.anim != oldAnim) a.curAnim = 0;  // restart the clip next tick
    if (r.flagsB & 0x800) a.f20 &= ~0x421u;
    auto sval = [&](int16_t v, int shift4000) -> int32_t {
        if (v > 0 && (v & 0x4000)) return int32_t(int8_t(S[size_t(v & 0x3fff)])) << shift4000;
        if (v > 0 && (v & 0x2000)) return int32_t(int8_t(S[size_t(v & 0x1fff)]));  // x DAT_004d26b8 (I: 1)
        return v;
    };
    if (!((r.flagsA & 0x18) || (r.flagsB & 0x104000)) || ((a.f20 & 0x421) && !(a.f20 & 0x4000))) return true;
    a.f20 &= ~0x100u;
    if (r.flagsB & 0x100000) return true;  // homing jump to the selector target: not ported
    if (!(r.flagsA & 0x18)) return true;
    a.f20 &= ~0x421u;
    if (r.flagsA & 8) {  // impulse along the facing
        const V3 v = rotY(a.yaw, 0, 0, sval(r.fwd, 2));
        a.vel[0] += v.x * 16, a.vel[2] += v.z * 16;
    }
    if (!(r.flagsA & 0x10)) {
        a.vel[1] -= sval(r.vert, 2) * 16;
    } else {
        a.f20 |= 1;
        const int32_t H = r.vert > 0 && (r.vert & 0x4000) ? int32_t(S[size_t(r.vert & 0x3fff)]) << 4
                        : r.vert > 0 && (r.vert & 0x2000) ? int32_t(S[size_t(r.vert & 0x1fff)]) : r.vert;
        a.vel[1] = int16_t(-0x50 - ((isqrt(H * 0x50 + 100) >> 1) << 4));  // §2.3
    }
    return true;
}

void TbWorld::actorStep(TbActor& a) {
    a.trig = {};
    if (!(a.f24 & 0x8000)) {  // first tick of 0x453de0
        if ((a.flags10 & 4) && (a.gdBit & ~7u) < 0x5e1 && (GD[500 + (a.gdBit >> 3)] >> (a.gdBit & 7) & 1)) {
            a.f20 |= 0x2000;  // collected / removed for good (GAME_DATA +500 bit)
            return;
        }
        a.f24 |= 0x8000;
        a.f20 |= 0x100;
        if (!a.rec()) dispatch(a, 2);  // level start (0x453de0: only while +0x17c is unset; a spawn given an event keeps it)
        if (a.flags1 & 0x200) S[1] = 0x80, a.maxHp = a.hp;
    }
    if ((a.flags0 & 0x4000000) && (a.f28 & 0x200)) {  // the player's 60-tick hit flash ran out
        uint8_t& fl = flash[a.id == 2 ? 0 : 1];
        if ((fl & 0x3c) == 0x3c) fl &= a.id == 2 ? 0xbf : 0x7f, a.f28 &= ~0x200u;
    }
    if (!a.rec()) return;
    if (hitIntake(a)) return;
    const TbStateRec* r = a.rec();
    if (r->flagsB & 0x2000) a.f24 |= 0x1000; else a.f24 &= ~0x1000u;
    if (a.flags0 & 0x4000000) deathEvent[a.id == 2 ? 0 : 1] = 0;
    TbActor* par = entity(a.parent);
    if (a.parent >= 0 && !par) detach(a);
    if (par) a.pos = partWorld(*par, a.parentPart);  // attached: 0x44e560 chain on the parent's current pose
    const TbPad& pd = padOf(a);
    // Face / flee the state's target (0x453de0, flagsA 0x1000 / 0x2000; 0x4000 snaps the yaw). Attached entities
    // turn their own coordinate (+0x12c), which only the drawing uses.
    if (!(r->flagsA & 1) && !(a.f20 & 0x80000000u) && (r->flagsA & 0x3000) && !par)
        if (TbActor* t = lookup(uint16_t(r->target), a)) {
            const bool flee = r->flagsA & 0x2000;
            int32_t ang = ratan2(flee ? a.pos.x - t->pos.x : t->pos.x - a.pos.x, flee ? a.pos.z - t->pos.z : t->pos.z - a.pos.z);
            if (r->flagsA & 0x4000) {
                a.yaw = int16_t(ang);
            } else {
                if (ang < 0) ang += 0x1000;
                int32_t y = a.yaw < 0 ? -((-a.yaw) & 0xfff) : (a.yaw & 0xfff);
                if (y < 0) y += 0x1000;
                int32_t dlt = ang - y;
                if (dlt < 0) dlt += 0x1000;
                if (dlt > 0x800) dlt -= 0x1000;
                if (!(r->flagsB & 0x400)) {
                    const int sh = r->flagsB & 0x20 ? (a.turnRate ? a.turnRate : 2) : 4;
                    const int32_t q = dlt >> (sh & 0x1f);
                    a.turn = int16_t(q ? q & 0xfff : dlt & 0xfff);
                } else if (dlt < 0 && dlt < -a.turnRate) {
                    a.yaw = int16_t(a.yaw - a.turnRate);
                } else if (a.turnRate < dlt) {
                    a.yaw = int16_t(a.yaw + a.turnRate);
                }
            }
        }
    auto spd = [&](int16_t v) -> int32_t { return v > 0 && (v & 0x6000) ? int32_t(int8_t(S[size_t(v & 0x9fff)])) : v; };
    std::array<int32_t, 3> s{};  // local target: x side, y vertical, z forward
    if (r->flagsC & 1) {
        unsupported.insert("swim");
    } else if (r->flagsB & 0x80000000u) {
        if (pd.sx || pd.sy) {  // §2.1 stick branch
            const int32_t mag = std::min(isqrt(pd.sx * pd.sx + pd.sy * pd.sy), 0x80);
            if ((r->flagsB & 0x4000000) && a.seq != uint16_t(mag > 0x77)) {
                dispatch(a, uint32_t(mag > 0x77) | 0x4000000);
                r = a.rec();
            }
            if ((r->flagsB & 0x8000000) && a.model) {  // anim pace from the stick (§5.3)
                const int32_t p = (r->flagsB & 0x4000000) && mag < 0x78 ? (mag * 0x1111) >> 12 : mag;
                a.paceAcc += p;
                const int32_t k = a.paceAcc >> 5;
                a.paceAcc -= k << 5;
                if (a.anim.tod) a.anim.ticksPerFrame = uint8_t((5 - k) * (a.anim.tod->resolution >> 1));
            }
            const int32_t v = std::max(int32_t(uint32_t(spd(r->fwd) * dt * mag) >> 7), 1);
            const int32_t delta = wrap(ratan2(pd.sx, pd.sy) + camYaw() - a.yaw);
            if (!(r->flagsB & 4)) {  // lean
                const int32_t l = std::clamp(wrap((delta >> 3) - a.roll), -0x78, 0x78);
                a.roll = int16_t(wrap(a.roll + l));
            }
            a.turn = int16_t(delta);
            if (r->flagsA & 2) {
                const int32_t f = v * (0x800 - std::abs(delta));
                s[2] += (f < 0 ? f - 0x7ff : f + 0x7ff) >> 11;
            }
        } else {
            a.roll = int16_t(a.roll - (a.roll >> 2));
        }
    } else {
        if (r->flagsA & 2) s[2] += spd(r->fwd) * dt;
        a.roll = int16_t(a.roll - (a.roll >> 2));
    }
    if (r->flagsA & 0x400000) s[2] += spd(r->fwd) * dt;
    if (r->flagsA & 0x800) s[1] -= spd(r->vert) * dt;  // climb / vertical speed (sign I)
    if (const int32_t step = wrap(a.turn) >> 2) {
        a.yaw = int16_t(wrap(a.yaw + step));
        a.turn = int16_t(a.turn - step);
    }
    // 0x45643b: a state with flagsB 0x20000 follows its attach point. On a tick whose frame hold has expired the yaw
    // turns by the attach part's yaw and the mover carries the entity to +0x7c (runtime: thrown template 401, capture
    // tbnpc/B_guard_attack: next position = last tick's attach point).
    if (!par && (r->flagsB & 0x20000) && a.attachPart >= 0 && size_t(a.attachPart) < a.anim.parts.size() && !a.anim.countdown) {
        a.yaw = int16_t(a.yaw + a.anim.parts[size_t(a.attachPart)].rot[1]);
        a.carry = {int16_t(a.carry[0] + a.attachPos.x - a.pos.x), int16_t(a.carry[1] + a.attachPos.y - a.pos.y),
                   int16_t(a.carry[2] + a.attachPos.z - a.pos.z)};
    }
    if (r->flagsA & 1) a.carry = {};
    std::vector<TbActor*> touched;  // actor contacts of the mover (local_f8)
    if (!par) {
        // local -> world: R(yaw) (s << 5), (w + 0x10) >> 5 (§2.1; pitch/roll leave a forward vector unchanged here)
        const int32_t c = rcos(a.yaw), sn = rsin(a.yaw);
        const std::array<int32_t, 3> target{((((c * (s[0] << 5)) + (sn * (s[2] << 5))) >> 12) + 0x10) >> 5, s[1],
                                            ((((-sn * (s[0] << 5)) + (c * (s[2] << 5))) >> 12) + 0x10) >> 5};
        const std::array<uint32_t, 3> before{a.f20, a.f24, a.f28};
        if (r->flagsA & 0x800000) move(a, target, touched);
        else {
            a.vel = {target[0] * 16, target[1] * 16, target[2] * 16};
            a.pos = {a.pos.x + target[0], a.pos.y + target[1], a.pos.z + target[2]};
        }
        a.trig = {a.f20 & ~before[0], a.f24 & ~before[1], a.f28 & ~before[2]};  // the mover's new bits (I)
        if (r->flagsA & 1) a.vel = {};
        if ((a.flags0 & 0x4000000) && a.pos.y > 32000) {  // fell out of the world (exe: s16 wrap): checkpoint
            a.pos = checkpoint == std::array<int16_t, 4>{} ? a.spawn : V3{checkpoint[0], checkpoint[1], checkpoint[2]};
            a.yaw = checkpoint == std::array<int16_t, 4>{} ? a.spawnYaw : checkpoint[3];
            a.vel = {};
            cam.init = false;
        }
    } else {
        a.vel = {};
    }
    contacts(a, touched);
    attackHits(a, touched);

    // Animation (0x469770): continue, or (re)start when the state's anim id differs from the playing one. Packet
    // 0xa00 of each applied TOD frame sets the attach point +0x84 (0x468280).
    uint32_t res = AnimMissing;
    a.animLast = false;
    if (a.model) {
        const Tod* tod0 = a.anim.tod;
        size_t next0 = a.anim.next;
        bool fresh = false;
        if (a.curAnim == r->anim) res = animTick(*a.model, a.anim, 0, 0);
        else if (findAnim(*a.model, r->anim) >= 0) {
            res = animTick(*a.model, a.anim, r->anim, r->mode);
            a.curAnim = r->anim;
            fresh = true;
        } else {
            animTick(*a.model, a.anim, 0, 0);  // absent anim: keep the current clip, report 0x400
        }
        // Trace G (state 21, anim 38: counter 1,1,2,2): 0x2000 is up on the tick the last frame's hold reaches 0 and on
        // the following loop-restart tick (+0x14e 0x3201), never on a fresh start (0x1201).
        a.animLast = (a.anim.frame == a.anim.last && a.anim.countdown == 0 && a.anim.last) || (!fresh && (a.anim.flags & 0x1000));
        if (const Tod* t = a.anim.tod) {
            if (t != tod0 || a.anim.next < next0) next0 = 0;
            for (size_t fr = next0; fr < a.anim.next && fr < t->frames(); ++fr)
                for (uint32_t k = t->frameStart[fr]; k < t->frameStart[fr + 1]; ++k)
                    if (t->packets[k].type == 0xa && t->packets[k].id) a.attachPart = int32_t(t->packets[k].id) - 1;
            // 0x469770: a frame step runs the asset's logic list (footsteps: '`' items) right after applying the frame,
            // before the hold reload (I: the '`' gate needs countdown +0x153 == 0, else holds > 1 tick never sound).
            if (next0 < a.anim.next)
                if (auto lg = animLogic_.find(t); lg != animLogic_.end()) {
                    const uint8_t hold = std::exchange(a.anim.countdown, uint8_t(0));
                    for (TbBlock& b : lg->second) runItems(a, b);  // shared rand() stream (oracle tbrand/rand.csv)
                    a.anim.countdown = hold;
                }
        }
        if (a.attachPart >= 0) a.attachPos = partWorld(a, a.attachPart);  // +0x7c
        // 0x468280 packet 0x900 (without tag-0x61 records, +0x78 = 0): box = the frame's, x/z inflated by +0x3e.
        if (a.boxes.empty() && a.anim.boxSet) {
            const auto& t = a.anim.box;
            a.box = {int16_t(t[0] - a.inflate), t[1], int16_t(t[2] - a.inflate), int16_t(t[3] + a.inflate), t[4],
                     int16_t(t[5] + a.inflate)};
            a.radius = a.anim.radius;
        }
    }
    // Program triggers (0x453de0 tail): bits raised this tick & +0x14/+0x18/+0x1c run block +0x2c first (its event
    // wins), then logic A / B; a state without blocks only runs the program.
    const std::array<uint32_t, 3> tr{a.trig[0] & a.progMask[0], a.trig[1] & a.progMask[1], a.trig[2] & a.progMask[2]};
    const bool fired = tr[0] || tr[1] || tr[2];
    uint32_t ev = 0x80000000;
    if (fired) {
        if (TbBlock* b = a.progBlock ? findBlock(a, a.progBlock) : nullptr) ev = runItems(a, *b);
        a.f20 &= ~tr[0] | 0x20004104u, a.f24 &= ~tr[1], a.f28 &= ~tr[2];
    }
    if (fired || r->logicA || r->logicB) {
        if ((ev & 0x90000000) && r->logicA) ev = runBlock(a, r->logicA);
        if ((ev & 0x90000000) && r->logicB) ev = runBlock(a, r->logicB);
        if (!(ev & 0x90000000)) {
            dispatch(a, ev);
            return;
        }
    }
    r = a.rec();
    if (res == AnimFinished) {
        a.curAnim = 0;
        if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
    } else {
        if (!(r->flagsA & 0x80) || !(a.f20 & 10)) return;
        a.f20 &= ~0xc000000u;
        if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
    }
    dispatch(a, 0x20000000);
}

// Mover 0x4450c0 (§1.5): gravity, k-shift smoothing, falling flags, ground grip, swept-shape slide, contact flags.
void TbWorld::move(TbActor& a, const std::array<int32_t, 3>& target0, std::vector<TbActor*>& touched) {
    const TbStateRec& r = *a.rec();
    std::array<int32_t, 3> target = target0;  // + the carried displacement +0x58 (0x403c60), cleared (0x445c40)
    for (size_t i = 0; i < 3; ++i) target[i] += a.carry[i];
    a.carry = {};
    std::array<int32_t, 3> w{a.vel[0] >> 4, a.vel[1] >> 4, a.vel[2] >> 4};
    if (!(a.f24 & 0x200) && !(a.f20 & 0x200) && !(r.flagsA & 0xa2008601u) && !(r.flagsC & 1)) w[1] += 5 * dt;
    int k[3] = {0, 0, 0};
    if (((r.flagsB & 0x80000000u) && (a.f20 & 1)) || (r.flagsA & 0x80000000u)) k[0] = k[2] = 10;
    if (r.flagsA & 0x80000000u) k[1] = 11;
    if ((r.flagsB & 0x20000) || (r.flagsA & 0x2000400)) k[0] = k[1] = k[2] = 12;
    auto all = [&](int x, int y, int z) { k[0] = x, k[1] = y, k[2] = z; };
    if (a.f20 & 0x80000) all(10, 9, 10);
    else if (a.f20 & 0x4000) all(10, 10, 10);
    else if (r.flagsC & 1) all(9, 9, 9);
    else if (r.flagsC & 8) all(8, 8, 8);
    else if (r.flagsA & 0xa00) all(11, 11, 11);
    else if (r.flagsA & 0x20000000) all(8, 8, 8);
    for (int i = 0; i < 3; ++i)
        if (k[i]) {
            int32_t d = (target[size_t(i)] - w[size_t(i)]) * (1 << k[i]);
            if (d > 0) d += 0xfff;
            w[size_t(i)] += d >> 12;
        }
    if (!(a.f20 & 1)) {
        if (w[1] > 50 && !(a.f20 & 0x200)) a.f20 |= 0x20;
    } else {
        a.f20 = w[1] < 0 ? a.f20 & ~0x400u : a.f20 | 0x400;
    }
    const bool wasGrounded = a.f20 & 2;
    a.f20 &= ~6u;
    a.f28 &= ~2u;
    // Ground grip (0x443f40): tangential velocity matched to the target by grip * friction.
    const int32_t grip = r.grip > 0 && (r.grip & 0x4000) ? int32_t(S[size_t(r.grip & 0x9fff)]) << 5 : r.grip;
    const uint16_t fric = a.groundPoly >= 0 ? col.surfaces[col.polys[size_t(a.groundPoly)].surf].friction : 4096;
    const int32_t g = std::clamp((grip * fric) >> 12, 0, 0x1000);
    auto applyGrip = [&]() {
        for (int i : {0, 2}) w[size_t(i)] += ((target[size_t(i)] - w[size_t(i)]) * g + 0xfff) >> 12;
        if (w[1] > 0) w[1] = std::max(0, w[1] - ((5 * dt * g + 0xfff) >> 12));
    };
    if (wasGrounded) applyGrip();
    // Shape from the actor box +0x60.. (min x, max y, min z, max x, min y, max z), §1.5 step 1: entities with tag-0x61
    // records morph toward the selected one (action 82); the others take TOD packet 0x900 in actorStep's anim step.
    if (a.boxIdx >= 0 && size_t(a.boxIdx) < a.boxes.size()) {  // 0x450460: +-20 per tick toward the record
        for (size_t i = 0; i < 6; ++i) {
            const int16_t t = a.boxes[size_t(a.boxIdx)][i];
            a.box[i] = a.boxInstant ? t : int16_t(a.box[i] < t ? std::min(a.box[i] + 20, int32_t(t)) : std::max(a.box[i] - 20, int32_t(t)));
        }
        a.radius = boxRadius(a.box);
    }
    const auto& b = a.box;
    const int32_t hx = (b[3] - b[0] + 1) >> 1, hy = (b[1] - b[4] + 1) >> 1, hz = (b[5] - b[2] + 1) >> 1;
    const V3 bc = rotY(a.yaw, b[0] + hx, 0, b[2] + hz);
    const V3 boxC{bc.x, b[1] - hy, bc.z};
    TbCollision::Shape shape;
    const int32_t longest = std::max({hx, hy, hz});
    if (a.flags0 & 0x10) shape.r = longest;
    else if ((a.flags0 & 1) && hy == longest) {
        shape.r = std::max(hx, hz);
        shape.half = hy - shape.r;
        if (shape.half < 21) shape.r = longest, shape.half = 0;
    } else shape.r = longest;
    shape.r = std::max(shape.r, 8);
    if (longest < 10) shape = {10, 0};
    V3 centre{a.pos.x + boxC.x, a.pos.y + boxC.y, a.pos.z + boxC.z};
    std::vector<TbCollision::Contact> contacts;
    col.sweep(centre, {w[0], w[1], w[2]}, shape, a.flags10, contacts);
    bool floor = false;
    for (const auto& ct : contacts) {
        const int64_t vn = int64_t(w[0]) * ct.n[0] + int64_t(w[1]) * ct.n[1] + int64_t(w[2]) * ct.n[2];
        if (ct.n[1] < kTbFloorNy) {
            floor = true;
            a.groundPoly = ct.poly;
        } else {
            a.f20 |= 4;
            if (ct.n[1] > 12000 && w[1] < -30) a.f24 |= 0x800000;
        }
        if (vn < 0)
            for (int i = 0; i < 3; ++i) w[size_t(i)] -= int32_t((vn * ct.n[size_t(i)]) / (int64_t(16384) * 16384));
    }
    if (floor) {
        a.f20 = (a.f20 | 2) & ~0x421u;
        if (!wasGrounded) applyGrip();
        if (w[1] > 0) w[1] = 0;
    } else {
        a.groundPoly = wasGrounded && contacts.empty() ? -1 : a.groundPoly;
    }
    // Actor contacts (0x448ce0 candidates: the contact list 0x4d2734 plus listed flags0 0x2000100 entities within
    // |v| + 50 + r): ponytail: the moved shape against each candidate's box (its yaw only), pushed out of solid
    // ones (not flags0 0x2000000); landing on top grounds the mover (8 on flags0-8 platforms) and marks the
    // candidate stood on (+0x20 0x10, I). The original sweeps per shape (0x43cb60/0x449750/0x449960/0x43cdf0).
    // The touched list (local_f8, max 15) gets a solid candidate the sweep runs into (approximated: moving into it
    // or landing on it) and, from the per-shape overlap passes 0x447350/0x447490/0x4476e0/0x447a60, an overlapping
    // candidate only when: it is non-solid (descriptor 0x80), the mover is non-solid and the candidate lacks
    // flags0 0x40000000 (descriptor 0x200), mover +0x24 0x1000 vs candidate flags0 0x10000000, mover flags10
    // 0x400000 vs a player, or a player mover vs candidate flags10 0x400000. Runtime (tbnpc/F_taz_spin): Taz
    // spinning in place next to the ARCA01 guard never raises +0x20 0x40/0x80 on either side.
    auto listed = [&](const TbActor& c) {
        return (c.flags0 & 0x2000000) || ((a.flags0 & 0x2000000) && !(c.flags0 & 0x40000000)) ||
               ((a.f24 & 0x1000) && (c.flags0 & 0x10000000)) || ((a.flags10 & 0x400000) && (c.flags0 & 0x4000000)) ||
               ((a.flags0 & 0x4000000) && (c.flags10 & 0x400000));
    };
    auto touch = [&](TbActor& c) {
        if (touched.size() < 15 && std::ranges::find(touched, &c) == touched.end()) touched.push_back(&c);
    };
    const int32_t reach = isqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) + 50 + shape.r + shape.half;
    for (TbActor* cp : order()) {
        TbActor& c = *cp;
        if (&c == &a || !c.radius || (c.f20 & 0x2000) || !(c.collidable || (c.active && (c.flags0 & 0x2000100) == 0x2000100)))
            continue;
        if (c.parent == int32_t(a.uid) || a.parent == int32_t(c.uid)) continue;
        if ((a.flags10 & 0x1000000) && (c.f20 & 0x40000)) continue;
        const int32_t lim = reach + c.radius;
        if (std::abs(centre.x - c.pos.x) > lim || std::abs(centre.y - c.pos.y) > lim || std::abs(centre.z - c.pos.z) > lim) continue;
        // nearest box point to the shape's core segment (y within ±half), in c's frame
        const V3 l = rotY(-c.yaw, centre.x - c.pos.x, centre.y - c.pos.y, centre.z - c.pos.z);
        const int32_t px = std::clamp(l.x, int32_t(c.box[0]), int32_t(c.box[3]));
        const int32_t pz = std::clamp(l.z, int32_t(c.box[2]), int32_t(c.box[5]));
        const int32_t s0 = l.y - shape.half, s1 = l.y + shape.half, b0 = c.box[4], b1 = c.box[1];
        const int32_t cy = s1 < b0 ? b0 : s0 > b1 ? b1 : std::clamp(l.y, std::max(s0, b0), std::min(s1, b1));
        const int32_t ly = std::clamp(cy, s0, s1);
        const double dx = l.x - px, dy = ly - cy, dz = l.z - pz;
        const double dd = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dd >= shape.r) continue;
        if (listed(c)) touch(c);
        if (c.flags0 & 0x2000000) continue;
        const bool top = l.y + shape.half <= c.box[4] + shape.r / 2;  // core above the box top: standing on it
        if (top) {
            const int32_t lift = l.y + shape.half + shape.r - c.box[4];
            if (lift > 0) centre.y -= lift;
            a.f20 = (a.f20 | (c.flags0 & 8 ? 8 : 2)) & ~0x421u;
            if (w[1] > 0) w[1] = 0, touch(c);
            c.f20 |= 0x10;
        } else if (dd > 0) {
            const double k = (shape.r - dd) / dd;
            const V3 push = rotY(c.yaw, int32_t(std::lround(dx * k)), 0, int32_t(std::lround(dz * k)));
            centre.x += push.x, centre.z += push.z;
            const double nl = std::sqrt(dx * dx + dz * dz);
            if (nl > 0) {  // drop the velocity component into the obstacle
                const V3 n = rotY(c.yaw, int32_t(dx / nl * 4096), 0, int32_t(dz / nl * 4096));
                const int64_t vn = int64_t(w[0]) * n.x + int64_t(w[2]) * n.z;
                if (vn < 0) w[0] -= int32_t(vn * n.x / (4096 * 4096)), w[2] -= int32_t(vn * n.z / (4096 * 4096)), touch(c);
            }
            a.f20 |= 4;
        }
    }
    a.pos = {centre.x - boxC.x, centre.y - boxC.y, centre.z - boxC.z};
    for (int i = 0; i < 3; ++i) a.vel[size_t(i)] = std::clamp(w[size_t(i)], -2047, 2047) * 16;
}

// ------------------------------------------------------------------------------------------------ camera
int16_t TbWorld::camYaw() const { return int16_t(ratan2(cam.look.x - cam.eye.x, cam.look.z - cam.eye.z)); }

// Camera rest pose. Runtime trace (ARCA01 level start, local/wine/tbplay/A_idle_jump.csv): look = T - (0,300,0), eye
// 1203 behind the yaw horizontally and 359 above T, i.e. 1200 from the spring aim T - (0,400,0). Used for the initial
// placement and for snaps (0x437180's "100 behind" reset was not observed and does not settle in this model).
void TbWorld::cameraSnap(const TbActor& t) {
    cam.look = {t.pos.x, t.pos.y - 300, t.pos.z};
    const V3 back = rotY(t.yaw, 0, 0, 1199);
    cam.eye = {t.pos.x - back.x, t.pos.y - 359, t.pos.z - back.z};
    const int64_t lx = cam.look.x - cam.eye.x, ly = cam.look.y - cam.eye.y, lz = cam.look.z - cam.eye.z;
    const int32_t ll = std::max(1, len3(lx, ly, lz));
    cam.body = {cam.eye.x + int32_t(lx * 400 / ll), cam.eye.y + int32_t(ly * 400 / ll), cam.eye.z + int32_t(lz * 400 / ll)};
    cam.vel = {};
    cam.acc = 0;
}

void TbWorld::cameraTick() {
    const TbActor& F = actors[size_t(camFocus)];
    const TbActor& Pn = actors[size_t(1 - camFocus)];
    const bool two = A4 & 0x10;
    if (!cam.init) {
        cam.init = true;
        cam.flags = two ? 0x200u | 0x100 : 0x280u;
        cam.dist = 1200;
        cam.blend = 0x1000;
        cameraSnap(F);
        cam.lastFocus = camFocus;
    }
    if (camScript.flags & 0x300) return;  // held by a model camera node (0x405b50): no follow
    auto d2 = [](const V3& a, const V3& b) {
        const int64_t x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
        return x * x + y * y + z * z;
    };
    if (cam.lastFocus != camFocus) {  // retarget (runtime: T jumps to the new focus, L glides through f8)
        const V3 eyeAt{F.pos.x, F.pos.y - 300, F.pos.z};
        if (d2(cam.body, F.pos) >= int64_t(2500) * 2500 || col.query(eyeAt, {cam.body.x - eyeAt.x, cam.body.y - eyeAt.y, cam.body.z - eyeAt.z}))
            cameraSnap(F);
        cam.lastFocus = camFocus;
    }
    if ((cam.flags & 0x80) && d2(cam.body, F.pos) >= int64_t(2500) * 2500) cameraSnap(F);
    cam.blend += (0x1000 - cam.blend) >> 3;
    V3 T;
    auto lerp = [](int32_t a, int32_t b, int32_t t, int sh) { return a + int32_t((int64_t(b - a) * t) >> sh); };
    if (!(cam.flags & 0x80))
        T = cam.blend == 0x1000 ? V3{(F.pos.x + Pn.pos.x) / 2, (F.pos.y + Pn.pos.y) / 2, (F.pos.z + Pn.pos.z) / 2}
                                : V3{lerp(F.pos.x, Pn.pos.x, cam.blend, 13), lerp(F.pos.y, Pn.pos.y, cam.blend, 13), lerp(F.pos.z, Pn.pos.z, cam.blend, 13)};
    else
        T = cam.blend == 0x1000 ? F.pos
                                : V3{lerp(cam.saved.x, F.pos.x, cam.blend, 12), lerp(cam.saved.y, F.pos.y, cam.blend, 12), lerp(cam.saved.z, F.pos.z, cam.blend, 12)};
    const TbPad& fp = padOf(F);
    const bool l2 = fp.held & PadL2, r2 = fp.held & PadR2;
    if (l2 != r2 && !(fp.held & PadL1)) cam.acc += l2 ? 0x5000 : -0x5000;  // direction Inferred
    if ((cam.flags & 0x100) && !(cam.flags & 0x80)) {  // 2P framing: keep feet and heads within ±190 x ±90
        const double fx = cam.look.x - cam.eye.x, fy = cam.look.y - cam.eye.y, fz = cam.look.z - cam.eye.z;
        const double fl = std::max(1.0, std::sqrt(fx * fx + fy * fy + fz * fz));
        const double f[3] = {fx / fl, fy / fl, fz / fl};
        double rt[3] = {f[2], 0, -f[0]};
        const double rl = std::max(1e-6, std::sqrt(rt[0] * rt[0] + rt[2] * rt[2]));
        rt[0] /= rl, rt[2] /= rl;
        const double up[3] = {f[1] * rt[2] - f[2] * rt[1], f[2] * rt[0] - f[0] * rt[2], f[0] * rt[1] - f[1] * rt[0]};
        int32_t e = 0;
        constexpr double kH = 248.0;  // projection distance: fit to the 2P runtime trace (F_2p_granny.csv, D +16/+28/+38)
        for (const TbActor* a : {&F, &Pn})
            for (int32_t yoff : {0, int32_t(a->box[4])}) {
                const double p[3] = {double(a->pos.x - cam.eye.x), double(a->pos.y + yoff - cam.eye.y), double(a->pos.z - cam.eye.z)};
                const double z = p[0] * f[0] + p[1] * f[1] + p[2] * f[2];
                if (z < 1) { e = 40; continue; }
                const double sx = (p[0] * rt[0] + p[2] * rt[2]) * kH / z, sy = (p[0] * up[0] + p[1] * up[1] + p[2] * up[2]) * kH / z;
                e = std::max({e, int32_t(std::abs(sx) - 190), int32_t(std::abs(sy) - 90)});
            }
        e = std::min(e, 40);
        if (e <= 0) cam.dist = std::max(1200, cam.dist - 30);
        else if (e > 15) cam.dist += e;
        const int64_t dx = F.pos.x - Pn.pos.x, dz = F.pos.z - Pn.pos.z;
        if (cam.dist > 3000 || dx * dx + dz * dz > int64_t(2700) * 2700 || std::abs(F.pos.y - Pn.pos.y) > 1000) {
            cam.flags |= 0x80, cam.blend = 0, cam.saved = T, cam.splitTicks = 0;
        }
    } else if (cam.flags & 0x100) {  // 2P split: single focus until the players regroup
        cam.dist -= (cam.dist - 1200) >> 2;
        const int64_t dx = F.pos.x - Pn.pos.x, dz = F.pos.z - Pn.pos.z;
        if (++cam.splitTicks > 30 && dx * dx + dz * dz < int64_t(2300) * 2300 && std::abs(F.pos.y - Pn.pos.y) < 1000)
            cam.flags &= ~0x80u, cam.blend = 0;
    }
    // Spring (¼ of the distance error per tick) measured from the eye to the aim T - (0,400,0) and applied to the body
    // (fits the runtime trace: rest |aim - eye| = 1203, switch tick body step (35, -3, 12) = spring (36, -1, 13)).
    const V3 aim{T.x, T.y - 400, T.z};
    const int64_t dx = aim.x - cam.eye.x, dy = aim.y - cam.eye.y, dz = aim.z - cam.eye.z;
    const int32_t dl = std::max(1, len3(dx, dy, dz));
    const int32_t e = (dl - cam.dist) >> 2;
    V3 mv{int32_t(dx * e / dl) + (cam.vel[0] >> 12), int32_t(dy * e / dl) + (cam.vel[1] >> 12), int32_t(dz * e / dl) + (cam.vel[2] >> 12)};
    for (int32_t& v : cam.vel) v -= (v * 400) >> 12;
    // Airborne vertical follow (runtime: jump tick eye.y -10 vs model -9). The §4.3 20-tick dead band was not observed
    // (after landing the eye keeps its raised height), so it is not applied.
    if (F.f20 & 0x421) mv.y -= fN(cam.look.y - T.y + 300, 4);
    if (cam.acc >> 12 != 0 && cam.acc >> 12 != -1) {  // L2/R2 orbit about the target
        const V3 rel{cam.body.x - T.x, cam.body.y - T.y, cam.body.z - T.z};
        const V3 rot = rotY(cam.acc >> 12, rel.x, rel.y, rel.z);
        mv.x += rot.x - rel.x, mv.z += rot.z - rel.z;
        cam.acc -= (cam.acc * 400) >> 12;
    }
    std::vector<TbCollision::Contact> ct;
    col.sweep(cam.body, mv, {300, 0}, 0, ct);
    cam.look.x -= fN(cam.look.x - T.x, 8);
    cam.look.z -= fN(cam.look.z - T.z, 8);
    cam.look.y -= fN(cam.look.y - T.y + 300, e < 0 ? 5 : 4);
    const int64_t lx = cam.look.x - cam.body.x, ly = cam.look.y - cam.body.y, lz = cam.look.z - cam.body.z;
    const int32_t ll = std::max(1, len3(lx, ly, lz));
    cam.eye = {cam.body.x - int32_t(lx * 400 / ll), cam.body.y - int32_t(ly * 400 / ll), cam.body.z - int32_t(lz * 400 / ll)};
}

} // namespace bb::game
