// LiT player controller (0x43a880), MoveActor (0x437390 = 0x436ff0 + 0x4371f0 + 0x436de0), jump arc (0x43a250),
// camera follow (0x4318f0) and collectibles. Section numbers refer to docs/re/GAMEPLAY.md.
#include "bb/game/lit_world.h"

#include "bb/levelscript.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>

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

// Pickup items (tag 0x31, LOGIC.md §2): what the type-14 interpreter would award (§7.4, tools/re/entities.py).
struct Item31 { LogicItem it; uint32_t flags = 0; int32_t p1 = 0; uint32_t p2 = 0; };
std::vector<Item31> items31(const ScriptItem& e) {
    std::vector<Item31> v;
    for (const ScriptTag& t : e.tags)
        if (t.id == 0x31 && t.data.size() >= 32)
            v.push_back({item(t.data, 12), le<uint32_t>(t.data, 20), le<int32_t>(t.data, 24), le<uint32_t>(t.data, 28)});
    return v;
}
bool awards(const LogicItem& it, int idx) {  // A4 incg / A37 addgi / A23 incgg
    if (it.act == 4 || it.act == 37) return it.y == idx;
    return it.act == 23 && (it.x == idx || it.y == idx);
}

constexpr int16_t kStay = -13;            // §3.1 nextState "stay"
constexpr uint16_t kNoButtons = 0xfff1;   // §3.1 authoring value "no button"
constexpr int32_t kDt = 2;                // §1.1 DAT_004b2384
constexpr int32_t kDeadZone = 50;         // §2.2 / §3.2
constexpr int16_t kCamTable[8][2] = {{0, 400}, {400, 400}, {400, 0}, {400, -400},
                                     {0, -400}, {-400, -400}, {-400, 0}, {-400, 400}};  // 0x4adad8
constexpr int16_t kYawProbe[26] = {0, 341, -341, 682, -682, 1023, -1023, 1364, -1364, 1705, -1705, 2046, -2046,
                                   2387, -2387, 2728, -2728, 3069, -3069, 3410, -3410, 3751, -3751, 4092, 0, 400};  // 0x4adaa8
constexpr int16_t kApproxBoxTop = -400;   // ponytail: used until the TOD bbox (packet 9) has set +0x52

} // namespace

LitWorld LitWorld::load(const BzeFile& bze, const ActorModel* playerModel) {
    LitWorld w;
    w.model_ = playerModel;
    const int i1 = bze.find(1), i4 = bze.find(4);
    if (i1 < 0 || i4 < 0) throw FormatError("LitWorld: needs sections 1 and 4");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1)), s4 = bze.payload(size_t(i4));
    const LevelScript ls = parseLevelScript(s1);
    bool player = false;
    std::map<uint16_t, const ScriptItem*> templates;
    std::vector<const ScriptItem*> type14;
    for (const ScriptItem& it : ls.items) {
        if (it.kind != ScriptItem::Kind::Chunk) continue;
        if (it.code == 0x36) {
            if (const ScriptTag* t = tag(it, 0x37)) {
                const uint32_t off = le<uint32_t>(t->data, 0), size = le<uint32_t>(t->data, 4);
                if (size_t(off) + size > s4.size()) throw FormatError("LitWorld: chunk 0x36 block outside section 4");
                w.hf = Heightfield::parse(std::span(s4).subspan(off, size));
            }
        } else if (it.code == 0x3b) {
            if (const ScriptTag* t = tag(it, 0x2b)) w.levelEye_ = v3(t);  // §5 initial eye
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
            const ScriptTag* id = tag(it, 0x27);
            if (it.code == 0x08 && id) templates[le<uint16_t>(id->data, 0)] = &it;
            if (!type) continue;
            if (type->data[0] == 14 && it.code != 0x08) type14.push_back(&it);
            if (type->data[0] != 1 || player) continue;
            player = true;  // §3.1: tables of the player entity (tag 0x13 == 1)
            w.spawn_ = v3(tag(it, 0x10));
            if (const ScriptTag* t = tag(it, 0x11)) w.spawnYaw_ = int16_t(wrapSigned(le<int16_t>(t->data, 2)));
            if (id) w.entityId_ = le<uint16_t>(id->data, 0);
            if (const ScriptTag* t = tag(it, 0x1f)) w.p.zone = t->data[0];
            for (const ScriptTag& t : it.tags) {
                const auto& d = t.data;
                if (t.id == 0x32) {
                    PlayerState s{le<uint16_t>(d, 0), {}};
                    for (size_t k = 0; k < 5; ++k) s.moves[k] = le<int16_t>(d, 2 + k * 2);
                    w.states.push_back(s);
                } else if (t.id == 0x34) {
                    w.moves.push_back({le<uint16_t>(d, 0), le<uint16_t>(d, 2), le<int16_t>(d, 4), le<uint32_t>(d, 8),
                                       le<uint32_t>(d, 12), le<uint16_t>(d, 16), le<int16_t>(d, 18), le<int16_t>(d, 20),
                                       le<int16_t>(d, 22)});
                } else if (t.id == 0x35) {
                    w.rules.push_back({le<uint16_t>(d, 0), le<uint16_t>(d, 2), le<uint16_t>(d, 4), le<uint16_t>(d, 6),
                                       le<int16_t>(d, 8), item(d, 10), le<uint16_t>(d, 20), le<uint32_t>(d, 24),
                                       le<uint32_t>(d, 28), le<uint32_t>(d, 32), le<uint32_t>(d, 36)});
                }
            }
        }
    }
    if (!player) throw FormatError("LitWorld: no player entity (tag 0x13 == 1)");
    if (w.hf.empty()) throw FormatError("LitWorld: no chunk 0x36 collision heightfield");
    if (!w.findState(1) || !w.findMove(uint16_t(w.findState(1)->moves[0])))
        throw FormatError("LitWorld: player tables lack state 1");

    // Collectibles (§7.4): classify like tools/re/entities.py.
    for (const ScriptItem* e : type14) {
        const std::vector<Item31> its = items31(*e);
        Pickup pk;
        bool known = true;
        if (std::ranges::any_of(its, [](const Item31& i) { return awards(i.it, 7); })) pk.kind = PickupKind::GoldenCarrot;
        else if (std::ranges::any_of(its, [](const Item31& i) { return awards(i.it, 5); })) pk.kind = PickupKind::Clock;
        else {
            known = std::ranges::any_of(its, [&](const Item31& i) {
                if (!(i.flags & 0x40100) || (i.flags & 0x20000000)) return false;
                const auto t = templates.find(uint16_t(i.p2));
                return t != templates.end() && t->second != e &&
                       std::ranges::any_of(items31(*t->second), [](const Item31& j) { return j.it.act == 48; });
            });
        }
        if (!known) continue;
        for (const Item31& i : its) {
            if (i.it.act == 12) pk.bits.emplace_back(i.it.x, i.it.y);
            const int idx = pk.kind == PickupKind::GoldenCarrot ? 7 : 5;
            if (pk.kind != PickupKind::Carrot && i.it.act == 23 && (i.it.x == idx || i.it.y == idx))
                pk.counter = i.it.x == idx ? i.it.y : i.it.x;
        }
        std::ranges::sort(pk.bits);
        pk.bits.erase(std::unique(pk.bits.begin(), pk.bits.end()), pk.bits.end());
        pk.scriptOffset = e->offset;
        pk.pos = v3(tag(*e, 0x10));
        if (const ScriptTag* t = tag(*e, 0x11)) pk.rotY = le<int16_t>(t->data, 2);
        if (const ScriptTag* t = tag(*e, 0x1b)) for (size_t k = 0; k < 3; ++k) pk.box[k] = le<int16_t>(t->data, k * 2);
        if (const ScriptTag* t = tag(*e, 0x1c)) for (size_t k = 0; k < 3; ++k) pk.box[3 + k] = le<int16_t>(t->data, k * 2);
        if (const ScriptTag* t = tag(*e, 0x38)) pk.inflate = le<int16_t>(t->data, 0);
        w.pickups.push_back(std::move(pk));
    }

    w.start(w.spawn_, w.spawnYaw_, w.p.zone);
    return w;
}

void LitWorld::start(V3 spawn, int16_t yaw, int16_t zone) {
    // New-game defaults (§8.1): health G[1] = max G[2] = 6, lives G[0] = 3.
    G[0] = 3;
    G[1] = G[2] = 6;
    spawn_ = spawn;
    spawnYaw_ = yaw;
    p = Player{};
    p.pos = spawn;
    p.yaw = yaw;
    p.zone = zone;
    checkpoint_ = spawn;  // ponytail: initial checkpoint 0x4b2460 writer not found; the level start is used
    checkpointYaw_ = yaw;
    cam = Camera{};
    cam.eye = cam.eyeCopy = levelEye_;
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
int16_t LitWorld::boxTop() const { return p.animState.box[4] ? p.animState.box[4] : kApproxBoxTop; }

// ---- logic VM subset (LOGIC.md §4/§5) used by player rules and zone triggers ----

bool LitWorld::cond(const LogicItem& it) {
    const uint16_t ab = uint16_t(it.a | it.b << 8);
    switch (it.cond) {
    case 0: return true;                                  // C0 true
    case 1: return int8_t(S[it.b]) == int8_t(it.a);       // C1 ciseq
    case 2: return G[it.b] == it.a;                       // C2 cigeq
    case 4: return it.a < G[it.b];                        // C4 cigltu
    case 11: return (p.flags14 & 0x2000020) == 0;         // C11
    case 12: return p.state == it.a;                      // C12
    case 14: return p.zone == it.a;                       // C14
    case 21: return p.timer < ab;                         // C21
    case 22: return p.timer > ab;                         // C22
    case 23: return p.timer == ab;                        // C23
    case 30: return (S[it.b] & it.a) != 0;                // C30 tismnz
    case 31: return (G[it.b] & it.a) != 0;                // C31 tigmnz
    case 34: case 39: return (S[it.b] & it.a) == 0;       // C34/C39 tismz
    case 35: case 40: return (G[it.b] & it.a) == 0;       // C35/C40 tigmz
    case 37: return S[it.b] != it.a;                      // C37 cisne
    case 38: return G[it.b] != it.a;                      // C38 cigne
    case 44: return p.zone != it.a;                       // C44
    case 46: return S[it.b] == it.a;                      // C46
    case 47: return G[it.b] == it.a;                      // C47
    case 90: return (pad.pressed & ((it.a & 1 ? PadTriangle : 0) | (it.a & 2 ? PadCircle : 0) |   // C90 (C57 bytes)
                                    (it.a & 4 ? PadCross : 0) | (it.a & 8 ? PadSquare : 0) | (it.a & 0x10 ? PadUp : 0) |
                                    (it.a & 0x20 ? PadRight : 0) | (it.a & 0x40 ? PadDown : 0) |
                                    (it.a & 0x80 ? PadLeft : 0))) != 0;
    default:
        unsupported.insert(std::format("C{}", it.cond));
        return false;  // out-of-table conditions also return false in the exe
    }
}

void LitWorld::act(const LogicItem& it) {
    switch (it.act) {
    case 0: return;
    case 3: ++S[it.y & 0xff]; return;                   // A3 incs
    case 11: S[it.y & 0xff] |= uint8_t(it.x); return;   // A11 orsi
    case 12: G[it.y & 0xff] |= uint8_t(it.x); return;   // A12 orgi
    case 13: S[it.y & 0xff] &= uint8_t(it.x); return;   // A13 andsi
    case 14: G[it.y & 0xff] &= uint8_t(it.x); return;   // A14 andgi
    case 18: S[it.y & 0xff] = uint8_t(it.x); return;    // A18 movsi
    case 19: G[it.y & 0xff] = uint8_t(it.x); return;    // A19 movgi
    case 25: p.timer = 0; return;                       // A25 reset self timer
    case 28: S[it.y & 0xff] = G[it.x & 0xff]; return;   // A28 movsg
    case 35: G[1] = 0; kill(int16_t(deathStateA92_ ? deathStateA92_ : defaultDeathState_)); return;  // A35 kill player
    case 72:                                            // A72 damage player X (§3.6)
        if (invuln_ == 0) {
            G[1] = uint8_t(G[1] - it.x);
            p.flags14 |= 0x2000000;
            invuln_ = 0x1e;
        }
        return;
    case 74: cam.flags |= 0x2000; return;               // A74 look disabled
    case 92: deathStateA92_ = it.x; return;             // A92 death-state override
    case 94: cam.flags |= 1; return;                    // A94
    case 36: S[it.y & 0xff] = uint8_t(S[it.y & 0xff] + it.x); return;  // A36 addsi
    case 39: S[it.y & 0xff] = uint8_t(S[it.y & 0xff] - it.x); return;  // A39 subsi
    case 58: return;                                    // A58 rumble (PadSetAct is a no-op on PC)
    default: unsupported.insert(std::format("A{}", it.act));
    }
}

// ---- tick ----

void LitWorld::tick(const PadInput& in) {
    ++ticks;
    // §2.2: all words zero while the player is scripted/dead (flags14 0x1000000).
    if (p.flags14 & 0x1000000) {
        pad.raw = pad.held = pad.prev = pad.pressed = 0;
        pad.sx = pad.sy = 0;
    } else {
        padTick(pad, in);
    }
    cameraTick();   // §1.1 step 3a: camera first, from the previous tick's player position
    playerTick();   // §7.1: the player runs first among behaviours
    // HUD actor 0x44acc0 (§3.6): health below 1 -> death state (A92 override, else the zone-trigger default).
    if (int8_t(G[1]) < 1 && !(p.flags14 & 0x1000000))
        kill(int16_t(deathStateA92_ ? deathStateA92_ : defaultDeathState_));
    ++p.timer;      // §7.1: +0x22 increments after the behaviour
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
    accelK_ = 0x800;
    safe_ = p.pos;
    turnErr_ = -1;
    cameraReset();
    p.flags18 = 0;
}

// 0x43a250: jump arc phase step (§3.3).
void LitWorld::arcTick() {
    const int16_t ph = p.phase;
    p.flags14 = ph < 0x800 ? p.flags14 & ~8u : p.flags14 | 8;
    if (ph < 0xc01 && (!(p.flags14 & 4) || (p.flags14 & 0x10))) {
        const int32_t s = rsin(ph) * p.jumpH;
        p.vy = (p.flags14 & 8) ? int16_t(uint16_t((-s) >> 11) & 0xfffe) : int16_t((-s) >> 12);  // down twice as fast
        p.phase = int16_t(p.phase + p.jumpStep);
        return;
    }
    p.vy = 0;
    const uint32_t f = p.flags14;
    p.flags14 = f & ~9u;
    if (!(f & 2)) p.flags14 |= 0x200;  // arc ended airborne -> free fall
}

// MoveActor 0x437390 (§4.3): horizontal trace with diagonal wall-slide retry (0x436ff0), vertical target
// (0x4371f0), fall/landing/smoothing (0x436de0). delta is in/out (world units this tick).
void LitWorld::moveActor(V3& d, Ground& r) {
    r.zone = p.zone;
    // --- 0x436ff0 ---
    const V3 save = p.pos;
    if (p.vy < 0) p.pos.y += p.vy;  // rising: trace from the raised height
    const int32_t yTrace = p.pos.y;
    const int16_t origDx = int16_t(d.x), origDz = int16_t(d.z);
    int32_t dx = d.x, dz = d.z;
    hf.trace(p.pos, dx, dz, r);
    int32_t accX = dx, accZ = dz;
    p.pos.x += dx;
    p.pos.z += dz;
    if (r.kind & KindDiagonal) {  // wall slide: retry the remaining X, then the remaining Z
        int32_t rx = int32_t(origDx) - dx, rz = 0;
        const int16_t remZ = int16_t(origDz - int16_t(dz));
        p.pos.y = yTrace;
        hf.trace(p.pos, rx, rz, r);
        if (rx != 0) { accX += rx; p.pos.x += rx; }
        rx = 0;
        rz = remZ;
        p.pos.y = yTrace;
        hf.trace(p.pos, rx, rz, r);
        if (rz != 0) { accZ += rz; p.pos.z += rz; }
    } else if (r.kind & KindBlocked) {
        p.flags14 |= 0x100000;
    }
    if (r.kind & KindOpen) {  // open sub-cell: floor of the zone below (§4.2)
        p.pos.y += 1;
        const int16_t keep = r.zone;
        hf.groundAt(p.pos, r);
        r.zone = keep;
    }
    p.groundBase = p.pos.y;
    p.hoff = r.hoff;
    p.pos = save;
    d.x = accX;
    d.z = accZ;
    // --- 0x437390 ---
    if (r.kind & KindBlocked) p.flags14 |= 0x100000;
    if (r.zone != -1) p.zone = r.zone;
    // --- 0x4371f0: vertical target ---
    const int32_t ground = p.hoff + p.groundBase;
    const uint32_t f = p.flags14;
    if (!(f & 1)) {
        if (!(f & 4)) d.y = (r.kind & KindNoZone) ? 200 : ground - p.pos.y;  // no zone: dy = 200 (§4.3)
        else if (ground < p.pos.y + d.y) d.y = ground - p.pos.y;
    } else {
        int32_t target = p.vy + p.pos.y;
        bool land = false;
        if (p.phase < 0x801) {
            if (target + boxTop() < r.ceiling) {  // head above the zone ceiling
                if (target <= ground) {
                    V3 q{p.pos.x + d.x, r.ceiling - 1, p.pos.z + d.z};
                    Ground above;
                    above.zone = -1;
                    hf.groundAt(q, above);
                    if (!(above.kind & KindOpen)) target = std::min(r.ceiling - int32_t(boxTop()), ground);
                    else if (!(target < ground)) land = true;
                } else {
                    land = true;
                }
            } else if (!(target < ground)) {
                land = true;
            }
        } else if (r.kind == KindOpen || target <= ground) {
            if (r.kind == KindOpen || r.kind == KindNoZone) p.groundBase += 1;
            if (!(target < ground)) land = true;
        } else {
            land = true;
        }
        if (land) {
            p.flags14 = f & ~9u;
            target = ground;
        }
        d.y = target - p.pos.y;
    }
    // --- 0x436de0: smoothing, fall, landing (§3.3) ---
    const int32_t dy0 = d.y;
    const int32_t to = p.pos.y + dy0;
    uint32_t keep = 0, cleared = 0;
    bool done = false;
    if (to < p.pos.y + 200 || (p.flags14 & 1)) {
        const uint32_t g = p.flags14;
        if (!(g & 1)) {
            if (!(g & 2)) {
                if (!(g & 4)) d.y = std::clamp(d.y, -70, 70);  // airborne snap clamp ±70
            } else if (d.y < 1) {
                if (-0x46 < d.y && (d.y & ~1) != 0) d.y >>= 1;  // grounded step smoothing
            } else if ((d.y & ~1) != 0) {
                d.y >>= 1;
            }
        }
        if (d.y + p.pos.y == p.hoff + p.groundBase && r.kind != KindOpen && r.kind != KindNoZone) {
            p.phase = 0x800;  // landing
            p.flags14 = (p.flags14 & 0xfffefdff) | 2;
            done = true;
        } else if (p.flags14 & 2) {
            done = true;
        } else {
            cleared = p.flags14 & ~2u;
            keep = p.flags14 & 5;
            p.flags14 = cleared;
        }
    } else {  // more than 200 below: free fall vy = 70 + 70·cos (§3.3)
        if (p.phase > 0xbff) p.phase = 0xc00;
        const int32_t s = rsin(p.phase + 0x400);
        p.phase = int16_t(p.phase + 0x44);
        const int16_t vy = int16_t(0x46 - int16_t((s * -0x46) >> 12));
        const uint32_t g = p.flags14;
        const int32_t next = (g & 0x10000) ? p.pos.y + 12 : vy + p.pos.y;  // glide: 12
        if (to <= next) {
            if (r.kind == KindOpen || r.kind == KindNoZone) {
                p.flags14 = g & ~2u;
                if (!(g & 4)) p.flags14 = (g & ~2u) | 0x200;
            } else {
                p.phase = 0x800;
                p.flags14 = (g & 0xfffefdff) | 2;
            }
            done = true;
        } else {
            d.y = (g & 0x10000) ? 12 : vy;
            cleared = p.flags14 & ~2u;
            keep = p.flags14 & 4;
            p.flags14 = cleared;
        }
    }
    if (!done && keep == 0) p.flags14 = cleared | 0x200;
    if (p.flags14 & 4) d.y = dy0;
}

void LitWorld::kill(int16_t deathState) {  // 0x437b90(state, NULL)
    invuln_ = 0;
    p.seq = 0;
    p.state = uint16_t(deathState);
    p.anim = 0;  // ponytail: restart the death clip (0x437b90 leaves +0x158; same-anim case not modelled)
    p.flags14 = (p.flags14 | 0x1000004) & 0xfffeffff;
    turnErr_ = -1;
}

void LitWorld::respawn() {  // end of the death clip (0x43a880 tail): checkpoint, health 6, 30 ticks invulnerable
    p.flags14 = 0;
    p.pos = checkpoint_;
    cam.yaw = int16_t(checkpointYaw_);
    p.yaw = cam.yaw;
    cameraSnap(checkpoint_);
    invuln_ = 0x1e;
    G[1] = 6;
}

// 0x4384a0 + 0x434b00: the first trigger volume containing the player runs its entries (§7.5). Only the
// flags listed in the switch are interpreted; logic items go through the VM subset above.
void LitWorld::zoneTriggers() {
    for (const Trigger& t : triggers) {
        if (!(t.flags & 2) && t.zone != p.zone) continue;
        const int16_t lx0 = int16_t(int16_t(p.pos.x) - int16_t(t.pos.x)), ly = int16_t(int16_t(p.pos.y) - int16_t(t.pos.y)),
                      lz0 = int16_t(int16_t(p.pos.z) - int16_t(t.pos.z));
        if (!(boxTop() + ly < 1 && t.minY <= ly)) continue;
        int32_t lx = lx0, lz = lz0;
        if (!(t.flags & 1)) {  // local frame: RotMatrixZYX(0x1000 - rot); only the yaw is modelled
            const V3 l = rotY(0x1000 - t.rotY, lx0, 0, lz0);
            lx = int16_t(l.x);
            lz = int16_t(l.z);
        }
        if (lx < 0 || lx > t.sizeX || lz < 0 || lz > t.sizeZ) continue;
        for (const TriggerEntry& e : t.entries) {
            if (e.actor != entityId_) continue;
            if (e.mask14 && !(p.flags14 & e.mask14)) continue;
            if (e.item.cond && !cond(e.item)) continue;
            if (e.item.act) act(e.item);
            if (e.flags & 0x40000000) p.pos = {e.arg[0], e.arg[1], e.arg[2]};  // teleport
            if (e.flags & 0x200000) { kill(int16_t(e.arg[0])); G[1] = 0; }  // kill, death state arg0
            if (e.flags & 0x400000) defaultDeathState_ = e.arg[0];
            if (e.flags & 0x100000) { checkpoint_ = {e.arg[0], e.arg[1], e.arg[2]}; checkpointYaw_ = p.yaw; }
            if (e.flags & 0x10000) break;
            if (e.flags & 0x40) cameraSnap({e.arg[0], e.arg[1], e.arg[2]});
            for (uint32_t fl : {0x80000000u, 0x800u, 0x800000u, 0x80000u, 0x200u, 0x2000u})
                if (e.flags & fl) unsupported.insert(std::format("trigger flag 0x{:x}", fl));
            if (!(e.flags & 0x8000)) return;
        }
        return;
    }
}

// Collectible touch (§7.4). Implemented (approximation): the exe marks pickups through the player's box push
// 0x433500 and the type-14 item interpreter applies the rewards; here the player's centre (radius 80) against
// the pickup box inflated by tag 0x38, reward applied on the touching tick.
void LitWorld::pickupTouch() {
    for (Pickup& k : pickups) {
        if (k.taken) continue;
        const V3 l = rotY(0x1000 - k.rotY, int16_t(p.pos.x - k.pos.x), 0, int16_t(p.pos.z - k.pos.z));
        const int32_t ly = p.pos.y - k.pos.y, r = 80 + k.inflate;
        if (l.x < k.box[0] - r || l.x > k.box[3] + r || l.z < k.box[2] - r || l.z > k.box[5] + r) continue;
        if (ly + boxTop() > k.box[1] + k.inflate || ly < k.box[4] - k.inflate) continue;
        k.taken = true;
        for (auto [x, y] : k.bits) G[y & 0xff] |= uint8_t(x);  // A12 orgi collected flag
        auto inc = [&](int idx) {
            if (idx == 7) { const int gc = goldenCarrots() + 1; G[7] = uint8_t(gc); G[0xfc] = uint8_t(gc >> 8); }
            else if (idx >= 0) G[size_t(idx)] = uint8_t(G[size_t(idx)] + 1);
        };
        switch (k.kind) {
        case PickupKind::Carrot:  // template 37 -> A48 gvcrt: health +1 if hurt, else carrots +1 (HUD cap 99)
            if (G[1] < G[2]) ++G[1];
            else G[3] = uint8_t(std::min(99, G[3] + 1));
            break;
        case PickupKind::GoldenCarrot: inc(7); inc(k.counter); break;  // A23 incgg
        case PickupKind::Clock: inc(5); inc(k.counter); break;         // A23 incgg (state-95 celebration not played)
        }
    }
}

// ---- player 0x43a880 ----

void LitWorld::playerTick() {
    const uint32_t prevFlags = p.flags14;  // DAT_004b3c24
    p.flags14 &= 0xfff9bfff;
    if ((cam.flags & 0x100) || (prevFlags & 0x80000000)) {
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
    }
    const PlayerMove* mv = findMove(uint16_t(moveId));
    p.flags18 = (mv->flagsB & 0x1000) ? p.flags18 | 2 : p.flags18 & ~2u;
    p.flags18 = (mv->flagsB & 0x2000) ? p.flags18 | 4 : p.flags18 & ~4u;

    // 2. Steering (§3.2). Local velocity s = (sx, sy, sz-forward).
    int16_t lx = 0, ly = 0, lz = 0;
    const bool frozen = p.flags14 & 0x80000000;
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
            }
            if (mv->flagsA & 0x100000) lz = int16_t(lz + mv->speed * kDt);
        }
        if (turnErr_ != -1) {  // turn Δ>>2 per tick, final snap
            int16_t s = turnErr_;
            const int16_t q = int16_t(turnErr_ >> 2);
            if (q == 0) turnErr_ = -1;
            else { turnErr_ = int16_t(turnErr_ - q); s = q; }
            p.yaw = int16_t(p.yaw + s);
        }
    }
    if (mv->flagsB & 0x10) p.yaw = int16_t(ratan2(cam.eye.x - p.pos.x, cam.eye.z - p.pos.z));  // face camera
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
    }
    if ((p.flags14 & 1) && (!(p.flags14 & 0x200) || (p.flags14 & 0x400)) && !frozen) arcTick();
    p.flags14 = (mv->flagsA & 0x810000) ? p.flags14 | 0x40 : p.flags14 & ~0x40u;  // attack active

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
    if ((p.flags14 & 0x81000000) == 0) moveActor(w, r);
    if (sum0 != w.z + w.x) p.flags14 |= 0x100000;  // blocked by a wall this tick
    // (0x433500 solid-actor push: no solid actors are simulated.)
    pickupTouch();
    p.pos.x += w.x;
    p.pos.y += w.y;
    p.pos.z += w.z;
    if (!(prevFlags & 0x200) && (prevFlags & 0x12) && (p.flags14 & 0x200)) p.flags18 |= 0x200;  // walked off a ledge

    // 6. Transitions (§3.1 step 3): first passing rule of group move.link.
    if (mv->link != 0) {
        int16_t next = kStay;
        for (const PlayerRule& ru : rules) {
            if (ru.group != mv->link) continue;
            if (ru.heldMask) continue;  // needs a held actor (none simulated)
            if (ru.parentMask && !((ru.parentMask & 0x8000) && (p.flags18 & 0x40))) continue;  // no support actor
            if (ru.flags14Mask && (((p.flags14 & ru.flags14Mask) != 0) == bool(ru.invert & 2))) continue;
            if (ru.flags18Mask && (((p.flags18 & ru.flags18Mask) != 0) == bool(ru.invert & 1))) continue;
            if (ru.any && !(pad.pressed & ru.any)) continue;
            if (ru.all != (pad.pressed & ru.all) && ru.all != kNoButtons) continue;
            if (!cond(ru.item)) continue;
            act(ru.item);
            next = ru.next;
            break;
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
            if (mv->flagsB & 0x800) p.flags14 &= 0xfffffdf6;
        }
    }
    p.flags14 = (mv->flagsA & 0x2000) ? (p.flags14 & 0xfffffdf6) | 0x10000 : p.flags14 & ~0x10000u;  // glide

    // 7. Animation (§3.1 step 4, §6).
    if (p.anim == mv->anim) {
        if (!(mv->mode & 0x8000) || lz != 0) {
            const uint32_t res = model_ ? animTick(*model_, p.animState, 0, 0) : AnimMissing;
            if (res == AnimFinished) {
                if (p.state == 1) p.seq = uint16_t(rand_() & 3);  // random idle fidget
                ++p.seq;
                p.anim = 0;
                if (p.flags14 & 0x1000000) respawn();
            } else if ((mv->flagsA & 0x80) && (p.flags14 & 0x12)) {  // advance on landing
                ++p.seq;
                p.anim = 0;
            }
        }
    } else {
        if (model_) animTick(*model_, p.animState, mv->anim, uint32_t(uint16_t(mv->mode)));
        p.anim = mv->anim;
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
            moveActor(z, g);
            safe_.y = p.pos.y + z.y;
            p.pos.y = safe_.y;
        }
    }
    p.flags14 &= 0x7fedefff;
    p.flags18 &= 0xfffffdbe;
    if (p.flags14 & 0x16) p.flags14 &= ~0x200u;
    if (invuln_ != 0) {
        if (invuln_ < 0x1e) p.flags14 &= 0xfdffffdf;
        --invuln_;
    }
    if (!(p.flags14 & 0x1000000)) zoneTriggers();
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
}

void LitWorld::cameraTick() {
    if (const Zone* z = hf.findZone(p.pos, p.zone)) p.zone = int16_t(z->id);
    if (!p.initialised) return;
    bool moved = p.lastMove != 0;  // param_2 = P+0x1b4
    if (cam.lastPlayer != p.pos) moved = true;
    cam.lastPlayer = p.pos;
    if ((cam.flags & 0x40) && !(cam.flags & 0x300)) {
        if (cam.recentreRequest < 10) { ++cam.recentreRequest; moved = true; }
        else cam.flags &= ~0x40u;
    }
    const int32_t dist = (p.flags14 & 0x1000000) ? 500 : cam.distance;
    // Manual rotation (§5 step 4): L2 +0x26, R2 -0x26 per tick; holds the yaw (flags |= 0x82).
    const uint32_t f = cam.flags & ~0x10u;
    cam.flags = f & 0xffffa3e7;
    cam.lastStep = 0;
    if (!(pad.pressed & PadL1) && !(p.flags14 & 0x1000000)) {
        const bool l2 = pad.pressed & PadL2, r2 = pad.pressed & PadR2;
        if (l2 && !r2 && !(f & 0x8000)) { cam.flags |= 0x82; cam.yaw = int16_t(cam.yaw + 0x26); cam.lastStep = 0x26; }
        else if (r2 && !l2 && !(f & 0x8000)) { cam.flags |= 0x82; cam.yaw = int16_t(cam.yaw - 0x26); cam.lastStep = -0x26; }
        else if (moved) cam.flags = f & 0xffffa3e5;
    }
    cam.yaw = int16_t(wrapSigned(cam.yaw));
    cam.cand = cam.candCopy;
    cam.eye = cam.eyeCopy;
    cam.target = cam.targetCopy;

    const V3 P = p.pos;
    auto rel = [&](const V3& c, int32_t& r, int16_t& a, bool fallback) {
        const int32_t dx = int16_t(int16_t(c.x) - int16_t(P.x)), dz = int16_t(int16_t(c.z) - int16_t(P.z));
        const int32_t r2 = dz * dz + dx * dx;
        if (fallback && r2 == 0) { r = 100; a = int16_t(p.yaw - 0x800); }
        else { r = isqrt(r2) + 1; a = int16_t(ratan2(dx, dz)); }
    };
    int32_t r;
    int16_t a;
    rel(cam.cand, r, a, true);
    if (moved) cam.recentreTimer = 0x3c;
    if (cam.recentreTimer > 0) --cam.recentreTimer;
    // Auto-recentre (§5 step 6): toward player.yaw + 0x800 by diff·30 >> 11.
    if (!(cam.flags & 2) && (cam.recentreTimer == 0 || moved)) {
        const int32_t step = angleDiff(p.yaw + 0x800, a) * 0x1e >> 0xb;
        if (step == 0) cam.flags |= 2;
        else a = int16_t(a + step);
    }
    if (!(cam.flags & 2)) cam.yaw = a;
    else a = cam.yaw;

    V3 off = rotY(a, 0, 0, r);
    int32_t probeY = P.y - 500;
    if (!(p.flags14 & 0x16)) {  // airborne: probe from just under the ceiling of the zone above
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

} // namespace bb::game
