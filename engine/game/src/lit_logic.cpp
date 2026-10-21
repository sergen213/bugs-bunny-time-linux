// LiT level-logic VM: TestLogicCond 0x42a7d0 (99 conditions, table 0x4ac550) and RunLogicAction 0x42c6d0
// (129 actions, table 0x4ac6e0). Semantics: docs/re/LOGIC.md §4/§5. `self` = the actor running the item
// (nullptr = Bugs); P = Bugs. The title front end's opcodes (C96, A93, A100, A128) are ported; the pause-menu
// options opcodes (C92-C95, C97, A102-A127, in every playable level) return false / do nothing and are listed in
// LitWorld::unsupported, as are camera effects without a simulated counterpart.
// C55/C56, A49/A51/A52 run overlay text scripts (lit_overlay.cpp).
#include "bb/game/lit_world.h"

#include <format>

namespace bb::game {
namespace {

// C57/C69/C90 mask byte: bits 0-3 triangle/circle/cross/square, bits 4-7 up/right/down/left -> pad word bits.
uint16_t padMask(uint8_t a) { return uint16_t((a & 0xf) << 4 | (a & 0xf0) << 8); }
// C79/C80 (0x42b820 / 0x42b850) mask byte: bits 0-3 L2/R2/L1/R1, bits 4-7 Select/L3/R3/Start.
uint16_t padMask2(uint8_t a) { return uint16_t((a & 0xf) | (a & 0xf0) << 4); }

// Angle from `from` to `to` minus `yaw`, wrapped to (-0x800, 0x800] (0x42ae30 / 0x42af90).
int32_t facing(const V3& from, const V3& to, int32_t yaw) { return angleDiff(ratan2(to.x - from.x, to.z - from.z), yaw); }

// C17-C20 side filter: A = 1 only diff <= 0, A = 2 only diff >= 0.
bool within(int32_t d, uint8_t a) { return d < 0x294 && d > -0x294 && (a != 1 || d < 1) && (a != 2 || d > -1); }

} // namespace

bool LitWorld::cond(const LogicItem& it, LitActor* self) {
    Mover& me = self ? static_cast<Mover&>(*self) : static_cast<Mover&>(p);
    const uint8_t A = it.a, B = it.b;
    const uint16_t ab = uint16_t(A | B << 8);
    const auto s8 = [](uint8_t v) { return int8_t(v); };
    const uint32_t t = me.timer;
    switch (it.cond) {
    case 0: return true;
    case 1: return s8(S[B]) == s8(A);
    case 2: return G[B] == A;
    case 3: return A < S[B];
    case 4: return A < G[B];
    case 5: return A <= S[B];
    case 6: return A <= G[B];
    case 7: return S[B] < A;
    case 8: return G[B] < A;
    case 9: return S[B] <= A;
    case 10: return G[B] <= A;
    case 11: return (p.flags14 & 0x2000020) == 0;
    case 12: return p.state == A;
    case 13: return true;                                   // self+0x6c (carried actor) == 0: carrying not simulated
    case 14: return p.zone == A;
    case 15: case 61: return S[B] == S[A];
    case 16: case 62: return G[B] == G[A];
    case 17: { const int32_t d = facing(me.pos, p.pos, p.yaw); return !(d < 0x294 && d > -0x294) && !(A == 1 && d > 0) && !(A == 2 && d < 0); }
    case 18: return within(facing(me.pos, p.pos, p.yaw), A);
    case 19: return within(facing(me.pos, p.pos, me.yaw), A);
    case 20: return !within(facing(me.pos, p.pos, me.yaw), A);
    case 21: return t < ab;
    case 22: return t > ab;
    case 23: return t == ab;
    case 24: return t / 30 < ab;
    case 25: return t / 30 > ab;
    case 26: return t % 30 == 0 && t / 30 == ab;
    case 27: return t / 1800 < ab;
    case 28: return t / 1800 > ab;
    case 29: return t / 1800 == ab;
    case 30: return (S[B] & A) != 0;
    case 31: return (G[B] & A) != 0;
    case 32: return (p.flags14 & 0x2000020) != 0;
    case 33: case 76: return (p.flags14 & 0x10) != 0;
    case 34: case 39: return (S[B] & A) == 0;
    case 35: case 40: return (G[B] & A) == 0;
    case 36: return (p.flags14 & 0x1000000) != 0;
    case 37: return S[B] != A;
    case 38: return G[B] != A;
    case 41: return (p.flags14 & 0x40) != 0;
    case 42: return (p.flags14 & 0x40) == 0;
    case 43: return false;                                  // self carries an actor: never (carrying not simulated)
    case 44: return p.zone != A;
    case 45: return camHold_ != 0;
    case 46: return S[B] == A;
    case 47: return G[B] == A;
    case 48: return (p.flags14 & 0x1000000) == 0;
    case 49: return (pad.pressed & PadR1) != 0;
    case 50: { const int32_t y = (me.yaw & 0xfff) >> 4; return A < y && y < B; }
    case 51: return (p.flags18 & 2) != 0;
    case 52: return (p.flags18 & 4) != 0;
    case 53: return (p.flags18 & 2) == 0;
    case 54: return (p.flags18 & 4) == 0;
    case 55: overlayStatus_ = overlay(it.x); return (A & overlayStatus_) != 0;  // 0x42b450
    case 56: return (A & overlayStatus_) != 0;
    case 57: case 69: {  // 0x42b4a0 / 0x42b500: Start (or Return) also counts as cross; C69 = none of them
        const bool any = ((pad.raw | (pad.raw & PadStart ? PadCross : 0)) & padMask(A)) != 0;
        return it.cond == 57 ? any : !any;
    }
    case 58: return me.reg == A;
    case 59: return int16_t(A) < me.reg;
    case 60: return me.reg < int16_t(A);
    case 63: return S[B] != S[A];
    case 64: return G[B] != G[A];
    case 65: return S[A] < S[B];
    case 66: return G[A] < G[B];
    case 67: return S[B] < S[A];
    case 68: return G[B] < G[A];
    case 70: return s8(A) < s8(S[B]);
    case 71: return s8(S[B]) < s8(A);
    case 72: return s8(S[A]) < s8(S[B]);
    case 73: return s8(S[B]) < s8(S[A]);
    case 74: return me.zone == A;
    case 75: return me.zone != A;
    case 77: return (p.flags14 & 0x10) == 0;
    case 78: { const PlayerState* st = findState(p.state); return st && p.seq <= 4 && uint16_t(st->moves[p.seq]) == A; }
    case 79: return (pad.raw & padMask2(A)) != 0;
    case 80: return (pad.raw & padMask2(A)) == 0;
    case 81: return (pad.pressed & PadR1) == 0;
    case 82: case 83: {  // 0x42b640: A bits -> flags14, B bits -> flags18
        static constexpr uint32_t a14[8] = {0x1, 0x2, 0x10, 0x8, 0x200, 0x400, 0x4000, 0x1000000};
        static constexpr uint32_t b18[5] = {0x2000000, 0x10000000, 0x80000000, 0x2, 0x4};
        bool any = false;
        for (int k = 0; k < 8; ++k) any |= (A >> k & 1) && (p.flags14 & a14[k]);
        for (int k = 0; k < 5; ++k) any |= (B >> k & 1) && (p.flags18 & b18[k]);
        return it.cond == 82 ? any : !any;
    }
    case 84: { const int32_t h = int8_t(p.pos.y >> 8); return s8(A) <= h && h <= s8(B); }
    case 85: case 87:  // 0x42b930 / 0x42b990: sound A on any voice / on self's voice (start pending or playing) == (B == 1)
        return sfx.playing(A, it.cond == 87 ? emitter(self) : 0) == (B == 1);
    case 86: return false;                                  // Bugs carrying: not simulated
    case 88: return s8(A) < s8(G[B]);
    case 89: return s8(G[B]) < s8(A);
    case 90: return (pad.pressed & padMask(A)) != 0;
    case 91: return (pad.pressed & padMask(A)) == 0;
    case 96: return (keys_ & 2) != 0;  // 0x42c670: Esc held (0x4b18d0 bit 1)
    case 98: return joystick == (A != 0);  // 0x42c690: config use-joystick (selects >D vs >J hint scripts)
    default:
        unsupported.insert(std::format("C{}", it.cond));
        return false;  // out-of-table conditions also return false in the exe
    }
}

void LitWorld::act(const LogicItem& it, LitActor* self) {
    Mover& me = self ? static_cast<Mover&>(*self) : static_cast<Mover&>(p);
    const uint16_t X = it.x;
    uint8_t& sy = S[it.y & 0xff];
    uint8_t& gy = G[it.y & 0xff];
    // Every G add/sub/inc/dec maps index 7 to the 16-bit golden-carrot total G[7] | G[0xfc] << 8 (LOGIC.md §7).
    auto gAdd = [&](uint16_t idx, int d) {
        if ((idx & 0xff) == 7) {
            const int v = goldenCarrots() + d;
            G[7] = uint8_t(v), G[0xfc] = uint8_t(v >> 8);
        } else {
            G[idx & 0xff] = uint8_t(G[idx & 0xff] + d);
        }
    };
    auto killSelf = [&] { if (self) self->flags14 |= 0x100; };
    auto skip = [&] { unsupported.insert(std::format("A{}", it.act)); };
    switch (it.act) {
    case 0: case 61: case 62: case 65: return;               // nop
    case 1: killSelf(); return;
    case 2: ++sy; killSelf(); return;
    case 3: ++sy; return;
    case 4: gAdd(it.y, 1); return;
    case 5: --sy; return;
    case 6: gAdd(it.y, -1); return;
    case 7: sy = 0; return;
    case 8: gy = 0; return;
    case 9: sy = uint8_t(-sy); return;
    case 10: gy = uint8_t(-gy); return;
    case 11: sy |= uint8_t(X); return;
    case 12: gy |= uint8_t(X); return;
    case 13: sy &= uint8_t(X); return;
    case 14: gy &= uint8_t(X); return;
    case 15: if (X) sy = uint8_t(rand_() % X); return;
    case 16: if (X) gy = uint8_t(rand_() % X); return;
    case 17: return;                                         // message queue 0x4b2480: no reader among the ported opcodes
    case 18: sy = uint8_t(X); return;
    case 19: gy = uint8_t(X); return;
    case 20: sy = S[X & 0xff]; return;
    case 21: gy = G[X & 0xff]; return;
    case 22: ++sy; ++S[X & 0xff]; return;
    case 23: gAdd(it.y, 1); gAdd(X, 1); return;
    case 24: ++sy; ++S[X & 0xff]; killSelf(); return;
    case 25: me.timer = 0; return;
    case 26:                                                 // force Bugs into state X facing self
        if (p.platform >= 0) { p.platform = -1; p.flags14 &= ~0x14u; }
        forceState(X);
        p.yaw = int16_t(me.yaw + 0x800);
        p.flags14 |= 0x2000000;
        return;
    case 27: gy = S[X & 0xff]; return;
    case 28: sy = G[X & 0xff]; return;
    case 29: if (sy && me.timer / 30 > 1) { --sy; me.timer = 0; } return;
    case 30: me.flags14 = (me.flags14 & ~0x400000u) | 0x200000; return;
    case 31: me.flags14 = (me.flags14 & ~0x200000u) | 0x400000; return;
    case 32: if (self) self->act3c &= ~0x40u; return;      // activation +0x3c &= ~0x40
    case 33: case 63: return;                                // drop / destroy carried actor: none carried
    case 34: playSound(self, X); return;
    case 35: G[1] = 0; kill(int16_t(deathStateA92_ ? deathStateA92_ : defaultDeathState_)); return;
    case 36: sy = uint8_t(sy + X); return;
    case 37: gAdd(it.y, X); return;
    case 38: me.yaw = int16_t(me.yaw + X * 16 + it.y); return;
    case 39: sy = uint8_t(sy - X); return;
    case 40: gAdd(it.y, -int(X)); return;
    case 41: camHold_ = X; p.flags14 |= 0x80000000; return;
    case 42: nextLevel = G[0x0e]; return;                    // goto level G[0x0e]
    case 43: p.yaw = int16_t(X << 4); return;
    case 45: sy = uint8_t(me.reg); return;
    case 46: me.reg = sy; return;
    case 47: p.flags18 |= 1; return;
    case 48: if (G[1] < G[2]) ++G[1]; else gAdd(it.y, 1); return;  // gvcrt
    case 49: overlayStatus_ = overlay(X); return;           // overlay script X / S[Y] / G[Y]
    case 51: overlayStatus_ = overlay(sy); return;
    case 52: overlayStatus_ = overlay(gy); return;
    case 50:                                                 // kill actor(s) with entity id X (0xff: all overlays)
        for (LitActor& a : actors)
            if (!a.dead && (X == 0xff ? a.type == 0x20 : a.entityId == X)) {
                a.flags14 |= 0x100;
                if (X != 0xff) break;
            }
        return;
    case 53: me.yaw = int16_t(me.yaw + int8_t(sy)); return;
    case 54: if (self) self->animState.flags |= AnimPause; return;
    case 55: if (self) self->animState.flags &= ~uint32_t(AnimPause); return;
    case 57: case 58: case 66: case 67: return;              // rumble: PadSetAct is a no-op on PC
    case 60:
        for (int k = 0; k < X && k < 8; ++k)
            if (!(gy >> k & 1)) gAdd(7, 1);
        return;
    case 64: if (self) self->act3c |= 0x40; return;
    case 69: if (it.y < 3) fade.rgb[it.y] = uint8_t(X); return;  // flash colour R / G / B (0x42d510)
    case 70: if (X == 0) fadeIn(1, 0x100); else if (X == 1) fadeOut(2, 0x100); return;  // 16-tick fade
    case 71: fadeIn(0x41, 0x400); return;                    // colour flash: 4 ticks in, 4 back
    case 72:                                                 // damage Bugs X (§3.6)
        if (invuln_ == 0) {
            G[1] = uint8_t(G[1] - X);
            p.flags14 |= 0x2000000;
            invuln_ = 0x1e;
        }
        return;
    case 73: cam.flags &= ~0x2000u; return;
    case 74: cam.flags |= 0x2000; return;
    case 75: me.yaw = int16_t(me.yaw + (((X * 16 + it.y) * pad.sx * 2) >> 7)); return;
    case 76: me.yaw = int16_t(me.yaw + ((int8_t(sy) * pad.sx * 2) >> 7)); return;
    case 77: cam.heightOffset = -200, cam.eyeHeight = 600, cam.distance = 1100; return;
    case 78: cam.heightOffset = X * 100 + it.y; return;
    case 79: cam.eyeHeight = X * 100 + it.y; return;
    case 80: cam.distance = X * 100 + it.y; return;
    case 81:  // speech XA control 0x42d750: status S[0x17] (8 start, 1 stop-all, 0 idle), advanced by speechTick
        if (X > 1) S[0x17] = 8, speech_ = uint8_t(X);
        else if (X == 1) speech_ = 0, S[0x17] = 1;
        else speech_ = 0, S[0x17] = 0;
        return;
    case 82: playSound(self, X, 2); return;                  // 0x42d7f0: release
    case 83: cam.flags |= 0x8000; return;
    case 84: cam.flags &= ~0x8000u; return;
    case 86: p.flags14 |= 0x80000000; return;
    case 87: me.flags18 |= 0x400; return;
    case 88: shadowMaxY = int8_t(X) * 100 + int8_t(it.y); return;  // 0x42d710 (movsx bytes)
    case 89: sy = uint8_t(angleDiff(ratan2(p.pos.x - me.pos.x, p.pos.z - me.pos.z), me.yaw) >> 4); return;
    case 90: cam.flags &= ~0x300u; return;
    case 91: shadowMaxY = int32_t(uint32_t(X) << 8 | it.y); return;  // 0x42d730 (upper ecx bits Inferred zero)
    case 92: deathStateA92_ = X; return;
    case 94: cam.flags |= 1; return;
    case 95: cam.flags &= ~1u; return;
    case 96: cam.flags = 0x10; return;
    case 97: p.flags14 &= ~9u; return;
    case 98: cam.flags |= 0x20; return;
    case 99: cam.flags &= ~0x20u; return;
    case 101: cam.flags |= 4; return;
    case 93:  // 0x42d910: title slot lines 1-6, "N. <golden> <clocks>" (saved) or "N. Empty", green on slot G[X]
        for (uint8_t k = 0; k < 6; ++k) {
            const bool sel = G[X & 0xff] == k + 1;
            overlay(uint16_t((saveMask >> k & 1 ? (sel ? 0x36 : 0x2e) : (sel ? 0x46 : 0x3e)) + k));
        }
        return;
    case 100: {  // 0x42dae0: slot cursor G[X] onto a saved slot, searching in the raw pad's direction (→/↓ forward)
        uint8_t& c = G[X & 0xff];
        const auto saved = [&](int s) { return (saveMask >> ((s - 1) & 31) & 1) != 0; };
        if (saved(c)) return;
        int v = c;
        if (!(pad.raw & (PadRight | PadDown))) {
            while (v > 0 && !saved(v)) --v;
            if (v < 1) {
                if (pad.raw & PadLeft) { c = uint8_t(c + 3); return; }
                for (v = 1; v < 7 && !saved(v); ++v) {}
            }
        } else {
            while (v < 7 && !saved(v)) ++v;
            if (v > 6) {
                if (pad.raw & PadRight) { c = uint8_t(c - 3); return; }
                for (v = 6; !saved(v);)
                    if (--v < 1) break;
            }
        }
        c = uint8_t(v);
        return;
    }
    case 128: quitRequest = true; return;  // 0x42e350: 0x45f2b0 = 0x14, the "Do you really want to quit?" dialog
    default: skip();  // A44 0x28, A56, A59/A68 shake, A85 camera target, A102-A127 pause-menu options
    }
}

} // namespace bb::game
