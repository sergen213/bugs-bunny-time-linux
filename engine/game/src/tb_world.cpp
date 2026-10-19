#include "bb/game/tb_world.h"
#include "bb/tb_level.h"

#include <algorithm>
#include <cmath>
#include <format>

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
void linkSkips(std::vector<TbLogicItem>& b) {
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

constexpr uint32_t kNoEvent = 0x50000000;

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

TbWorld TbWorld::load(const BzeFile& bze, const Scene& scene) {
    TbWorld w;
    w.col = TbCollision::fromLevel(bze);
    const int i1 = bze.find(1);
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1));
    const TbScript script = parseTbScript(s1);
    std::map<uint16_t, const TbChunk*> templates;
    for (const TbChunk& ch : script.chunks) {
        const auto id = ch.tag(0x27);
        if (ch.type == 0x08 && !id.empty()) templates.emplace(le16(id, 0), &ch);
        if (const auto p = ch.tag(0x3c); ch.type == 0x3a && !p.empty()) w.edgeMask = le16(p, 12);  // §4.3
        for (const TbTag& t : ch.tags)  // §6: first action-126 setter = the level-start track (I/H)
            if (t.id == 0x53 && le16(t.data, 0) == 126 && !w.music) w.music = t.data[2];
    }
    bool found[2] = {};
    for (const TbChunk& ch : script.chunks) {
        const auto id = ch.tag(0x27), pos = ch.tag(0x10);
        if ((ch.type != 0x07 && ch.type != 0x0a && ch.type != 0x63) || id.empty() || pos.empty()) continue;
        const uint16_t eid = le16(id, 0);
        if ((eid != 2 && eid != 3) || found[eid - 2]) continue;
        found[eid - 2] = true;
        TbActor& a = w.actors[size_t(eid - 2)];
        a.id = eid;
        a.spawn = a.pos = {s16(pos, 0), s16(pos, 4), s16(pos, 8)};
        if (const auto r = ch.tag(0x11); !r.empty()) a.spawnYaw = a.yaw = int16_t(wrap(s16(r, 2)));
        if (const auto f = ch.tag(0x16); !f.empty()) a.flags0 = le32(f, 0), a.flags1 = le32(f, 4), a.flags10 = le32(f, 8);
        const TbChunk* tmpl = nullptr;
        if (auto t = templates.find(le16(id, 2)); t != templates.end()) tmpl = t->second;
        for (const TbChunk* src : {tmpl, &ch}) {  // template first, own records win (tb_moves.py character)
            if (!src) continue;
            std::map<uint16_t, std::vector<TbLogicItem>> blocks;
            std::vector<TbLogicItem>* cur = nullptr;
            for (const TbTag& t : src->tags) {
                if (t.id == 0x2f) {
                    const TbStateRec s = parseState(t.data);
                    auto it = std::ranges::find(a.states, s.id, &TbStateRec::id);
                    if (it != a.states.end()) *it = s; else a.states.push_back(s);
                } else if (t.id == 0x31) {
                    TbTransition tr{le16(t.data, 0), {le16(t.data, 2), le16(t.data, 4), le16(t.data, 6), le16(t.data, 8), le16(t.data, 10)}};
                    auto it = std::ranges::find(a.trans, tr.event, &TbTransition::event);
                    if (it != a.trans.end()) *it = tr; else a.trans.push_back(tr);
                }
                if (t.id == 0x4d) {
                    cur = &(blocks[le16(t.data, 0)] = {});
                } else if (t.id >= 0x4e && t.id <= 0x60 && cur) {
                    TbLogicItem it;
                    it.tag = t.id;
                    std::copy_n(t.data.begin(), std::min(t.data.size(), it.d.size()), it.d.begin());
                    cur->push_back(it);
                } else {
                    cur = nullptr;
                }
            }
            for (auto& [bid, items] : blocks) a.blocks[bid] = std::move(items);
        }
        for (auto& [bid, items] : a.blocks) linkSkips(items);
        for (const TbChunk* src : {&ch, tmpl}) {  // tag 0x61 box records: the instance's, else the template's
            if (!src || !a.boxes.empty()) continue;
            for (const TbTag& t : src->tags)
                if (t.id == 0x61) a.boxes.push_back({s16(t.data, 2), s16(t.data, 4), s16(t.data, 6), s16(t.data, 8), s16(t.data, 10), s16(t.data, 12)});
        }
        if (!a.boxes.empty()) a.box = a.boxes[0];
        for (const ActorInstance& ai : scene.actors)
            if (ai.entityId == eid && ai.chunk != 0x08 && ai.model < scene.actorModels.size()) {
                a.model = &scene.actorModels[ai.model];
                break;
            }
    }
    if (!found[0] || !found[1]) throw FormatError("tb world: Bugs (id 2) and Taz (id 3) entities required");
    // §2.1: run speed S[0x66] (Bugs) / S[0x67] (Taz) = 25 in most level-logic setters (402/450, 150 setters);
    // level logic is not run here, so the common value is preset (M).
    w.S[0x66] = w.S[0x67] = 25;
    return w;
}

void TbWorld::setTwoPlayer(bool on) {
    A4 = on ? uint8_t(A4 | 0x10) : uint8_t(A4 & ~0x12);
    cam.init = false;  // follow flags are chosen once at camera init (§4.3)
}

void TbWorld::tick(const PadInput& p1, const PadInput& p2) {
    tbPadTick(pads[0], p1, edgeMask);
    if (A4 & 0x10) tbPadTick(pads[1], p2, edgeMask);
    else pads[1] = {};  // the P2 report is read only in 2-player mode (GAMEPLAY.md §2.3)
    dt = ticks == 0 ? 0 : 2;
    lastCallFriend = 0;
    camFocus = focus();  // 0x4011c0 latch before 0x45eee0: the camera follows the old focus this tick
    cameraTick();
    switchTick();
    for (TbActor& a : actors) actorTick(a);
    ++ticks;
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
    if (cooldown < 45) ++cooldown;
    auto r1Rule = [&](uint16_t held) {
        const bool r1 = held & PadR1;
        if (r1 && !(A4 & 8) && !(p.f20 & 0x2000000) && !(S[3] & 0xc) && !GD[0x9f]) {
            A4 |= 8;
            A4 ^= 4;
            for (TbActor& a : actors) a.f28 |= 0x100;  // "switched" entity programs (not interpreted)
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
    if (!force && (GD[0x9f] || (S[3] & 3) || airTicks > 2 || (f.f24 & 0x40) || (p.f20 & 0x2000000) || cooldown < 45))
        return false;  // refusal sound 0x5f (not played)
    // place() 0x44ecd0
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
        const auto fl = col.query({c.x, c.y - 150, c.z}, {0, 400, 0}, 20, q);
        bool clear = true;
        for (int k = 0; k < 4 && clear; ++k) clear = !col.query(c, rotY(k * 0x400, 0, 0, 60), 20, q);
        if (fl && fl->n[1] < kTbFloorNy && clear) best = qd, result = c;
    }
    if (best < 0x57e5) return false;
    p.pos = result;
    p.yaw = lookYaw_;
    p.f20 = (p.f20 | 0x10000000) & ~0x4200u;
    p.f24 &= ~0x40u;
    cooldown = 0;
    lastCallFriend = 1;
    return true;
}

// ------------------------------------------------------------------------------------------------ logic VM
bool TbWorld::cond(TbActor& a, const TbLogicItem& it, bool cur) {
    const std::span<const uint8_t> d(it.d);
    switch (it.tag) {
    case 0x4f: return a.f20 & le32(d, 4);
    case 0x50: return a.f24 & le32(d, 4);
    case 0x51: return a.f28 & le32(d, 4);
    case 0x5c: return cur;  // ELSE: no test (op 0x12), NOT inverts
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
    case 0x52: {  // 'R' {u16 flags, u8 pad, u8 cond, s16 X, s16 Y}
        const int ci = d[3], x = s16(d, 4), y = s16(d, 6);
        auto Sv = [&](int i) -> int { return i >= 0 && size_t(i) < S.size() ? S[size_t(i)] : 0; };
        switch (ci) {
        case 0x01: return Sv(y) == (x & 0xff);
        case 0x05: return Sv(y) >= x;
        case 0x07: return Sv(y) < x;
        case 0x10: return a.stateTicks > (uint32_t(uint16_t(y)) << 8 | uint16_t(x));
        case 0x18: return Sv(y) & x;
        case 0x19: return 0x58 + y >= 0 && size_t(0x58 + y) < GD.size() && (GD[size_t(0x58 + y)] & x);
        case 0x49: return x <= 0 && 0 <= y;  // water depth +0x1a2: no water simulated
        default: unsupported.insert(std::format("R{:#x}", ci)); return false;
        }
    }
    default:
        unsupported.insert(std::string(1, char(it.tag)));
        return false;
    }
}

void TbWorld::action(TbActor& a, const TbLogicItem& it) {
    const std::span<const uint8_t> d(it.d);
    if (it.tag == 0x53) {
        const int id = le16(d, 0), x = s16(d, 2), y = s16(d, 4);
        if (id == 10 && y >= 0 && size_t(y) < S.size()) S[size_t(y)] = uint8_t(x);   // 0x430ce0
        else if (id == 126) music = x & 0xff;                                      // 0x432db0
        else if (id == 79) {                                                       // 0x432320 forced switch
            if (!(GD[0xa2] & 3) && (x == 0 || x == actors[size_t(1 - focus())].id)) {
                A4 ^= 4;
                for (TbActor& c : actors) c.f28 |= 0x100;
            }
        } else if (id == 82) {  // 0x432530: select collision box record (tag 0x61), arg2 & 2 = no morph
            if (x < int(a.boxes.size()) && x != a.boxIdx) a.boxIdx = x, a.boxInstant = y & 2;
        } else unsupported.insert(std::format("S{}", id));
        return;
    }
    if (it.tag == 0x55) {  // surface ops 28/29/30 (§1.3)
        const uint32_t idx = le32(d, 0), A = le32(d, 4), B = le32(d, 8);
        for (TbSurface& s : col.surfaces)
            if (B == 0x80000000u || s.id == B) {
                if (idx == 28) s.flags |= A;
                else if (idx == 29) s.flags &= ~A;
                else if (idx == 30) s.friction = uint16_t(A);
            }
        if (idx < 28 || idx > 30) unsupported.insert(std::format("U{}", idx));
        return;
    }
    (void)a;
    unsupported.insert(std::string(1, char(it.tag)));
}

// VM 0x45d7a0 at tag level: statement markers (0x08 IF: ends the block when the previous statement was true,
// 0x10 IF, 0x20 nested: skips when false), chains (1 AND: skip when false, 2 OR: keep when true, 4 NOT),
// 'N' returns its event, actions run when true; a false AND / nested marker jumps to the current skip target.
uint32_t TbWorld::runBlock(TbActor& a, uint16_t block) {
    auto bit = a.blocks.find(block);
    if (bit == a.blocks.end()) return 0x80000000;
    const std::vector<TbLogicItem>& b = bit->second;
    bool cur = true, stmt = false;
    int32_t skip = -1;
    for (size_t i = 0; i < b.size();) {
        const TbLogicItem& it = b[i];
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
                    return kNoEvent;
                }
                cur = true, stmt = true, skip = it.skip;
            }
            if ((f & 1) && !cur) {
                if (doSkip()) continue;
                return kNoEvent;
            }
            if (!((f & 2) && cur)) cur = cond(a, it, cur) != bool(f & 4);
        } else if (it.tag == 0x4e) {
            if (cur) return le32(it.d, 0);
            if (doSkip()) continue;
            return kNoEvent;
        } else {
            if (!cur) {
                if (doSkip()) continue;
                return kNoEvent;
            }
            action(a, it);
        }
        ++i;
    }
    return cur ? kNoEvent : 0x10000000;
}

// ------------------------------------------------------------------------------------------------ states
// 0x44d990: event -> transition sequence -> state entry (§3.1, §2.3).
void TbWorld::dispatch(TbActor& a, uint32_t ev) {
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
            if (a.f20 & 0x2000) return;
            ev = 1;
        } else if (const int32_t s = findState(id); s >= 0) {
            st = s;
        } else {
            return;  // missing state: the exe spins forever here
        }
    }
    if (!(ev & 0x27000000) || tr < 0) {
        int32_t t = -1;
        if (ev & 0x8000000) {
            for (int32_t e : {int32_t(a.rec() ? a.rec()->ownHurt : 0), 0, 3})
                if (e && (t = findTr(uint32_t(e))) >= 0) break;
        } else {
            if (ev & 0x10000000) ev = 4;  // death overrides DAT_004d2720/0x4d5948 not set
            t = findTr(ev & 0xffff);
            if (t < 0 && ev == 2) t = findTr(1);
        }
        if (t < 0) return;
        tr = t;
        a.seq = 0;
        st = findState(a.trans[size_t(t)].seq[0]);
        if (st >= 0 && (a.states[size_t(st)].flagsB & 0x4000000)) {  // walk/run split by stick magnitude
            const TbPad& p = padOf(a);
            a.seq = isqrt(p.sx * p.sx + p.sy * p.sy) >= 0x78;
            st = findState(a.trans[size_t(t)].seq[a.seq]);
        }
        if (st < 0) return;
    }
    a.tr = tr;
    a.state = st;
    a.stateTicks = 0;
    const TbStateRec& r = a.states[size_t(st)];
    if (!(r.mode & 0x40) || r.anim != oldAnim) a.curAnim = 0;  // restart the clip next tick
    if (r.flagsB & 0x800) a.f20 &= ~0x421u;
    auto sval = [&](int16_t v, int shift4000) -> int32_t {
        if (v > 0 && (v & 0x4000)) return int32_t(int8_t(S[size_t(v & 0x3fff)])) << shift4000;
        if (v > 0 && (v & 0x2000)) return int32_t(int8_t(S[size_t(v & 0x1fff)]));  // x DAT_004d26b8 (I: 1)
        return v;
    };
    if (!((r.flagsA & 0x18) || (r.flagsB & 0x104000)) || ((a.f20 & 0x421) && !(a.f20 & 0x4000))) return;
    a.f20 &= ~0x100u;
    if (r.flagsB & 0x100000) return;  // homing jump to the selector target: not ported
    if (!(r.flagsA & 0x18)) return;
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
}

void TbWorld::actorTick(TbActor& a) {
    if (!a.started) {
        a.started = true;
        dispatch(a, 2);  // level start
    }
    const TbStateRec* r = a.rec();
    if (!r) return;
    ++a.stateTicks;
    const TbPad& pd = padOf(a);
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
    // local -> world: R(yaw) (s << 5), (w + 0x10) >> 5 (§2.1; pitch/roll leave a forward vector unchanged here)
    const int32_t c = rcos(a.yaw), sn = rsin(a.yaw);
    const std::array<int32_t, 3> target{((((c * (s[0] << 5)) + (sn * (s[2] << 5))) >> 12) + 0x10) >> 5, s[1],
                                        ((((-sn * (s[0] << 5)) + (c * (s[2] << 5))) >> 12) + 0x10) >> 5};
    if (r->flagsA & 0x800000) move(a, target);
    else {
        a.vel = {target[0] * 16, target[1] * 16, target[2] * 16};
        a.pos = {a.pos.x + target[0], a.pos.y + target[1], a.pos.z + target[2]};
    }
    if (r->flagsA & 1) a.vel = {};
    if (a.pos.y > 32000) {  // fell out of the world (exe: s16 wrap test): back to the spawn (checkpoint I)
        a.pos = a.spawn, a.vel = {}, a.yaw = a.spawnYaw;
        cam.init = false;
    }

    // Animation (0x469770): continue, or (re)start when the state's anim id differs from the playing one.
    uint32_t res = AnimMissing;
    if (a.model) {
        if (a.curAnim == r->anim) res = animTick(*a.model, a.anim, 0, 0);
        else if (findAnim(*a.model, r->anim) >= 0) {
            res = animTick(*a.model, a.anim, r->anim, r->mode);
            a.curAnim = r->anim;
        } else {
            animTick(*a.model, a.anim, 0, 0);  // absent anim: keep the current clip, report 0x400
        }
    }
    uint32_t ev = 0x80000000;
    if (r->logicA) ev = runBlock(a, r->logicA);
    if ((ev & 0x90000000) && r->logicB) ev = runBlock(a, r->logicB);
    if (!(ev & 0x90000000)) {
        dispatch(a, ev);
        return;
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
void TbWorld::move(TbActor& a, const std::array<int32_t, 3>& target) {
    const TbStateRec& r = *a.rec();
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
    // Shape from the actor box +0x60.. (min x, max y, min z, max x, min y, max z), §1.5 step 1: tag-0x61 record
    // (action 82) when the entity has records, else TOD packet 0x900 (applied only with +0x123 = 0xff, 0x468280).
    if (a.boxIdx >= 0 && size_t(a.boxIdx) < a.boxes.size())  // 0x450460: +-20 per tick toward the record
        for (size_t i = 0; i < 6; ++i) {
            const int16_t t = a.boxes[size_t(a.boxIdx)][i];
            a.box[i] = a.boxInstant ? t : int16_t(a.box[i] < t ? std::min(a.box[i] + 20, int32_t(t)) : std::max(a.box[i] - 20, int32_t(t)));
        }
    else
        a.box = a.anim.box;
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
