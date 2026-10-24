// TB entity systems around the class-14 interpreter (GAMEPLAY_TB.md §3.3–§3.5): the view list 0x45eee0 that decides
// which entities tick, the class dispatch 0x4cea60, and the combat parts of 0x453de0 — actor contacts, attack probes
// 0x447fd0, pending-hit intake, death and the player-down rule 0x44ea50.
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace bb::game {
namespace {

int64_t dist2(const V3& a, const V3& b) {
    const int64_t x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

// Camera basis of eye -> look (PS1: x right, y down, z forward) for the view list's GTE matrix (0x406280: view y ×
// 0x639a94 / 4096 = 2560) and the render's float one; a wider render FOV (viewTanX/Y) squeezes view x / y so the
// exe's 0.64 / 0.48 view spans it.
struct ViewBasis {
    V3 eye;
    double r[3], d[3], f[3], xs, ys;
    ViewBasis(const V3& e, const V3& look, double tanX, double tanY) : eye(e) {
        f[0] = look.x - e.x, f[1] = look.y - e.y, f[2] = look.z - e.z;
        const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl > 0) f[0] /= fl, f[1] /= fl, f[2] /= fl; else f[0] = f[1] = 0, f[2] = 1;
        r[0] = f[2], r[1] = 0, r[2] = -f[0];
        const double rl = std::max(1e-9, std::sqrt(r[0] * r[0] + r[2] * r[2]));
        r[0] /= rl, r[2] /= rl;
        d[0] = f[1] * r[2] - f[2] * r[1], d[1] = f[2] * r[0] - f[0] * r[2], d[2] = f[0] * r[1] - f[1] * r[0];
        xs = 0.64 / std::max(tanX, 0.64), ys = 0.48 / std::max(tanY, 0.48);
    }
    // View coordinates with the squeeze (y not yet scaled by the GTE's 0.625).
    std::array<double, 3> operator()(double px, double py, double pz) const {
        px -= eye.x, py -= eye.y, pz -= eye.z;
        return {(px * r[0] + py * r[1] + pz * r[2]) * xs, (px * d[0] + py * d[1] + pz * d[2]) * ys,
                px * f[0] + py * f[1] + pz * f[2]};
    }
};

// GTE perspective (0x4038f0, H = 400, offsets 0): IR clamped to s16, divisor z + 1 (z >= 0) or -z, screen clamped to
// [-0x400, 0x3ff].
std::array<int32_t, 2> gteProject(const std::array<double, 3>& v) {
    const int32_t x = std::clamp(int32_t(v[0]), -0x8000, 0x7fff), y = std::clamp(int32_t(v[1] * 2560 / 4096), -0x8000, 0x7fff);
    const int32_t z = int32_t(v[2]), q = z < 0 ? -z : z + 1;
    return {std::clamp(x * 400 / q, -0x400, 0x3ff), std::clamp(y * 400 / q, -0x400, 0x3ff)};
}

}  // namespace

// 0x45ee90 + 0x45eee0 (view 0). Every entity starts the tick out of view (+0x24 |= 4) and unlisted; then, in list
// order: the contact list 0x4d2734 takes flags0 & 0x2000100 == 0x100; an activation block +0x118 must return
// 0x40000000; the sleep counter +0x50 drops by the camera's movement (|Δ|/4 · 0x4038 + 0x302a >> 12) and must be
// < 1; +0x6e 0x20 never lists; 2 culls beyond +0x76 from the camera (sleep = the excess) unless 0x40; 1 culls an
// entity whose zone +0x32 is not in the visible-zone table unless 0x40; 4/8 test the bounding sphere (+0x4a) against
// the near plane (z + r < 400) and the side planes (0xd7a/0x8a0 x, 0xf77/0x419 up, 0xf6d/0x43e down), refine a
// cutting sphere (once it has a model instance +0x9c or ran a tick) with the box (centre sphere, then flag 4's
// corners) and cull unless 0x10/0x40; with 1 too, an
// entity outside the camera's zone must meet its zone's portal rectangle. Listed entities in view clear +0x24 & 4;
// directors (16/35) stay 4.
void TbWorld::viewList() {
    const TbActor& F = actors[size_t(focus())];
    const V3 eye = freeView ? (*freeView)[0] : cam.eye == V3{} ? F.pos : cam.eye;
    const int64_t mx = eye.x - prevEye_.x, my = eye.y - prevEye_.y, mz = eye.z - prevEye_.z;
    const int32_t speed = isqrt(int32_t(std::min<int64_t>((mx * mx + my * my + mz * mz) >> 4, 0x7fffffff)));
    const int32_t dec = (speed * 0x4038 + 0x302a) >> 12;
    prevEye_ = eye;
    const ViewBasis vb(eye, freeView ? (*freeView)[1] : cam.look, viewTanX, viewTanY);
    TbActor& partner = actors[size_t(1 - focus())];
    const std::vector<TbActor*> all = order();  // activation blocks may spawn
    // Release of a culled flags0 0x1000 entity (+0x14e |= 0x4000, +0x14c = 0, model instance +0x9c freed): 0x469210
    // rebuilds it at its next anim tick from the current lists at LOD 0, the state's anim from frame 0.
    auto release = [&](TbActor& a) {
        if (!(a.flags0 & 0x1000)) return;
        a.curAnim = 0, a.modelInst = false;
        if (a.lod) rigSwap(a, nullptr, nullptr, 0), a.lod = 0;
    };
    // 0x45eee0 caps (tag 0x3d): collision list collCap_ - 1 entries; the view list holds the two player slots and
    // viewCap_ - 3 entries: an entity that would overflow it ends the pass, the rest of the list stays unlisted
    // (not ticked, no contacts) this tick.
    int listed = 0, colliders = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        TbActor& a = *all[i];
        a.f24 |= 4;
        a.active = false;
        a.collidable = (a.flags0 & 0x2000100) == 0x100 && !(a.view & 0x20) && colliders < collCap_ - 1 &&
                       !(&a == &partner && (GD[0xa2] & 3));
        colliders += a.collidable;
        if (a.f20 & 0x2000) { a.far = false; continue; }
        if (a.actBlock && !(runBlock(a, a.actBlock) & 0x40000000)) { a.far = false; release(a); continue; }
        a.sleep -= dec;
        if (a.sleep >= 1) continue;
        a.sleep = 0;
        if (a.view & 0x20) { a.far = false; continue; }
        bool out = false;
        if (a.view && a.behaviour != 20 && a.parent != int32_t(F.uid) && F.parent != int32_t(a.uid)) {
            if (a.view & 2) {
                const int32_t dist = approxLen(a.pos.x - eye.x, a.pos.y - eye.y, a.pos.z - eye.z);  // 0x403e10 + 0x404000
                if (dist > a.range) {
                    if (!(a.view & 0x40)) {
                        a.sleep = dist - a.range;
                        a.far = true;
                        release(a);
                        continue;
                    }
                    out = true;
                }
            }
            // LOD (0x45eee0 @0x45f4b0): view flag 0x400, unless the range test kept it as out (0x40), once the entity ran
            // a tick (+0x24 0x8000), with LOD distances (+0x121): camera distance 0x404000 (× 400 / projection 400).
            if ((a.view & 0x400) && !out && (a.f24 & 0x8000) && a.lod1)
                lodTick(a, uint32_t(approxLen(a.pos.x - eye.x, a.pos.y - eye.y, a.pos.z - eye.z)));
            // Zone (view flag 1, 0x45f5d2): the entity's zone byte must be in the visible-zone table the last render
            // filled (portalWalk), else culled (kept as out with 0x40).
            const auto visIdx = [&](size_t from) {
                size_t i = from;
                while (i < visZones.size() && visZones[i].zone != a.zone) ++i;
                return i;
            };
            if ((a.view & 1) && !out && visIdx(0) == visZones.size()) {
                if (!(a.view & 0x40)) { a.far = false; release(a); continue; }
                out = true;
            }
            if ((a.view & 0xc) && !out) {
                auto view = [&](double px, double py, double pz) {
                    const auto v = vb(px, py, pz);
                    return std::array<int32_t, 3>{int32_t(v[0]), int32_t(v[1] * 2560 / 4096), int32_t(v[2])};
                };
                // Signed distances to the side and top / bottom planes (unit normals, Q12).
                auto planeX = [](int32_t x, int32_t z) { return (std::abs(x) * 0xd7a - z * 0x8a0) >> 12; };
                auto planeY = [](int32_t y, int32_t z) {
                    return y < 0 ? (-y * 0xf77 - z * 0x419) >> 12 : (y * 0xf6d - z * 0x43e) >> 12;
                };
                const auto [x, y, z] = view(a.pos.x, a.pos.y, a.pos.z);
                const int32_t rad = a.radius, ux = planeX(x, z), uy = planeY(y, z);
                out = z + rad < 400 || ux > rad || uy > rad;
                // A sphere cutting a side plane: the box's own sphere (centre, half diagonal) decides, and with flag 4
                // its eight corners: out when no corner reaches both halves inside the side and the top / bottom planes.
                if (!out && (ux + rad > 0 || uy + rad > 0) && (a.modelInst || (a.f24 & 0x8000))) {
                    const auto& b = a.box;
                    const int32_t hx = (b[3] - b[0]) >> 1, hy = (b[4] - b[1]) >> 1, hz = (b[5] - b[2]) >> 1;
                    const int32_t r2 = hx * hx + hy * hy + hz * hz;
                    // Own rotation (RotMatrix pitch / yaw / roll) under the parent part's matrix (+0xe8 chain) when
                    // attached; the position is already the chain's world point.
                    Mat M;
                    rotMatrixXYZ(M, a.pitch, a.yaw, a.roll);
                    const TbActor* par = a.parent >= 0 ? entity(a.parent) : nullptr;
                    const Mat P = par ? coordMat(par, a.parentPart) : Mat{};
                    auto world = [&](int32_t lx, int32_t ly, int32_t lz) {
                        V3 w = apply(M, {lx, ly, lz});
                        if (par) w = applyLV(P, w);
                        return view(a.pos.x + w.x, a.pos.y + w.y, a.pos.z + w.z);
                    };
                    const auto [cx, cy, cz] = world(hx + b[0], hy + b[1], hz + b[2]);
                    bool part = false;  // the centre sphere cuts a plane: the corners decide
                    if (const int32_t e = planeX(cx, cz); e >= 1) (e * e - r2 < 1 ? part : out) = true;
                    if (const int32_t e = planeY(cy, cz); !out && e > 0) (e * e - r2 < 1 ? part : out) = true;
                    if (!out && part && (a.view & 4)) {
                        uint32_t bits = 0;
                        for (int k = 0; k < 8 && bits != 0xf; ++k) {
                            const auto [vx, vy, vz] = world(k & 1 ? b[0] : b[3], k & 2 ? b[1] : b[4], k & 4 ? b[2] : b[5]);
                            bits |= vx < 1 ? (planeX(vx, vz) < 0 ? 3 : 2) : (planeX(vx, vz) < 0 ? 3 : 1);
                            bits |= vy < 0 ? (planeY(vy, vz) < 0 ? 0xc : 4) : (planeY(vy, vz) < 0 ? 0xc : 8);
                        }
                        out = bits != 0xf;
                    }
                }
                if (out && !(a.view & 0x50)) { a.far = false; release(a); continue; }
                // In view in another zone than the camera's (@0x45fc8a): the GTE screen box of two box corners (pos +
                // min / max, y as pos.y - box y) must meet that zone's portal rectangle (the render's 640×480 y-up
                // space against GTE 512×240 y-down, as the exe compares them).
                if (!out && (a.view & 1) && a.zone != visZones[0].zone)
                    if (const size_t i = visIdx(1); i < visZones.size()) {
                        const VisZone& e = visZones[i];
                        const auto p = gteProject(vb(a.pos.x + a.box[0], a.pos.y - a.box[1], a.pos.z + a.box[2]));
                        const auto q = gteProject(vb(a.pos.x + a.box[3], a.pos.y - a.box[4], a.pos.z + a.box[5]));
                        const int32_t x0 = int16_t(std::min(p[0], q[0])), x1 = int16_t(std::max(p[0], q[0]));
                        const int32_t y0 = int16_t(std::min(p[1], q[1])), y1 = int16_t(std::max(p[1], q[1]));
                        if (int16_t(e.x1) < x0 || x1 < int16_t(e.x0) || int16_t(e.y1) < y0 || y1 < int16_t(e.y0)) {
                            if (!(a.view & 0x40)) { a.far = false; release(a); continue; }
                            out = true;
                        }
                    }
            }
        }
        if (listed >= viewCap_ - 3) {
            for (size_t j = i + 1; j < all.size(); ++j) all[j]->active = all[j]->collidable = all[j]->far = false;
            break;
        }
        listed += !(a.flags0 & 0x4000000) && a.behaviour != 6;  // players take the two slots; class 6 is not listed
        if ((a.flags0 & 0x4000000) && a.behaviour != 6 && a.behaviour != 16 && a.behaviour != 35 && a.id == ((GD[0xa4] & 4) ? 3 : 2))
            focusPtr_ = &a == &actors[1] ? 1 : 0;
        a.active = true, a.far = false;
        if (!out) a.f24 &= ~4u;
        if (a.behaviour == 16 || a.behaviour == 35) a.f24 |= 4;
    }
    // A partner out of the game (GD+0xa2 & 3) keeps its view-list slot empty (0x45eee0 local_7d0[1] stays -1): not
    // ticked, not drawn (ARQ01: Bugs down while Taz is in, his +0x82 frozen from 1491).
    if (GD[0xa2] & 3) actors[size_t(1 - focus())].active = false;
    if (lookMode_) actors[size_t(focus())].f24 |= 4, actors[size_t(focus())].f20 |= 0x80000000u;  // the look view hides it
    // View flag 0x4000: an entity whose parent (+0x90) is hidden (+0x24 & 4) is hidden and leaves the list (@0x46009e).
    for (TbActor* a : all)
        if (a->active && (a->view & 0x4000))
            if (const TbActor* par = entity(a->parent); par && (par->f24 & 4)) a->f24 |= 4, a->active = false, release(*a);
    cheatPad();  // 0x45eee0 tail
}

// 0x40fbc0 (every input poll 0x40e0c0): the DirectInput key states 0..254 in order against "RULETHEEARTH" (table
// 0x48871c, followed by words no key matches): a held key that is the next one advances, one that is neither it nor
// (past the first) the last one resets; more than 11 matched arms the pad cheats for the session (0x4d095c, never
// cleared). A key held over polls matches again while it is also the next one (the doubled E needs one press).
void TbWorld::cheatKeys(const std::array<bool, 256>& dikHeld) {
    static constexpr uint8_t kWord[12] = {19, 22, 38, 18, 20, 35, 18, 18, 30, 19, 20, 35};  // R U L E T H E E A R T H
    auto word = [](int i) { return i < 12 ? int(kWord[i]) : -1; };
    for (int k = 0; k < 0xff; ++k) {
        if (!dikHeld[size_t(k)]) continue;
        if (word(cheats.typed) == k) ++cheats.typed;
        else if (cheats.typed == 0 || word(cheats.typed - 1) != k) { cheats.typed = 0; return; }
    }
    if (cheats.typed > 11) cheats.on = true;
}

// 0x43bce0 (0x45eee0 tail): once armed, each new nonzero raw P1 word (GD+0xe) steps the 11 sequences of table 0x4ce8e8
// (○○○○ then three of □ △ ✕); a mismatch restarts that sequence. Completed: 0 the level select (GD+0x8c 1, the pause
// menu's eighth entry, template 0x1d block 312; GD+0xab 0xff), 1 GD+0x58 = 999 (2), 2 GD+0xa2 4, +0xad |= 0xfd,
// +0xae 0xff (4), 3 health 6 / 6, lives >= 3, a downed partner back at the focus (8), 4 GD+0xd2..0xdd = 1 (0x10);
// sequences 5..10 set nothing.
void TbWorld::cheatPad() {
    static constexpr uint16_t kSeq[11][3] = {{0x80, 0x80, 0x80}, {0x80, 0x80, 0x10}, {0x80, 0x10, 0x80}, {0x80, 0x10, 0x10},
                                             {0x10, 0x80, 0x80}, {0x10, 0x80, 0x10}, {0x10, 0x10, 0x80}, {0x10, 0x10, 0x10},
                                             {0x40, 0x80, 0x80}, {0x40, 0x80, 0x10}, {0x40, 0x80, 0x20}};
    const uint16_t raw = pads[0].raw;
    // s16 last vs u16 word (exe compare): a word with the Left bit (0x8000) never equals it, so it counts every tick.
    if (!cheats.on || int(cheats.last) == int(raw)) return;
    cheats.last = int16_t(raw);
    if (!raw) return;
    for (size_t c = 0; c < 11; ++c) {
        uint8_t& at = cheats.progress[c];  // 7 = completed, left on the 0 terminator: the next press restarts it
        const uint16_t want = at < 4 ? 0x20 : at < 7 ? kSeq[c][at - 4] : 0;
        if (want != raw) { at = 0; continue; }
        if (++at < 7) continue;
        switch (c) {
        case 0: GD[0x8c] |= 1, GD[0xab] = 0xff; break;
        case 1: GD[0x58] = 999 & 0xff, GD[0x59] = 999 >> 8, GD[0x8c] |= 2; break;
        case 2: GD[0xa2] |= 4, GD[0xad] |= 0xfd, GD[0xae] = 0xff, GD[0x8c] |= 4; break;
        case 3: {
            GD[0xa5] = GD[0xa6] = 6, GD[0x8c] |= 8;
            if (GD[0xa0] < 3) GD[0xa0] = 3;
            TbActor& p = actors[size_t(1 - focus())];
            if (p.f20 & 0x2000000) {
                p.pos = actors[size_t(focus())].pos;
                p.f20 = p.f24 = p.f28 = 0, p.hp = 6;
                GD[0xa2] &= 0xfc;
            }
            break;
        }
        case 4: std::fill(GD.begin() + 0xd2, GD.begin() + 0xde, uint8_t(1)), GD[0x8c] |= 0x10; break;
        default: break;
        }
    }
}

int TbWorld::findZone(const V3& p) const {
    const int32_t x = int16_t(p.x), y = int16_t(p.y), z = int16_t(p.z);  // 0x403bb0: s16 copy
    for (size_t i = 0; i < zoneModels_.size(); ++i) {
        const ZoneModel& m = zoneModels_[i];
        if (!m.present) continue;
        const auto& b = m.box;  // x0, y0, z0, sx, sy, sz (u16 sizes): [x0, x0+sx] × [y0-sy, y0] × [z0, z0+sz]
        if (b[0] - 5 <= x && x <= b[0] + 5 + uint16_t(b[3]) && y <= b[1] + 5 && b[1] - uint16_t(b[4]) - 5 <= y &&
            b[2] - 5 <= z && z <= b[2] + 5 + uint16_t(b[5]))
            return int(i);
    }
    return -1;
}

// 0x465710 (world entity, class 7, drawn last every frame): entry 0 = camera zone with (-256, -106)..(256, 109), the
// rest free; 0x406840 over the camera zone's portals, then breadth-first over the table (entries 1.. until a free
// one) for each zone's model. A portal: its five points through the render matrix (float, projection 400, view x ×
// 1.25, y × -1.25: the 640×480 viewport, y up; divisor z + 1, or -z behind) give a screen box; one meeting the
// viewport (-320..319, -239..240) takes the first free entry with the box clamped to it (truncated). A portal whose
// zone is listed already, or a full table, ends that zone's walk (the exe returns).
void TbWorld::portalWalk() {
    visZones.fill({-1, 0, 0, 0, 0});
    const int fz = freeView ? findZone((*freeView)[0]) : -1;
    visZones[0] = {int16_t(fz >= 1 ? fz : camZone), -256, -106, 256, 109};
    const ViewBasis vb(freeView ? (*freeView)[0] : cam.eye, freeView ? (*freeView)[1] : cam.look, viewTanX, viewTanY);
    constexpr float kX0 = -320, kX1 = 319, kY0 = -239, kY1 = 240;
    auto walk = [&](int16_t zone) {
        if (zone < 0 || size_t(zone) >= zoneModels_.size()) return;
        for (const ZonePortal& p : zoneModels_[size_t(zone)].portals) {
            size_t slot = 0;
            while (slot < visZones.size() && visZones[slot].zone != p.zone && visZones[slot].zone != -1) ++slot;
            if (slot == visZones.size() || visZones[slot].zone == p.zone) return;
            float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            for (size_t k = 0; k < p.pt.size(); ++k) {
                const auto v = vb(p.pt[k].x, p.pt[k].y, p.pt[k].z);
                const float z = float(v[2]), q = z >= 0 ? 400 / (z + 1) : -(400 / z);
                const float sx = float(v[0]) * 1.25f * q, sy = float(v[1]) * -1.25f * q;
                x0 = k ? std::min(x0, sx) : sx, x1 = k ? std::max(x1, sx) : sx;
                y0 = k ? std::min(y0, sy) : sy, y1 = k ? std::max(y1, sy) : sy;
            }
            if (x0 > kX1 || x1 < kX0 || y0 > kY1 || y1 < kY0) continue;
            visZones[slot] = {p.zone, int32_t(std::max(x0, kX0)), int32_t(std::max(y0, kY0)), int32_t(std::min(x1, kX1)),
                              int32_t(std::min(y1, kY1))};
        }
    };
    walk(int16_t(camZone));
    for (size_t i = 1; i < visZones.size() && visZones[i].zone >= 1; ++i) walk(visZones[i].zone);
}

// Dispatch table 0x4cea60 by class +0x31. Classes 0/6/7/20 (static model, system, flipbook link) only draw.
void TbWorld::classTick(TbActor& a) {
    auto gdBit = [&](uint16_t b) { return (b & ~7u) < 0x5e1 && (GD[500 + (b >> 3)] >> (b & 7) & 1); };
    switch (a.behaviour) {
    case 14: actorStep(a); return;
    case 2: linkTick(a); return;     // 0x4599e0 texture-link writer
    case 4: spriteTick(a); return;   // 0x453b40 sprite flipbook
    case 33: hungTick(a); return;    // 0x459b10 hung on a part
    case 30: projTick(a); return;    // 0x44d1f0 projectile
    case 5: blockTick(a); return;
    case 18: platformTick(a, 1); return;     // 0x450a90 seesaw
    case 21: platformTick(a, 0x10); return;  // 0x450ae0 swinging platform
    case 40: platformTick(a, 0x1000); return;  // 0x450ba0 (VIBOSS01)
    case 16:  // 0x450bc0: first tick removes entities whose GAME_DATA bit is set (flags10 & 4); first block each tick
        if (!(a.f24 & 0x8000)) {
            if ((a.flags10 & 4) && gdBit(a.gdBit)) {
                a.f20 |= 0x2000;
                return;
            }
            a.f24 |= 0x8000;
        }
        // flags1 0x10 always, 0x4 within 2000 (xz) of the focus: the camera follows this director (B+0x44, 0x439700).
        if (a.flags1 & 0x14) {
            const TbActor& f = actors[size_t(focus())];
            const int32_t dx = f.pos.x - a.pos.x, dz = f.pos.z - a.pos.z;
            if ((a.flags1 & 0x10) || isqrt(dx * dx + dz * dz) < 2000) camTarget = int32_t(a.uid);
        }
        if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
        return;
    case 35:  // 0x450cd0
        if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
        return;
    case 31: textTick(a); return;  // 0x450d00 overlay text
    case 0:  // 0x450890 first tick (no model instance yet): +0x24 0x8000; no asset links = removed; tag-0x61 record
             // +0x123 at once, without such records the box +0x60 / +0x68 is scaled by +0xec (GTE, diagonal matrix)
        if (!(a.f24 & 0x8000)) {
            a.f24 |= 0x8000;
            if (a.links.empty()) { a.f20 |= 0x2000; return; }
            if (!a.boxes.empty()) a.boxInstant = true, boxStep(a);  // 0x450440(a, 0)
            else
                for (size_t i = 0; i < 6; ++i) a.box[i] = int16_t((int32_t(a.box[i]) * a.scale[i % 3]) >> 12);
        }
        return;
    case 20:  // 0x453d00 (flipbook link): first tick +0x24 0x8000
        a.f24 |= 0x8000;
        return;
    case 6: case 7: return;
    case 9: case 39:  // 0x44d6f0 sky dome / 0x450e00: first tick only (no model instance +0x9c yet): no asset links =
                      // removed, else anim role 0xb mode 2 started once (never advanced) and scale 1 (+0xec..)
        if (a.inst) return;
        a.inst = true;
        if (a.links.empty()) { a.f20 |= 0x2000; return; }
        if (a.model && findAnim(*a.model, 0xb) >= 0) animTick(*a.model, a.anim, 0xb, 2);
        a.scale = {4096, 4096, 4096};
        return;
    case 10: return;  // 0x450df0 stores the entity in 0x4d26f4, which nothing reads (its 2 references: this, the clear
                      // in 0x460630)
    default: unsupported.insert(std::format("class{}", a.behaviour)); return;
    }
}

// 0x453de0 head: a pending hit (+0x20 0x8000000 by a player / 0x4000000 by an enemy on a vulnerable state, a stomp,
// or V 0x80080000's +0x24 0x40000000) costs +0x66 health unless flashing (+0x28 0x200) or dead; players start their
// 60-tick flash (0x4d2058/9). Health < 1 without flags0 0x200 = death (+0x20 0x2000000, event 0x10000000, GD+0xa2
// 0x10/0x20), else event 0x8000000 when the attacker named a transition (+0x5e). The new state's blocks run at once
// and carried children (flags0 & 0x2802) drop. Returns true when the tick ends here.
bool TbWorld::hitIntake(TbActor& a) {
    const TbStateRec& r = *a.rec();
    const uint32_t f = a.f20;
    const bool hit = ((f & 0xc000000) && (r.flagsA & 0x10000) && (f & 0x8000000)) ||
                     ((r.flagsB & 0x8000) && (f & 0x10) && (f & 0x8000000)) || ((r.flagsB & 8) && (f & 0x4000000)) ||
                     (a.f24 & 0x40000000);
    a.f20 &= ~0x10u;  // stood on this tick (set by the mover of the entity on top; I)
    if (hit && !(a.f28 & 0x200) && !(f & 0x2000000)) {
        const bool player = a.flags0 & 0x4000000;
        if (player && a.dmgIn) {
            flash[a.id == 2 ? 0 : 1] = a.id == 2 ? 0x40 : 0x80;
            a.f28 |= 0x200;
        }
        a.hp = int16_t(std::max(0, a.hp - a.dmgIn));
        a.dmgIn = 0;
        if (a.flags1 & 0x200) S[1] = uint8_t((a.hp << 7) / std::max<int16_t>(a.maxHp, 1));
        uint32_t ev = 0x8000000;
        if (!(a.flags0 & 0x200) && a.hp < 1) {
            a.f20 |= 0x2000000;
            ev = 0x10000000;
            if (player) GD[0xa2] |= a.id == 2 ? 0x10 : 0x20;
        }
        if (a.hitTr != -1 || ev == 0x10000000) {
            const bool changed = dispatch(a, ev);
            if (!changed && ev == 0x10000000) {
                a.f20 &= ~0xc000000u;
                a.f28 &= ~0x20000u;
                return true;
            }
            if (!a.curAnim) a.animLast = false, a.anim.flags &= ~0x3000u;  // +0x14c 0: +0x14e &= 0xcfff
            a.attachPart = -1;
            // The exe clamps each (0x450650) before detaching it, i.e. on its +0xe4 = +0x12c local record: the ray runs
            // to the local position and the clamp rewrites only +0x13c, which the detach leaves unused (+0xe4 back to
            // +0xec, +0xfc untouched). No observable effect, so not run here.
            forEachEntity([&](TbActor& c) {
                if (c.parent != int32_t(a.uid) || !(c.flags0 & 0x2802)) return;
                c.f20 &= ~0x8000u;
                c.f24 |= 0x80;
                if (c.progMask[1] & 0x80) runProgram(c, 0, 0x80);
                detach(c);
            });
            for (uint16_t b : {a.rec()->logicA, a.rec()->logicB})
                if (b) {
                    const uint32_t e = runBlock(a, b);
                    if (!(e & 0x90000000)) dispatch(a, e);
                }
            if (a.f20 & 0x2000) return true;
        }
    }
    a.f20 &= ~0xc000000u;
    a.f24 &= ~0x40000000u;
    a.f28 &= ~0x20000u;
    return false;
}

// 0x453de0 contact loop over the mover's actor contacts: both sides remember each other (+0x54) and raise +0x20
// 0x40 (touching a non-player) / 0x80 (a player); a contact attacker (+0x20 0x1000, state flagsA 0x1000000 with
// 0x40000 vs players / 0x80000 vs others) hurts a hittable (flags0 0x80), vulnerable victim; standing on a
// stompable state (flagsB 0x8000) hurts it, on a bouncy one (0x10000000) raises +0x24 8 and stands on nothing (the
// ride block's floor dropped), as on a class-5 block for a stander with +0x192 0x100. The bits given to the touched
// entity (0x10: stood on) that its program masks (+0x14/+0x18) run its program at once (0x457a07 -> 0x45ea90).
// Pole grab (0x456fb0): a state with flagsA 0x100 (players also in the air: +0x20 0x80420) touching a pole (flags0
// 0x10000) while hung on nothing, the pole ahead (atan2 of the offset - 0x400 - yaw within half a turn) and the
// offset's y within (pole +0x6a - own +0x6a, pole +0x62): it hangs on the pole's root record at the offset turned
// by -its yaw (+0x13c), facing it (+0x136 = atan2 - 0x800); no move, no velocity, +0x20 0x200 (0x421 cleared); the
// carried entities (flags0 0x2800) are let go.
bool TbWorld::contacts(TbActor& a, const std::vector<TbActor*>& touched, std::array<int32_t, 3>& disp) {
    const bool aPl = a.flags0 & 0x4000000;
    bool drop = false;
    for (TbActor* cp : touched) {
        TbActor& c = *cp;
        const bool cPl = c.flags0 & 0x4000000;
        a.contact = int32_t(c.uid), c.contact = int32_t(a.uid);
        const uint32_t ab = cPl ? 0x80 : 0x40;
        a.f20 |= ab, a.trig[0] |= ab;
        uint32_t c20 = aPl ? 0x80 : 0x40;
        uint32_t c24 = 0;
        c.f20 |= c20;
        if ((c.f20 & 0x1000) && c.rec() && a.rec()) {
            const uint32_t ca = c.rec()->flagsA;
            if ((ca & 0x1000000) && (a.flags0 & 0x80) && (aPl ? (ca & 0x40000) : (ca & 0x80000)) &&
                (cPl ? (a.rec()->flagsA & 0x10000) : (a.rec()->flagsB & 8))) {
                a.dmgIn = c.rec()->dmg;
                a.f20 |= cPl ? 0x8000000 : 0x4000000;
                if (cPl && c.id == 3) a.f28 |= 0x20000;
                c24 = aPl ? 2u : 0x100000u;
                if (aPl && a.hp <= a.dmgIn) c24 |= 1;
                c.f24 |= c24;
                a.hitTr = c.rec()->hitTrans;
            }
        }
        if (!(a.f20 & 0x6000000) && (c.f20 & 0x10) && c.behaviour == 14 && c.rec() && a.rec()) {
            if (c.rec()->flagsB & 0x8000) {
                c.dmgIn = a.rec()->dmg;
                c.f20 |= aPl ? 0x8000000 : 0x4000000;
                if (aPl && a.id == 3) c.f28 |= 0x20000;
                const uint32_t b = cPl ? (c.hp <= c.dmgIn ? 3u : 2u) : 0x100000u;
                a.f24 |= b, a.trig[1] |= b;
                c.hitTr = a.rec()->hitTrans;
            }
            if (c.rec()->flagsB & 0x10000000) a.f24 |= 8, a.trig[1] |= 8, drop = true;
        } else if ((c.f20 & 0x10) && c.behaviour == 5 && (a.turnRate & 0x100)) {
            a.f24 |= 8, a.trig[1] |= 8, drop = true;
        }
        if ((c.flags0 & 0x10000) && a.rec() && (a.rec()->flagsA & 0x100) && ((a.f20 & 0x80420) || !aPl) && a.parent < 0) {
            const int16_t dx = int16_t(a.pos.x - c.pos.x), dy = int16_t(a.pos.y - c.pos.y), dz = int16_t(a.pos.z - c.pos.z);
            if (uint32_t(ratan2(dx, dz) - 0x400 - uint16_t(a.yaw)) % 0x1000 < 0x800 && dy < c.box[1] && c.box[4] < a.box[4] + dy) {
                Mat m;
                mulRotY(m, -c.yaw);
                a.local = apply16(m, {dx, dy, dz});
                a.localYaw = int16_t(ratan2(a.local.x, a.local.z) - 0x800);
                link(c, a);
                a.parentPart = -1, a.rideRec = false;
                disp = {};
                a.f20 = (a.f20 & 0xfffffbdeu) | 0x200;
                a.vel = {};
                const int32_t self = int32_t(a.uid);
                forEachEntity([&](TbActor& k) {
                    if (k.parent != self || !(k.flags0 & 0x2800)) return;
                    k.f20 &= ~0x8000u;
                    k.f24 |= 0x80;
                    if (k.progMask[1] & 0x80) runProgram(k, 0, 0x80, 0);
                    detach(k);
                });
            }
        }
        if (c.f20 & 0x10) c20 |= 0x10;
        if (const uint32_t m20 = c20 & c.progMask[0], m24 = c24 & c.progMask[1]; m20 || m24) runProgram(c, m20, m24);
    }
    return drop;
}

// 0x44e5d0: the entity's root record matrix (Rx·Rz or Rz·Rx with flags10 0x100000, then Ry, scaled by +0xec) at +0xfc.
Mat TbWorld::recordMat(const TbActor& a) const {
    Mat M;
    if (a.flags10 & 0x100000) mulRotZ(M, a.roll), mulRotX(M, a.pitch);
    else mulRotX(M, a.pitch), mulRotZ(M, a.roll);
    mulRotY(M, a.yaw);
    for (size_t i = 0; i < 9; ++i) M.m[i] = int16_t((int32_t(M.m[i]) * a.scale[i % 3]) >> 12);
    M.t = a.pos;
    return M;
}

// The record chain of an attached entity (0x453de0 @0x456a.., 0x403400 per level): the parent record (its +0x84 part
// or root) applied to the local position +0x13c; `yaw` = heading of Ry(+0x136) through it (0x4040d0).
V3 TbWorld::hangPos(const TbActor& a, const TbActor& par, int16_t* yaw) const {
    const Mat m = a.parentPart >= 0 ? coordMat(&par, a.parentPart) : recordMat(par);
    const V3 o = apply(m, a.local);
    if (yaw) {
        const V3 d = apply16(m, rotY(a.localYaw, 0, 0, 0x1000));
        *yaw = int16_t(ratan2(d.x, d.z));
    }
    if (a.parentPart >= 0) return chainPoint(par, a.parentPart, a.local);  // 0x44e560: one coordinate at a time
    return {m.t.x + o.x, m.t.y + o.y, m.t.z + o.z};
}

// 0x453de0 push block. probed = false: contact c (flags0 0x4000) when the point 50 up and box+0x78 ahead lies in its
// box (0x4486a0) raises +0x20 0x100000; a push state (flagsA 0x8000000) hands half the blocked move (target - move)
// to c's +0x58 and 48000/4096 of it to the velocity. Axis blocks (c.flags10 2) first align: yaw = c's + the side's
// quarter, the move projected on that heading. probed = true (the 0x447fd0 probe 100 ahead): only the alignment.
// Then +0x20 0x40000 and c's +0x24 0x10 (class 5 moves by +0x58 that tick), c's program for 0x10.
void TbWorld::push(TbActor& a, TbActor& c, std::array<int32_t, 3>& disp, const std::array<int32_t, 3>& target, bool probed) {
    if (!probed) {
        Mat R;
        rotMatrixXYZ(R, a.pitch, a.yaw, a.roll);
        const V3 o = apply(R, {0, -50, int16_t(a.box[5] + 0x78)});
        const V3 d{int16_t(a.pos.x + o.x - c.pos.x), int16_t(a.pos.y + o.y - c.pos.y), int16_t(a.pos.z + o.z - c.pos.z)};
        if (uint32_t(d.x * d.x + d.z * d.z) > uint32_t(c.radius) * c.radius) return;
        const V3 l = rotY(-c.yaw, d.x, d.y, d.z);
        const int32_t inf = c.inflate;
        if (!(c.box[0] - inf < l.x && l.x < c.box[3] + inf && c.box[2] - inf < l.z && l.z < c.box[5] + inf && l.y < c.box[1] && c.box[4] < l.y))
            return;
        a.f20 |= 0x100000;
        if (!(a.rec()->flagsA & 0x8000000)) return;
    }
    auto shove = [&](int32_t dx, int32_t dz) {
        if (probed) return;
        c.carry[0] = int16_t(c.carry[0] + (int16_t(dx) >> 1)), c.carry[2] = int16_t(c.carry[2] + (int16_t(dz) >> 1));
        a.vel[0] = int16_t(a.vel[0] + ((int16_t(dx) * 48000) >> 12)), a.vel[2] = int16_t(a.vel[2] + ((int16_t(dz) * 48000) >> 12));
    };
    bool side = false;
    if (!(c.flags10 & 2)) {
        shove(target[0] - disp[0], target[2] - disp[2]);
    } else {
        const V3 rel = rotY(-c.yaw, a.pos.x - c.pos.x, a.pos.y - c.pos.y, a.pos.z - c.pos.z);
        int32_t u = 0;
        if (c.box[0] < rel.x && rel.x < c.box[3]) {
            if (c.box[5] < rel.z) u = -0x800, side = true;
            else if (rel.z < c.box[2]) u = 0, side = true;
        } else if (c.box[2] < rel.z && rel.z < c.box[5]) {
            if (c.box[3] < rel.x) u = -0x400, side = true;
            else if (rel.x < c.box[0]) u = 0x400, side = true;
        }
        if (side) {
            a.yaw = int16_t(u + c.yaw), a.roll = 0;
            const int32_t cs = rcos(a.yaw), sn = rsin(a.yaw), n = cs * cs + sn * sn;
            const int32_t dot = disp[2] * cs + disp[0] * sn, want = target[2] * cs + target[0] * sn;
            disp[0] = sn * dot / n, disp[2] = cs * dot / n;
            shove(sn * want / n - disp[0], cs * want / n - disp[2]);
        }
    }
    if (!(c.flags10 & 2) || side) {
        a.f20 |= 0x40000;
        c.f24 |= 0x10;
        if (!probed && (c.progMask[0] & 0x10)) runProgram(c, 0x10);
    }
}

// 0x453de0 ride block on the mover's floor entity (request +0x10). A new floor: its record (+0x84 part with flags0
// 0x80000000 / flags1 0x400000, else the root) takes the entity: flags10 0x20000 (players) on +0x104 with the local
// position through the transposed record, others with local yaw +0x136 on +0xe8 (move cleared). A floor with flags10 1
// pushes the stander off 0x1e a tick instead. Riding: the floor gets +0x20 0x40 / 0x80 and +0x54. No floor while
// riding (+0x20 8): roll 0, left. Runtime ARCA02 / VICASG04 captures (local/wine/compare/w13/classes).
void TbWorld::ride(TbActor& a, TbActor* floor, std::array<int32_t, 3>& disp) {
    if (floor && floor != entity(a.parent)) {
        if (a.parent >= 0) detach(a);
        const bool rec = a.flags10 & 0x20000;
        if ((floor->flags10 & 1) || !rec) {
            Mat m;
            mulRotX(m, 0x1000 - floor->pitch), mulRotY(m, 0x1000 - floor->yaw), mulRotZ(m, 0x1000 - floor->roll);
            a.local = apply16(m, {int16_t(a.pos.x - floor->pos.x + disp[0]), int16_t(a.pos.y - floor->pos.y + disp[1]),
                                  int16_t(a.pos.z - floor->pos.z + disp[2])});
        }
        auto slide = [&](int32_t dx, int32_t dz) {
            Mat m;
            mulRotX(m, floor->pitch), mulRotZ(m, floor->roll), mulRotY(m, floor->yaw);
            const V3 d = apply16(m, {int16_t(dx), 0, int16_t(dz)});
            a.vel = {d.x << 4, 0, d.z << 4};
            a.carry[0] = int16_t(d.x), a.carry[2] = int16_t(d.z);
            a.f20 = (a.f20 & 0xfffffef5u) | 0x20;
            floor = nullptr;
        };
        if (floor->flags10 & 1) {
            int32_t dx = 0x1e, dz = 0, best = a.local.x - floor->box[3];
            if (best < floor->box[0] - a.local.x) dx = -0x1e, best = floor->box[0] - a.local.x;
            if (best < a.local.z - floor->box[5]) dx = 0, dz = 0x1e, best = a.local.z - floor->box[5];
            if (best < floor->box[2] - a.local.z) dx = 0, dz = -0x1e;
            slide(dx, dz);
        }
        if (floor && !rec && (a.flags1 & 0x1000000)) {
            const int32_t cx = a.local.x + a.box[0] + ((a.box[3] - a.box[0]) >> 1), cz = a.local.z + a.box[2] + ((a.box[5] - a.box[2]) >> 1);
            int32_t dx = floor->box[3] < cx ? 0x14 : cx < floor->box[0] ? -0x14 : 0;
            int32_t dz = floor->box[5] < cz ? 0x14 : cz < floor->box[2] ? -0x14 : 0;
            if (dx && dz) dx = dx > 0 ? 0xe : -0xe, dz = dz > 0 ? 0xe : -0xe;
            if (dx || dz) slide(dx, dz);
            else a.vel = {};
        }
        if (floor) {
            a.parentPart = (floor->flags0 & 0x80000000u) || (floor->flags1 & 0x400000) ? floor->attachPart : -1;
            if (!rec) {
                a.localYaw = int16_t(a.yaw - floor->yaw);
                disp = {};
            } else {
                const Mat m = a.parentPart >= 0 ? coordMat(floor, a.parentPart) : recordMat(*floor);
                Mat t;
                for (size_t i = 0; i < 3; ++i)
                    for (size_t j = 0; j < 3; ++j) t.m[i * 3 + j] = m.m[j * 3 + i];
                a.local = apply16(t, {int16_t(a.pos.x - m.t.x), int16_t(a.pos.y - m.t.y), int16_t(a.pos.z - m.t.z)});
            }
            link(*floor, a);
            a.rideRec = rec;
            a.f20 = (a.f20 & ~2u) | 8;
            if (!(a.f24 & 8)) a.f20 = (a.f20 & 0xfffffbdcu) | 8;
        }
    }
    if (floor) {
        if (TbActor* p = entity(a.parent); p && (a.f20 & 8)) p->f20 |= (a.flags0 & 0x4000000) ? 0x80 : 0x40, p->contact = int32_t(a.uid);
        return;
    }
    if (a.f20 & 8) {
        a.roll = 0;
        detach(a);
        a.f20 &= 0xfffffdd7u;
    }
}

namespace {
// 0x4048b0: a·b with each product row sum >> 12 stored s16.
Mat mul(const Mat& a, const Mat& b) {
    Mat o;
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j)
            o.m[i * 3 + j] = int16_t((a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j]) >> 12);
    return o;
}
Mat transpose(const Mat& a) {  // 0x404bc0
    Mat t;
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = 0; j < 3; ++j) t.m[i * 3 + j] = a.m[j * 3 + i];
    return t;
}
}  // namespace

// The record chain's rotation (0x404a50 from the own record up the +0x48 supers): hung entities Ry(+0x136) under the
// parent part, others their root record.
Mat TbWorld::chainRot(const TbActor& a) const {
    const TbActor* par = a.parent >= 0 && !a.rideRec ? const_cast<TbWorld*>(this)->entity(a.parent) : nullptr;
    if (!par) return recordMat(a);
    Mat own;
    mulRotY(own, a.localYaw);
    return mul(coordMat(par, a.parentPart), own);
}

// 0x44f7b0 for modes 0x10 (class 21), 1 (18) and 0x1000 (40). First tick: the class-0xb anim (mode 2), +0x24 0x8000;
// 0x10: the half amplitude +0x178 >>= 1, centre +0x184 = pos + record · (0, 0, amp), speed +0x3c (0 = 0x10); 0x1001:
// roll and tilt +0x17a 0, +0x82 = 20. Mode 0x10: pos = centre + record · (0, 0, sin(phase) · amp), phase += speed,
// +0x20 0x20000 on the tick it crosses 0x400 / 0xc00. Modes 0x1001 (seesaw, limit 0x164 / +0x40): the riders' moment
// (local x · mass) tilts the roll by moment · k / box side (k = +0x3c or 0x20) toward ±limit (+0x178 bit 1 / 2 blocks
// a side) while ridden, within 10 ticks of the last rider or with +0x20 0x800000 / 0x1000000 (driven to a side); else
// the roll decays 1/16 a tick. Reaching a driven side: riders on the far half get +0x28 0x80000. Tail: first block
// +0x114, record, per-tick bits cleared. Runtime ARCA02 / VICASG04 / VIBOSS01 (local/wine/compare/w13/classes).
void TbWorld::platformTick(TbActor& a, uint32_t mode) {
    const V3 pos0 = a.pos;
    const std::array<int16_t, 3> ang0{a.pitch, a.yaw, a.roll};
    if (!a.inst) {
        a.inst = true;
        if (a.model && findAnim(*a.model, 0xb) >= 0) {
            animTick(*a.model, a.anim, 0xb, 2);
            if (a.boxes.empty() && a.anim.boxSet) {  // 0x468280 packet 0x900: the frame's box, x / z inflated by +0x3e
                const auto& t = a.anim.box;
                a.box = {int16_t(t[0] - a.inflate), t[1], int16_t(t[2] - a.inflate), int16_t(t[3] + a.inflate), t[4], int16_t(t[5] + a.inflate)};
                a.radius = a.anim.radius;
            }
        }
        a.f24 |= 0x8000;
        if (mode & 0x10) {
            a.arg = int16_t(a.arg >> 1);
            const V3 d = applyLV(recordMat(a), {0, 0, a.arg});
            a.home = {int16_t(a.pos.x + d.x), int16_t(a.pos.y + d.y), int16_t(a.pos.z + d.z)};
            a.phase = 0;
            if (!a.hp) a.hp = 0x10;
        } else {
            a.roll = 0, a.phase = 0, a.stateTicks = 0x14;
        }
    }
    std::vector<TbActor*> kids;  // +0x8c
    for (TbActor* c : order())
        if (c->parent == int32_t(a.uid)) kids.push_back(c);
    if (mode & 0x10) {
        const int32_t v = (rsin(a.phase) * a.arg) >> 12;
        const int16_t old = a.phase;
        const uint16_t nu = uint16_t(a.hp + old) & 0xfff;
        a.phase = int16_t(nu);
        if ((old < 0x400 && 0x3ff < nu) || (old < 0xc00 && 0xbff < nu)) a.f20 |= 0x20000;
        else a.f20 &= ~0x20000u;
        const V3 d = applyLV(recordMat(a), {0, 0, v});
        a.pos = {int16_t(a.home.x + d.x), int16_t(a.home.y + d.y), int16_t(a.home.z + d.z)};
    } else {
        const int32_t lim = mode & 1 ? 0x164 : a.f40;
        const int32_t lo = a.box[0] - a.inflate, hi = a.box[3] + a.inflate;
        const uint32_t u = a.f20;
        bool damp = a.stateTicks >= 10 && kids.empty() && !(u & 0x1800000);
        if (!damp) {
            int32_t v = 0;
            if (kids.empty()) a.phase = int16_t(a.phase - (a.phase >> 2)), v = a.phase;
            else {
                a.stateTicks = 0;
                for (TbActor* c : kids) v += (int16_t(c->local.x) * c->mass) >> 12;
                a.phase = int16_t(v);
            }
            if (u & 0x800000) v = hi;
            if (u & 0x1000000) v = lo;
            const int32_t k = a.hp ? a.hp : 0x20, side = v < 0 ? lo : hi;
            damp = a.arg & (v < 0 ? 1 : 2);
            if (!damp) {
                if (v < 0 ? -lim < a.roll : a.roll < lim) a.roll = int16_t(a.roll + int16_t(v * k / side) * (v < 0 ? -1 : 1));
                if (a.roll * k / side == 0) a.roll = int16_t(a.roll - (a.roll >> 4));
            }
        }
        if (damp) a.roll = int16_t(a.roll - (a.roll >> 4));
        // At a limit: a driven side (+0x20 0x800000 / 0x1000000) ends; the riders on the far half get +0x28 0x80000.
        auto tip = [&](auto far) {
            for (TbActor* c : kids)
                if (far(int32_t(int16_t(c->local.x)))) {
                    c->f28 |= 0x80000;
                    if (c->progMask[2] & 0x80000) runProgram(*c, 0, 0, 0x80000);
                }
        };
        if (lim <= a.roll) {
            a.roll = int16_t(lim);
            if (u & 0x800000) tip([&](int32_t x) { return x < lo >> 1; });
            a.f20 &= ~0x800000u;
        }
        if (a.roll <= -lim) {
            a.roll = int16_t(-lim);
            if (a.f20 & 0x1000000) tip([&](int32_t x) { return hi >> 1 < x; });
            a.f20 &= ~0x1000000u;
        }
    }
    if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
    a.f20 &= 0xefae772fu, a.f24 &= 0xfb6fba04u, a.f28 &= 0xfff6b77au;
    moverTail(a, pos0, ang0, a.box, false);
}

// Class 5 0x4516e0 (pushable block). First tick: +0x24 0x8000, the tag-0x61 box (0x450440), box scaled by +0xec,
// +0x192 0x40 = no gravity (+0x20 0x200). +0x192 0x80 = fixed (+0x58 x / z cleared). Pushed (+0x24 0x10, the 0x453de0
// push block): first block +0x114; the carry +0x58 (×64) in the block's frame keeps only the axes +0x192 allows
// (0x24 / 0x28 local +z / -z, 0x12 / 0x11 +x / -x), back to world as the mover's carry (target 0, +0x194 = 0xf);
// the mover's displacement in the block frame keeps the axes its free mask +0x194 allows (0x20 = both z, 0x10 both x)
// and at most the asked carry, then moves the block. Its contacts get +0x20 0x40 (program at once with mask 0x40),
// the block 0x40 / 0x80 (players), which with its +0x14 run its program block. Then zones (flags0 0x1000000), the
// per-tick bits, a hung block's chain position, the box morph. Runtime AZCASG02 (pushpoke: local/wine/compare/w13/classes).
void TbWorld::blockTick(TbActor& a) {
    if (!a.inst) {
        a.inst = true;
        a.f24 |= 0x8000;
        if (!a.boxes.empty()) a.boxInstant = true, boxStep(a);  // 0x450440(a, 0)
        for (size_t i = 0; i < 6; ++i) a.box[i] = int16_t((int32_t(a.box[i]) * a.scale[i % 3]) >> 12);
        if (a.turnRate & 0x40) a.f20 |= 0x200;
    }
    const V3 pos0 = a.pos;
    const std::array<int16_t, 3> ang0{a.pitch, a.yaw, a.roll};
    const std::array<int16_t, 6> box0 = a.box;
    uint32_t hit = 0;
    if (a.turnRate & 0x80) {
        a.carry[0] = a.carry[2] = 0;
    } else if (a.f24 & 0x10) {
        if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
        Mat R;
        rotMatrixXYZ(R, a.pitch, a.yaw, a.roll);
        const Mat T = transpose(R);
        auto sh6 = [](const std::array<int32_t, 3>& v) { return V3{int16_t(v[0] << 6), int16_t(v[1] << 6), int16_t(v[2] << 6)}; };
        auto clampAxes = [](V3& l, uint32_t m) {  // bits 4 / 8 local +z / -z, 2 / 1 +x / -x
            if (!(m & 4) && l.z > 0) l.z = 0;
            if (!(m & 8) && l.z < 0) l.z = 0;
            if (!(m & 2) && l.x > 0) l.x = 0;
            if (!(m & 1) && l.x < 0) l.x = 0;
        };
        const uint16_t tr = uint16_t(a.turnRate);
        V3 want = apply16(T, sh6({a.carry[0], a.carry[1], a.carry[2]}));
        clampAxes(want, (tr & 0x24 ? 4u : 0u) | (tr & 0x28 ? 8u : 0u) | (tr & 0x12 ? 2u : 0u) | (tr & 0x11 ? 1u : 0u));
        V3 w = apply16(R, want);
        a.carry = {int16_t((int16_t(w.x) + 0x20) >> 6), int16_t((int16_t(w.y) + 0x20) >> 6), int16_t((int16_t(w.z) + 0x20) >> 6)};
        a.hover = 0xf;  // +0x194: the mover's free-axis mask
        std::vector<TbActor*> touched;
        const MoveOut mo = move(a, {}, touched, nullptr);
        V3 got = apply16(T, sh6(mo.disp));
        uint16_t m = tr;
        if (m & 0x20) m = uint8_t(m | 0xc);
        if (m & 0x10) m = uint8_t(m | 3);
        clampAxes(got, m & uint16_t(a.hover));
        for (int32_t V3::*ax : {&V3::x, &V3::z}) {
            const int32_t s = want.*ax;
            if ((s > 0 && s < got.*ax) || (s < 0 && got.*ax < s)) got.*ax = s;
        }
        w = apply16(R, got);
        a.pos = {int16_t(a.pos.x + int16_t((int16_t(w.x) + 0x20) >> 6)), int16_t(a.pos.y + int16_t((int16_t(w.y) + 0x20) >> 6)),
                 int16_t(a.pos.z + int16_t((int16_t(w.z) + 0x20) >> 6))};
        for (TbActor* c : touched) {
            a.contact = int32_t(c->uid), c->contact = int32_t(a.uid);
            const uint32_t b = (c->flags0 & 0x4000000) ? 0x80 : 0x40;
            hit |= b, a.f20 |= b;
            c->f20 |= 0x40;
            if (c->progMask[0] & 0x40) runProgram(*c, 0);
        }
    }
    if ((hit & a.progMask[0]) && a.progBlock) runBlock(a, a.progBlock);
    a.f20 &= 0xfffdff7fu;
    if (a.flags0 & 0x1000000) zoneTick(a);
    a.f20 &= 0x73ae7f2fu, a.f24 &= 0xff6fbb04u, a.f28 &= 0xffffb77au;
    if (TbActor* p = a.rideRec ? nullptr : entity(a.parent)) a.pos = hangPos(a, *p);
    moverTail(a, pos0, ang0, box0, false);
    if (a.boxMorph) boxStep(a);
}

// 0x465b80 (record `rec` of the record asset): flags0 0x20 entities write its texture word into their link's alias
// (0x465c80; no tag 0x0b link = the exe's alias 0, never drawn: not kept); others draw it, the box x / z ±(w - 1) / 2
// + +0x3e from the feet up h - 1.
void TbWorld::spriteShow(TbActor& a, uint16_t rec) {
    const auto it = a.spr.records >= 0 ? spriteAssets_.find(uint16_t(a.spr.records)) : spriteAssets_.end();
    if (it == spriteAssets_.end() || rec >= it->second.recs.size()) return;
    const auto& r = it->second.recs[rec];
    a.spr.shown = rec;
    if (a.flags0 & 0x20) {
        if (a.texLink >= 0) textureLinks[uint16_t(a.texLink)] = r[1];
        return;
    }
    const int32_t w = int32_t(r[2]) - 1, h = int16_t(r[3] - 1);
    const int16_t lo = int16_t((-w >> 1) - a.inflate), hi = int16_t((w >> 1) + a.inflate);
    a.box = {lo, 0, lo, hi, int16_t(-h), hi};
}

// 0x465dc0 sprite-sequence player (class 2, and 0x469770 for flags0 0x200000). Mode 0 continues (0x10 held: running;
// 0x100 ended: 0x100, or 0x200 with 0x800). Mode 0x20 ends it, 0x10 holds, 4 releases a hold; mode & 0xb starts the
// flipbook of the linked asset whose class is `anim` (none: ended, 0x400): step 0's record shown, flags = mode | 0x1200,
// hold = its ticks. A tick whose countdown is 0 shows the step's record; on the last step it sets 0x2000 and loops
// (0x1000, step 0) or with mode 2 ends (0x100); the asset's blocks run; then the next step. Records: the entity's first
// class-0x17 link. Runtime ARCA01 class 2 (local/wine/compare/w13/classes/arca01_links).
uint32_t TbWorld::spriteSeq(TbActor& a, uint16_t anim, uint16_t mode) {
    TbActor::SpriteSeq& s = a.spr;
    if (!mode) {
        if (s.flags & 0x10) return 0x200;
        if (s.flags & 0x100) return s.flags & 0x800 ? 0x200 : 0x100;
    } else {
        if (mode & 0x20) { s.flags = 0x100; return 0x20; }
        if ((mode & 4) && (s.flags & 0x10)) s.flags &= ~0x10;
        else if (mode & 0x10) { s.flags |= 0x10; return 0x200; }
    }
    s.flags &= 0xcfff;
    if (!a.inst) {  // the instance: records = the first class-0x17 link
        a.inst = true;
        for (uint16_t l : a.links)
            if (const auto it = spriteAssets_.find(l); it != spriteAssets_.end() && it->second.role == 0x17) { s.records = l; break; }
    }
    s.flags &= 0xceff;
    const SpriteAsset* fb = s.asset >= 0 ? &spriteAssets_[uint16_t(s.asset)] : nullptr;
    if (mode & 0xb) {
        if (!(mode & 8)) {
            fb = nullptr;
            for (uint16_t l : a.links)
                if (const auto it = spriteAssets_.find(l); it != spriteAssets_.end() && it->second.role == anim) { fb = &it->second, s.asset = l; break; }
        }
        if (!fb || fb->steps.empty()) { s.flags = 0x100; return 0x400; }
        const auto rs = s.records >= 0 ? spriteAssets_.find(uint16_t(s.records)) : spriteAssets_.end();
        if ((a.flags0 & 0x200000) && rs != spriteAssets_.end() && !rs->second.recs.empty()) {  // box from record 0
            const auto& r0 = rs->second.recs[0];
            const int16_t half = int16_t(a.inflate + (2 - ((r0[0] & 0x180) >> 7) == 2 ? r0[2] >> 2 : r0[2] >> 1));
            a.box = {int16_t(-half), 0, int16_t(-half), half, int16_t(-int16_t(r0[3])), half};
            a.radius = uint16_t(half + r0[2] * 2);
        }
        a.curAnim = anim;
        s.frame = 0, s.flags = uint16_t(mode | 0x1200);
        s.last = uint16_t(fb->steps.size() - 1);
        s.reload = s.countdown = fb->steps[0][1];
        spriteShow(a, fb->steps[0][0]);
    }
    if (!fb || fb->steps.empty()) return 0x200;
    bool step = false;
    if (!s.countdown) {
        spriteShow(a, fb->steps[std::min<size_t>(s.frame, fb->steps.size() - 1)][0]);
        if (s.frame == s.last) {
            s.flags |= 0x2000;
            if (s.flags & 2) {
                s.flags = uint16_t((s.flags & 0xfdff) | 0x100);
                return s.flags & 0x800 ? 0x200 : 0x100;
            }
            s.frame = 0, s.reload = s.countdown = fb->steps[0][1];
            s.flags |= 0x1000;
        }
        for (TbBlock& b : spriteAssets_[uint16_t(s.asset)].logic) runItems(a, b);
        step = !s.countdown;
    }
    if (step && size_t(s.frame) + 1 < fb->steps.size()) ++s.frame, s.reload = s.countdown = fb->steps[s.frame][1];
    --s.countdown;
    return 0x200;
}

// Class 2 0x4599e0 (texture-link writers): first tick flags0 0x20 (not 0x200000), +0x24 0x8000, level-start event 2;
// a state with logic blocks runs them (A, then B unless A gave an event) and takes the event; then the state's anim id
// selects the flipbook (0x465dc0, mode on a change); its end runs flagsA 4 (removed) or event 0x20000000.
void TbWorld::linkTick(TbActor& a) {
    if (!a.inst) {
        a.flags0 = (a.flags0 & ~0x200000u) | 0x20;
        a.f24 |= 0x8000;
        dispatch(a, 2);
    }
    const TbStateRec* r = a.rec();
    if (!r) return;
    if (r->logicA || r->logicB) {
        uint32_t ev = 0x80000000;
        if (r->logicA) ev = runBlock(a, r->logicA);
        if ((ev & 0x90000000) && r->logicB) ev = runBlock(a, r->logicB);
        if (!(ev & 0x90000000)) dispatch(a, ev), r = a.rec();
    }
    const bool same = a.curAnim == r->anim;
    if (spriteSeq(a, same ? 0 : r->anim, same ? 0 : r->mode) == 0x100) {
        a.curAnim = 0;
        if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
        dispatch(a, 0x20000000);
    }
}

// Class 4 0x453b40 (sprite flipbook over the first class-0x17 link's records, 2 ticks each, looped; 0x14e = 0x2000 on
// the wrap). First tick: +0x24 0x8000, box from record 0 (x / z ±w / 2, top -h; flags1 0x4000000: lifted h / 2),
// record 0 shown. Then first block +0x114. Runtime VIQ02 templates 74 / 75 (local/wine/compare/w13/classes/viq02_c4).
// The look view also hides an entity hung (through any parents, +0x90) on the focus: +0x24 |= 4 (0x453b40, 0x453de0).
void TbWorld::hideCarried(TbActor& a) {
    for (const TbActor* p = entity(a.parent); p; p = entity(p->parent))
        if (p == &actors[size_t(focus())]) { a.f24 |= 4; return; }
}

void TbWorld::spriteTick(TbActor& a) {
    if (lookMode_) hideCarried(a);
    TbActor::SpriteSeq& s = a.spr;
    if (!a.inst) {
        a.inst = true;
        a.f24 |= 0x8000;
        for (uint16_t l : a.links)
            if (const auto it = spriteAssets_.find(l); it != spriteAssets_.end() && it->second.role == 0x17) { s.records = l; break; }
        const SpriteAsset* rs = s.records >= 0 ? &spriteAssets_[uint16_t(s.records)] : nullptr;
        if (rs && !rs->recs.empty()) {
            const uint16_t w = rs->recs[0][2], h = rs->recs[0][3];
            s.frame = 0, s.last = uint16_t(rs->recs.size() - 1), s.reload = s.countdown = 2;
            a.box = {int16_t(-(w >> 1)), 0, int16_t(-(w >> 1)), int16_t(w >> 1), int16_t(-int16_t(h)), int16_t(w >> 1)};
            if (a.flags1 & 0x4000000) a.pos.y = int16_t(a.pos.y + (h >> 1));
            a.radius = uint16_t(w >> 1);
            spriteShow(a, 0);
        }
    }
    if (!s.countdown) {
        ++s.frame;
        s.countdown = s.reload;
        if (s.last < s.frame) s.frame = 0, s.flags = 0x2000;
        spriteShow(a, s.frame);
    }
    --s.countdown;
    if (a.firstBlock >= 0) runBlock(a, uint16_t(a.firstBlock));
}

// Class 33 0x459b10 (an entity hung on a part, e.g. id 72 on Taz): first tick the tag-0x61 box, +0x24 0x8000, event
// 2; the state's anim (0x469770; its end: flagsA 4 removes, else event 0x20000000); hung on +0xe8 it takes the
// record chain's position and heading (pitch / roll 0); no parent removes it; flags1 0x10000000 keeps its own record
// yaw at minus the parent's (world heading fixed). flags0 0x900000: +0x58 = the tick's displacement. Runtime ARCASG03
// (local/wine/compare/w13/classes/arcasg03_c33).
void TbWorld::hungTick(TbActor& a) {
    if (!a.inst) {
        a.inst = true;
        if (!a.boxes.empty()) a.boxInstant = true, boxStep(a);
        a.f24 |= 0x8000;
        dispatch(a, 2);
    }
    const V3 pos0 = a.pos;
    const std::array<int16_t, 3> ang0{a.pitch, a.yaw, a.roll};
    const std::array<int16_t, 6> box0 = a.box;
    const TbStateRec* r = a.rec();
    if (!r) return;
    if (a.flags0 & 0x200000) {
        const bool same = a.curAnim == r->anim;
        if (spriteSeq(a, same ? 0 : r->anim, same ? 0 : r->mode) == AnimFinished && same) {
            a.curAnim = 0;
            if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
            dispatch(a, 0x20000000);
        }
    } else if (a.model) {
        if (a.curAnim == r->anim) {
            if (animTick(*a.model, a.anim, 0, 0) == AnimFinished) {
                a.curAnim = 0;
                if (r->flagsA & 4) { a.f20 |= 0x2000; return; }
                dispatch(a, 0x20000000);
            }
        } else if (findAnim(*a.model, r->anim) >= 0) {
            animTick(*a.model, a.anim, r->anim, r->mode);
            a.curAnim = r->anim;
        } else {
            animTick(*a.model, a.anim, 0, 0);
        }
    }
    TbActor* par = entity(a.parent);
    if (par && !a.rideRec) {
        int16_t yaw = 0;
        a.pos = hangPos(a, *par, &yaw);
        a.yaw = yaw, a.pitch = a.roll = 0;
    }
    if (!par) a.f20 |= 0x2000;
    else if (a.flags1 & 0x10000000) a.localYaw = int16_t(-(par->parent >= 0 && !par->rideRec ? par->localYaw : par->yaw));
    if (a.boxMorph) boxStep(a);
    if (a.flags0 & 0x900000) a.carry = {int16_t(a.pos.x - pos0.x), int16_t(a.pos.y - pos0.y), int16_t(a.pos.z - pos0.z)};
    moverTail(a, pos0, ang0, box0, false);
}

// Class 30 0x44d1f0 (template 151, spawned by Bugs state 237's '^' in the look view). First tick (no model instance
// yet): +0x24 0x8000, always listed (+0x6e 0x40), +0x82 = 0; a focus spawn (flags1 0x20000) in the look view with
// GD+0xa2 4 leaves from the look point (B+0x2c) along the look angles (0x4d5970 / 0x4d5972); otherwise +0x38 = 0
// mirrors its pitch, else it pitches at the entity +0x38 selects (its box half-height). Template 0x16 spawns there
// with its angles; the velocity is (0, 0, +0x3e) through its record matrix (s16, not x16). Every tick: older than 60
// ticks it is removed; the ray pos -> +0x70 (0x4463e0, q (flags10 >> 24 & 0xf0) | 0x101, masks flags0 0x100; the
// look-view shot does not hit the focus) shortens the step. A hittable entity hit (flags0 0x180) takes +0x54 both
// ways and +0x20 0x40, or with a vulnerable state (flagsB 8) the shot's +0x3c as damage, +0x5e = its +0x192 and
// 0x4000800; template +0x3a spawns, the victim's program runs for its 0x4000040 bits, the shot is removed. Else it
// moves by the step (zones with flags0 0x1000000); any other hit spawns +0x3a and removes it, no hit adds +0x42 to the
// vertical speed.
void TbWorld::projTick(TbActor& a) {
    const bool snipe = (a.flags1 & 0x20000) && lookMode_ && (GD[0xa2] & 4);
    if (!a.inst) {
        a.inst = true;
        a.f24 |= 0x8000;
        a.view |= 0x40;
        a.stateTicks = 0;
        if (snipe) {
            a.pitch = lookPitch_, a.yaw = lookYaw_, a.roll = 0;
            a.pos = cam.body;
        } else if (!a.f38) {
            a.pitch = int16_t(-a.pitch);
        } else if (const TbActor* t = lookup(uint32_t(int32_t(a.f38)), a)) {
            const int32_t dx = t->pos.x - a.pos.x, dy = (t->box[4] >> 1) - a.pos.y + t->pos.y, dz = t->pos.z - a.pos.z;
            a.roll = 0;
            a.pitch = int16_t(-ratan2(dy, isqrt(dx * dx + dz * dz)));
        }
        if (TbActor* e = spawn(0x16, a.pos, a.yaw)) e->pitch = a.pitch, e->roll = a.roll;
        const V3 v = apply(recordMat(a), {0, 0, a.inflate});
        a.vel = {int16_t(v.x), int16_t(v.y), int16_t(v.z)};
    }
    if (a.stateTicks > 60) {
        a.f20 |= 0x2000;
        return;
    }
    V3 d{int16_t(a.vel[0]), int16_t(a.vel[1]), int16_t(a.vel[2])};
    const uint32_t q = ((a.flags10 >> 24) & 0xf0) | 0x101;
    const auto hit = snipe ? rayHit(a, a.pos, d, 5, q, {0x100, 0, 0, 0, 0, 0}, true, {&actors[size_t(focus())]})
                           : rayHit(a, a.pos, d, 5, q, {0x100, 0, 0, 0, 0, 0});
    auto impact = [&] {
        if (a.f3a)
            if (TbActor* e = spawn(uint16_t(a.f3a), a.pos, a.yaw)) e->pitch = a.pitch, e->roll = a.roll;
        a.f20 |= 0x2000;
    };
    if (hit && *hit && ((*hit)->flags0 & 0x180)) {
        TbActor& c = **hit;
        a.contact = int32_t(c.uid), c.contact = int32_t(a.uid);
        if (c.rec() && (c.rec()->flagsB & 8)) c.dmgIn = a.hp, c.hitTr = a.turnRate, c.f20 |= 0x4000800;
        else c.f20 |= 0x40;
        impact();
        if (const uint32_t m = c.f20 & c.progMask[0] & 0x4000040) runProgram(c, m);
        return;
    }
    a.pos = {int16_t(a.pos.x + d.x), int16_t(a.pos.y + d.y), int16_t(a.pos.z + d.z)};
    if (a.flags0 & 0x1000000) zoneTick(a);
    if (hit) return impact();
    a.vel[1] = int16_t(a.vel[1] + a.mass);
}

// 0x447fd0: listed entities with flags0 & maskIn whose box (in their own frame, yaw only) lies within 125 of p.
std::vector<TbActor*> TbWorld::probe(TbActor& a, const V3& p, uint32_t maskIn, size_t max) {
    std::vector<TbActor*> out;
    for (TbActor* ep : order()) {
        TbActor& e = *ep;
        if (out.size() >= max) break;
        if (!e.active || !(e.flags0 & maskIn) || (e.f20 & 0x2000) || &e == &a) continue;
        if (e.parent == int32_t(a.uid) || a.parent == int32_t(e.uid)) continue;
        const int32_t R = 125 + e.radius;
        const int32_t dx = p.x - e.pos.x, dy = p.y - e.pos.y, dz = p.z - e.pos.z;
        if (std::abs(dx) > R || std::abs(dy) > R || std::abs(dz) > R) continue;
        if (int64_t(dx) * dx + int64_t(dy) * dy + int64_t(dz) * dz > int64_t(R) * R) continue;
        if (e.box == std::array<int16_t, 6>{}) continue;
        const V3 l = rotY(-e.yaw, dx, dy, dz);
        auto outside = [](int32_t v, int32_t lo, int32_t hi) { return v > hi ? v - hi : lo - v; };
        if (outside(l.x, e.box[0], e.box[3]) < 0x7e && outside(l.y, e.box[4], e.box[1]) < 0x7e &&
            outside(l.z, e.box[2], e.box[5]) < 0x7e)
            out.push_back(&e);
    }
    return out;
}

// The 0x453de0 "ahead" point: 100 up and 100 past the box front (+0x6c) in the entity's frame (0x404160 / 0x404800).
V3 TbWorld::aheadPoint(const TbActor& a) {
    Mat R;
    rotMatrixXYZ(R, a.pitch, a.yaw, a.roll);
    const V3 o = apply(R, {0, -100, int16_t(a.box[5] + 100)});
    return {int16_t(a.pos.x + o.x), int16_t(a.pos.y + o.y), int16_t(a.pos.z + o.z)};
}

namespace {
// GTE IR0 scale (0x401f90 + 0x402da0): (v · k) >> 12 per component, saturated to s16.
V3 gteScale(const V3& v, int32_t k) {
    auto s = [k](int32_t c) { return std::clamp(int32_t(c * k) >> 12, -0x8000, 0x7fff); };
    return {s(v.x), s(v.y), s(v.z)};
}
uint32_t carryQuery(const TbActor& e) { return ((e.flags10 >> 24) & 0xf0) | 0x101; }
} // namespace

// 0x44e7d0: of the carriable entities (flags0 0x2802; Bugs too while the state has flagsC 0x100000) the probe 0x447fd0
// finds at p (at most 5), the lowest (largest y); unless the state has flagsC 0x40000 it must be in sight: the ray
// from a's centre column (y clamped into a's box) to c's box middle (0x4463e0, q (flags10 >> 24 & 0xf0) | 0x101, masks
// flags0 0x100, c not collidable for it) hits nothing.
TbActor* TbWorld::pickAhead(TbActor& a, const V3& p) {
    const uint32_t bugs0 = actors[0].flags0;
    if (a.rec()->flagsC & 0x100000) actors[0].flags0 |= 0x2000;
    TbActor* c = nullptr;
    for (TbActor* e : probe(a, p, 0x2802, 5))
        if (!c || c->pos.y < e->pos.y) c = e;
    if (c && !(a.rec()->flagsC & 0x40000)) {
        const int32_t mid = ((c->box[4] - c->box[1]) >> 1) + c->box[1];
        const int32_t cy = std::max<int32_t>(std::min<int32_t>(c->pos.y - a.pos.y + mid, a.box[1]), a.box[4]);
        const V3 s{a.pos.x, int16_t(a.pos.y + cy), a.pos.z};
        V3 d{int16_t(c->pos.x - a.pos.x), int16_t(c->pos.y - a.pos.y + mid - cy), int16_t(c->pos.z - a.pos.z)};
        if (rayHit(a, s, d, 5, carryQuery(a), {0x100, 0, 0, 0, 0, 0}, true, {c})) c = nullptr;
    }
    actors[0].flags0 = bugs0;
    return c;
}

// 0x450650: a let-go child at its world position is pulled back toward the parent's centre column when the ray from
// there to it, lengthened by r = clamp(radius / 2, 40, 80), hits something (no self; both not collidable): it ends r
// short of the hit (at the column when nearer than r). Returns false (child unmoved) when nothing is hit.
bool TbWorld::dropClamp(TbActor& p, TbActor& c) {
    const int32_t dy = int16_t(c.pos.y - p.pos.y);
    const int32_t cy = std::max<int32_t>(std::min<int32_t>(dy, p.box[1]), p.box[4]);
    const V3 s{p.pos.x, int16_t(p.pos.y + cy), p.pos.z};
    V3 d{int16_t(c.pos.x - p.pos.x), int16_t(dy - cy), int16_t(c.pos.z - p.pos.z)};
    const int32_t r = std::clamp<int32_t>(c.radius >> 1, 0x28, 0x50);
    int32_t len = isqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    len += len == 0;
    d = gteScale(d, ((r + len) * 0x1000) / len);
    if (!rayHit(p, s, d, 5, carryQuery(c), {0x100, 0, 0, 0, 0, 0}, false, {&p, &c})) return false;
    len = isqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < r) d = {};
    else len += len == 0, d = gteScale(d, ((len - r) * 0x1000) / len);
    c.pos = {int16_t(s.x + d.x), int16_t(s.y + d.y), int16_t(s.z + d.z)};
    return true;
}

// 0x44e6d0: every child of c (theirs first) is thrown with it: +0x20 0x10000 (0x800a cleared), the thrower's yaw,
// detached, clamped (0x450650 from the thrower), its program for 0x10000.
void TbWorld::releaseNested(TbActor& c, TbActor& thrower) {
    std::vector<TbActor*> kids;
    forEachEntity([&](TbActor& g) { if (g.parent == int32_t(c.uid)) kids.push_back(&g); });
    for (TbActor* g : kids) {
        releaseNested(*g, thrower);
        g->f20 = (g->f20 & 0xffff7df5u) | 0x10000;
        g->yaw = thrower.yaw;
        detach(*g);
        dropClamp(thrower, *g);
        if (g->progMask[0] & 0x10000) runProgram(*g, 0x10000);
    }
}

// 0x453de0 release block (states with flagsA 0x40 throw, flagsB 0x40000 put down; an attach part set): the child
// hung on that part loses +0x20 0x8202 and gets 0x10000 (thrown; its own children follow, 0x44e6d0) or 0x400000
// (put down), is detached, clamped (0x450650), takes the zone and yaw, and runs its program for those bits.
void TbWorld::carryRelease(TbActor& a) {
    const TbStateRec& r = *a.rec();
    if (!((r.flagsA & 0x40) || (r.flagsB & 0x40000)) || a.attachPart < 0) return;
    TbActor* c = nullptr;
    forEachEntity([&](TbActor& e) { if (!c && e.parent == int32_t(a.uid) && !e.rideRec && e.parentPart == a.attachPart) c = &e; });
    if (!c) return;
    c->f20 = (c->f20 & 0xffff7dfdu) | ((r.flagsB & 0x40000) ? 0x400000u : 0x10000u);
    if (c->f20 & 0x10000) releaseNested(*c, a);
    detach(*c);
    dropClamp(a, *c);
    c->zone = a.zone;
    c->yaw = a.yaw;
    if (c->progMask[0] & c->f20 & 0x410000) runProgram(*c, 0x410000);
}

// 0x453de0 grab block (after the anim and logic; state flagsA 0x20, an attach part, the frame hold +0x153 at 0): the
// entity 0x44e7d0 finds ahead (flagsC 0x80000: at the attach point +0x7c) leaves its parent, is hung on the attach
// part at local 0 (+0xe8, +0xe4 = its +0x12c record), +0x20 0x8000 (0x41000a cleared), +0x24 0x80 cleared, its
// mass joins (0x44d910), its program runs for 0x8000; +0x54 cleared. Its world position follows on its own tick.
void TbWorld::carryGrab(TbActor& a) {
    const TbStateRec& r = *a.rec();
    if (!(r.flagsA & 0x20) || a.attachPart < 0 || a.anim.countdown) return;
    TbActor* c = pickAhead(a, (r.flagsC & 0x80000) ? a.attachPos : aheadPoint(a));
    if (!c) return;
    if (c->parent >= 0) detach(*c);
    a.contact = -1;
    c->f20 = (c->f20 & 0xffbefff5u) | 0x8000;
    c->f24 &= ~0x80u;
    c->local = {}, c->localYaw = 0;
    link(a, *c);
    c->parentPart = a.attachPart;
    if (c->progMask[0] & 0x8000) runProgram(*c, 0x8000);
}

// 0x453de0 attack hits: states with flagsA 0x40000/0x80000 (vs players / others) hit the contact list (0x1000000
// with the mover) and/or everything a probe at the TOD 0xa00 point finds (0x200000, or without 0x1000000). Each hit
// writes the state's damage and hit transition into the victim (+0x66, +0x5e) and +0x24 0x100000 / 2 (|1 lethal).
void TbWorld::attackHits(TbActor& a, const std::vector<TbActor*>& touched) {
    const TbStateRec& r = *a.rec();
    if (r.flagsA & 0x1040000) a.f20 |= 0x1000; else a.f20 &= ~0x1000u;
    if (!(r.flagsA & 0xc0000) || (a.f20 & 0x410000) || (a.f24 & 0x80)) return;
    std::vector<TbActor*> t;
    if ((r.flagsA & 0x1000000) && (r.flagsA & 0x800000)) t = touched;
    if (((r.flagsA & 0x200000) || !(r.flagsA & 0x1000000)) && a.attachPart >= 0 && t.size() < 10) {
        const std::vector<TbActor*> more = probe(a, a.attachPos, 0x80, 10 - t.size());
        t.insert(t.end(), more.begin(), more.end());
    }
    if (r.flagsA & 0x40000000) unsupported.insert("attack part probe 0x448240");
    const bool aPl = a.flags0 & 0x4000000;
    const TbActor* par = entity(a.parent);
    const bool free = !par || (par->flags0 & 0x1000c) || (par->rec() && (par->rec()->flagsA & 0x20000) && par->attachPart >= 0);
    for (TbActor* cp : t) {
        TbActor& c = *cp;
        const bool cPl = c.flags0 & 0x4000000;
        if (!(c.flags0 & 0x80) || (c.f20 & 0xe000000) || !c.rec() || !free) continue;
        if (aPl ? !(c.rec()->flagsA & 0x10000) : !(c.rec()->flagsB & 8)) continue;
        if (cPl ? !(r.flagsA & 0x40000) : !(r.flagsA & 0x80000)) continue;
        a.contact = int32_t(c.uid), c.contact = int32_t(a.uid);
        c.dmgIn = r.dmg;
        c.f20 |= aPl ? 0x8000000 : 0x4000000;
        if (aPl && a.id == 3) c.f28 |= 0x20000;
        const uint32_t b = cPl ? (c.hp <= c.dmgIn ? 3u : 2u) : 0x100000u;
        a.f24 |= b, a.trig[1] |= b;
        c.hitTr = r.hitTrans;
    }
}

// 0x44ea50 (dispatcher, dead player on a plain event): with the partner present the dead one leaves the game
// (GD+0xa2 1/2) and control moves on; both down: a life (GD+0xa0) brings both back with 6 health at the checkpoint
// 0x4d26e0 (the focus arrives there and calls the partner, forced); no life left: level 5 (RETRY), 3 lives.
void TbWorld::playerDown(TbActor& a) {
    // An absent player (GD+0xa2 1/2: already out after its own death) keeps getting plain events from its idle
    // logic; the runtime capture shows lives untouched then (D: GD+0xa0 3, 0xa2 1), so only the first call counts (I).
    if (GD[0xa2] & (a.id == 2 ? 1 : 2)) return;
    a.vel = {};
    TbActor& f = actors[size_t(focus())];
    if (!(GD[0xa2] & 3)) {
        if (&a == &f) GD[0xa4] ^= 4;
        GD[0xa2] = a.id == 2 ? uint8_t((GD[0xa2] | 1) & ~0x10) : uint8_t((GD[0xa2] | 2) & ~0x20);
        return;
    }
    f.vel = {};
    GD[0xa2] &= 0xcc;
    for (TbActor& p : actors) p.f20 &= ~0x2000000u, p.hp = 6;
    if (!GD[0xa0]) {
        restart = true, nextLevel = 5;  // GD+0 = 5 (RETRY), 0x4d593a = 1
        GD[0xa5] = GD[0xa6] = 6, GD[0xa0] = 3;
        return;
    }
    --GD[0xa0];
    teleport(f, {checkpoint[0], checkpoint[1], checkpoint[2]});
    f.yaw = lookYaw_;  // checkpoint yaw +0x4d26ec is overwritten by 0x4d5972
    f.attachPart = -1;
    f.f20 |= 0x10000000;
    f.f24 &= ~0x40u;
    if (f.progMask[0] & 0x10000000) runProgram(f, 0x10000000);
    callFriend(true);
}

// V 8 / V 0x80000010: block p1 (entity list < 10000, else global) on every listed entity with flags0 0x8000
// within radius of the caller; its event is dispatched there.
void TbWorld::runNear(TbActor& a, uint32_t block, uint32_t radius) {
    std::vector<TbActor*> near;
    for (TbActor* e : order())
        if (e->active && (e->flags0 & 0x8000) && dist2(e->pos, a.pos) <= int64_t(radius) * radius) near.push_back(e);
    for (TbActor* e : near)
        if (TbBlock* b = findBlock(*e, uint16_t(block))) {
            const uint32_t r = runItems(*e, *b);
            if (!(r & 0x90000000)) dispatch(*e, r);
        }
}

// V 4 / 0x80000001 / 0x100000 placement: detached, not grounded, sleep counter cleared.
void TbWorld::teleport(TbActor& t, V3 p) {
    if (t.parent >= 0) {
        t.f20 &= ~0x208u;
        detach(t);
    }
    t.groundPoly = -1;
    t.f20 &= ~2u;
    t.pos = p;
    t.sleep = 0;
}

}  // namespace bb::game
