#include "bb/game/pad.h"

namespace bb::game {

namespace {
constexpr int kDeadZone = 0x32;  // 0x4b3234, set in 0x4474b0 (GAMEPLAY §2.2)

// 0x4ae1f0: remap words +0x2e..+0x3c (physical □ ○ △ ✕ L1 L2 R1 R2) per preset (bugs.exe bytes).
constexpr uint16_t kPresets[4][8] = {
    {0x80, 0x20, 0x10, 0x40, 0x04, 0x01, 0x08, 0x02},
    {0x80, 0x04, 0x20, 0x40, 0x08, 0x01, 0x10, 0x02},
    {0x04, 0x08, 0x10, 0x80, 0x20, 0x01, 0x40, 0x02},
    {0x20, 0x08, 0x80, 0x10, 0x01, 0x02, 0x04, 0x40},
};

// ✕/□/L1 counters: reset to 2 while released, count down while held, at 0 the bit enters the repeat mask
// -> the button stays in the pressed word for the first 3 ticks of a press (§2.2).
void window(uint8_t& c, bool held, uint16_t bit, uint16_t& mask) {
    if (!held) c = 2;
    else if (c == 0) mask |= bit;
    else --c;
}

// Remap of the low byte (0x4491c0): each held physical button ORs its remap word.
uint16_t remapped(const Pad& p, uint16_t h) {
    static constexpr uint16_t kBit[8] = {PadSquare, PadCircle, PadTriangle, PadCross, PadL1, PadL2, PadR1, PadR2};
    uint16_t lo = 0;
    for (int i = 0; i < 8; ++i)
        if (h & kBit[i]) lo |= p.remap[size_t(i)];
    return uint16_t((h & 0xff00) | lo);
}
} // namespace

void padPreset(Pad& p, uint8_t preset) {
    for (size_t i = 0; i < 8; ++i) p.remap[i] = kPresets[preset < 4 ? preset : 0][i];
}

void padTick(Pad& p, const PadInput& in, bool paused, bool zero, std::span<const uint16_t>* demo, bool demoMute) {
    if (zero && !paused) {  // 0x4491c0 else branch
        p.raw = p.held = p.prev = p.pressed = 0;
        p.sx = p.sy = 0;
        p.camStep = 0;
        return;
    }
    p.camStep = paused || demo ? 0 : in.camStep;  // Enhanced right-stick camera; the zeroed branch above clears it too
    p.repeatMask = 0;
    if (!paused) {
        window(p.cross, p.held & PadCross, PadCross, p.repeatMask);
        window(p.square, p.held & PadSquare, PadSquare, p.repeatMask);
        window(p.l1, p.held & PadL1, PadL1, p.repeatMask);
    }
    p.prev = p.held & p.repeatMask;
    p.held = 0;

    if (demo) {  // G[0x0f] != 0: the stream replaces the words
        if (demoMute) {
            p.raw = p.held = p.prev = p.pressed = 0;
            return;
        }
        uint16_t w[4]{};
        for (size_t i = 0; i < 4 && i < demo->size(); ++i) w[i] = (*demo)[i];
        *demo = demo->subspan(demo->size() < 4 ? demo->size() : 4);
        p.pressed = w[0], p.prev = w[1], p.held = w[2], p.raw = w[3];
        if (in.buttons & PadStart) p.pressed |= PadSelect, p.held |= PadSelect, p.raw |= PadSelect;
        p.sx = (p.pressed & PadLeft) ? int16_t(-0x80) : (p.pressed & PadRight) ? int16_t(0x7f) : int16_t(0);
        p.sy = (p.pressed & PadUp) ? int16_t(0x7f) : (p.pressed & PadDown) ? int16_t(-0x80) : int16_t(0);
        return;
    }

    // Pad present (the report type is always 0x41 / 0x73 on PC: PadGetState 0x402fe0 returns 6, so the exe's
    // "controller removed" Start injection never runs).
    const uint16_t lo = in.buttons & 0xff, hi = in.buttons & 0xff00;
    p.raw = uint16_t(lo | hi);
    if ((hi & 0xf000) == 0 && in.analog) {  // analog pad, no D-pad: the stick sets direction bits
        p.held = uint16_t(lo | (hi & 0xfff));
        if (!paused) p.held = remapped(p, p.held);
        p.pressed = p.held & ~p.prev;
        auto dir = [&](uint16_t bit) { p.pressed |= bit; p.held |= bit; p.raw |= bit; };
        const int cx = p.centreX, cy = p.centreY;
        if (in.lx < cx - kDeadZone) { dir(PadLeft); p.sx = int16_t(in.lx - cx); }
        else if (in.lx > cx + kDeadZone) { dir(PadRight); p.sx = int16_t(in.lx - cx); }
        else p.sx = 0;
        if (in.ly < cy - kDeadZone) { dir(PadUp); p.sy = int16_t(cy - in.ly - 1); }
        else if (in.ly > cy + kDeadZone) { dir(PadDown); p.sy = int16_t(cy - in.ly - 1); }
        else p.sy = 0;
        return;
    }
    p.held = paused ? in.buttons : remapped(p, in.buttons);
    p.pressed = p.held & ~p.prev;
    // Digital: D-pad -> full deflection ±0x7f / -0x80 (§2.2).
    p.sx = (hi & PadLeft) ? int16_t(-0x80) : (hi & PadRight) ? int16_t(0x7f) : int16_t(0);
    p.sy = (hi & PadUp) ? int16_t(0x7f) : (hi & PadDown) ? int16_t(-0x80) : int16_t(0);
}

void PadTape::step(PadInput& in) {
    if (mode == Record) {
        data.push_back(uint8_t(~(in.buttons >> 8)));
        data.push_back(uint8_t(~in.buttons));
        if (data.size() > 0xffffa) mode = Off;
        at += 2;
    } else if (mode == Play) {
        // Bytes past the file: the exe reads whatever its buffer holds there (two passes); bbtime reads released.
        const uint8_t b2 = at < data.size() ? data[at] : 0xff, b3 = at + 1 < data.size() ? data[at + 1] : 0xff;
        in.buttons = uint16_t(uint8_t(~b2) << 8 | uint8_t(~b3));
        if (data.size() < at) mode = Off;
        at += 2;
    }
}

} // namespace bb::game
