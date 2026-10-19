#include "bb/game/pad.h"

namespace bb::game {

namespace {
constexpr int kDeadZone = 0x32;  // DAT_004b3234, set in 0x4474b0 (GAMEPLAY §2.2)

// ✕/□/L1 counters: reset to 2 while released, count down while held, at 0 the bit enters the repeat mask
// -> the button stays in the pressed word for the first 3 ticks of a press (§2.2).
void window(uint8_t& c, bool held, uint16_t bit, uint16_t& mask) {
    if (!held) c = 2;
    else if (c == 0) mask |= bit;
    else --c;
}
} // namespace

void padTick(Pad& p, const PadInput& in) {
    p.repeatMask = 0;
    window(p.cross, p.held & PadCross, PadCross, p.repeatMask);
    window(p.square, p.held & PadSquare, PadSquare, p.repeatMask);
    window(p.l1, p.held & PadL1, PadL1, p.repeatMask);
    p.prev = p.held & p.repeatMask;

    const uint16_t lo = in.buttons & 0xff, hi = in.buttons & 0xff00;
    p.raw = uint16_t(lo | hi);
    if ((hi & 0xf000) == 0 && in.analog) {  // analog pad, no D-pad: the stick sets direction bits
        p.held = uint16_t(lo | (hi & 0xfff));
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
    p.held = in.buttons;
    p.pressed = p.held & ~p.prev;
    // Digital: D-pad -> full deflection ±0x7f / -0x80 (§2.2).
    p.sx = (hi & PadLeft) ? int16_t(-0x80) : (hi & PadRight) ? int16_t(0x7f) : int16_t(0);
    p.sy = (hi & PadUp) ? int16_t(0x7f) : (hi & PadDown) ? int16_t(-0x80) : int16_t(0);
}

} // namespace bb::game
