// LiT player controller (0x43a880), MoveActor (0x437390 = 0x436ff0 + 0x4371f0 + 0x436de0), jump arc (0x43a250),
// camera follow (0x4318f0) and collectibles. Section numbers refer to docs/re/GAMEPLAY.md.
#include "bb/game/lit_world.h"

#include "bb/levelscript.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
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
constexpr int32_t kDt = 2;                // §1.1 DAT_004b2384
constexpr int32_t kDeadZone = 50;         // §2.2 / §3.2
constexpr int16_t kCamTable[8][2] = {{0, 400}, {400, 400}, {400, 0}, {400, -400},
                                     {0, -400}, {-400, -400}, {-400, 0}, {-400, 400}};  // 0x4adad8
constexpr int16_t kYawProbe[26] = {0, 341, -341, 682, -682, 1023, -1023, 1364, -1364, 1705, -1705, 2046, -2046,
                                   2387, -2387, 2728, -2728, 3069, -3069, 3410, -3410, 3751, -3751, 4092, 0, 400};  // 0x4adaa8
constexpr int16_t kApproxBoxTop = -400;   // ponytail: used until the TOD bbox (packet 9) has set +0x52

} // namespace

std::string LitWorld::levelName(int id) {
    return id >= 0 && size_t(id) < std::size(kLevels) ? kLevels[id] : std::string();
}

LitWorld LitWorld::load(const BzeFile& bze, const Scene* scene) {
    LitWorld w;
    w.scene_ = scene;
    const int i1 = bze.find(1), i4 = bze.find(4);
    if (i1 < 0 || i4 < 0) throw FormatError("LitWorld: needs sections 1 and 4");
    const std::vector<uint8_t> s1 = bze.payload(size_t(i1)), s4 = bze.payload(size_t(i4));
    const LevelScript ls = parseLevelScript(s1);
    std::map<uint32_t, const ActorInstance*> at;  // ScriptItem::offset -> scene actor (TOD model)
    if (scene)
        for (const ActorInstance& a : scene->actors) at[a.scriptOffset] = &a;
    bool player = false;
    std::span<const uint8_t> prog;  // chunk 0 tag 0x01: u32 n, n x 16 programs x 8 B {u16 1-based blob, ...} (0x41e030)
    for (const ScriptItem& it : ls.items) {
        if (it.kind != ScriptItem::Kind::Chunk) continue;
        if (it.code == 0x00) {  // sound table entries tag 0x04 {u16 id, u8 bank, u8 program, u16 flags, u8, u8} (0x41de40)
            if (const ScriptTag* t = tag(it, 0x01)) {
                const uint32_t off = le<uint32_t>(t->data, 0), size = le<uint32_t>(t->data, 4);
                if (size_t(off) + size <= s4.size()) prog = std::span(s4).subspan(off, size);
            }
            for (const ScriptTag& t : it.tags) {
                if (t.id != 0x04) continue;
                const uint16_t id = le<uint16_t>(t.data, 0), flags = le<uint16_t>(t.data, 4);
                // 0x41edb0: program entry (bank * 16 + program) of DAT_0054afc4; flag 0x400 = per-language speech table.
                const size_t e = 4 + (size_t(t.data[2]) * 16 + t.data[3]) * 8;
                const int32_t blob = !(flags & 0x400) && e + 2 <= prog.size() ? int32_t(prog[e] | prog[e + 1] << 8) - 1 : -1;
                if (id >= w.sounds_.size()) w.sounds_.resize(id + 1u, {-1, 0});
                w.sounds_[id] = {blob, flags};
            }
        } else if (it.code == 0x36) {
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
            if (type && type->data[0] == 1 && it.code != 0x08) {
                if (player) continue;
                player = true;  // §3.1: tables of the player entity (tag 0x13 == 1)
                w.spawn_ = v3(tag(it, 0x10));
                if (const ScriptTag* t = tag(it, 0x11)) w.spawnYaw_ = int16_t(wrapSigned(le<int16_t>(t->data, 2)));
                if (const ScriptTag* t = tag(it, 0x27)) w.p.entityId = le<uint16_t>(t->data, 0);
                if (const ScriptTag* t = tag(it, 0x1f)) w.p.zone = t->data[0];
                if (auto m = at.find(it.offset); m != at.end()) w.model_ = &scene->actorModels[m->second->model];
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
                continue;
            }
            ActorDef d;  // §7.1 tag map
            d.chunk = it.code;
            if (type) d.type = type->data[0];
            if (auto m = at.find(it.offset); m != at.end()) d.model = int32_t(m->second->model), d.idleAnim = m->second->anim;
            for (const ScriptTag& t : it.tags) {
                const auto& x = t.data;
                switch (t.id) {
                case 0x10: d.pos = v3(&t); break;
                case 0x11: for (size_t k = 0; k < 3; ++k) d.rot[k] = le<int16_t>(x, k * 2); break;
                case 0x12: for (size_t k = 0; k < 3; ++k) d.scale[k] = le<int32_t>(x, k * 4); break;
                case 0x15: d.cls = x[0]; break;
                case 0x16: d.flags8 = le<uint32_t>(x, 0), d.flagsC = le<uint32_t>(x, 4); break;
                case 0x1b: for (size_t k = 0; k < 3; ++k) d.box[k] = le<int16_t>(x, k * 2); break;
                case 0x1c: case 0x42: for (size_t k = 0; k < 3; ++k) d.box[3 + k] = le<int16_t>(x, k * 2); break;
                case 0x1d: d.param = le<int16_t>(x, 0); break;
                case 0x1e: d.reg = le<int16_t>(x, 0); break;
                case 0x1f: d.zone = x[0]; break;
                case 0x27: d.entityId = le<uint16_t>(x, 0); break;
                case 0x38: d.inflate = le<int16_t>(x, 0); break;
                case 0x3a: d.act3c = le<uint16_t>(x, 0), d.act3e = le<uint16_t>(x, 2); break;
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
    checkpoint_ = spawn;  // ponytail: initial checkpoint 0x4b2460 writer not found; the level start is used
    checkpointYaw_ = yaw;
    cam = Camera{};
    cam.eye = cam.eyeCopy = levelEye_;
    actors.clear();  // live list DAT_004b3740: chunks 0x07 / 0x0a in file order (§7.1)
    for (size_t i = 0; i < defs.size(); ++i)
        if (defs[i].chunk != 0x08) actors.push_back(instantiate(i));
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

// Sound start 0x41deb0 mode 1 (mode 2 releases a looping voice: no voices are simulated).
void LitWorld::playSound(uint32_t id) {
    if (id < sounds_.size() && sounds_[id][0] >= 0) sfx.push_back(uint16_t(sounds_[id][0]));
}

// ---- tick ----

void LitWorld::tick(const PadInput& in) {
    ++ticks;
    sfx.clear();
    // §2.2: all words zero while the player is scripted/dead (flags14 0x1000000) or a level change is pending.
    if ((p.flags14 & 0x1000000) || nextLevel >= 0) {
        pad.raw = pad.held = pad.prev = pad.pressed = 0;
        pad.sx = pad.sy = 0;
    } else {
        padTick(pad, in);
    }
    cameraTick();   // §1.1 step 3a: camera first, from the previous tick's player position
    actorsTick();   // §7.1: activation, then behaviours in active-list order (player among them)
    // HUD actor 0x44acc0 (§3.6): health below 1 -> death state (A92 override, else the zone-trigger default).
    if (int8_t(G[1]) < 1 && !(p.flags14 & 0x1000000))
        kill(int16_t(deathStateA92_ ? deathStateA92_ : defaultDeathState_));
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
            if (e.flags & 0x800) { p.pos = arg; p.platform = -1; p.flags14 &= ~0x14u; cameraSnap(arg); }  // 0x4379b0
            if (e.flags & 0x40000000) {  // teleport self
                m.pos = arg;
                if (!self && p.platform >= 0) { p.platform = -1; p.flags14 = (p.flags14 & ~0x14u) | 0x202; }
            }
            if (e.flags & 0x80000) nextLevel = e.arg[0];  // level exit: GD+0 = arg, DAT_004b39bc = 1
            if (e.flags & 0x200000) { kill(int16_t(e.arg[0])); G[1] = 0; }  // kill, death state arg0
            if (e.flags & 0x400000) defaultDeathState_ = e.arg[0];
            if (e.flags & 0x100000) { checkpoint_ = arg; checkpointYaw_ = p.yaw; }
            if (e.flags & 0x10000) break;
            if (e.flags & 0x200) playSound(uint32_t(e.arg[0]));
            if (e.flags & 0x40) cameraSnap(arg);
            for (uint32_t fl : {0x800000u, 0x2000u})  // respawn point 0x4b2180 (no reader found), 0x437510
                if (e.flags & fl) unsupported.insert(std::format("trigger flag 0x{:x}", fl));
            if (!(e.flags & 0x8000)) return;
        }
        return;
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
    if ((p.flags14 & 1) && (!(p.flags14 & 0x200) || (p.flags14 & 0x400)) && !frozen) arcTick(p);
    p.flags14 = (mv->flagsA & 0x810000) ? p.flags14 | 0x40 : p.flags14 & ~0x40u;  // attack active
    playerAttacks(*mv);

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
    if ((p.flags14 & 0x81000000) == 0) moveActor(p, w, r, boxTop());
    if (sum0 != w.z + w.x) p.flags14 |= 0x100000;  // blocked by a wall this tick
    playerContacts(w);  // 0x433500 mask 0x9a690a: solids, platforms, pickups (approximation)
    p.pos.x += w.x;
    p.pos.y += w.y;
    p.pos.z += w.z;
    if (!(prevFlags & 0x200) && (prevFlags & 0x12) && (p.flags14 & 0x200)) p.flags18 |= 0x200;  // walked off a ledge

    // 6. Transitions (§3.1 step 3): first passing rule of group move.link.
    if (mv->link != 0) {
        int16_t next = kStay;
        for (const PlayerRule& ru : rules) {
            if (ru.group != mv->link) continue;
            if (ru.heldMask) continue;  // needs a held actor (carrying is not simulated)
            if (ru.parentMask && !(p.platform >= 0 && (actors[size_t(p.platform)].flags8 & ru.parentMask)) &&
                !((ru.parentMask & 0x8000) && (p.flags18 & 0x40)))
                continue;
            if (ru.flags14Mask && (((p.flags14 & ru.flags14Mask) != 0) == bool(ru.invert & 2))) continue;
            if (ru.flags18Mask && (((p.flags18 & ru.flags18Mask) != 0) == bool(ru.invert & 1))) continue;
            if (ru.any && !(pad.pressed & ru.any)) continue;
            if (ru.all != (pad.pressed & ru.all) && ru.all != kNoButtons) continue;
            if (!cond(ru.item, nullptr)) continue;
            act(ru.item, nullptr);
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
            moveActor(p, z, g, boxTop());
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
}

void LitWorld::cameraTick() {
    if (const Zone* z = hf.findZone(p.pos, p.zone)) p.zone = int16_t(z->id);
    if (!p.initialised) return;
    if (camHold_ > 0) {  // A41 hold 0x4b3dac: camera frozen, Bugs frozen (flags14 0x80000000, cleared each tick)
        --camHold_;
        p.flags14 |= 0x80000000;
        return;
    }
    bool moved = p.lastMove != 0;  // param_2 = P+0x1b4
    if (cam.lastPlayer != p.pos) moved = true;
    cam.lastPlayer = p.pos;
    if ((cam.flags & 0x40) && !(cam.flags & 0x300)) {
        if (cam.recentreRequest < 10) { ++cam.recentreRequest; moved = true; }
        else cam.flags &= ~0x40u;
    }
    const int32_t dist = (p.flags14 & 0x1000000) ? 500 : cam.distance;
    if (cam.flags & 0xc00) {  // scripted points (type-36 directors, 0x4318f0 0xc00 branch): no follow this tick
        cam.flags &= ~0x10u;
        const V3 P = p.pos;
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
