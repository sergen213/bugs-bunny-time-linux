// LiT pad emulation (GAMEPLAY.md §2, input fn 0x4491c0): an 8-byte PS1 pad report -> GAME_DATA pad words.
#pragma once
#include <cstdint>

namespace bb::game {

// Logical/physical button bits of the pad words (§2.1/§2.2): high byte = byte2, low byte = byte3, active-high.
enum : uint16_t {
    PadL2 = 0x01, PadR2 = 0x02, PadL1 = 0x04, PadR1 = 0x08, PadTriangle = 0x10, PadCircle = 0x20, PadCross = 0x40,
    PadSquare = 0x80, PadSelect = 0x100, PadL3 = 0x200, PadR3 = 0x400, PadStart = 0x800, PadUp = 0x1000,
    PadRight = 0x2000, PadDown = 0x4000, PadLeft = 0x8000,
};

// What the SDL layer fills each tick: held buttons (active-high, bits above) plus the left stick 0..255 (0x80
// neutral, up = small Y, like the PS1 report bytes 6/7). analog = report type 0x73, else 0x41 digital.
struct PadInput {
    uint16_t buttons = 0;
    uint8_t lx = 0x80, ly = 0x80;
    bool analog = false;
    bool help = false;  // F1 key-down this tick (WM_KEYDOWN 0x70, not a pad button): help screen toggle (LitWorld::tick)
    bool esc = false;   // Esc held this tick (0x4b18d0 bit 1, the window's extra keys): condition C96
};

// GD pad words (§2.2) plus the press-window counters of 0x4491c0.
struct Pad {
    uint16_t raw = 0;      // GD+0x10 physical
    uint16_t held = 0;     // GD+0x0c logical (remap preset 0 = identity)
    uint16_t prev = 0;     // GD+0x0e previous held & repeat mask
    uint16_t pressed = 0;  // GD+0x0a / +0x3e
    int16_t sx = 0, sy = 0;  // GD+0x12 (right +) / +0x14 (up +)
    uint8_t centreX = 0x7f, centreY = 0x7f;  // GD+0x1a / +0x20 (new game 0x7f)
    uint8_t cross = 2, square = 2, l1 = 2;   // counters 0x4b39b2 / 0x4b238a / 0x4b28cc
    uint16_t repeatMask = 0;                 // DAT_004b3228
};

// One tick of 0x4491c0 for the normal run state (not paused, player not scripted, no demo/recording).
// Remap presets 1-3 (G[0x11], table 0x4ae1f0) are not applied: preset 0 (identity) only.
void padTick(Pad& pad, const PadInput& in);

} // namespace bb::game
