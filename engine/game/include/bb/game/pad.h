// LiT pad emulation (GAMEPLAY.md §2, input fn 0x4491c0): an 8-byte PS1 pad report -> GAME_DATA pad words.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

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
    // Key capture of the pause menu's Controls pages: key = the lowest DirectInput scan code held (keyboard state
    // 0x9ca8c0 scanned from 1) when one of the letter keys Q-P / A-L / Z-M is held, else 0 (C94 0x42bad0); joyButton
    // = 0x403210: the lowest held joystick button 0..15, 0xfe none held, 0xff no joystick (C95 0x42c140).
    uint8_t key = 0, joyButton = 0xff;
    // Enhanced right-stick camera (BBLIT-Remaster; 0 = none, the exe): LiT camera yaw step this tick in 4.12 units,
    // taken through the same manual rotation as L2 (+0x26) / R2 (-0x26) when neither is held (LitWorld::cameraTick).
    int16_t camStep = 0;
};

// GD pad words (§2.2) plus the press-window counters of 0x4491c0.
struct Pad {
    uint16_t raw = 0;      // GD+0x10 physical
    uint16_t held = 0;     // GD+0x0c logical (remapped)
    uint16_t prev = 0;     // GD+0x0e previous held & repeat mask
    uint16_t pressed = 0;  // GD+0x0a / +0x3e
    int16_t sx = 0, sy = 0;  // GD+0x12 (right +) / +0x14 (up +)
    int16_t camStep = 0;     // PadInput::camStep of this tick (0 while paused / zeroed / in a demo)
    uint8_t centreX = 0x7f, centreY = 0x7f;  // GD+0x1a / +0x20 (new game 0x7f)
    uint8_t cross = 2, square = 2, l1 = 2;   // counters 0x4b39b2 / 0x4b238a / 0x4b28cc
    uint16_t repeatMask = 0;                 // 0x4b3228
    // Remap words GD+0x2e..+0x3c: the logical bits physical □ ○ △ ✕ L1 L2 R1 R2 stand for (0x448fa0 preset).
    std::array<uint16_t, 8> remap{PadSquare, PadCircle, PadTriangle, PadCross, PadL1, PadL2, PadR1, PadR2};
};

// 0x448fa0: remap words of preset `preset` (G[0x11], table 0x4ae1f0, 4 x 8 u16; 0 = identity). Run by the new-game
// init and every level init (0x4474b0). Indices >= 4 read past the table in the exe; bbtime keeps preset 0 then.
void padPreset(Pad& pad, uint8_t preset);

// One tick of 0x4491c0 after the report is built. paused = S[12] (0x4b226c) != 0: no ✕/□/L1 windows (no repeat mask)
// and no remap. zero = a level change pending (0x4b39bc) or the player scripted (flags14 0x1000000) while not paused:
// every word 0. demo: the level's demo stream (G[0x0f] != 0, chunk 0x47 tag 0x48) as {+0x3e, +0x0e, +0x0c, +0x10}
// per tick, consumed from the front (exhausted = zero words); a physical Start becomes Select 0x100. demoMute =
// speech status S[0x17] not 0 / 4: the demo branch zeroes the words instead (stream not advanced).
void padTick(Pad& pad, const PadInput& in, bool paused, bool zero = false, std::span<const uint16_t>* demo = nullptr,
             bool demoMute = false);

// The exe's /r:<file> record and /p:<file> playback (0x408f70 switches, 0x4491c0 head, files 0x409520 / 0x4095a0): two
// bytes per input pass = PS1 report bytes 2 and 3 (active-low), record capped at 0xffffa bytes; playback replaces those
// bytes until it passes the file length, then input is live again. The exe loads a playback file only for a boot
// into level 0 (title) and writes the recording at exit; its buffer lies past the end of the 1 MB texture staging
// buffer 0x4b1ca4 (+0x100000: a heap overrun, or address 0x100000 with the software renderers), bbtime uses its own.
struct PadTape {
    enum Mode : uint8_t { Off, Record, Play } mode = Off;
    std::vector<uint8_t> data;  // recorded / loaded bytes
    size_t at = 0;              // 0x4b1d08 read / write position
    void step(PadInput& in);    // before padTick, every LiT tick
};

} // namespace bb::game
