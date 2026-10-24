#include "bb/game/tb_world.h"
#include "bb/mods.h"
#include "bb/tb_actor.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
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
// fN(x) of the camera (§4.3): |x|*N < 4096 ? x*|x|*N >> 12 : x.
int32_t fN(int32_t x, int32_t n) { return std::abs(x) * n < 4096 ? int32_t((int64_t(x) * std::abs(x) * n) >> 12) : x; }
int32_t len3(int64_t x, int64_t y, int64_t z) { return int32_t(std::lround(std::sqrt(double(x * x + y * y + z * z)))); }
int32_t lenFast(int32_t x, int32_t y, int32_t z) {  // 0x404060: the largest + (the other two) / 4
    x = std::abs(x), y = std::abs(y), z = std::abs(z);
    const int32_t hi = std::max(x, y), lo = std::min(x, y);
    return hi <= z ? ((hi + lo) >> 2) + z : ((z + lo) >> 2) + hi;
}

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
    for (const TbChunk* src : chain)  // +0x98: the asset links of the first chunk of the chain that has any
        if (a.links.empty())
            for (const TbTag& t : src->tags)
                if (t.id == 0x0f) a.links.push_back(le16(t.data, 0));
    for (auto& [bid, items] : a.blocks) linkSkips(items);
    for (const TbTag& t : own.tags) {
        const std::span<const uint8_t> d = t.data;
        switch (t.id) {
        case 0x0b: a.texLink = le16(d, 0); break;  // +0x168: the texture link 0x465c80 writes
        case 0x10: a.spawn = a.pos = {s16(d, 0), s16(d, 4), s16(d, 8)}; break;
        case 0x11: a.pitch = s16(d, 0), a.spawnYaw = a.yaw = s16(d, 2), a.roll = s16(d, 4); break;  // raw (0x4347c0): 0x1000 stays 0x1000
        case 0x12: a.scale = {s16(d, 0), s16(d, 4), s16(d, 8)}; break;
        case 0x16:
            a.flags0 = le32(d, 0), a.flags1 = le32(d, 4), a.flags10 = le32(d, 8);
            a.progBlock = le16(d, 12);
            a.progMask = {le32(d, 16), le32(d, 20), le32(d, 24)};
            break;
        case 0x1b: a.box[0] = s16(d, 0), a.box[1] = s16(d, 2), a.box[2] = s16(d, 4); break;
        case 0x1c: a.box[3] = s16(d, 0), a.box[4] = s16(d, 2), a.box[5] = s16(d, 4), a.radius = le16(d, 6); break;
        case 0x1d:  // +0x192 (classes 14 / 0x1e / 5 / 0), class 8 +0x16c (not kept), others +0x178
            if (a.behaviour == 0x0e || a.behaviour == 0x1e || a.behaviour == 5 || a.behaviour == 0) a.turnRate = s16(d, 0);
            else a.arg = s16(d, 0);
            break;
        case 0x1e: a.hp = s16(d, 0); break;
        case 0x37:  // +0x3e inflation of the box x/z (and of later 0x61 records)
            a.inflate = s16(d, 0), a.mass = a.weight = s16(d, 2), a.f38 = s16(d, 4), a.f3a = s16(d, 6), a.f40 = s16(d, 8);
            a.box[3] = int16_t(a.box[3] + a.inflate), a.box[5] = int16_t(a.box[5] + a.inflate);
            a.box[0] = int16_t(a.box[0] - a.inflate), a.box[2] = int16_t(a.box[2] - a.inflate);
            a.radius = uint16_t(a.radius + a.inflate);
            break;
        case 0x39:
            a.view = le16(d, 0), a.range = le16(d, 2), a.actBlock = uint16_t(le32(d, 4));
            a.lod1 = d.size() > 8 ? d[8] : 0, a.lod2 = d.size() > 10 ? d[10] : 0;
            break;
        case 0x65: a.gdBit = le16(d, 0); break;
        case 0x45: a.shadowId = le16(d, 0); break;  // +0x36 shadow entity id
        case 0x38: a.drawBias = int8_t(d[0] + 0x10); break;
        case 0x1f: a.zone = d[0]; break;
        case 0x61:
            a.boxes.push_back({int16_t(s16(d, 2) - a.inflate), s16(d, 4), int16_t(s16(d, 6) - a.inflate),
                               int16_t(s16(d, 8) + a.inflate), s16(d, 10), int16_t(s16(d, 12) + a.inflate)});
            break;
        default: break;
        }
    }
    // The tag 0x1b / 0x1c box stays until a first tick selects tag-0x61 record +0x123 (0x450440(a, 0): classes 0, 5,
    // 14, 33); until then the mover's sphere is built from it (TRQ02 309 unticked: (-548, 0, -199)..(194, -519, 347)).
}

constexpr uint32_t kNoEvent = 0x50000000;  // block ended true
constexpr uint32_t kFalse = 0x10000000;    // block ended false

// Items that create entities: V (0x45ad70 non-0x80000000 branch, bits 0x810) template p3; '^' (0x45cc40, bits 1 2
// 0x200 0x400 0x20000) template @8.
void spawnTemplates(const TbBlock& b, std::set<uint16_t>& out) {
    for (const TbLogicItem& it : b) {
        if (it.tag == 0x56 && !(le32(it.d, 0) & 0x80000000u) && (le32(it.d, 0) & 0x810)) out.insert(le16(it.d, 12));
        if (it.tag == 0x5e && (le32(it.d, 4) & 0x20603)) out.insert(le16(it.d, 8));
    }
}

bool isInstance(const TbChunk& ch) { return ch.type == 0x07 || ch.type == 0x0a || ch.type == 0x63; }

} // namespace

uint8_t tbJoyAxis(uint8_t axis) {
    // MulDiv rounds half away from zero; device 0..255 (centre 127), dead zone MulDiv(2000, 255, 10000) = 51.
    auto mulDiv = [](int a, int b, int c) { const int n = a * b; return (n >= 0 ? n + c / 2 : n - c / 2) / c; };
    constexpr int kDead = 51, kRange = 255 - kDead;
    int v = int(axis) - 127;
    if (std::abs(v) <= kDead / 2) v = 0;
    else v += v < 0 ? kDead / 2 : -kDead / 2;
    const int r = std::clamp(mulDiv(v, 128 - -127, kRange), -127, 128);
    return uint8_t(std::min(r + 0x80, 0xff));
}

void tbPadTick(TbPad& p, const PadInput& in, uint16_t edgeMask, std::span<const uint8_t> remap, bool paused) {
    p.prevHeld = p.held;
    p.prevRaw = p.raw;
    uint16_t raw = in.buttons;
    uint8_t bx = 0x80, by = 0x80;
    if (in.analog) {  // 0x40e710: the stick also sets the d-pad bits near its ends
        bx = tbJoyAxis(in.lx), by = tbJoyAxis(in.ly);
        if (bx < 0x10) raw |= PadLeft;
        if (bx > 200) raw |= PadRight;
        if (by < 0x10) raw |= PadUp;
        if (by > 200) raw |= PadDown;
    }
    p.raw = raw;
    auto mapped = [&](uint16_t h) {  // physical □ ○ △ ✕ L1 L2 R1 R2 -> remap words +0x18..+0x26
        static constexpr uint16_t kBits[8] = {PadSquare, PadCircle, PadTriangle, PadCross, PadL1, PadL2, PadR1, PadR2};
        uint16_t lo = 0;
        for (size_t k = 0; k < 8; ++k)
            if ((h & kBits[k]) && 2 * k + 1 < remap.size()) lo |= uint16_t(remap[2 * k] | remap[2 * k + 1] << 8);
        return uint16_t((h & 0xff00) | lo);
    };
    const bool stick = in.analog && !(raw & 0xf000);
    p.held = stick || !paused ? mapped(raw) : raw;
    p.pressed = p.held & ~(p.prevHeld & edgeMask);
    if (stick) {  // 0x460f80: the direction bits go into every word, not edge-masked
        auto axis = [](uint8_t b, bool flip) -> int {
            const int d = b < 0x4d ? (b - 0x4d) * 128 : b > 0xb1 ? (b - 0xb2) * 128 : 0;
            return (flip ? -d : d) / 78;
        };
        const uint16_t dir = uint16_t((bx < 0x4d ? PadLeft : bx > 0xb1 ? PadRight : 0) | (by < 0x4d ? PadUp : by > 0xb1 ? PadDown : 0));
        p.held |= dir, p.pressed |= dir, p.raw |= dir;
        p.sx = int8_t(axis(bx, false));
        p.sy = int8_t(axis(by, true));
    } else {
        // Digital: Right / Up 0x7f, Left / Down -0x80 (oracle peek of GD+0x14/0x15, local/wine/compare/w12/tb/water.csv).
        p.sx = int8_t(raw & PadLeft ? -0x80 : raw & PadRight ? 0x7f : 0);
        p.sy = int8_t(raw & PadUp ? 0x7f : raw & PadDown ? -0x80 : 0);
    }
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

std::vector<const ActorModel*> TbWorld::builtModels() const {
    std::vector<const ActorModel*> out;
    auto add = [&](const TbActor& a) {
        if (a.ownModel && std::ranges::find(out, a.ownModel.get()) == out.end()) out.push_back(a.ownModel.get());
    };
    for (const TbActor& a : entities) add(a);
    for (const auto& [id, t] : templates_) add(t);
    return out;
}

TbWorld TbWorld::load(const BzeFile& bze, const Scene& scene, const std::array<uint8_t, 0x300>* gd, uint8_t language) {
    TbWorld w;
    w.col = TbCollision::fromLevel(bze);
    const int i1 = bze.find(1);
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const TbScript script = parseTbScript(s1);
    std::map<uint16_t, const TbChunk*> templates, firstInstance;
    struct Asset { uint16_t role = 0; const TbChunk* ch = nullptr; std::span<const uint8_t> tod, model; };
    std::map<uint16_t, Asset> assets;
    // Sounds (level loader chunk 0, as LiT): 0x01 programs (section 4), 0x02 bank (0x429e50, section 3), 0x04 sound
    // (0x406ff0, 12 B), 0x4b / 0x4c speech programs / bank: entry GD+0x93 of 7 × {off, size} in the language section
    // that command 0x4a reads (EN 6, FR 5, DE..NL 7..11). GD+0x93 = the carried one, else the new game's 0x401ad0.
    const size_t lang = gd ? (*gd)[0x93] : language;
    const size_t L = lang > 6 ? 0 : lang;  // 0x4347c0 head: > 6 -> 0
    static constexpr int kLangSection[7] = {6, 5, 7, 8, 9, 10, 11};
    const int i3 = bze.find(3), i4 = bze.find(4), il = bze.find(kLangSection[L]);
    std::vector<uint8_t> s3, s4, sp;
    if (i3 >= 0) s3 = bze.payload(size_t(i3));
    if (i4 >= 0) s4 = bze.payload(size_t(i4));
    if (il >= 0) sp = bze.payload(size_t(il));
    auto part = [](std::span<const uint8_t> sec, std::span<const uint8_t> ref) {
        const uint32_t off = le32(ref, 0), size = le32(ref, 4);
        return size_t(off) + size <= sec.size() ? sec.subspan(off, size) : std::span<const uint8_t>{};
    };
    auto entry = [L](std::span<const uint8_t> ref) { return ref.size() >= 8 * (L + 1) ? ref.subspan(8 * L) : ref.first(0); };
    w.sfx.tb = true, w.sfx.volume = 128;  // GD+0x97 default 0x80 (oracle w13 swim capture)
    for (const TbChunk& ch : script.chunks) {
        if (ch.type == 0x00)
            for (const TbTag& t : ch.tags) {
                if (t.id == 0x04) w.sfx.addEntry(t.data);
                if (t.id == 0x01) w.sfx.programs = Sfx::programTable(part(s4, t.data));
                if (t.id == 0x02) w.sfx.bank = Sfx::loadBank(part(s3, t.data), BzeVariant::TimeBusters);
                if (t.id == 0x4b && !entry(t.data).empty()) w.sfx.speechPrograms = Sfx::programTable(part(sp, entry(t.data)));
                if (t.id == 0x4c && !entry(t.data).empty()) w.sfx.speech = Sfx::loadBank(part(sp, entry(t.data)), BzeVariant::TimeBusters);
            }
        const auto id = ch.tag(0x27);
        if (ch.type == 0x08 && !id.empty()) templates.emplace(le16(id, 0), &ch);
        if (isInstance(ch) && !id.empty()) firstInstance.emplace(le16(id, 0), &ch);
        // 0x4347c0 tag 0x16: every record (instance or template) with flags0 0x4000000 and flags1 0x400 becomes
        // 0x4d236c, so a later template (181, a Bugs copy: VIQ01, GRANNY, TRCA01 …) is the load placement's target.
        if (const auto d = ch.tag(0x16); (ch.type == 0x08 || isInstance(ch)) && d.size() >= 8 && (le32(d, 0) & 0x4000000) && (le32(d, 4) & 0x400))
            w.loadAtOrigin = ch.type == 0x08;
        if (ch.type == 0x22 && !id.empty()) assets[le16(id, 2)] = {le16(id, 0), &ch, ch.tag(0x25), ch.tag(0x24)};
        if (const auto p = ch.tag(0x3c); ch.type == 0x3a && !p.empty())  // §4.3
            w.edgeMask = le16(p, 12), w.nearClip = le16(p, 0), w.viewDist2 = le16(p, 10);
        if (const auto p = ch.tag(0x3d); ch.type == 0x3a && !p.empty())  // pool sizes (0x4347c0 case 0x3d: each + 5)
            w.shapeRecs_ = s16(p, 0x1c), w.viewCap_ = s16(p, 0xc), w.collCap_ = s16(p, 0x20), w.pathFree_ = le16(p, 0x1e),
            w.poolCap = le16(p, 6) + 5;
        if (ch.type != 0x22 && ch.type != 0x08) parseBlocks(ch, nullptr, w.globalBlocks_);  // global blocks only
        if (ch.type == 0x09) {  // trigger zone (loader 0x4347c0 case 9, list 0x4d2760 appended)
            Zone& z = w.zones_.emplace_back();
            if (const auto d = ch.tag(0x10); !d.empty()) z.origin = {s16(d, 0), s16(d, 4), s16(d, 8)};
            if (const auto d = ch.tag(0x11); !d.empty()) z.rot = {s16(d, 0), s16(d, 2), s16(d, 4)};
            if (const auto d = ch.tag(0x16); !d.empty()) z.flags8 = le32(d, 0), z.flagsC = le32(d, 4);
            if (const auto d = ch.tag(0x1c); !d.empty()) z.extX = s16(d, 0), z.height = s16(d, 2), z.extZ = s16(d, 4);
            TbBlock* cur = nullptr;
            for (const TbTag& t : ch.tags) {  // blocks < 10000 pushed at the head of +0x28
                if (t.id == 0x4d) {
                    cur = le16(t.data, 0) < 10000 ? &z.blocks.emplace(z.blocks.begin(), le16(t.data, 0), TbBlock{})->second : nullptr;
                } else if (t.id >= 0x4e && t.id <= 0x60 && cur) {
                    TbLogicItem it;
                    it.tag = t.id;
                    std::copy_n(t.data.begin(), std::min(t.data.size(), it.d.size()), it.d.begin());
                    cur->push_back(it);
                } else {
                    cur = nullptr;
                }
            }
            for (auto& [id, b] : z.blocks) linkSkips(b);
        }
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
    for (const TbChunk& ch : script.chunks) {  // chunk 0x20 zone models (0x4d5040[tag 0x21])
        const auto id = ch.tag(0x21), ref = ch.tag(0x24);
        if (ch.type != 0x20 || id.empty() || ref.empty() || le16(id, 0) >= w.zoneModels_.size()) continue;
        const std::span<const uint8_t> m = region(ch, ref);
        if (m.size() < 0x28) throw FormatError("tb world: zone model too short");
        ZoneModel& z = w.zoneModels_[le16(id, 0)];
        z.present = true;
        for (size_t i = 0; i < 6; ++i) z.box[i] = s16(m, 0x1c + 2 * i);
        if (const uint32_t extra = le32(m, 0xc)) {
            if (extra + 4 > m.size()) throw FormatError("tb world: zone portal list outside its model");
            const uint32_t n = le32(m, extra);
            if ((m.size() - extra - 4) / 0x24 < n) throw FormatError("tb world: zone portal list outside its model");
            for (uint32_t k = 0; k < n; ++k) {
                const std::span<const uint8_t> e = m.subspan(extra + 4 + k * 0x24, 0x24);
                ZonePortal& p = z.portals.emplace_back();
                p.zone = int8_t(e[3]);
                for (size_t v = 0; v < 5; ++v) p.pt[v] = {s16(e, 4 + 6 * v), s16(e, 6 + 6 * v), s16(e, 8 + 6 * v)};
            }
        }
    }
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
        if (!rig && m->anims.empty()) return it->second = nullptr;
        mods::applyToModel(scene, *m);  // mod model files / swaps / clips (no-op without mods), before addLogic keys the clips
        return it->second = std::move(m);
    };
    // An asset chunk's 'M' blocks (encoder 0x433ab0 order: the asset's logic list +0x14).
    auto assetBlocks = [&](const TbChunk& ch) {
        std::vector<TbBlock> logic;
        bool open = false;
        for (const TbTag& t : ch.tags) {
            if (t.id == 0x4d) logic.emplace_back(), open = true;
            else if (open && t.id >= 0x4e && t.id <= 0x60) {
                TbLogicItem li;
                li.tag = t.id;
                std::copy_n(t.data.begin(), std::min(t.data.size(), li.d.size()), li.d.begin());
                logic.back().push_back(li);
            } else open = false;
        }
        for (TbBlock& b : logic) linkSkips(b);
        return logic;
    };
    // The asset blocks of each anim of a model: anim role -> the entity's first tag-0x0f link with that role and a TOD
    // (as the anims were built), along the template chain (instances often link none).
    auto addLogic = [&](const ActorModel& m, const std::vector<const TbChunk*>& chain) {
        for (const ActorAnim& an : m.anims)
            for (const TbTag& tg : chain | std::views::transform([](const TbChunk* c) -> const std::vector<TbTag>& {
                                       return c->tags;
                                   }) | std::views::join) {
                if (tg.id != 0x0f) continue;
                const auto as = assets.find(le16(tg.data, 0));
                if (as == assets.end() || as->second.role != an.id || as->second.tod.empty()) continue;
                if (std::vector<TbBlock> logic = assetBlocks(*as->second.ch); !logic.empty()) w.animLogic_.try_emplace(&an.tod, std::move(logic));
                break;
            }
    };
    // Sprite-sequence assets: tag 0x3f records (block +4 -> {u16 @2 count, 16-byte records @+0xc}), tag 0x3e flipbook
    // {u16 n, u16 0, u32 off, u32 size} -> n × {u8 record, u8 ticks}, the asset blocks.
    for (const auto& [aid, as] : assets) {
        const auto rec = as.ch->tag(0x3f), fl = as.ch->tag(0x3e);
        if (rec.empty() && fl.size() < 12) continue;
        SpriteAsset& sa = w.spriteAssets_[aid];
        sa.role = as.role;
        if (!rec.empty()) {
            const auto b = region(*as.ch, rec);
            const size_t ph = le32(b, 4);
            for (size_t i = 0, n = le16(b, ph + 2); i < n; ++i) {
                const size_t r = ph + 0xc + 16 * i;
                sa.recs.push_back({le16(b, r + 6), le16(b, r + 10), le16(b, r + 0xc), le16(b, r + 0xe)});
            }
        }
        if (fl.size() >= 12) {
            const auto b = region(*as.ch, fl.subspan(4));
            for (size_t i = 0, n = le16(fl, 0); i < n && 2 * i + 1 < b.size(); ++i) sa.steps.push_back({b[2 * i], b[2 * i + 1]});
        }
        sa.logic = assetBlocks(*as.ch);
    }
    // Every instance chunk is an entity of the list 0x4d4fd0 in file order (the loader appends, 0x4609b0); the pool
    // slots: 0..16 the view / world entities (0x460a40, 0x462030), then each chunk 0x07 / 0x08 / 0x0a / 0x63 its own.
    w.slotUsed.assign(17, true);
    bool found[2] = {};
    for (const TbChunk& ch : script.chunks) {
        if (ch.type == 0x08) w.slotUsed.push_back(true);
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
        a.slot = int(w.slotUsed.size());
        w.slotUsed.push_back(true);
        for (const ActorInstance& ai : scene.actors)
            if (ai.scriptOffset == ch.offset && ai.model < scene.actorModels.size()) a.model = &scene.actorModels[ai.model];
        if (!a.model && (a.ownModel = modelOf(ch, true))) a.model = a.ownModel.get();
        if (a.model) addLogic(*a.model, chainOf(ch));
    }
    if (w.poolCap < (1 << 30)) w.slotUsed.resize(std::max(w.slotUsed.size(), size_t(w.poolCap)), false);
    if (!found[0] || !found[1]) throw FormatError("tb world: Bugs (id 2) and Taz (id 3) entities required");
    for (auto& [bid, items] : w.globalBlocks_) linkSkips(items);
    // Spawnable templates (V 0x10/0x800 targets) reachable from any logic: prototype entity (0x460b00 copies it
    // whole) with its model, rig and anims.
    // 0x18: overlay text (S30 / R 0x22, 0x4680b0); 0x1d: pause menu (0x4014c0); 0x1f: HUD sheet holder (0x462430)
    std::set<uint16_t> want{0x18, 0x1d, 0x1f}, done;
    for (TbActor* a : w.order())
        for (const auto& [bid, b] : a->blocks) spawnTemplates(b, want);
    for (const auto& [bid, b] : w.globalBlocks_) spawnTemplates(b, want);
    auto animSpawns = [&] {  // '^' items live mostly in the anim assets' logic
        for (const auto& [tod, blocks] : w.animLogic_)
            for (const TbBlock& b : blocks) spawnTemplates(b, want);
        for (const auto& [aid, sa] : w.spriteAssets_)
            for (const TbBlock& b : sa.logic) spawnTemplates(b, want);
    };
    animSpawns();
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
        animSpawns();
        w.templates_.emplace(t, std::move(proto));
    }
    // Effect sprites (0x451d70) and rig-swap sources (0x469320) by entity id: the first chunk with the id in the entity
    // list 0x4d26d0 (file order); the sprite is its first class-0x17 link, frames = its record 0 byte 2.
    for (const auto& [aid, as] : assets) w.assetRole_[aid] = as.role;
    for (const TbChunk& ch : script.chunks) {
        const auto id = ch.tag(0x27);
        if ((!isInstance(ch) && ch.type != 0x08) || id.empty() || w.linksById_.contains(le16(id, 0))) continue;
        std::vector<uint16_t> links;  // as loadEntity's +0x98
        for (const TbChunk* src : chainOf(ch))
            if (links.empty())
                for (const TbTag& t : src->tags)
                    if (t.id == 0x0f) links.push_back(le16(t.data, 0));
        w.linksById_[le16(id, 0)] = links;
        TbWorld::FxSprite& fs = w.fxSprites_[le16(id, 0)];
        for (uint16_t l : links) {
            const auto as = assets.find(l);
            const auto seq = as == assets.end() || as->second.role != 0x17 ? std::span<const uint8_t>{} : as->second.ch->tag(0x3f);
            if (seq.empty()) continue;
            const auto b = region(*as->second.ch, seq);
            const size_t ph = le32(b, 4);
            fs = {l, ph + 0xe < b.size() ? b[ph + 0xe] : uint8_t(0)};
            break;
        }
    }
    // Shadow models (0x464480): the first role-12 asset of the shadow entity's links, its model at asset +0x18.
    for (const auto& [id, links] : w.linksById_)
        for (uint16_t l : links)
            if (const auto as = assets.find(l); as != assets.end() && as->second.role == 12) {
                if (!as->second.model.empty())
                    w.shadowModels_[id] = std::make_shared<ActorModel>(buildTbActorModel(scene, region(*as->second.ch, as->second.model)));
                break;
            }
    // Rig-swap objects for every (model list, rig list, k) a swap can ask for: each entity's own lists (LOD k 0..2,
    // view flag 0x400) and the V 0x80020000 sources by id (k = p3, then their LODs).
    auto rigPair = [&](const std::vector<uint16_t>& ml, const std::vector<uint16_t>& rl, int k) {
        const auto pick = [&](const std::vector<uint16_t>& l, uint16_t role) -> const Asset* {
            for (uint16_t x : l)
                if (auto as = assets.find(x); as != assets.end() && as->second.role == role) return &as->second;
            return nullptr;
        };
        const Asset* m = pick(ml, uint16_t(13 + k));
        const Asset* r = pick(rl, uint16_t(1 + k));
        if (!m || !r || m->model.empty() || r->tod.empty()) return;
        const std::pair<uint16_t, uint16_t> key{le16(m->ch->tag(0x27), 2), le16(r->ch->tag(0x27), 2)};
        if (w.rigMeshes_.contains(key)) return;
        auto built = std::make_shared<ActorModel>(buildTbActorModel(scene, region(*m->ch, m->model)));
        built->skeleton = parseTbTod(region(*r->ch, r->tod));
        built->modelAsset = key.first, built->skeletonAsset = key.second;
        mods::applyToModel(scene, *built);
        w.rigMeshes_.emplace(key, std::move(built));
    };
    auto rigSources = [&](const TbActor& a) {
        if (a.view & 0x400)
            for (int k = 0; k < 3; ++k) rigPair(a.links, a.links, k);
        for (const auto& [bid, b] : a.blocks)
            for (const TbLogicItem& it : b) {
                if (it.tag != 0x56 || (le32(it.d, 0) & 0x80020000u) != 0x80020000u) continue;
                auto src = [&](uint32_t p) {
                    const auto l = p ? w.linksById_.find(uint16_t(p)) : w.linksById_.end();
                    return l == w.linksById_.end() ? a.links : l->second;
                };
                const auto ml = src(le32(it.d, 4)), rl = src(le32(it.d, 8));
                rigPair(ml, rl, int(it.d[12]));
                for (int k = 0; k < 3; ++k) rigPair(ml, rl, k);
            }
    };
    for (TbActor* a : w.order()) rigSources(*a);
    for (const auto& [t, proto] : w.templates_) rigSources(proto);
    // Camera track paths (list 0x4d4fb4): recorded by a 0x453de0 entity with flags1 0x40 on its first tick. No PC
    // level has one (allLevels test), which leaves '_' path ops and flagsB 0x100 path steering without a path.
    auto recorder = [](const TbActor& a) { return a.behaviour == 14 && (a.flags1 & 0x40); };
    if (std::ranges::any_of(w.actors, recorder) || std::ranges::any_of(w.templates_, [&](const auto& t) { return recorder(t.second); }))
        w.unsupported.insert("camera track path");
    // Overlay text block (chunk 0x48 tag 0x49/0x68, entry GD+0x93 of 7 x {off, size} into section 3) copied to
    // 0x4d5b88, and the HUD sheet frame sizes (0x462430).
    for (const TbChunk& ch : script.chunks) {
        if (ch.type != 0x48) continue;
        auto ref = ch.tag(0x49);
        if (ref.empty()) ref = ch.tag(0x68);
        if (const auto e = entry(ref); !e.empty()) {
            const auto block = region(ch, e);
            w.textBlock_.assign(block.begin(), block.end());
            for (auto& p : w.panelState_) p[0] = 0x20;  // 0x4660c0: panels idle at origin 0
        }
        break;
    }
    for (const int32_t t : scene.hudSheet)
        w.hudFrames_.push_back(t >= 0 ? std::array<int16_t, 2>{int16_t(scene.textures[size_t(t)].width), int16_t(scene.textures[size_t(t)].height)}
                                      : std::array<int16_t, 2>{1, 1});
    // End of load (0x4347c0): Bugs / Taz health from GAME_DATA +0xa5 / +0xa6 (new game: 6, lives 3).
    if (gd) w.GD = *gd;
    else w.GD[0xa0] = 3, w.GD[0xa5] = w.GD[0xa6] = 6, w.GD[0x99] = 1;  // new game (0x401ad0 with slot -1)
    w.GD[0x93] = uint8_t(L);
    // 0x459de0 -> 0x460d80 before every level load: pad records GD+8 / GD+0x30 get +0x10..+0x16 = ← → ↑ ↓ and the
    // remap words +0x18..+0x26 (□ ○ △ ✕ L1 L2 R1 R2) of preset GD+0x95 / +0x96 (table 0x4ceb14, 4 × 8 u16). Nothing
    // on PC writes those bytes (only a save carries them: 0x401a00 / 0x4014c0 keep them over a new game); indices
    // >= 4 read past the table in the exe, bbtime keeps preset 0 then. Part of every save (R: local/saves/tb).
    static constexpr uint16_t kPresets[4][8] = {{0x80, 0x20, 0x10, 0x40, 4, 1, 8, 2}, {0x80, 4, 0x20, 0x40, 8, 1, 0x10, 2},
                                                {4, 8, 0x10, 0x80, 0x20, 1, 0x40, 2}, {0x20, 8, 0x80, 0x10, 1, 2, 4, 0x40}};
    for (size_t pad = 0; pad < 2; ++pad) {
        const size_t rec = 8 + 0x28 * pad;
        const uint8_t preset = w.GD[0x95 + pad] < 4 ? w.GD[0x95 + pad] : 0;
        uint16_t words[12] = {PadLeft, PadRight, PadUp, PadDown};
        std::ranges::copy(kPresets[preset], words + 4);
        for (size_t k = 0; k < 12; ++k) w.GD[rec + 0x10 + 2 * k] = uint8_t(words[k]), w.GD[rec + 0x11 + 2 * k] = uint8_t(words[k] >> 8);
    }
    w.actors[0].hp = w.GD[0xa5], w.actors[1].hp = w.GD[0xa6];
    // §2.1: run speed S[0x66] (Bugs) / S[0x67] (Taz) = 25 in most level-logic setters (402/450, 150 setters);
    // preset in case the level's setter is never reached (M).
    w.S[0x66] = w.S[0x67] = 25;
    w.musicVol_ = w.GD[0x98];  // 0x401ad0: 0x63af80 = config music volume GD+0x98
    w.focusPtr_ = w.camFocus = (w.GD[0xa4] & 4) ? 1 : 0;  // 0x4347c0: 0x4d236c by GD+0xa4 & 4
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
    GD[0xa4] = on ? uint8_t(GD[0xa4] | 0x10) : uint8_t(GD[0xa4] & ~0x12);
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
    sfx.mix(cam.eye, cam.look, [&](int32_t e, uint16_t, V3& pos) {
        if (e <= 2) return true;  // Bugs / Taz (0x4d236c, 0x4d2368): fixed levels
        const TbActor* a = entity(e - 1);
        pos = a ? a->pos : V3{cam.look.x + 0x100000, cam.look.y, cam.look.z};  // freed entity: out of range
        return false;
    });
    speechTick();  // 0x42a030
    glyphs.clear(), textBoxes.clear();  // 0x45ee90: the overlay sprite / box lists restart every tick
    // 0x4615b0: pad 0 = P1's report; pad 1 = P2's in 2-player mode, else 0x4614f0 on P1's report (the type-0x73 right
    // stick bytes 4/5, always 0x7f from the PC joystick, and R3, never set: a zero record on PC).
    const std::span<const uint8_t> gd(GD);
    tbPadTick(pads[0], p1, edgeMask, gd.subspan(0x20, 16), S[9]);
    if (GD[0xa4] & 0x10) tbPadTick(pads[1], p2, edgeMask, gd.subspan(0x48, 16), S[9]);
    else pads[1] = {};
    GD[0x94] = 3;       // 0x4615b0: both pad reads succeed (keyboard / SDL); the pause menu's "no controller" test
    dt = ticks == 0 ? 0 : 2;
    lastCallFriend = 0;
    camFocus = focus();  // 0x4011c0: 0x4d5028 = 0x4d236c before 0x45eee0 (the pass may move the pointer)
    for (uint8_t& f : flash)  // 0x4011c0: hit flash counters run to 0x3c
        if ((f & 0xc0) && (f & 0x3c) < 0x3c) ++f;
    pauseTick();
    // 0x45eee0: the camera 0x439700 stands still while paused; the level load placed it (0x439570), no follow on the
    // first tick (oracle tbintro/full/cam.csv counter 1: the bare 0x437180 pose).
    // The template record (loadAtOrigin) has position 0 and yaw 0: the view starts at (0, -h, -100) and the first
    // follow tick comes in from there (oracle VIQ01 /b24: eye (0, 0, -100), then vel (0, -221705, 0), flags 0x290).
    static const TbActor kOrigin{};
    if (!S[9]) dt ? cameraTick() : cameraInit(loadAtOrigin ? kOrigin : actors[size_t(camFocus)]);
    panelTick();  // 0x45eee0 head: text panel slides (0x4667a0)
    viewList();
    switchTick();
    // 0x460630 head: the world entity takes the zone of the camera body (B+0x2c), else of the focus, else keeps its
    // own; the latched focus (0x4d5930 = 0x4d5028) the zone it stands in. Other entities keep theirs.
    if (int z = findZone(cam.body); z >= 1 || (z = findZone(actors[size_t(focus())].pos)) >= 1) camZone = uint8_t(z);
    if (const int z = findZone(actors[size_t(camFocus)].pos); z >= 1) actors[size_t(camFocus)].zone = uint8_t(z);
    // Entity loop 0x460630 over the view list: the focus as 0x45eee0 found it (before this tick's switch), then the
    // partner (the build loop leaves players out of its in-order append, 0x45ff56; oracle w9tb4 view-list reads),
    // then the others in list order, spawns appended and ticked in the same pass; +0x82 += 1 after each tick unless
    // +0x28 & 0x20; an entity flagged +0x20 & 0x2000 is not ticked and is destroyed right after its slot (0x45ecd0).
    // While paused (S[9]) only entities with flags1 0x40000000 tick (the pause menu, its texts); the others still
    // die (0x460630).
    auto step = [&](TbActor& a) {
        if (!a.active) return;
        if (!S[9] || (a.flags1 & 0x40000000u)) {
            if (!(a.f20 & 0x2000)) {
                classTick(a);
                if (!(a.f28 & 0x20) || S[9]) ++a.stateTicks;
            }
        }
        // its slot is free for the spawns later in this pass (the entity itself goes at the end of the tick)
        if ((a.f20 & 0x2000) && &a != &actors[0] && &a != &actors[1]) freeSlot(a);
    };
    step(actors[size_t(camFocus)]);
    step(actors[size_t(1 - camFocus)]);
    for (size_t i = 0; i < order().size(); ++i)
        if (TbActor& a = *order()[i]; &a != &actors[0] && &a != &actors[1]) step(a);
    for (TbActor& p : actors) p.f20 &= 0x7fffffffu;  // 0x460630 end: the camera's hold of the players lasts a tick
    // HUD: the class 6 entity 0x463170 (template 0x1f holder, spawned at the load: appended after the level's
    // entities; the oracle's counter slides in on the pickup tick, hud/slide.csv).
    hudTick();
    textFlush();  // 0x4681c0: deferred (priority) overlay texts
    // Render pass 0x465130 / 0x464820: a model camera node holds the view (ARCA01 intro, RETRY). Not on the first tick
    // after the load (oracle tbintro/full/cam.csv: the intro's node pose from counter 3, frame 2 of its anim).
    if (dt) cameraNodes();
    portalWalk();  // 0x465130 → 0x465710 for the world entity (always listed, view 0x40): the visible-zone table
    fxTick();  // 0x451370 weather, 0x453350 particles
    shadowPass();  // 0x465130 shade steps, 0x464480 blob shadows
    // The next tick's view-list build 0x45eee0 destroys (0x45ecd0) the flagged entities in list order, but takes the
    // one after each it destroys as live without its flag test (that one dies on a later pass: GRANNY's 370..372,
    // oracle w13 checks k_granny_spawns); those 0x460630 destroyed after their own tick (slot already freed) are out
    // of the list.
    std::vector<const TbActor*> gone;
    bool skip = false;
    for (TbActor* p : std::vector<TbActor*>(order())) {
        TbActor& d = *p;
        const bool inLoop = d.slot < 0 && &d != &actors[0] && &d != &actors[1];
        if (skip && !inLoop) { skip = false; continue; }
        if (!(d.f20 & 0x2000) || &d == &actors[0] || &d == &actors[1]) continue;
        skip = !inLoop;
        // 0x45ecd0: dying program (mask 0x2000), detach, release children
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
        moverForget(d);
        pathFree_ += d.pathSlot;  // 0x43bc80
        freeSlot(d);
        gone.push_back(&d);
    }
    if (!gone.empty()) {
        entities.remove_if([&](const TbActor& e) { return std::ranges::find(gone, &e) != gone.end(); });
        listOwner_ = nullptr;
    }
    for (TbActor* p : {&actors[0], &actors[1]})  // 0x453de0 tail: health mirrors GD+0xa5/+0xa6
        GD[p->id == 2 ? 0xa5 : 0xa6] = uint8_t(p->hp);
    ++ticks;
    fadeTick();  // 0x464ef0, once per rendered tick
}

// ------------------------------------------------------------------------------------------------ pause
// 0x4014c0 head (before the world tick): Start pressed on pad 0 or 1 (GD+8 | GD+0x30 & 0x800; re-armed only once
// released after a resume, latch 0x4d02d0) pauses outside cinematics (GD+0x9f) and demos (GD+0x8a, never set on PC):
// S[9] = 1 and the pause menu entity, template 0x1d (flags1 0x40000000; +0xc |= 0x400 for pad 0's press, else 0x800).
// While S[9] = 1 every tick runs 0x429180 (music stream stopped in place) and 0x42b5c0 (Sfx::pauseAll); its logic
// sets S[9] = 2 to resume: latch, S[9] = 0, PlayMusic(-1) (the stream goes on where it stopped) and 0x42b680
// (Sfx::resumeAll). musicTrack follows `music` only while not paused (0x407460, every tick of 0x4011c0). A pending
// reload (0x4d593a) skips the head: 0x4014c0 unpauses (reloadStart) and 0x401910's 16 fade ticks call 0x4011c0 only,
// so an S[9] = 2 set then stays (oracle w13_ls_trace.csv: 0x4d593a 1 with S[9] 2).
void TbWorld::pauseTick() {
    const bool start = (pads[0].pressed | pads[1].pressed) & PadStart;
    if (restart) {
    } else if (pauseLatch_) {
        if (!start) pauseLatch_ = false;
    } else if (!S[0]) {
        if (S[9] == 1) {
            sfx.pauseAll();
        } else if (S[9] == 2) {
            pauseLatch_ = true, S[9] = 0;
            sfx.resumeAll();
        } else if (start && !GD[0x9f] && !GD[0x8a]) {
            // (The camera-node hold 0x4d515c & 0x300 also blocks it, but every render 0x465130 clears those bits, so
            // they are only seen in load-mode frames; bbtime keeps the hold for RETRY's view and leaves it out here.)
            S[9] = 1;
            if (TbActor* m = spawn(0x1d, {}, 0)) m->flags1 |= (pads[0].pressed & PadStart) ? 0x400 : 0x800;
            sfx.pauseAll();
        }
    }
    musicPaused = S[9] != 0;
    if (!musicPaused) musicTrack = music;
}

// 0x42a030. Without audio (0x4d1f8c 0) or file: 8 -> 2, 2 -> 4 with the timer (0x4cca44 0), 4 -> 0 once
// 6000 * 1000 / 44100 ms passed (table 0x481678 is 6000 for every clip; the 30 Hz tick clock). With audio: 0 stops the
// stream and forgets the clip (0x4cca40 -1), 2 opens and plays it (4), 8 -> 2, 4 -> 0 when the stream ended.
void TbWorld::speechTick() {
    uint8_t& st = S[0x22];
    if (st == 8) {
        st = 2;
    } else if (st == 2) {
        speechTimer_ = !speechAudio, speechEnded = false, speechStart_ = ticks;
        if (speechAudio) speechLast_ = speechClip;
        st = 4;
    } else if (st == 4) {
        if (speechTimer_ ? uint64_t(ticks - speechStart_) * 1000 >= uint64_t(6000 * 1000 / 44100) * 30 : speechEnded) st = 0;
    } else if (st == 0 && speechAudio) {
        speechLast_ = -1;
    }
}

// 0x469770 with the state's mode & 0xb and & 0x80: clip anim + 0x9a; a new clip stops the speech player (0x42b740)
// and starts (S[0x22] 2); the same clip starts again only once the last ended (S[0x22] 0). Started: 0x200.
bool TbWorld::speechAnim(uint8_t clip) {
    speechClip = clip;
    if (speechLast_ == clip && S[0x22]) return false;
    S[0x22] = 2, speechLast_ = clip;
    return true;
}

// ------------------------------------------------------------------------------------------------ switching
void TbWorld::switchTick() {
    TbActor& f = actors[size_t(focus())];
    TbActor& p = actors[size_t(1 - focus())];
    if (!(GD[0xa4] & 0x10)) {
        f.flags1 = (f.flags1 & ~0x800u) | 0x400;
        p.flags1 = (p.flags1 & ~0x400u) | 0x800;
    }
    airTicks = f.f20 & 0xa ? 0 : uint8_t(std::min(airTicks + 1, 3));
    if (cooldown < 45 && !S[9]) ++cooldown;
    auto r1Rule = [&](uint16_t held) {
        const bool r1 = held & PadR1;
        if (r1 && !(GD[0xa4] & 8) && !(p.f20 & 0x2000000) && !(S[3] & 0xc) && !GD[0x9f]) {
            GD[0xa4] |= 8;
            if (!S[9]) {  // paused: only latched
                GD[0xa4] ^= 4;
                for (TbActor& a : actors) a.f28 |= 0x100;  // "switched" entity programs (not interpreted)
            }
        } else if (!r1) {
            GD[0xa4] &= ~8;
        }
    };
    auto l1Rule = [&](uint16_t held) {
        if (!(held & PadL1)) GD[0xa4] &= ~0x20;
        else if (!(GD[0xa4] & 0x20)) GD[0xa4] |= 0x20, callFriend(false);
    };
    if (!(GD[0xa4] & 0x10)) {
        r1Rule(pads[0].held);
        l1Rule(pads[0].held);
    } else {
        GD[0xa4] = uint8_t((GD[0xa4] & ~1) | 2);
        if ((f.flags1 & 0x400) && f.id == 3) {
            std::swap(f.flags1, p.flags1);  // only the pad bits differ in practice
            GD[0xa4] |= 8;
            GD[0xa4] ^= 4;
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

// 0x468280 packet 0xa00 / 0x44e560 coordinate chain: the part's origin through its parents to the actor root, the
// record coordinate 0x44e5d0 (rotation scaled by +0xec, position; VICASG04 entity 131's scale 1.25, oracle w13 J).
V3 TbWorld::partLocal(const TbActor& a, int32_t part, V3 v) const {
    // per coordinate 0x403400: s16 vector, matrix rows >> 12, + its translation, stored back as s16
    const std::vector<TodPart>& parts = a.anim.parts;
    for (int32_t i = part, guard = 0; i >= 0 && size_t(i) < parts.size() && guard < 64; ++guard) {
        const TodPart& t = parts[size_t(i)];
        Mat c;
        for (size_t k = 0; k < 9; ++k) c.m[k] = int16_t(std::lround(t.m[k] * 4096.0f));
        const V3 r = apply(c, v);
        v = {int16_t(r.x + t.trans[0]), int16_t(r.y + t.trans[1]), int16_t(r.z + t.trans[2])};
        i = t.parent;
    }
    return v;
}

V3 TbWorld::partWorld(const TbActor& a, int32_t part) const { return chainPoint(a, part, {}); }

// 0x44e560: a point through the coordinate chain, one coordinate at a time (0x403400): the parts from `part` (-1: none)
// up, then the root: a hung entity's record (+0x136 yaw, +0x13c) and on through its parent's chain (a grab hangs it at
// once, before its own tick moves +0xfc: VICASG04 131 grabbing Bugs lets his snowball go there, oracle w13 J); else
// the record matrix 0x44e5d0 (rotation scaled by +0xec, at +0xfc).
V3 TbWorld::chainPoint(const TbActor& a, int32_t part, V3 v, int depth) const {
    v = partLocal(a, part, v);
    if (const TbActor* par = a.parent >= 0 && depth < 8 ? entity(a.parent) : nullptr) {
        const V3 r = rotY(a.localYaw, v.x, v.y, v.z);
        return chainPoint(*par, a.parentPart, {int16_t(r.x + a.local.x), int16_t(r.y + a.local.y), int16_t(r.z + a.local.z)}, depth + 1);
    }
    const Mat m = recordMat(a);
    const V3 r = apply(m, v);
    return {int16_t(m.t.x + r.x), int16_t(m.t.y + r.y), int16_t(m.t.z + r.z)};
}

int TbWorld::takeSlot() {
    const size_t s = size_t(std::ranges::find(slotUsed, false) - slotUsed.begin());
    if (s >= size_t(poolCap)) return -1;
    if (s == slotUsed.size()) slotUsed.push_back(false);
    slotUsed[s] = true;
    return int(s);
}

TbActor* TbWorld::spawn(uint16_t tmpl, V3 pos, int16_t yaw) {
    auto t = templates_.find(tmpl);
    // 0x460b00 returns 0 for an id without a template (chunk 0x08, list 0x4d26d0): spawns 82 / 83 / 185 / 252 / 284
    // name ids no template of their level has, so nothing is made in the original either. It also returns 0 when the
    // entity pool has no free slot below its count.
    if (t == templates_.end()) return nullptr;
    const int slot = takeSlot();
    if (slot < 0) return nullptr;
    TbActor& e = entities.emplace_back(t->second);  // 0x460b00: copy of the template entity, appended to the list
    e.slot = slot;
    e.uid = nextUid_++;
    e.listPos = nextListPos_++;
    e.active = true;  // and to this tick's view list: it ticks later in the same pass
    e.model = e.ownModel.get();
    e.pos = e.spawn = pos;
    e.yaw = e.spawnYaw = yaw;
    listOwner_ = nullptr;
    return &e;
}

// V 0x40000 (0x45ad70): child hung on parent's +0x84 part (else its root coordinate). keepWorld (with V 0x800000): the
// local position is the child's world one in the parent's record frame (transposed rotation, 0x404bc0 / 0x403470) and
// its local yaw the yaw difference; otherwise both 0 (the part's origin).
void TbWorld::attach(TbActor& parent, TbActor& child, bool keepWorld) {
    if (parent.parent == int32_t(child.uid)) detach(parent);
    if (child.parent >= 0) {
        detach(child);
        child.f20 &= ~0x208u;
    }
    child.parentPart = parent.attachPart;  // +0xe8 = the parent's +0x84 part, else its root coordinate
    child.local = {}, child.localYaw = 0;  // its record at the part's origin (+0x13c / +0x134 cleared)
    if (keepWorld) {
        const Mat m = child.parentPart >= 0 ? coordMat(&parent, child.parentPart) : recordMat(parent);
        Mat t;
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = 0; j < 3; ++j) t.m[i * 3 + j] = m.m[j * 3 + i];
        child.local = apply(t, {int16_t(child.pos.x - m.t.x), int16_t(child.pos.y - m.t.y), int16_t(child.pos.z - m.t.z)});
        child.localYaw = int16_t(child.yaw - parent.yaw);
    }
    link(parent, child);
    child.parentPart = parent.attachPart;
    child.pos = keepWorld ? hangPos(child, parent) : partWorld(parent, child.parentPart);
}

// 0x44d910: a previous parent is left (its flags0 8: +0x20 &= ~0x208), then the child's mass +0x42 joins the parent
// and every ancestor's.
void TbWorld::link(TbActor& parent, TbActor& child) {
    if (const TbActor* old = entity(child.parent)) {
        if (old->flags0 & 8) child.f20 &= ~0x208u;
        detach(child);
    }
    child.parent = int32_t(parent.uid);
    int guard = 0;
    for (TbActor* p = &parent; p && guard < 64; p = entity(p->parent), ++guard) p->mass = int16_t(p->mass + child.mass);
}

// 0x44d870: the mass leaves every ancestor; +0x90 / +0xe8 / +0x104 cleared (+0xe4 back to +0xec).
void TbWorld::detach(TbActor& child) {
    int guard = 0;
    for (TbActor* p = entity(child.parent); p && guard < 64; p = entity(p->parent), ++guard) p->mass = int16_t(p->mass - child.mass);
    child.parent = -1;
    child.parentPart = -1;
    child.rideRec = false;
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
    // The +0x148 player: the TOD one, or a sprite sequence's (classes 2 / 4, flags0 0x200000) at the same offsets.
    const AnimState& an = a.anim;
    const bool spr = a.behaviour == 2 || a.behaviour == 4 || (a.flags0 & 0x200000);
    const uint32_t frame = spr ? a.spr.frame : an.frame, hold = spr ? a.spr.countdown : an.countdown;
    if (mode & 4) {  // anim frame counter +0x14a == p8 + 1 with the frame hold +0x153 expired
        if (p8 + 1 != frame || hold != 0) return false;
    } else if (mode & 8) {  // +0x14e & 0x2000: last frame shown (TbActor::animLast)
        if (!(spr ? a.spr.flags & 0x2000 : a.animLast)) return false;
    } else if ((mode & 0x80) && !((spr ? a.spr.flags : an.flags) & 0x1000)) {  // +0x14e & 0x1000: clip (re)started
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
        case 0x82000000u: {  // line of sight from 3/4 of the box height (query 9 / 0x209 radius p8): blocked unless
                             // nothing is hit or the nearest hit is the target's own shape (0x4463e0 actor pass)
            auto eye = [](const TbActor& e) {
                const int32_t h = e.box[4] - e.box[1];
                return V3{e.pos.x, e.pos.y - (h >> 2) + e.box[1] + h, e.pos.z};
            };
            const V3 s = eye(a), g = eye(*t);
            V3 d{g.x - s.x, g.y - s.y, g.z - s.z};
            const auto h = rayHit(a, s, d, p8 ? int32_t(p8) : 5, p8 ? 0x209 : 9);
            if (h && *h != t) return false;
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
        case 0x2d: return uint8_t(Sv(y)) < uint8_t(Sv(x));        // 0x42ff80 (pause hints: index S[0x70] < count S[0x71])
        case 0x44: return (gd8(y) & x) == x;                      // 0x430a20
        case 0x3b:  // 0x430980: text panel Y (0xffff: any) state & X
            if (y == 0xffff) return std::ranges::any_of(panelState_, [&](const auto& p) { return p[0] & x & 0xff; });
            return y < panelState_.size() && (panelState_[y][0] & x & 0xff);
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
        case 0x48: return int32_t(a.fxHit) == int32_t(x);       // 0x430bd0: (s16)+0x128 (slot of the touching particle) == X
        case 0x49: return int32_t(x) <= a.depth && a.depth <= int32_t(y);  // 0x430bf0: water depth +0x1a2 in [X, Y]
        case 0x4a: return false;                                 // 0x40fc30
        case 0x08: return gd16(y) < x;                           // 0x4302b0
        case 0x24: return int32_t(x) == a.hp;                    // 0x4307c0: health +0x3c == X
        case 0x26: return a.hp < int32_t(x);                     // 0x430800
        case 0x27: return int8_t(Sv(y)) == int8_t(Sv(x));        // 0x42fe50
        case 0x2b: return uint8_t(Sv(x)) < uint8_t(Sv(y));       // 0x42fec0
        case 0x42: return a.climb & x;                           // 0x4309d0: pole bits +0x1a0 & X
        case 0x43: return (Sv(y) & x) == x;                      // 0x4309f0
        case 0x52: return saveSlot >= 0;                         // 0x430cd0: a save slot chosen (0x48840c)
        case 0x80: a.view |= 0x40; return true;                  // 0x431680 (returns the entity: true)
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
        case 4: return t->flags1 & mask;  // 0x432ed0
        case 5: return t->flags10 & mask;  // 0x432ef0
        case 6: return t->groundSurf >= 0 && (col.surfaces[size_t(t->groundSurf)].flags & mask);  // 0x432f10: +0x144
        case 7: return t->groundPoly >= 0 && col.surfaces[col.polys[size_t(t->groundPoly)].surf].id == mask;  // 0x432f40
        case 8: return t->rec() && (t->rec()->flagsA & mask);  // 0x432fb0
        case 9: return t->rec() && (t->rec()->flagsB & mask);  // 0x432fe0
        case 10: return int32_t(int8_t(t->boxIdx)) == int32_t(mask);  // 0x433010: +0x123
        case 11: {  // 0x433040: the selector's entity has the id of the +0x1ac link
            const TbActor* s = lookup(mask, *t);
            const TbActor* l = entity(t->link);
            return s && l && l->id == s->id;
        }
        case 12: return true;  // 0x40aca0
        case 13: return int32_t(le32(d, 8)) == cam.pathId;  // 0x432f70: B+0xbc node id (+8) == the item's target
        default: return false;  // the table has 14 entries (0x4cd8e4)
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
        const bool part = a.attachPart >= 0 && !(mode & 0x4000);  // else the actor's own coordinate
        const uint16_t tmpl = le16(d, 8);
        const bool fromFocus = &a == &actors[size_t(focus())];
        const auto partScale = [&]() -> std::array<int16_t, 3> {  // the coordinate's scale (+0x44 block / +0xec)
            if (!part || size_t(a.attachPart) >= a.anim.parts.size()) return a.scale;
            const auto& s = a.anim.parts[size_t(a.attachPart)].scale;
            return {int16_t(s[0]), int16_t(s[1]), int16_t(s[2])};
        };
        if (mode & 1)  // spawned as a child of the part (or the root): at its origin, the part's scale with flags0 0x200000
            if (TbActor* e = spawn(tmpl, part ? a.attachPos : a.pos, a.yaw)) {
                e->zone = a.zone;  // 0x45cc40: the spawner's zone
                const int32_t keep = a.attachPart;
                if (!part) a.attachPart = -1;
                attach(a, *e);
                a.attachPart = keep;
                e->scale = (a.flags0 & 0x200000) ? partScale() : std::array<int16_t, 3>{4096, 4096, 4096};
                e->pitch = e->roll = 0;
            }
        if (mode & 2)  // spawned free at the attach point with the actor's rotation (0x2000: the part's direction)
            if (TbActor* e = spawn(tmpl, part ? a.attachPos : a.pos, a.yaw)) {
                e->pitch = a.pitch, e->roll = a.roll, e->zone = a.zone;
                if (fromFocus) e->flags1 |= 0x20000;
                if (part && !(mode & 0x2000)) {
                    e->scale = partScale();
                } else if (part) {  // the part chain's +z (0x44e560 mode 2): yaw = atan2(x, z), pitch = atan2(y, |xz|)
                    // A player's class-14 spawn in the look view with GD+0xa2 4 leaves from the look point along it.
                    e->flags1 |= 0x20;
                    if ((a.id == 2 || a.id == 3) && lookMode_ && (GD[0xa2] & 4) && e->behaviour == 14) {
                        e->pitch = lookPitch_, e->yaw = lookYaw_, e->roll = 0;
                        e->pos = {int16_t(cam.look.x), int16_t(cam.look.y), int16_t(cam.look.z)};
                    } else {
                        const Mat M = coordMat(&a, a.attachPart);
                        const V3 dir = apply(M, {0, 0, 0x1000});
                        e->yaw = int16_t(ratan2(dir.x, dir.z));
                        e->pitch = int16_t(ratan2(dir.y, isqrt(dir.x * dir.x + dir.z * dir.z)));
                        e->roll = 0;
                    }
                }
            }
        if ((mode & 0x20) && part) {  // release what hangs on the attach part
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
        // Bits 4 (grab the focus), 0x200 / 0x400 / 0x20000 (turned spawns) and 0x10000 (ground decal 0x465540) are in
        // no level of the PC data (all 950 '^' items of the 58 levels: modes 1, 2, 0x801, 0x802, 0x2802, 0x2008).
        if ((mode & 8) && part) {  // the focus hanging on this actor lets go turned round, with event `template`
            TbActor& f = actors[size_t(focus())];
            if (f.parent == int32_t(a.uid)) {
                if (mode & 0x80) f.pos = a.attachPos;
                f.pitch = a.pitch, f.yaw = int16_t(a.yaw + 0x800), f.roll = a.roll;
                detach(f);
                dispatch(f, le32(d, 8));
                f.f20 &= 0xfbfff5ffu;
            }
        }
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
        if (u & 0x20000) {  // 0x469320: model of p1's links, rig of p2's (0 = self; else 0x45a120, else id in 0x4d26d0)
            auto src = [&](uint32_t p, uint32_t id) -> const std::vector<uint16_t>* {
                if (!p) return &a.links;
                if (const TbActor* t = lookup(p, a)) return &t->links;
                const auto l = linksById_.find(uint16_t(id));  // the exe's fallback compares both with p1
                return l == linksById_.end() ? nullptr : &l->second;
            };
            rigSwap(a, src(p1, p1), src(p2, p1), uint8_t(p3));
        }
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
        // V 0x80100000 (throw at the snipe target 0x4d5110) needs 0x4d598c & 1, set only by S 86 (0x4325c0): no PC
        // level uses S 86 nor this V word, so it does nothing in the original.
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
        if (u & 0x200000) fxEmit(a, p1, uint16_t(p3));  // 0x451e10 -> 0x452ab0: emit slot p3
        return 0;
    }
    uint32_t res = 0;
    TbActor* made = nullptr;
    if (u & 0x810) {  // spawn template p3 (p2 times, at least once) at the attach point or the actor
        uint32_t n = 0;
        do {
            const V3 at = (u & 0x40000000u) || a.attachPart < 0 ? a.pos : a.attachPos;
            made = spawn(uint16_t(p3), at, a.yaw);
            if (made) {
                made->zone = a.zone;  // 0x45ad70: the spawner's zone (both 0x40000 branches)
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
            if (u & 0x40000) attach(a, *t, u & 0x800000);  // the target hangs on this actor's attach part
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
            if (u & 0x800000)  // 0x448780 over the zones holding this actor: a water zone (flagsC 0x1000) sets its y to
                for (const Zone& z : zones_) {  // the surface (origin y + height), on +0xe4 (its local one when hung)
                    if (a.f20 & 0x2000) break;
                    if (!inZone(a, z) || !(z.flagsC & 0x1000)) continue;
                    (a.parent >= 0 && !a.rideRec ? a.local.y : a.pos.y) = int16_t(z.origin.y + z.height);
                }
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
    if (u & 0x200) {  // release attached children matching +0x08 & p1 or +0x0c & p2 at their world position
        std::vector<TbActor*> kids;
        forEachEntity([&](TbActor& c) {
            if (c.parent == int32_t(a.uid) && ((p1 & c.flags0) || (p2 & c.flags1))) kids.push_back(&c);
        });
        for (TbActor* c : kids) {
            const V3 at = hangPos(*c, a);  // 0x44e560 over its record chain
            c->f20 = (c->f20 & 0xffff7bdeu) | 0x10000;
            detach(*c);
            c->pos = at;
            c->zone = a.zone;
            if (u & 0x20000000) dropClamp(a, *c);  // 0x450650
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
        auto gdAdd = [&](uint16_t o, uint16_t v, bool wide) {  // GD(o) += v: a u16 field, else its byte
            if (wide) {
                const uint16_t s = uint16_t((GD[0x58u + o] | GD[0x59u + o] << 8) + v);
                GD[0x58u + o] = uint8_t(s), GD[0x59u + o] = uint8_t(s >> 8);
            } else if (0x58u + o < GD.size()) {
                GD[0x58u + o] = uint8_t(GD[0x58u + o] + v);
            }
        };
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
                GD[0xa4] ^= 4;
                for (TbActor& c : actors) c.f28 |= 0x100;
            }
            return 0;
        case 82:  // 0x432530: select collision box record (tag 0x61), arg2 & 2 = no morph
            if (x < a.boxes.size() && int32_t(x) != a.boxIdx) a.boxIdx = x, a.boxMorph = true, a.boxInstant = y & 2;
            return 0;
        case 53:  // 0x431a40: hurt the focus by X (+0x66, +0x20 0x4000000) unless a level change or fade is pending
            // or the camera holds it (+0x20 0x80000000); then 0x4d271c = 30. Nothing in bblit2.exe decrements or
            // resets 0x4d271c (its only references: this set, this test, the camera 0x4398e3), so it works once per run.
            if (invuln < 1 && !restart && !fade.mode) {
                TbActor& f = actors[size_t(focus())];
                if (!(f.f20 & 0x80000000u)) f.dmgIn = int16_t(x), f.f20 |= 0x4000000, invuln = 0x1e, a.f24 |= 2;
            }
            return 0;
        case 126: music = x & 0xff; return 0;  // 0x432db0
        case 128:                              // 0x431290: displacement +0x58 x += X, z += Y (applied by the next mover)
            a.carry[0] = int16_t(a.carry[0] + int16_t(x)), a.carry[2] = int16_t(a.carry[2] + int16_t(y));
            return 0;
        case 118: fxField(a, x, y); return 0;          // 0x432c80 -> 0x452060: field X of the slot a owns = Y
        case 138: fxClear(int16_t(y)); return 0;       // 0x432cb0 -> 0x4529d0: free slot Y's particles
        case 110: fxWeather(x, y); return 0;           // 0x432bb0: weather streaks
        // 0x430c90 log stub. Pad rumble 39 / 40 / 42 / 43 / 48 / 130 / 131 / 134 writes the actuator records 0x4d2780..
        // (the distance forms against the look point), which only 0x460e10 reads; on PC its 0x40fc30 returns 0, so it
        // zeroes them every pad poll and 0x40ae00 (the actuator write) is empty: no effect.
        case 0: case 39: case 40: case 42: case 43: case 48: case 130: case 131: case 134: return 0;
        case 24: if (sv) *sv = uint8_t(*sv - int8_t(x)); return 0;      // 0x431070
        case 30: textStatus = textSpawn(uint16_t(x), int16_t(y)); return 0;   // 0x4312f0: text X, priority Y
        case 32: if (sv) textStatus = textSpawn(*sv, 0); return 0;     // 0x431380: text S[Y]
        case 75:                                                        // 0x4320d0: start text panel Y with record X
            if (y < panelState_.size()) panelState_[y] = {0x80, uint8_t(x)};
            return 0;
        case 51:                                                         // 0x4319f0: X 0 fade in, 1 fade out (holds)
            if (x == 0) fadeIn(1, 0x100);
            else if (x == 1) fadeOut(2, 0x100);
            return 0;
        case 52: fadeIn(0x41, 0x400); return 0;                         // 0x431a30: dark-light-dark blink
        case 31:                                                         // 0x431320: remove class-31 entities
            forEachEntity([&](TbActor& e) { if (e.behaviour == 31 && (x == 0xffff || e.id == x)) e.f20 |= 0x2000; });
            return 0;
        case 62:  // 0x431ba0 speech: X > 1 clip X - 0x66 (S[0x22] 8); 1 holds silent (1), 0 stops (0x42a030 at once),
                  // both restore GD+0x98 (music volume) from its load copy 0x63af80
            speechClip = x > 1 ? uint8_t(x - 0x66) : 0;
            if (x > 1) S[0x22] = 8;
            else if (x == 1) S[0x22] = 1, GD[0x98] = musicVol_;
            else if (S[0x22]) S[0x22] = 0, GD[0x98] = musicVol_, speechTick();
            return 0;
        case 63: playSound(a, x, 2); return 0;                           // 0x431c40 release
        case 74: camScript.flags |= 4; return 0;  // 0x431fb0: B+0x3c |= 4 for every view; nothing in the exe reads the bit
        case 87: a.pitch = 0, a.yaw = lookYaw_, a.roll = 0, lookLock_ = 0; return 0;  // 0x432610 -> 0x439570 (+ H 400)
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
        case 1: if (sv) *sv = uint8_t(-*sv); return 0;                   // 0x430d40
        case 6: if (gv) *gv &= uint8_t(x); return 0;                     // 0x430e10
        case 15:  // 0x430ef0: GD(Y) += 1, GD(X) += 1 (its u16 test takes X's parity but Y's bound)
            gdAdd(y, 1, (y & 1) == 0 && y < 0x31), gdAdd(x, 1, (x & 1) == 0 && y < 0x31);
            return 0;
        case 17: if (gv && x < S.size()) *gv = S[x]; return 0;           // 0x430e70
        case 18: if (sv && 0x58u + x < GD.size()) *sv = GD[0x58u + x]; return 0;  // 0x430ea0
        case 23: a.yaw = int16_t(a.yaw + int16_t(x) * 16 + int16_t(y)); return 0;  // 0x431170 (+0xe4 record +10)
        case 25: gdAdd(y, uint16_t(-x), (y & 1) == 0 && y < 0x31); return 0;   // 0x431090 GD(Y) -= X
        case 41:                                                         // 0x431600 shake
            if (!camShake.amp) camShake.phase = 0;
            camShake.speed = int16_t(x * 11), camShake.amp = int16_t(y);
            return 0;
        case 49:                                                         // 0x4318e0 shake by the look point's distance
            if (const int32_t r = lenFast(a.pos.x - cam.look.x, a.pos.y - cam.look.y, a.pos.z - cam.look.z);
                !camShake.amp && r < 0x800) {
                const int32_t k = int32_t(y) * r;
                camShake.phase = 0, camShake.speed = int16_t(x * 11), camShake.amp = int16_t(y - k / 0x800);
            }
            return 0;
        case 44: {                                                       // 0x431650: remove and drop every child
            std::vector<TbActor*> kids;
            forEachEntity([&](TbActor& c) { if (c.parent == int32_t(a.uid)) kids.push_back(&c); });
            for (TbActor* c : kids) c->f20 |= 0x2000, detach(*c);
            return 0;
        }
        case 50: if (y < 3) fade.rgb[y] = uint8_t(x); return 0;         // 0x4319b0: fade colour 0x4d23c4 / 0x4d26ad / 0x4d26ac
        case 54: camScript.flags &= ~0x2000u; return 0;                  // 0x431ac0: the look view allowed
        case 55: camScript.flags |= 0x2000; return 0;                    // 0x431af0: forbidden
        case 64: camScript.flags |= 0x200000; return 0;                  // 0x431c90: no L2 / R2 orbit
        case 65: camScript.flags &= ~0x200000u; return 0;                // 0x431c60
        case 70: camScript.flags |= 1; return 0;                         // 0x431f20
        case 72: camScript.flags |= 0x10; return 0;                      // 0x431f80: no look view next camera tick
        case 120: cam.flags &= ~0x80u; return 0;                         // 0x432cd0 -> 0x4374f0: both players framed
        case 121: cam.flags |= 0x80; return 0;                           // 0x432cf0 -> 0x437510: the focus alone
        case 69:  // 0x431da0: the six language entries, GD(X)'s one highlighted (0x12 + k - 1, else 0xc + k - 1). The
                  // installed-language mask 0x639984 it tests (texts 0..0xb) is never written by bblit2.exe: always 0.
            for (uint16_t k = 1; k <= 6; ++k) textSpawn(uint16_t((gv && *gv == k ? 0x12 : 0xc) + k - 1), 0);
            return 0;
        case 78: {  // 0x4321e0: a downed partner (+0x20 0x2000000) stands up here: flags and velocity cleared, health 6,
                    // hit flash, +0x178 = its event-1 transition
            TbActor& p = actors[size_t(1 - focus())];
            if (!(p.f20 & 0x2000000)) return 0;
            p.pos = a.pos, p.f20 = p.f24 = p.f28 = 0, p.vel = {}, p.hp = 6;
            GD[0xa2] &= 0xfc;
            if (p.id == 2) flash[0] = 0x40;
            if (p.id == 3) flash[1] = 0x80;
            p.f28 |= 0x200;
            const auto t = std::ranges::find(p.trans, uint16_t(1), &TbTransition::event);
            p.tr = t == p.trans.end() ? -1 : int32_t(t - p.trans.begin());
            return 0;
        }
        case 80:                                                         // 0x432390: scale (S[X] · 256 + S[Y]) · 16
            if (x < S.size() && y < S.size()) a.scale.fill(int16_t((S[x] * 256 + S[y]) * 16));
            return 0;
        case 83: a.attachPart = 0; return 0;                             // 0x432590: +0x84 = 0
        case 88: speedMul_ = int8_t(x); return 0;                        // 0x432630
        case 89: if (sv && x < S.size()) *sv = uint8_t(*sv + S[x]); return 0;  // 0x432640
        case 94: if (sv && 0x58u + x < GD.size()) *sv = uint8_t(*sv + GD[0x58u + x]); return 0;  // 0x4327c0
        case 109: nearClip = x; return 0;                                // 0x432ba0
        case 117: viewDist2 = x; return 0;                               // 0x432c70
        case 119: quitToMenu = true; return 0;                           // 0x432cc0 (+ 0x429ce0 streams stopped)
        case 127:                                                        // 0x431260: draw bias of Y (its own id: self)
            if (TbActor* t = int16_t(y) == int16_t(a.id) ? &a : lookup(uint32_t(int32_t(int16_t(y))), a)) t->drawBias = int8_t(x);
            return 0;
        case 135: if (TbActor* t = lookup(y, a)) t->gdBit = x; return 0;  // 0x432df0: +0x48 of Y = X
        case 136:                                                        // 0x432e20 -> 0x464050: GD+0x5a as a percentage
            if (gv) *gv = uint8_t(std::min(100u, uint32_t(GD[0x5a] | GD[0x5b] << 8) * 100 / 6010));
            return 0;
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
            case 12: t.hover = int16_t(A); break;      // 0x433200: +0x194
            case 17: t.shadowId = uint16_t(A); break;  // 0x4332c0: +0x36
            case 20: pathAlloc(t); break;  // 0x4334b0: a CameraTrackPath slot (its +2 word feeds only the path follow)
            case 22: break;
            case 23: t.range = uint16_t(A); break;
            case 24: t.progMask[2] |= A; break;
            case 25: t.progMask[2] &= ~A; break;
            default: unsupported.insert(std::format("U{}", idx)); break;  // 19 / 21 (grips): in no PC level
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
            c.flags |= 0x400, c.eye = p;
            if (f & 0x20) c.flags |= 0x4000;
            if (f & 0x40) c.flags |= 0x10000;
        } else if (f & 4) {
            c.flags &= ~0x4400u;
        }
        if (f & 2) {
            c.flags |= 0x800, c.look = p;
            if (f & 0x20) c.flags |= 0x8000;
            if (f & 0x40) c.flags |= 8;
        } else if (f & 8) {
            c.flags &= ~0x8800u;
        }
        if (f & 0x40000000u) c.flags = le32(d, 8) ? c.flags | 0x80000 : c.flags & ~0x80000u;
        // Follow-camera sub-ops (one per item, this order): constants B+0x11c..0x148, the rest length, reset, defaults.
        // 0x400..0x4000 drive the camera track path (0x43a1b0 / 0x43a0e0 / 0x43a110 / 0x43a0f0 / 0x43a130 / 0x43a150):
        // its list 0x4d4fb4 is filled only by entities with flags1 0x40 on their first 0x453de0 tick, and no PC level
        // has one (TbWorld::load asserts it), so 0x43a1b0 finds no path, B+0xbc stays 0 and the path camera 0x43a2c0
        // never runs: the other fields it alone reads change nothing.
        const int32_t v = int32_t(le32(d, 8));
        if (f & 0x7c00) {
        } else if (f & 0x8000) {
            cam.wallR = v;
        } else if (f & 0x10000) {
            cam.wallPush = v;
        } else if (f & 0x20000) {
            cam.aimH = v;
        } else if (f & 0x40000) {
            cam.band = v;
        } else if (f & 0x80000) {  // B+0x12c: not read by the follow
        } else if (f & 0x200000) {
            cam.lookH = v;
        } else if (f & 0x400000) {
            cam.radius = v;
        } else if (f & 0x800000) {
            cam.pushSpeed = v;
        } else if (f & 0x1000000) {
            cam.resetSpeed = v;
        } else if (f & 0x2000000) {
            cam.guideSpeed = v;
        } else if (f & 0x4000000) {
            cam.damp = v;
        } else if (f & 0x8000000) {
            cam.rest = cam.dist = v;
        } else if (f & 0x10000000) {
            cameraReset(actors[size_t(focus())]);
        } else if (f & 0x100000) {  // 0x437440 defaults
            const TbCamera k;
            cam.splitTicks = 0, cam.blend = 0x1000, cam.rest = cam.dist = k.rest, cam.wallR = k.wallR, cam.wallPush = k.wallPush;
            cam.aimH = k.aimH, cam.band = k.band, cam.lookH = k.lookH, cam.radius = k.radius, cam.pushSpeed = k.pushSpeed;
            cam.resetSpeed = k.resetSpeed, cam.guideSpeed = k.guideSpeed, cam.damp = k.damp;
        } else if (f & 0x80) {
            c.flags &= 0xffee23d7u;
        } else if (f & 0x100) {
            c.flags |= 0x100000;  // (with B+0x156 = v: read by the look mode, not simulated)
        }
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

uint32_t TbWorld::runItem(TbActor& a, TbLogicItem& it) { return isCond(it.tag) ? uint32_t(cond(a, it, true)) : action(a, it); }

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
    // Restart the clip next tick; 0x44d990 also keeps only +0x14e's 0x4000, so a block run before the next anim tick
    // (the hit's logic, 0x453de0 head) sees neither 0x2000 (last frame) nor 0x1000 (restarted) (ARQ01 377 -> 189).
    if (!(r.mode & 0x40) || r.anim != oldAnim) {
        a.curAnim = 0, a.animLast = false;
        a.anim.flags &= ~0x3000u, a.spr.flags &= ~0x3000u;
    }
    if (r.flagsB & 0x800) a.f20 &= ~0x421u;
    auto sval = [&](int16_t v, int shift4000) -> int32_t {
        if (v > 0 && (v & 0x4000)) return int32_t(int8_t(S[size_t(v & 0x3fff)])) << shift4000;
        if (v > 0 && (v & 0x2000)) return int32_t(int8_t(S[size_t(v & 0x1fff) % S.size()])) * speedMul_;  // x 0x4d26b8
        return v;
    };
    if (!((r.flagsA & 0x18) || (r.flagsB & 0x104000)) || ((a.f20 & 0x421) && !(a.f20 & 0x4000))) return true;
    a.f20 &= ~0x100u;
    if (a.parent >= 0) detach(a), a.f20 &= ~0x208u;  // 0x44d870: leaves what it rides / hangs on, world pose kept
    if ((r.flagsB & 0x4000) && entity(a.contact)) a.yaw = int16_t(entity(a.contact)->yaw + 0x800);  // back to the contact
    // flagsA 0x4000: face the selector target (flagsC 0x100 without the homing bit: aimed ahead of its velocity by
    // the ticks |d| / fwd); flagsA 0x2000 the other way. 0x44d990 writes the record yaw and +0xf6 together.
    const TbActor* tg = nullptr;
    if ((r.flagsA & 0x4000) && (tg = lookup(uint32_t(int32_t(r.target)), a))) {
        int32_t dx = tg->pos.x - a.pos.x, dz = tg->pos.z - a.pos.z;
        int32_t y;
        if (!(r.flagsC & 0x100) || (r.flagsB & 0x100000)) {
            y = ratan2(dx, dz) + ((r.flagsA & 0x2000) ? 0x800 : 0);
        } else {
            if (r.fwd) {
                const int32_t n = isqrt(dx * dx + dz * dz) / r.fwd;
                dx += int16_t(tg->vel[0]) * n >> 4, dz += int16_t(tg->vel[2]) * n >> 4;
            }
            y = ratan2(dx, dz);
        }
        a.yaw = int16_t(y & 0xfff);
    }
    if (!(r.flagsB & 0x100000)) {
        if (!(r.flagsA & 0x18)) return true;
        a.f20 &= ~0x421u;
        if (r.flagsA & 8) {  // impulse along the facing: (0, 0, fwd) through Rx(pitch)·Ry(yaw) (flags10 0x10), else the
            Mat M;           // yaw of (0, 0, 1000) through Rx·Ry·Rz
            mulRotX(M, a.pitch), mulRotY(M, a.yaw);
            if (!(a.flags10 & 0x10)) {
                mulRotZ(M, a.roll);
                const V3 f = apply(M, {0, 0, 1000});
                M = {}, mulRotY(M, ratan2(f.x, f.z));
            }
            const V3 v = apply(M, {0, 0, int16_t(sval(r.fwd, 2))});
            a.vel[0] = int16_t(a.vel[0] + v.x * 16), a.vel[1] = int16_t(a.vel[1] + v.y * 16), a.vel[2] = int16_t(a.vel[2] + v.z * 16);
        }
        if (!(r.flagsA & 0x10)) {
            a.vel[1] = int16_t(a.vel[1] - sval(r.vert, 2) * 16);
        } else {
            a.f20 |= 1;
            const int32_t H = r.vert > 0 && (r.vert & 0x4000) ? int32_t(S[size_t(r.vert & 0x3fff)]) << 4
                            : r.vert > 0 && (r.vert & 0x2000) ? int32_t(S[size_t(r.vert & 0x1fff) % S.size()]) * speedMul_ : r.vert;
            a.vel[1] = int16_t(-0x50 - ((isqrt(H * 0x50 + 100) >> 1) << 4));  // §2.3
        }
    } else {
        // Homing jump (flagsB 0x100000) onto the selector target: the apex vert above its middle (box +0x6a / 2), the
        // rise speed for that height, the flight ticks to land on it, the horizontal speed |d| / ticks along the yaw to
        // it (flagsC 0x100: its velocity led for those ticks, flagsA 0x4000 facing that point). Detached, the record
        // yaw 0x44d990 writes is +0xf6 itself. No target: (0, -vert, fwd) along the facing.
        int32_t h = sval(r.vert, 4), vy, vz;
        if (const TbActor* t = lookup(uint32_t(int32_t(r.target)), a); !t) {
            vy = -r.vert, vz = sval(r.fwd, 4);
        } else {
            int32_t y = ratan2(t->pos.x - a.pos.x, t->pos.z - a.pos.z);
            if (r.flagsA & 0x2000) y = (y - 0x800) & 0xfff;
            const int32_t top = (t->box[4] >> 1) + t->pos.y;  // +0x6a: the box top
            if (top <= a.pos.y) h = top - h - a.pos.y;
            h = std::abs(h);
            vy = int16_t(-10 - ((isqrt((h * 0x800 + 0x100) / 10) - 0x10) * 10 >> 5));
            const int32_t v5 = vy - 5;
            const int32_t n = ((isqrt((v5 * v5 + (top - a.pos.y) * 0x14) * 0x100) >> 4) - v5) / 10 - 1;
            int32_t dx = t->pos.x - a.pos.x, dz = t->pos.z - a.pos.z;
            if (r.flagsC & 0x100) {
                dx += int16_t(t->vel[0]) * n >> 4, dz += int16_t(t->vel[2]) * n >> 4;
                if (r.flagsA & 0x4000) y = ratan2(dx, dz);
            }
            a.yaw = int16_t(y & 0xfff);
            vz = isqrt((dx * dx + dz * dz) * 4) >> 1;
            if (n > 1) vz /= n;
        }
        const V3 v = rotY(a.yaw, 0, vy, int16_t(vz));
        a.vel = {int16_t(v.x << 4), int16_t(v.y << 4), int16_t(v.z << 4)};
        a.f20 &= ~1u;
    }
    if (r.flagsC & 0x10) a.hover = a.pitch, a.hoverSpan = int16_t(int16_t(a.vel[1]) >> 4);  // +0x194 / +0x196
    return true;
}

void TbWorld::actorStep(TbActor& a) {
    a.trig = {};
    a.f20 &= 0xffcbffffu;  // 0x453de0 head: 0x200000 (carriable ahead), 0x100000 / 0x40000 (push) are per tick
    // 0x453de0 tail on every exit: 0x44e5d0 rebuilds +0xa4 and a moved / turned entity's shape cache record is
    // marked stale (+0x88: 0x20000000 position, 0x40000000 angles).
    struct Tail {
        TbWorld& w; TbActor& a; V3 p; std::array<int16_t, 3> r; std::array<int16_t, 6> b;
        ~Tail() { w.moverTail(a, p, r, b); }
    } tail{*this, a, a.pos, {a.pitch, a.yaw, a.roll}, a.box};
    if (!(a.f24 & 0x8000)) {  // first tick of 0x453de0
        if (!a.boxes.empty()) a.boxInstant = true, boxStep(a);  // 0x450440(a, 0): tag-0x61 record +0x123 at once
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
    if (a.flags0 & 0x4000000) {  // 0x453de0: the look view hides and holds the focus; a held player stops in x / z
        if (lookMode_ && &a == &actors[size_t(focus())]) a.f24 |= 4, a.f20 |= 0x80000000u;
        if (!GD[0x8a] && (a.f20 & 0x80000000u)) a.vel[0] = a.vel[2] = 0;
    }
    if (lookMode_) hideCarried(a);
    // flags0 8: the start-of-tick forward of the record chain, in its frame (local_114), for +0x12a at the end.
    V3 fwd0;
    if (a.flags0 & 8) {
        const Mat m = chainRot(a);
        fwd0 = apply({{m.m[0], m.m[3], m.m[6], m.m[1], m.m[4], m.m[7], m.m[2], m.m[5], m.m[8]}, {}}, {0, 0, 0x1000});
    }
    if (!a.rec()) return;
    if (hitIntake(a)) return;
    const TbStateRec* r = a.rec();
    if (r->flagsB & 0x2000) a.f24 |= 0x1000; else a.f24 &= ~0x1000u;
    if (a.flags0 & 0x4000000) {  // players: +0x24 0x800 = the state's flagsB 0x1000
        deathEvent[a.id == 2 ? 0 : 1] = 0;
        if (r->flagsB & 0x1000) a.f24 |= 0x800; else a.f24 &= ~0x800u;
    }
    TbActor* par = entity(a.parent);
    if (a.parent >= 0 && !par) detach(a);
    const bool hung = par && !a.rideRec;  // +0xe8: steering turns the local yaw (+0x136), the record places it
    std::array<int32_t, 3> s{};  // local move: x side, y vertical (up -), z forward
    if (!steer(a, r, hung, s)) return;  // flagsB 0x100 without a CameraTrackPath slot: 0x453de0 returns
    const bool live = !(r->flagsA & 1) && !(a.f20 & 0x80000000u);
    // 0x45643b: a state with flagsB 0x20000 follows its attach point. On a tick whose frame hold has expired the yaw
    // turns by the attach part's yaw and the mover carries the entity to +0x7c (runtime: thrown template 401, capture
    // tbnpc/B_guard_attack: next position = last tick's attach point).
    if (!hung && (r->flagsB & 0x20000) && a.attachPart >= 0 && size_t(a.attachPart) < a.anim.parts.size() && !a.anim.countdown) {
        a.yaw = int16_t(a.yaw + a.anim.parts[size_t(a.attachPart)].rot[1]);
        a.carry = {int16_t(a.carry[0] + a.attachPos.x - a.pos.x), int16_t(a.carry[1] + a.attachPos.y - a.pos.y),
                   int16_t(a.carry[2] + a.attachPos.z - a.pos.z)};
    }
    // flagsA 0x8000: the vertical move rises by the state's +0x14 (S form, not scaled by the tick) and +0x20 gets
    // 0x80000 (0x421 cleared); otherwise 0x80000 is cleared.
    if (r->flagsA & 0x8000) {
        s[1] = int16_t(s[1] - (r->vert > 0 && (r->vert & 0x6000) ? int32_t(int8_t(S[size_t(r->vert & 0x9fff)])) : r->vert));
        a.f20 = (a.f20 & 0xfffffbdeu) | 0x80000;
    } else {
        a.f20 &= ~0x80000u;
    }
    carryRelease(a);
    if (r->flagsA & 1) a.carry = {}, a.vel = {};
    // local -> world (0x453de0, before 0x4450c0): M = Ry(yaw)·Rz(roll)·Rx(pitch) (flags10 0x100000: Ry·Rx·Rz), the
    // s16 vector s << 5 through it (0x403400), (w + 0x10) >> 5 (§2.1). Pitch steers dives (Granny 220, oracle w13).
    std::array<int32_t, 3> target{};
    if (live) {
        Mat M;
        if (a.flags10 & 0x100000) mulRotZ(M, a.roll), mulRotX(M, a.pitch);
        else mulRotX(M, a.pitch), mulRotZ(M, a.roll);
        mulRotY(M, a.yaw);
        const V3 w = apply(M, {int16_t(s[0] << 5), int16_t(s[1] << 5), int16_t(s[2] << 5)});
        target = {(w.x + 0x10) >> 5, (w.y + 0x10) >> 5, (w.z + 0x10) >> 5};
    }
    // Hung (+0xe8, moving states not 0x80080000) / riding (+0x104, always): where the parent's record carries the
    // local position now (local_14c, the mover's attached displacement); hung ones take the record's heading, riders
    // turn by the parent's last turn +0x12a scaled by its +0x3a and their grip.
    V3 follow;
    if ((hung && live && !(a.f20 & 0x80080000u)) || (par && a.rideRec)) {
        int16_t yaw = a.yaw;
        const V3 w = hangPos(a, *par, &yaw);
        follow = {int16_t(w.x - a.pos.x), int16_t(w.y - a.pos.y), int16_t(w.z - a.pos.z)};
        if (hung) a.yaw = yaw;
        else a.yaw = int16_t(a.yaw + int16_t((par->turnDelta * ((int32_t(uint16_t(par->f3a)) * r->grip) >> 12)) >> 12));
    }
    std::vector<TbActor*> touched;  // actor contacts of the mover (local_f8)
    std::array<int32_t, 3> disp = target;  // psVar9 +0x20: the move applied below
    const std::array<uint32_t, 3> before{a.f20, a.f24, a.f28};
    bool dropFloor = false;
    TbActor* floor = nullptr;
    if (!(r->flagsA & 0x800000)) {  // no mover: velocity = move + follow + carry (x16), position += the move
        a.vel = {(target[0] + follow.x + a.carry[0]) * 16, (target[1] + follow.y + a.carry[1]) * 16,
                 (target[2] + follow.z + a.carry[2]) * 16};
        a.f20 &= ~2u;
    } else {
        const MoveOut mo = move(a, target, touched, r, follow);
        disp = mo.disp;
        floor = mo.floor;
        if (!(a.f20 & 0x80000) && !a.rideRec)
            for (size_t i = 0; i < 3; ++i) disp[i] -= std::array<int32_t, 3>{follow.x, follow.y, follow.z}[i];
        a.trig = {a.f20 & ~before[0], a.f24 & ~before[1], a.f28 & ~before[2]};  // the mover's new bits (I)
        // A climber (flagsA 0x200) that the mover put on the ground or against a wall (+0x20 6) lets go of its pole
        // (parent flags0 0x10004).
        if (TbActor* p = entity(a.parent); p && (r->flagsA & 0x200) && (a.f20 & 6) && (p->flags0 & 0x10004)) {
            detach(a);
            a.f20 &= ~0x200u;
        }
        dropFloor = contacts(a, touched, disp);
        // The push block: moving forward on the ground into a pushable (flags0 0x4000). Otherwise touching a carriable
        // (flags0 0x2802, not its parent) raises +0x20 0x200000 too; for a player only if the item lets him (flags1
        // 0x10000 Bugs, 0x40000 Taz).
        for (TbActor* c : touched)
            if (s[2] && (c->flags0 & 0x4000) && (a.f20 & 10)) push(a, *c, disp, target, false);
            else if ((c->flags0 & 0x2802) && a.parent != int32_t(c->uid) &&
                     (!(a.flags0 & 0x4000000) || (a.id == 2 && (c->flags1 & 0x10000)) || (a.id == 3 && (c->flags1 & 0x40000))))
                a.f20 |= 0x200000;
        if ((r->flagsA & 0x8000000) && !(a.f20 & 0x40000)) {  // pushing without a contact: probe 100 ahead (mask 0x4000)
            Mat R;
            rotMatrixXYZ(R, a.pitch, a.yaw, a.roll);
            const V3 o = apply(R, {0, -100, int16_t(a.box[5] + 100)});
            if (const auto hit = probe(a, {a.pos.x + o.x, a.pos.y + o.y, a.pos.z + o.z}, 0x4000, 1); !hit.empty())
                push(a, *hit[0], disp, target, true);
        }
        ride(a, dropFloor ? nullptr : floor, disp);
    }
    // Carriable ahead (state flagsB 2, before the position update): +0x20 0x200000 when 0x44e7d0 finds one.
    if ((r->flagsB & 2) && pickAhead(a, aheadPoint(a))) a.f20 |= 0x200000;
    if (!(r->flagsA & 0x800000) && disp == std::array<int32_t, 3>{}) a.f20 |= 0x100;  // no mover, no move: at rest
    // Position (0x453de0 @0x4582..): free / riding on +0x104 += the move; hung: the move turned into the parent's
    // yaw frame joins the local position, the record places it (a pole climb, flagsA 0x200, moves along y only).
    par = entity(a.parent);
    if (par && !a.rideRec) {
        if (r->flagsA & 0x200) a.local.y = int16_t(a.local.y + disp[1]);
        else {
            Mat m;
            mulRotY(m, -par->yaw);
            const V3 l = applyLV(m, {disp[0], disp[1], disp[2]});
            a.local = {int16_t(a.local.x + l.x), int16_t(a.local.y + l.y), int16_t(a.local.z + l.z)};
        }
        a.pos = hangPos(a, *par);
    } else if (!(r->flagsA & 0x200)) {
        a.pos = {int16_t(a.pos.x + disp[0]), int16_t(a.pos.y + disp[1]), int16_t(a.pos.z + disp[2])};  // s16 (0x403c40)
    }
    attackHits(a, touched);
    if (a.rec()->flagsC & 0x1089) a.f20 &= ~0x200u;  // before the anim (0x453de0): swim states hold gravity off one tick

    // Animation (0x469770): continue, or (re)start when the state's anim id differs from the playing one. Packet
    // 0xa00 of each applied TOD frame sets the attach point +0x84 (0x468280). flags0 0x200000: a sprite sequence.
    uint32_t res = AnimMissing;
    a.animLast = false;
    if (a.flags0 & 0x200000) {
        const bool same = a.curAnim == r->anim;
        res = spriteSeq(a, same ? 0 : r->anim, same ? 0 : r->mode);
        a.animLast = a.spr.flags & 0x2000;
    } else if (a.model) {
        a.modelInst = true;
        const Tod* tod0 = a.anim.tod;
        size_t next0 = a.anim.next;
        bool fresh = false, spoke = false;
        if (a.curAnim == r->anim) res = animTick(*a.model, a.anim, 0, 0);
        else if ((r->mode & 0x80) && (r->mode & 0xb) && !(r->mode & 0x30) && speechAnim(uint8_t(r->anim + 0x9a))) {
            // 0x469770 returns 0x200 right after starting the clip: +0x14e loses 0x3100, no frame, no asset logic
            res = AnimPlaying, spoke = true;
            a.anim.flags &= ~0x3100u, a.anim.holdEnd = false;
        }
        else if (findAnim(*a.model, r->anim) >= 0) {
            res = animTick(*a.model, a.anim, r->anim, r->mode);
            a.curAnim = r->anim;
            fresh = true;
        } else {
            animTick(*a.model, a.anim, 0, 0);  // absent anim: keep the current clip, report 0x400
        }
        // Trace G (state 21, anim 38: counter 1,1,2,2): 0x2000 is up on the tick the last frame's hold reaches 0 and on
        // the following loop-restart tick (+0x14e 0x3201), never on a fresh start (0x1201).
        a.animLast = !spoke && ((a.anim.frame == a.anim.last && a.anim.countdown == 0 && a.anim.last) || (!fresh && (a.anim.flags & 0x1000)));
        if (const Tod* t = a.anim.tod) {
            if (t != tod0 || a.anim.next < next0 || fresh) next0 = 0;  // a restarted clip re-applies frame 0
            // 0x468280 per applied frame: +0x84 = 0, a packet 0xa00 sets it and +0x7c, the part's world origin (a frame
            // without one leaves no attach point and +0x7c as it was)
            for (size_t fr = next0; fr < a.anim.next && fr < t->frames(); ++fr) {
                a.attachPart = -1;
                for (uint32_t k = t->frameStart[fr]; k < t->frameStart[fr + 1]; ++k)
                    if (t->packets[k].type == 0xa && t->packets[k].id) a.attachPart = int32_t(t->packets[k].id) - 1;
                if (a.attachPart >= 0) a.attachPos = partWorld(a, a.attachPart);  // +0x7c, before the asset logic
            }
            // 0x469770 @0x469a41: the tick that counts the frame hold +0x153 down to 0 runs the asset's logic list
            // (footsteps: '`' items), whether or not it applied a frame (a 2-tick hold steps on one tick, runs the
            // logic on the next; oracle w13 jump: sound 19, the S 82 box of the jump-out).
            if (a.anim.holdEnd)
                if (auto lg = animLogic_.find(t); lg != animLogic_.end())
                    for (TbBlock& b : lg->second) runItems(a, b);  // shared rand() stream (oracle tbrand/rand.csv)
        }
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
    bool logicEvent = false;
    if (fired || r->logicA || r->logicB) {
        if ((ev & 0x90000000) && r->logicA) ev = runBlock(a, r->logicA);
        if ((ev & 0x90000000) && r->logicB) ev = runBlock(a, r->logicB);
        if (!(ev & 0x90000000)) dispatch(a, ev), logicEvent = true;
    }
    r = a.rec();
    if (!logicEvent) {
        if (res == AnimFinished) {
            a.curAnim = 0;
            if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
            dispatch(a, 0x20000000);
        } else if ((r->flagsA & 0x80) && (a.f20 & 10)) {
            a.f20 &= ~0xc000000u;
            if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
            dispatch(a, 0x20000000);
        }
    }
    carryGrab(a);
    // 0x450440(a, 1) late in 0x453de0 (after the anim and the logic): a selected tag-0x61 box (S 82) morphs.
    if (a.boxMorph) boxStep(a);
    // Fell out of the world (0x453de0 @0x458a9c, players): the s16 y wrapped from below (start-of-tick y > 30000, now
    // < -30000) while falling faster than 0xc80: back to the checkpoint 0x4d26e0..ec as stored, velocity 0, camera
    // reset (0x43a0e0).
    if ((a.flags0 & 0x4000000) && int16_t(a.pos.y) < -30000 && tail.p.y > 30000 && int16_t(a.vel[1]) > 0xc80) {
        a.pos = {checkpoint[0], checkpoint[1], checkpoint[2]};
        a.yaw = checkpoint[3];
        a.vel = {};
        cam.init = false;
    }
    for (TbActor* c : order())  // children take its zone (0x453de0 tail)
        if (c->parent == int32_t(a.uid)) c->zone = a.zone;
    // Trigger zones last (0x453de0 tail, after the logic's event): flags0 0x1000000 or a state with flagsB 0x800000.
    // The tail first clears the per-tick bits (+0x20 &= 0xefae772f, +0x24 &= 0xfb6fba04 incl. the water bits 0x60,
    // +0x28 &= 0xfff6b77a); a water zone (0x44f550) re-raises 0x60, so the next tick's logic sees them.
    const uint32_t wet = a.f24;
    a.f20 &= 0xefae772fu, a.f24 &= 0xfb6fba04u, a.f28 &= 0xfff6b77au;
    if ((a.flags0 & 0x1000000) || (a.rec()->flagsB & 0x800000))
        if (const uint32_t z = zoneTick(a); !(z & 0x90000000)) dispatch(a, z);
    if ((wet & 0x40) && !(a.f24 & 0x40)) a.roll = a.pitch = 0, a.f20 &= ~0x200u;  // left the water
    if ((a.f24 & 0x40) && a.depth < 200) a.f24 |= 0x20;                         // shallow: depth <= 199
    if (!(wet & 0x40) && (a.f24 & 0x40)) {  // entered: gravity off, pitch / roll into [-0x800, 0x800]
        a.f20 |= 0x200;
        for (int16_t* v : {&a.pitch, &a.roll})
            if (*v > 0x800 || *v < -0x800) {
                int32_t r = *v % 0x1000;
                if (r > 0x800) r -= 0x1000;
                if (r < -0x800) r += 0x1000;
                *v = int16_t(r);
            }
        const int32_t self = int32_t(a.uid);  // carried entities with flags0 0x2802 are let go
        forEachEntity([&](TbActor& c) {
            if (c.parent != self || !(c.flags0 & 0x2802)) return;
            c.f20 &= ~0x8000u;
            c.f24 |= 0x80;
            if (c.progMask[1] & 0x80) runProgram(c, 0, 0x80, 0);
            detach(c);
        });
    }
    // Riding on +0x104 (0x453de0 @0x458d..): the local position follows the world one through the record (16x, rounded).
    if (TbActor* p = a.rideRec ? entity(a.parent) : nullptr) {
        const V3 w = hangPos(a, *p);
        const Mat m = a.parentPart >= 0 ? coordMat(p, a.parentPart) : recordMat(*p);
        Mat t;  // transposed rotation (0x404bc0)
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = 0; j < 3; ++j) t.m[i * 3 + j] = m.m[j * 3 + i];
        const V3 d = apply16(t, {int16_t((w.x - a.pos.x) << 4), int16_t((w.y - a.pos.y) << 4), int16_t((w.z - a.pos.z) << 4)});
        a.local = {int16_t(a.local.x - int16_t((d.x + 8) >> 4)), int16_t(a.local.y - int16_t((d.y + 8) >> 4)),
                   int16_t(a.local.z - int16_t((d.z + 8) >> 4))};
    }
    if (a.flags0 & 8) {  // +0x12a: heading of the start-of-tick forward in the end-of-tick chain
        const V3 f = apply(chainRot(a), fwd0);
        a.turnDelta = f.x == 0 && f.z == 0 ? 0 : int16_t(ratan2(f.x, f.z));
    }
}

std::vector<std::array<V3, 4>> TbWorld::waterZoneTops() const {
    std::vector<std::array<V3, 4>> out;
    for (const Zone& z : zones_) {
        if (!(z.flagsC & 0x1000)) continue;
        Mat m;  // zoneTick's world -> zone rotation; its transpose takes the zone's x / z back to the world
        if (!(z.flags8 & 1)) mulRotX(m, 0x1000 - z.rot[0]), mulRotY(m, 0x1000 - z.rot[1]), mulRotZ(m, 0x1000 - z.rot[2]);
        std::array<V3, 4> q;
        for (size_t k = 0; k < 4; ++k) {
            const int32_t lx = k == 1 || k == 2 ? z.extX : 0, lz = k >= 2 ? z.extZ : 0;
            q[k] = {z.origin.x + ((m.m[0] * lx + m.m[6] * lz) >> 12), z.origin.y + z.height, z.origin.z + ((m.m[2] * lx + m.m[8] * lz) >> 12)};
        }
        out.push_back(q);
    }
    return out;
}

bool TbWorld::inZone(const TbActor& a, const Zone& z) const {  // 0x448780's per-zone test
    const int32_t dx = int16_t(a.pos.x - z.origin.x), dy = int16_t(a.pos.y - z.origin.y), dz = int16_t(a.pos.z - z.origin.z);
    if (dy + a.box[4] >= 1 || dy < z.height) return false;
    int32_t rx = dx, rz = dz;
    if (!(z.flags8 & 1)) {
        Mat m;
        mulRotX(m, 0x1000 - z.rot[0]), mulRotY(m, 0x1000 - z.rot[1]), mulRotZ(m, 0x1000 - z.rot[2]);
        rx = int16_t((m.m[0] * dx + m.m[1] * dy + m.m[2] * dz) >> 12);
        rz = int16_t((m.m[6] * dx + m.m[7] * dy + m.m[8] * dz) >> 12);
    }
    return rx >= 0 && rx <= z.extX && rz >= 0 && rz <= z.extZ;
}

uint32_t TbWorld::zoneTick(TbActor& a) {
    uint32_t res = 0x80000000;
    if (restart) return res;  // 0x4d593a: a level change is pending
    for (Zone& z : zones_) {
        if (a.f20 & 0x2000) break;
        // 0x448780: the entity's top (y + box top +0x6a) at or above the origin, its feet no higher than the height;
        // x / z of (pos - origin) rotated by 0x1000 - the zone's angles (0x404210 X, Y, Z) within [0, extent].
        if (!inZone(a, z)) continue;
        const bool player = a.flags0 & 0x4000000;  // 8 / 9: 0x4d236c / 0x4d2368 = the focus / its partner
        const TbActor* foc = &actors[size_t(focus())];
        const TbActor* par = &actors[size_t(1 - focus())];
        for (auto& [id, b] : z.blocks) {
            if ((a.id == 0x15 || id == 0x15) && (a.flags1 & 2)) {
                a.f20 |= 0x2000;
                return 0x80000000;
            }
            if (a.id != id && !(player && (id == 10 || (&a == foc && id == 8) || (&a == par && id == 9)))) continue;
            if (z.flagsC & 0x1000) a.f24 |= 0x40, a.depth = int16_t(a.pos.y - z.height - z.origin.y);  // water: +0x1a2
            if (const uint32_t r = runItems(a, b); !(r & 0x90000000)) res = r;
            break;
        }
    }
    return res;
}

// ------------------------------------------------------------------------------------------------ camera
int16_t TbWorld::camYaw() const { return int16_t(ratan2(cam.look.x - cam.eye.x, cam.look.z - cam.eye.z)); }

namespace {
// 0x404210 with z = 0 (Rx, then 0x404430 Ry on the left) applied to an s16 vector by 0x403470 (s16 results).
V3 camRot(int32_t pitch, int32_t yaw, const V3& v) {
    Mat M;
    rotMatrixYXZ(M, pitch, yaw, 0);
    return apply16(M, v);
}
// The angles 0x437800 derives from a direction: yaw = atan2(x, z), pitch = -atan2(y, |xz|).
V3 camAlong(const V3& d, int32_t len) {
    return camRot(-ratan2(d.y, len3(d.x, 0, d.z)), ratan2(d.x, d.z), {0, 0, len});
}
int32_t sq3(const V3& d) {  // the exe's int32 sum of squares (wraps)
    return int32_t(uint32_t(d.x) * uint32_t(d.x) + uint32_t(d.y) * uint32_t(d.y) + uint32_t(d.z) * uint32_t(d.z));
}
int32_t len(const V3& d) { return len3(d.x, d.y, d.z); }
} // namespace

// 0x437180: eye = body = the focus at height - lookH, 100 behind its yaw; look = the focus at height - lookH.
void TbWorld::cameraPlace(const TbActor& t) {
    const int32_t h = (t.f24 & 0x40) ? 0 : cam.lookH;
    const V3 at{t.pos.x, t.pos.y - h, t.pos.z};
    cam.look = cam.prevLook = cam.prevEye = at;
    cam.eye = cam.body = cam.prevBody = {at.x + ((rsin(t.yaw + 0x800) * 100) >> 12), at.y, at.z + ((rcos(t.yaw + 0x800) * 100) >> 12)};
}

// 0x439570 -> 0x437530: view reset around t; flag 4 makes the next follow tick place it once more (with velocity).
// 0x437530 places (0x437180) before it sets B+0x134 = 300: the level load uses the value the block held before, 0 for
// the session's first level (oracle tbintro/full/cam.csv counter 1: y −27 = Bugs' y).
void TbWorld::cameraInit(const TbActor& t) {
    const int32_t h = cam.init ? cam.lookH : loadLookH;
    cam = {};
    cam.init = true;
    cam.flags = (GD[0xa4] & 0x10) ? 0x300u : 0x280u;
    cam.lookH = h;
    cameraReset(t);
    cam.lookH = TbCamera{}.lookH;
}

// 0x437260 (also '_' 0x10000000): flag 4, eye 100 behind t; the next follow tick places it again with velocity.
void TbWorld::cameraReset(const TbActor& t) {
    cam.flags = (cam.flags & 0xffffff84u) | 4;
    cam.blend = 0x1000;
    if (cam.flags & 0x100) cam.flags |= 0x80;
    cameraPlace(t);
    cam.vel = {}, cam.splitTicks = 0, cam.acc = 0, cam.ticks = 0;
    cam.lastFocus = camFocus;
}

// Follow camera 0x439700 -> 0x437800 (view 0 block B = 0x4d5120), transcribed; GAMEPLAY_TB.md §4.3. Not bit-exact:
// rays never hit actors (0x4463e0 query flag 1), and the 2-player framing measures the players with an approximate
// projection (0x406280 view matrix / RTPS not ported).
// 0x439700: every exit runs the shake 0x439650 on the look point after the follow / look / hold pose.
void TbWorld::cameraTick() {
    cameraFollow();
    CamShake& s = camShake;
    if (!s.amp) {
        s.off = 0;
        return;
    }
    s.off = int16_t((rsin(s.phase) * s.amp) >> 11);
    cam.look.x += s.off;
    s.phase = int16_t(s.phase + s.speed);  // s16: past 0x7fff it wraps below 0x800 and the decay pauses
    if (s.phase >= 0x800) s.amp = s.amp > 3 ? int16_t(s.amp - 3) : 0;
}

// The look view (0x439700 when L2 + R2 are held on P's pad record, flags1 0x400 ? 0 : 1, and nothing forbids it):
// the stick turns pitch (0x4d9a98, ±0x3de) and yaw (0x4d9a9a around P's, ±0x7bc) by 40/128 a tick; the look point is
// P's head (y − 400, 0 in water) kept off the walls by the 400 sphere of 0x440360, the eye 400 behind it. P is held
// (+0x20 0x80000000), its +0x24 0x10000 program runs once. The snipe variant (0x4d598c & 1) needs S 86: in no PC level.
// Leaving it (any follow tick): P faces the view's yaw and 0x4373c0 places the follow camera behind it.
bool TbWorld::lookView(TbActor& P) {
    const TbPad& lp = pads[(P.flags1 & 0x400) ? 0 : 1];
    const bool held = (lp.pressed & (PadL2 | PadR2)) == (PadL2 | PadR2);
    if (lookLock_ == 1 || !held || invuln || (P.f20 & 0x20d4421) || (P.f24 & 0x1000) || (camScript.flags & 0x2010) || S[9] ||
        ((P.f20 & 0x20000000) && (P.f24 & 0x80000)) || GD[0x9f]) {
        lookUp_ = -200, lookTurn_ = 0;
        if (lookMode_) {
            P.pitch = 0, P.yaw = lookYaw_, P.roll = 0;
            lookLock_ = 3;
            cameraReset(P);
            cam.flags |= 0x400;
        }
        camScript.flags &= 0xffbfffefu;
        lookMode_ = false;
        P.f24 &= ~0x10000u;
        return false;
    }
    P.f20 |= 0x80000000u;
    camScript.flags |= 0x400000;
    lookUp_ = int16_t(std::clamp(lookUp_ - int16_t(lp.sy * 40 >> 7), -0x3de, 0x3de));
    lookTurn_ = int16_t(std::clamp(lookTurn_ + int16_t(lp.sx * 40 >> 7), -0x7bc, 0x7bc));
    const int32_t yaw = P.yaw + lookTurn_;
    V3 head{P.pos.x, P.pos.y - ((P.f24 & 0x40) ? 0 : 400), P.pos.z};
    V3 push{};
    cameraSweep(P, head, push, 400, true);
    head = {head.x + push.x, head.y + push.y, head.z + push.z};
    const V3 off = camRot(lookUp_, yaw, {0, 0, 400});
    cam.body = cam.look = head;
    cam.eye = {head.x - off.x, head.y - off.y, head.z - off.z};
    lookPitch_ = lookUp_, lookYaw_ = int16_t(yaw);
    lookMode_ = true;
    if (!(P.f24 & 0x10000)) {
        P.f24 |= 0x10000;
        if (P.progMask[1] & 0x10000) runProgram(P, 0, 0x10000);
        actors[size_t(1 - camFocus)].f24 &= ~0x10000u;
    }
    return true;
}

void TbWorld::cameraFollow() {
    const TbActor& F = actors[size_t(camFocus)];       // 0x4d236c (the focus)
    const TbActor& Pn = actors[size_t(1 - camFocus)];  // 0x4d2368
    // 0x439700: param_1 = the focus, or the director in B+0x44 while its state has flagsB 0x200 (it gets +0x20
    // 0x80000000). 0x437800 aims, sweeps and resets at param_1; the snap, focus-change and 2P tests stay on the focus.
    TbActor* P = &actors[size_t(camFocus)];
    if (TbActor* t = entity(camTarget); t && t->rec() && (t->rec()->flagsB & 0x200)) {
        t->f20 |= 0x80000000u;
        P = t;
    }
    if (!cam.init) cameraInit(*P);  // the top of 0x437800 without flag 0x200 (as 0x437530)
    // Held by a model camera node (B+0x3c & 0x300, 0x464820 wrote eye / look / body): no follow; 0x436fd0 clears
    // flag 4 and 0x436fe0 sets 0x2000, so the next follow tick restores the pre-hold pose and tests its line of sight.
    if (camScript.flags & 0x300) {
        cam.flags = (cam.flags & ~4u) | 0x2000;
        camTarget = -1;
        return;
    }
    if (lookView(*P)) return;
    // Scripted points ('_' op 0x15, B+0x3c 0xc00): look = the look point (0x800) or P - (0, 400, 0), eye = the eye
    // point (0x400), each by 1/4 with 0x20; 0x4000 / 0x8000 also make them the restored pose, body = eye. One-shot
    // 0x10000 drops the eye point, 8 the look point; then 0x436fd0 / 0x436fe0 as the hold above.
    if (const uint32_t sf = camScript.flags; sf & 0xc00) {
        auto to = [&](V3& v, const V3& t) {
            if (sf & 0x20) v = {v.x - ((v.x - t.x) >> 2), v.y - ((v.y - t.y) >> 2), v.z - ((v.z - t.z) >> 2)};
            else v = t;
        };
        to(cam.look, sf & 0x800 ? camScript.look : V3{P->pos.x, P->pos.y - 400, P->pos.z});
        if (sf & 0x400) to(cam.eye, camScript.eye);
        if (sf & 0x4000) cam.prevEye = cam.prevBody = cam.eye;
        if (sf & 0x8000) cam.prevLook = cam.look;
        cam.body = cam.eye;
        if (sf & 0x10000) camScript.flags &= 0xfffebbffu;
        if (camScript.flags & 8) camScript.flags &= 0xffff77f7u;
        camScript.flags |= 0x40;
        cam.flags = (cam.flags & ~4u) | 0x2000;
        return;
    }
    // B+0xbc (track path) is never set on PC (see '_'); 0x10000 / 8 without a point skip the follow for a tick.
    const bool hold = camScript.flags & 0x10008;
    camScript.flags &= 0xfffe23e7u;
    if (hold) return;
    const bool low = (P->f24 & 0x40) && !(P->f24 & 0x20);
    const int32_t aimH = low ? 0 : cam.aimH, lookH = low ? 0 : cam.lookH;
    auto resetFlags = [&](uint32_t f) {
        cam.flags = (f & 0xffffff84u) | 4;
        cam.blend = 0x1000;
        if (cam.flags & 0x100) cam.flags |= 0x80;
    };
    cam.body = cam.prevBody, cam.look = cam.prevLook, cam.eye = cam.prevEye;
    const V3 rel{int16_t(cam.body.x - F.pos.x), int16_t(cam.body.y - F.pos.y + lookH), int16_t(cam.body.z - F.pos.z)};
    if (!(cam.flags & 0x80) || sq3(rel) < 0x5f5e11 || (cam.flags & 0x2000)) {
        if (cam.lastFocus != camFocus || (cam.flags & 0x2000)) {
            const V3 from{int16_t(F.pos.x), int16_t(F.pos.y - lookH), int16_t(F.pos.z)};
            if (col.query(from, rel, 8) || col.query(cam.body, {-rel.x, -rel.y, -rel.z}, 8)) {
                resetFlags(cam.flags);
                cam.flags &= ~0x2000u;
            }
        }
    } else {
        resetFlags(cam.flags);
    }
    cam.lastFocus = camFocus;
    if (cam.blend < 0x1000) {
        const int32_t s = (0x1000 - cam.blend) >> 3;
        cam.blend = s == 0 ? 0x1000 : cam.blend + s;
    }
    V3 T;
    if (!(cam.flags & 0x80) && !(GD[0xa2] & 3))
        T = cam.blend == 0x1000 ? V3{(Pn.pos.x + F.pos.x) >> 1, (Pn.pos.y + F.pos.y) >> 1, (Pn.pos.z + F.pos.z) >> 1}
                                : V3{((Pn.pos.x - F.pos.x) * cam.blend >> 13) + F.pos.x, ((Pn.pos.y - F.pos.y) * cam.blend >> 13) + F.pos.y,
                                     ((Pn.pos.z - F.pos.z) * cam.blend >> 13) + F.pos.z};
    else
        T = cam.blend == 0x1000 ? P->pos
                                : V3{((P->pos.x - cam.saved.x) * cam.blend >> 12) + cam.saved.x, ((P->pos.y - cam.saved.y) * cam.blend >> 12) + cam.saved.y,
                                     ((P->pos.z - cam.saved.z) * cam.blend >> 12) + cam.saved.z};
    cam.ticks = (cam.ticks + 1) & 0x7fffffff;
    if (!(cam.flags & 0x38)) {  // L2 / R2 orbit (pad word GD+8: bit 1 L2, bit 2 R2, bit 4 L1; I)
        const TbPad& fp = padOf(*P);
        const bool l2 = fp.held & PadL2, r2 = fp.held & PadR2;
        if (!(fp.held & PadL1) && !(P->f20 & 0x2000000) && camTarget < 0 && !(camScript.flags & 0x220000) && l2 != r2) cam.acc += l2 ? 0x5000 : -0x5000;
    } else {
        cam.acc = 0;
    }
    auto hdist = [](int32_t x, int32_t z) { return len3(x, 0, z); };
    auto split = [&] {  // one focus from now on (the players are too far apart)
        cam.splitTicks = 0, cam.blend = 0, cam.flags |= 0x80, cam.saved = T;
    };
    if ((cam.flags & 0x100) && !(cam.flags & 0x80) && !(GD[0xa2] & 3)) {
        // 2P framing: screen error e of both players' feet / heads against ±190 x ±90 (approximate projection).
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
        if (e == 0 || e < 0) {
            if (cam.rest < cam.dist) cam.dist -= 30;
        } else if (e > 15) {
            cam.dist += e;
        }
        const int32_t dy = Pn.pos.y - F.pos.y;
        if (cam.dist > 3000 || hdist(Pn.pos.x - F.pos.x, Pn.pos.z - F.pos.z) > 2700 || dy > 1000 || dy < -1000) split();
    } else if ((cam.flags & 0x100) && !(GD[0xa2] & 3)) {  // 2P split: regroup when close, in front and in sight
        ++cam.splitTicks;
        cam.dist -= (cam.dist - cam.rest) >> 2;
        const int32_t dy = Pn.pos.y - F.pos.y;
        const int64_t depth = int64_t(Pn.pos.x - cam.eye.x) * (cam.look.x - cam.eye.x) + int64_t(Pn.pos.y - cam.eye.y) * (cam.look.y - cam.eye.y) +
                              int64_t(Pn.pos.z - cam.eye.z) * (cam.look.z - cam.eye.z);
        if (dy < 1000 && dy > -1000 && hdist(Pn.pos.x - F.pos.x, Pn.pos.z - F.pos.z) < 0x8fc && cam.splitTicks > 30 && depth > 0 &&
            !col.query(cam.body, {int16_t(Pn.pos.x - cam.body.x), int16_t(Pn.pos.y - lookH - cam.body.y), int16_t(Pn.pos.z - cam.body.z)}, 8))
            cam.blend = 0, cam.flags &= ~0x80u;
    }
    // Line of sight body -> focus (q 0xb), and back (q 9): a wall between them pushes the body along the summed hit
    // normals (flag 0x10), a near-vertical sum resets (flag 4), a camera-guide surface (flag 0x10) slides on it (0x20).
    V3 dir;  // scratch +0x10: push direction (Q12 · 2)
    const V3 toF{int16_t(P->pos.x - cam.body.x), int16_t(P->pos.y - cam.body.y - lookH), int16_t(P->pos.z - cam.body.z)};
    std::optional<TbHit> hit = col.query(cam.body, toF, 8, 0xb);
    if (hit && (cam.flags & 0x20) && cam.guidePoly == hit->poly) hit.reset();
    uint32_t fl = cam.flags & ~0x18u;
    if (hit) {
        const TbSurface& sf = col.surfaces[col.polys[size_t(hit->poly)].surf];
        const V3 n1{hit->n[0] >> 2, hit->n[1] >> 2, hit->n[2] >> 2};
        std::optional<TbHit> back = hit;
        if (!(sf.flags & 0x10)) {
            back = col.query({int16_t(P->pos.x), int16_t(P->pos.y - lookH), int16_t(P->pos.z)}, {-toF.x, -toF.y, -toF.z}, 8, 9);
            if (back) cam.n2 = {back->n[0] >> 2, back->n[1] >> 2, back->n[2] >> 2};
        }
        if (back) {
            cam.guidePoly = hit->poly;
            const V3 sum{int16_t(n1.x + cam.n2.x), int16_t(n1.y + cam.n2.y), int16_t(n1.z + cam.n2.z)};
            const int32_t h = len3(sum.x, 0, sum.z);
            dir = camRot(-ratan2(sum.y, h), ratan2(sum.x, sum.z), {0, 0, 0x2000});
            if (sf.flags & 0x10) {
                cam.guideN = n1;
                const TbPoly& P = col.polys[size_t(hit->poly)];
                int64_t c[3] = {};
                for (uint32_t k = P.edge; k < P.edge + P.nv; ++k) {
                    const V3& v = col.verts[col.edges[k].v];
                    c[0] += v.x, c[1] += v.y, c[2] += v.z;
                }
                const int32_t n = std::max<int32_t>(1, P.nv);
                cam.guidePt = {int32_t(c[0] / n), int32_t(c[1] / n), int32_t(c[2] / n)};
                fl = cam.flags | 0x20;
            } else {
                fl = cam.flags | (h < 5 ? 4u : 0x10u);
            }
        }
    }
    cam.flags = fl;
    if (cam.flags & 4) {  // reset: 10 per tick away from the focus' facing, eye 100 behind it (0x437180)
        cam.vel = {rsin(P->yaw + 0x800) * cam.resetSpeed, 0, rcos(P->yaw + 0x800) * cam.resetSpeed};
        cameraPlace(*P);
        cam.flags &= ~4u;
    }
    const bool pushed = (cam.flags & 0x10) && (cam.flags & 0x100) && !(cam.flags & 0x80);
    V3 d = pushed ? V3{F.pos.x - cam.eye.x, F.pos.y - aimH - cam.eye.y, F.pos.z - cam.eye.z}
                  : V3{T.x - cam.eye.x, T.y - aimH - cam.eye.y, T.z - cam.eye.z};
    const int32_t dl = len(d);
    if (dl < 2000) cam.flags &= ~0x2000u;
    int32_t e = (dl - cam.dist) >> 2;  // spring: a quarter of the length error per tick, eye -> aim
    cam.flags = e == 0 ? cam.flags | 0x1000 : cam.flags & ~0x1000u;
    if (e == 0 && !(cam.flags & 0x38)) {
        cam.flags &= ~0x400u;
    } else if (!(cam.flags & 0x20)) {
        if (cam.flags & 0x10) {
            cam.flags &= ~0x400u;
            cam.vel = {dir.x * cam.pushSpeed, dir.y * cam.pushSpeed, dir.z * cam.pushSpeed};
            if ((cam.flags & 0x100) && !(cam.flags & 0x80)) split();
        }
    } else {  // camera guide surface
        cam.flags = (cam.flags & ~0x400u) | 0x40;
        const V3& n = cam.guideN;
        const V3 v{cam.guidePt.x - cam.body.x, cam.guidePt.y - cam.body.y, cam.guidePt.z - cam.body.z};
        const int32_t dot = (n.y * v.y + n.x * v.x + n.z * v.z) >> 12;
        const int32_t l94 = len(v);
        if (-cam.radius < dot) {
            d = {-n.x, -n.y, -n.z};
        } else {
            d = v;
            const int32_t l = len(d);
            const V3 u{int16_t(n.x + int16_t((d.x << 12) / l)), int16_t(n.y + int16_t((d.y << 12) / l)), int16_t(n.z + int16_t((d.z << 12) / l))};
            const int32_t g = cam.guideSpeed;
            cam.vel = {cam.vel[0] + (u.x * g >> 12) * 0x1000, cam.vel[1] + (u.y * g >> 12) * 0x1000, cam.vel[2] + (u.z * g >> 12) * 0x1000};
        }
        if (cam.radius < dot) d = {T.x - cam.body.x, T.y - cam.body.y - aimH, T.z - cam.body.z}, cam.flags &= ~0x60u;
        const V3 w{cam.guidePt.x - T.x, cam.guidePt.y - T.y, cam.guidePt.z - T.z};
        const int32_t l96 = len(w);
        if ((n.x * w.x + n.y * w.y + n.z * w.z) >> 12 < 0) d = {T.x - cam.body.x, T.y - cam.body.y - aimH, T.z - cam.body.z}, cam.flags &= ~0x60u;
        if (cam.flags & 0x20) e = int16_t((l94 - cam.dist + l96) >> 2);
    }
    const V3 mv = camAlong(d, int16_t(e));
    V3 move = pushed ? V3{cam.vel[0] >> 12, cam.vel[1] >> 12, cam.vel[2] >> 12}
                     : V3{(cam.vel[0] >> 12) + cam.orbit.x + mv.x, (cam.vel[1] >> 12) + mv.y, (cam.vel[2] >> 12) + cam.orbit.z + mv.z};
    for (int32_t& v : cam.vel) {
        v -= (v * cam.damp) >> 12;
        if (v >> 12 == 0 || v >> 12 == -1) v = 0;
    }
    if (!(cam.flags & 0x40) && !pushed) {
        if (P->f20 & 0x421) {  // airborne: the body follows the look height
            move.y -= fN(cam.look.y - T.y + lookH, 4);
            cam.ticks = 0;
        }
        if (cam.ticks > 20 && !(cam.flags & 0x1000)) {  // vertical band B+0x128: body within 50 of look - 100
            const int32_t v = cam.look.y - cam.body.y - aimH + lookH;
            int32_t a = std::abs(v);
            if (a > cam.band) {
                a -= cam.band;
                move.y += a < 0x1000 ? a * v >> 12 : v;
            }
        }
        if (cameraSweep(*P, cam.body, move, cam.radius, false)) {
            cam.flags &= ~0x400u;
            if ((cam.flags & 0x18) && sq3(move) < 100) cam.flags |= 4;
            cam.ticks = 0;
        }
    }
    cam.body = {cam.body.x + move.x, cam.body.y + move.y, cam.body.z + move.z};
    cam.look.x -= fN(cam.look.x - T.x, 8);
    cam.look.z -= fN(cam.look.z - T.z, 8);
    const int32_t ly = cam.look.y - T.y + lookH;
    cam.look.y -= fN(ly, ly < 0 ? 5 : 4);
    const V3 lb{cam.look.x - cam.body.x, cam.look.y - cam.body.y, cam.look.z - cam.body.z};
    const int16_t yaw = int16_t(ratan2(lb.x, lb.z));
    const V3 off = camRot(int16_t(-ratan2(lb.y, len3(lb.x, 0, lb.z))), yaw, {0, 0, -400});
    cam.eye = {cam.body.x + off.x, cam.body.y + off.y, cam.body.z + off.z};
    cam.prevBody = cam.body, cam.prevLook = cam.look, cam.prevEye = cam.eye;
    if (cam.eye.y < cam.look.y && !(cam.flags & 0x400) && len3(lb.x, 0, lb.z) < cam.wallR) {  // too close: sidestep
        const int32_t a = ratan2(lb.x, lb.z) - yaw < 0 ? yaw + 0x400 : yaw - 0x400;
        cam.vel[0] += rsin(a) * cam.wallPush;
        cam.vel[2] += rcos(a) * cam.wallPush;
    }
    cam.orbit = {};
    if (cam.acc >> 12 != 0 && cam.acc >> 12 != -1) {  // orbit step applied by the next tick's move
        const int32_t x = cam.body.x - T.x, z = cam.body.z - T.z;
        const int32_t r = len3(x, 0, z), a = ratan2(x, z) + (cam.acc >> 12);
        cam.orbit = {(rsin(a) * r >> 12) - x, 0, (rcos(a) * r >> 12) - z};
        cam.acc -= cam.acc * 400 >> 12;
    }
}

// 0x450440(a, 1): the selected tag-0x61 box record +0x123 is reached by ±20 per tick and bound (at once with S 82's
// flag 2); the radius +0x4a follows; reaching it clears the morph.
void TbWorld::boxStep(TbActor& a) {
    if (a.boxIdx < 0 || size_t(a.boxIdx) >= a.boxes.size()) return;
    const auto& t = a.boxes[size_t(a.boxIdx)];
    for (size_t i = 0; i < 6; ++i)
        a.box[i] = a.boxInstant ? t[i] : int16_t(a.box[i] < t[i] ? std::min(a.box[i] + 20, int32_t(t[i])) : std::max(a.box[i] - 20, int32_t(t[i])));
    a.boxInstant = false;
    a.radius = boxRadius(a.box);
    if (a.box == t) a.boxMorph = false;
}

} // namespace bb::game
