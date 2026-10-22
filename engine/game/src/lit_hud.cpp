// LiT HUD actor 0x44acc0 (type 6, created by 0x448bd0 with the 47 element slots 0x4b23a0), its helpers
// 0x449c00 / 0x44a9a0 / 0x44ab70 / 0x44a710 / 0x44a800 / 0x44a580, the screen fade 0x44be90 / 0x44bec0 / 0x44bef0
// and the level table's loading screens (0x430e60). Docs: GAMEPLAY.md §3.6 / §1.2, RUNTIME_REFERENCE.md.
#include "bb/game/lit_world.h"

#include <algorithm>
#include <initializer_list>

namespace bb::game {
namespace {

constexpr uint16_t kDigit[10] = {472, 473, 474, 475, 476, 477, 478, 479, 480, 481};  // 0x4ae230 template ids
// 0x4ae248: per slot icon template (counters: icon at p; health p / p+1 / p+2 = full / half / empty carrot;
// timer p..p+2 = stopwatch hands and face; boss p..p+2; slots 26-28 inventory items).
constexpr uint16_t kIcon[32] = {486, 0,   0,   485, 0,   0,   0,   487, 0,   0,   0,   488, 493, 492, 489, 490,
                                491, 495, 496, 494, 924, 0,   0,   0,   925, 923, 961, 483, 962, 0,   0,   0};
// 0x4ae288: element origin per slot, 512x240 screen space centred; 48-55 = pause-screen golden / clock totals (0x449d80).
constexpr int16_t kPos[56][2] = {
    {-228, -80}, {-200, -80}, {-176, -80}, {180, -80}, {204, -80}, {228, -80}, {228, -80}, {160, 90}, {204, 90},
    {228, 90},   {228, 90},   {-220, 90},  {-64, 89},  {-16, 90},  {-32, -80}, {0, -80},   {32, -80}, {-220, 90},
    {-220, 90},  {-220, 90},  {-224, 82},  {-200, 82}, {-176, 82}, {-152, 82}, {0, 0},     {0, 0},    {0, 95},
    {0, 90},     {0, 90},     {-96, 58},   {-28, 58},  {-4, 58},   {20, 58},   {44, 58},   {68, 58},  {-96, 30},
    {-28, 30},   {-4, 30},    {20, 30},    {44, 30},   {68, 30},   {-96, 86},  {-28, 86},  {-4, 86},  {20, 86},
    {44, 86},    {68, 86},    {0, 0},      {156, -80}, {180, -80}, {204, -80}, {228, -80}, {136, 80}, {180, 80},
    {204, 80},   {228, 80}};
// Exe level table 0x4ad380 +4: loading-screen level id per level id (0 = none).
constexpr uint8_t kLoading[111] = {
    1,   0,  1,  107, 106, 106, 103, 105, 105, 94, 94, 94, 94, 94, 94, 97, 97, 96, 96, 96, 96, 99,  99,  99,
    98,  101, 101, 101, 102, 81, 81, 81, 83, 83, 83, 82, 82, 82, 82, 82, 85, 85, 92, 92, 91, 91, 91, 91,
    92,  90, 93, 107, 88, 87, 86, 0,  1,  106, 94, 97, 96, 99, 98, 101, 102, 81, 83, 82, 85, 92, 107, 0,
    108, 94, 101, 81, 93, 87, 78, 79, 80, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  109, 110};

}  // namespace

int LitWorld::loadingScreen(int target, uint8_t world) {
    if (target < 0 || size_t(target) >= std::size(kLoading) || !kLoading[target]) return 0;
    if (target != 3) return kLoading[target];
    switch (world) {  // LS01 hub: the time period's loading screen
    case 1: return 0x68;  // L_SALOAD
    case 2: return 0x5f;  // L_MLOAD
    case 3: return 0x64;  // L_PLOAD
    case 4: return 0x54;  // L_30LOAD
    case 5: return 0x59;  // L_FLOAD
    default: return 1;    // LOADING
    }
}

bool LitWorld::holdsLoading(int t) {
    static constexpr int kIds[] = {2, 0x45, 0x31, 0x32, 0x2c, 0x41, 0x42, 0x43, 0x28, 0x3d, 0x3e, 0x3f,
                                   0x40, 0x3a, 0x3b, 0x3c, 0x39, 6, 7, 0x36, 0x35, 0x34, 0x46};
    return std::ranges::contains(kIds, t);
}

void LitWorld::fadeTick() {  // 0x44bef0 (also inlined at the top of 0x44c4b0)
    Fade& f = fade;
    if (!f.mode) return;
    if (f.mode & 4) {
        f.mode = 1, f.speed = 0x100, f.level = 0, f.dark = 0x1000;
    } else if (f.mode & 8) {
        f.mode = 2, f.speed = 0x100, f.level = 0x1000, f.dark = 0;
    } else if (S[0x19] == 0) {  // 0x4b2279
        if (f.mode & 2) {
            if (f.level <= 0) {
                f.level = 0, f.dark = 0x1000, f.mode = 0x20;
            } else {
                f.dark = int16_t(0x1000 - f.level);
                f.level = int16_t(f.level - f.speed);
            }
        } else if (f.mode & 1) {
            if (f.level >= 0x1000) {
                f.level = 0x1000, f.dark = 0;
                f.mode = f.mode & 0x40 ? uint8_t((f.mode & 0xfe) | 2) : 0x10;  // a flash fades back
            } else {
                f.dark = int16_t(0x1000 - f.level);
                f.level = int16_t(f.level + f.speed);
            }
        }
    }
    if (f.mode & 0x30) f.mode = 0, f.dark = 0;  // done: the frame is drawn at full brightness again
}

std::array<uint8_t, 4> LitWorld::fadeOverlay() const {
    if (!fade.mode) return {};
    const int lit = 0x1000 - fade.dark;
    if (fade.mode & 0x40) {
        if (!lit) return {};
        return {uint8_t(fade.rgb[0] * lit >> 12), uint8_t(fade.rgb[1] * lit >> 12), uint8_t(fade.rgb[2] * lit >> 12),
                uint8_t(std::min(255, lit * 255 / 4096))};
    }
    return {0, 0, 0, uint8_t(255 - std::min(255, lit >> 4))};
}

void LitWorld::hudShow(size_t slot, uint16_t tmpl, size_t pos, bool instant) {
    // Copies the template actor (the template list 0x4b2390; none -> nothing happens) and gives it a fresh identity part.
    if (!tmpl || std::ranges::none_of(defs, [&](const ActorDef& d) { return d.chunk == 8 && d.entityId == tmpl; })) return;
    HudItem& h = hud[slot];
    h = {.tmpl = tmpl, .shown = true, .x = kPos[pos][0], .y = kPos[pos][1]};
    if (instant) h.state = 0, h.hold = 0x3c, h.z = 0x4b0;
    else h.state = 1, h.hold = 0, h.z = 8000;
}

bool LitWorld::hudCounter(size_t p, uint16_t v, bool instant) {
    HudItem &icon = hud[p], &tens = hud[p + 1], &units = hud[p + 2];
    const uint16_t t = kDigit[v % 100 / 10], u = kDigit[v % 10];
    if (!icon.shown) hudShow(p, kIcon[p], p, instant);
    else if (icon.state == 0 && icon.hold == 0) icon.state = 1;
    if (!tens.shown) {
        hudShow(p + 1, t, p + 1, instant);
    } else if (tens.state == 2 || tens.tmpl == t || units.z > 0x4b0) {
        if (tens.state == 0 && tens.hold == 0) tens.state = 1;
    } else {  // tens digit changes once the units digit is in place: swap it
        tens.hold = 0, tens.state = 2, tens.z = units.z;
    }
    if (!units.shown) {
        hudShow(p + 2, u, p + 2, instant);
        return true;
    }
    if (units.tmpl != u) units.state = 2, units.hold = 0;
    else if (units.state == 0 && units.hold == 0) units.state = 1;
    return false;
}

void LitWorld::hudAnim(size_t p, int32_t speed) {
    HudItem* e[3] = {&hud[p], &hud[p + 1], &hud[p + 2]};
    if (!e[0]->shown) return;
    if (e[1]->state == 0) {
        if (e[2]->state == 0) e[0]->hold = e[1]->hold;
        else e[0]->hold = e[1]->hold = 0x29;
    } else if (e[0]->state == 0 && e[1]->state == 1) {
        e[0]->hold = 0x29;
    }
    for (HudItem* h : e) {
        if (h->hold) {
            --h->hold;
        } else if (h->state == 0) {  // fly out
            h->z += (h->z >> 2) + 200;
            if (h->z > 8000) h->shown = false;
        } else if (h->state == 1) {  // fly in
            h->z = h->z - (h->z >> 2) - speed;
            if (h->z < 0x4b0) h->state = 0, h->hold = 0x28, h->z = 0x4b0;
        } else if (h->state == 2) {
            h->shown = false, h->z = 0;
        }
    }
}

void LitWorld::hudHealth(size_t p, bool instant) {
    const int shown = hudState_.health;
    for (int i = 0; i < G[2] && i / 2 < 3; i += 2)  // one carrot per two half-carrots (three slots: p, p+1, p+2)
        hudShow(p + size_t(i / 2), shown >= i + 2 ? kIcon[p] : shown >= i + 1 ? kIcon[p + 1] : kIcon[p + 2],
                p + size_t(i / 2), instant);
}

void LitWorld::hudTimer(size_t p, int32_t t, bool hide) {
    if (hide) {
        hud[p].shown = hud[p + 1].shown = hud[p + 2].shown = false;
        return;
    }
    if (!hud[p].shown)
        for (size_t k = 0; k < 3; ++k) hudShow(p + k, kIcon[p + k], p + k, true);
    hud[p].rot = {0, 0, int16_t((t * 0x1000 / 0x3c0 + 0x800) & 0xfff)};      // minute hand: a turn per 32 s
    hud[p + 1].rot = {0, 0, int16_t((t * 0x1000 / 0x1e00 + 0x800) & 0xfff)};  // a turn per 256 s
}

void LitWorld::hudBoss(size_t p) {
    uint8_t& shown = hudState_.boss;
    const uint8_t target = S[1];  // 0x4b2261: boss hit points · 128 / max (type 14, 0x440120)
    if (target == 0 && shown == 0) {
        if (hud[p].shown) hud[p].shown = hud[p + 1].shown = hud[p + 2].shown = false;
        return;
    }
    if (shown == 0) shown = target;
    if (shown < target) ++shown;
    else if (target < shown) --shown;
    if (!hud[p].shown) {
        for (size_t k = 0; k < 3; ++k) hudShow(p + k, kIcon[p + k], p + k, true);
        hudState_.spin = 0;
    } else {
        hudState_.spin = int16_t((hudState_.spin + 0x50) & 0xfff);
        hud[p].rot = {0, hudState_.spin, 0};
        hud[p + 1].rot = {};
    }
    hud[p + 1].scaleX = 0x1000 - 0x20 * shown;
}

// 0x449d80 (pause entry, HUD hidden): carrots and health as the HUD shows them, then golden carrots G[7] | G[0xfc] << 8
// and clocks G[5] as 3-digit counters with their icons (slots 3-6 / 7-10 at origins 48-55), the completion
// (golden + clocks) * 100 / 457 with a percent sign (slots 20-23, leading zeros dropped) and the level's "n / max"
// lines for golden carrots G[G[0xb6]] / G[0xb7], clocks G[G[0xb8]] / G[0xb9] (moved up a line without the golden one)
// and G[4] / G[0xba], each at depth 1500. All shown instantly (hold 60, z 1200); hudTick is frozen while paused.
void LitWorld::hudTotals() {
    hudCounter(0, G[3], true);
    hudHealth(0xe, true);
    const auto row = [&](size_t slot, size_t pos, std::initializer_list<uint16_t> tmpls, int32_t z = 0) {
        for (const uint16_t t : tmpls) {
            hudShow(slot, t, pos++, true);
            if (z) hud[slot].z = z;
            ++slot;
        }
    };
    const auto d = [](int v) { return kDigit[v % 10]; };
    const int golden = goldenCarrots();
    row(3, 48, {kIcon[3], kDigit[golden / 100 & 0xf], d(golden / 10), d(golden)});
    row(7, 52, {kIcon[7], kDigit[G[5] / 100], d(G[5] / 10), d(G[5])});
    const int pc = (golden + G[5]) * 100 / 0x1c9;
    const uint16_t h = kDigit[pc / 100 & 0xf], t = d(pc / 10);
    if (h != kDigit[0]) hudShow(20, h, 20, true);
    if (h != kDigit[0] || t != kDigit[0]) hudShow(21, t, 21, true);
    row(22, 22, {d(pc), kIcon[20]});
    const auto line = [&](size_t slot, size_t pos, uint16_t icon, int v, int max) {
        row(slot, pos, {icon, d(v / 10), d(v), kIcon[25], d(max / 10), d(max)}, 0x5dc);
    };
    if (G[0xb7]) line(29, 29, kIcon[3], G[G[0xb6]], G[0xb7]);
    if (G[0xb9]) line(35, G[0xb7] ? 35 : 29, kIcon[7], G[G[0xb8]], G[0xb9]);
    if (G[0xba]) line(41, 41, kIcon[24], G[4], G[0xba]);
}

void LitWorld::hudTick() {  // 0x44acc0
    HudState& st = hudState_;
    if (level == 3 || nextLevel >= 0 || S[12] != 0) {  // hub, level change pending or paused: frozen
        st.health = int8_t(G[1]);
        deathStateA92_ = 0;
        return;
    }
    if (G[3] != st.carrots) {
        if (G[3] > 99) G[3] = 99;
        bool instant = false;
        uint16_t v = st.carrots;
        if (st.carrots == 0xff) st.carrots = G[3], v = G[3], instant = true;
        else if (st.carrots < G[3]) v = uint16_t(st.carrots + 1);
        else if (st.carrots > G[3]) v = uint16_t(st.carrots - 1);
        if (hudCounter(0, v, instant)) {  // one step per new units digit
            if (st.carrots < G[3]) st.carrots = uint8_t(std::min(st.carrots + 1, 99));
            else if (G[3] < st.carrots) --st.carrots;
        }
    }
    hudAnim(0, st.carrots < G[3] ? (G[3] - st.carrots) * 200 : 200);

    const uint8_t gi = G[0xb6];  // per-level golden-carrot counter index (LS01 logic sets it)
    if (G[gi] != st.golden) {
        bool instant = false;
        uint16_t v;
        if (st.golden == 0xff) st.golden = G[gi], v = G[gi], instant = true;
        else v = uint16_t(st.golden < G[gi] ? st.golden + 1 : st.golden - 1);
        if (hudCounter(3, v, instant)) {
            if (st.golden < G[gi]) ++st.golden;
            else if (G[gi] < st.golden) --st.golden;
        }
    }
    hudAnim(3, 200);

    const uint8_t ci = G[0xb8];  // clock counter index: counts up only
    if (G[ci] != st.clocks) {
        const bool instant = st.clocks == 0xff;
        uint16_t v = uint16_t(st.clocks + 1);
        if (instant) st.clocks = G[ci], v = G[ci];
        if (hudCounter(7, v, instant) && st.clocks != G[ci]) ++st.clocks;
    }
    hudAnim(7, 200);

    if (int8_t(G[1]) != st.health) {
        if (int8_t(G[1]) < 1 && !(p.flags14 & 0x1000000)) {  // §3.6 death
            G[1] = 0;
            kill(int16_t(deathStateA92_ ? deathStateA92_ : defaultDeathState_));
        }
        const bool instant = st.health == -1;
        st.health = int8_t(G[1]);
        hudHealth(0xe, instant);
    }
    hudAnim(0xe, 200);

    if (G[0x90] == 0) {
        st.boss = 0;
        hud[0xb].shown = hud[0xc].shown = hud[0xd].shown = false;
    } else {
        hudBoss(0xb);
    }

    // Stopwatches: G[n] = seconds (0xff: one tick) starts timer k; its remaining seconds go to G[n + 1].
    static constexpr uint8_t kTimers[4][3] = {{9, 0xa, 0}, {0xb, 0xc, 1}, {0x30, 0x31, 1}, {0x32, 0x33, 1}};
    for (size_t k = 0; k < 4; ++k) {
        uint16_t& t = st.timers[k];
        if (const uint8_t g = G[kTimers[k][0]]) {
            t = g == 0xff ? 1 : uint16_t(g * 30);
            G[kTimers[k][0]] = 0;
        }
        if (!t) continue;
        if (!(cam.flags & 0x100)) --t;
        hudTimer(0x11, t, t == 0);
        G[kTimers[k][1]] = t ? uint8_t(t / 30 + kTimers[k][2]) : 0;
    }

    auto item = [&](bool on, size_t slot) {  // inventory icons: shown while the flag is set, never animated
        if (!on) hud[slot].shown = false;
        else if (!hud[slot].shown) hudShow(slot, kIcon[slot], slot, true);
    };
    item(G[8] & 0x80, 26);
    item(G[0x2d] & 2, 28);
    item(G[0x2d] & 4, 27);
    deathStateA92_ = 0;
}

}  // namespace bb::game
