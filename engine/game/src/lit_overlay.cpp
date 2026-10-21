// LiT overlay text (GAMEPLAY.md §7.6): spawner 0x44e310 (C55 / A49 / A51 / A52), type-32 behaviour 0x4470e0 and
// the script interpreter 0x44d100, which emits the tick's text glyphs (LitWorld::glyphs; the app draws them).
#include "bb/game/lit_world.h"

#include <algorithm>
#include <format>

namespace bb::game {
namespace {

// Exe .data tables. 0x4ae3a8: script byte -> 1-based font glyph (0 = not a glyph). 0x4ae4a8: byte -> pen advance
// (x the glyph scale 0x4efb88, 1.0 except below the low-resolution threshold of 0x408100, where it is 1.5).
constexpr uint8_t kGlyph[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x3f, 0x40, 0x00, 0x00, 0x41, 0x82, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x4b, 0x4c, 0x00, 0x4d, 0x00, 0x4e,
    0x00, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6b, 0x67, 0x68, 0x69, 0x6a,
    0x6d, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x00, 0x00, 0x4f, 0x75, 0x76, 0x77, 0x00, 0x00, 0x78, 0x00, 0x00,
    0x00, 0x79, 0x00, 0x7a, 0x7b, 0x7c, 0x7d, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x7f, 0x00, 0x00, 0x6f,
    0x54, 0x62, 0x52, 0x55, 0x53, 0x00, 0x00, 0x56, 0x59, 0x51, 0x57, 0x58, 0x5c, 0x63, 0x5b, 0x5a,
    0x00, 0x66, 0x5f, 0x64, 0x5d, 0x00, 0x5e, 0x00, 0x00, 0x61, 0x65, 0x60, 0x50, 0x00, 0x00, 0x00};
constexpr uint8_t kAdvance[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x09, 0x00, 0x00, 0x0d, 0x0b, 0x05, 0x07, 0x07, 0x0e, 0x0d, 0x05, 0x0a, 0x05, 0x09,
    0x0b, 0x07, 0x09, 0x0a, 0x0a, 0x09, 0x0a, 0x09, 0x0a, 0x0a, 0x05, 0x05, 0x00, 0x08, 0x00, 0x0a,
    0x00, 0x0d, 0x09, 0x0b, 0x0b, 0x09, 0x08, 0x0c, 0x0a, 0x05, 0x0b, 0x0b, 0x09, 0x0d, 0x0d, 0x0d,
    0x0b, 0x0e, 0x0b, 0x09, 0x0c, 0x0c, 0x0d, 0x11, 0x0d, 0x0d, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0b, 0x08, 0x09, 0x09, 0x08, 0x07, 0x0a, 0x08, 0x05, 0x08, 0x09, 0x07, 0x0b, 0x0b, 0x0a,
    0x08, 0x0c, 0x09, 0x08, 0x08, 0x0a, 0x0a, 0x0f, 0x0b, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x13, 0x13, 0x13, 0x13,
    0x1e, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x00, 0x00, 0x09, 0x09, 0x09, 0x09, 0x00, 0x00, 0x05, 0x00, 0x00,
    0x00, 0x0d, 0x00, 0x0d, 0x0d, 0x0d, 0x0d, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x0a,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x00, 0x00, 0x09, 0x08, 0x08, 0x08, 0x08, 0x07, 0x07, 0x07, 0x07,
    0x00, 0x0b, 0x0a, 0x0a, 0x0a, 0x00, 0x0a, 0x00, 0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x00};

} // namespace

uint32_t LitWorld::overlay(uint16_t script, LitActor* resume) {
    LitActor* a = resume;
    size_t pc = 0;
    if (!a) {
        // Guard (the exe reads past its table): the first offset is the table size.
        if (overlayText.size() < 2 || script >= (overlayText[0] | overlayText[1] << 8) / 2u) return 0;
        const int k = spawn(68, {}, 0);  // 0x448d40(0x44, 0, 0); no template 68 -> nothing (0x44ccd0 found no font)
        if (k < 0) return 0;
        a = &actors[size_t(k)];
        a->resume = -1;
        a->script = uint16_t(script - 0x8000);
        a->zone = p.zone;
        if (S[12] == 1 || S[12] == 3) a->flagsC |= 0x40000000;
        pc = overlayText[script * 2u] | overlayText[script * 2u + 1] << 8;
    } else {
        pc = size_t(a->resume);
        a->resume = -1;
    }
    const uint32_t r = overlayRun(*a, pc);
    return (r & 0x80 ? 8u : 0u) | (r & 0x20 ? 2u : 0u) | (r & 0x10 ? 1u : 0u) | (r & 0x40 ? 4u : 0u);
}

void LitWorld::overlayActor(size_t i) {
    LitActor& a = actors[i];
    if (a.param >= 1 && a.timer == a.param) {  // waited the >W ticks: next page, else done
        if (a.resume >= 0) overlay(0, &a);
        else a.flags14 |= 0x100;
        return;
    }
    if (a.script & 0x8000) {  // the spawning call already drew this tick
        a.script &= 0x7fff;
        return;
    }
    // Redraws the first page each tick (a >W n page returns at its >W, which resets the wait: the timer is kept).
    const uint16_t t = a.timer;
    if (size_t(a.script) * 2 + 1 < overlayText.size())
        overlayRun(a, overlayText[a.script * 2u] | overlayText[a.script * 2u + 1] << 8);
    a.timer = t;
}

// Text bytes become glyphs at the pen (advance kAdvance; ' ' +6, '\t' +24, '\n' 13 lower at the line start x);
// '>' commands: CX n / CC n pen + line x (CC: centre the line on it), CY n pen + line y, CP n palette, CL back to
// the line start (status 0), P d status |= digit bits, W n wait n ticks (pause: the rest resumes after the wait),
// W0 / none status 4 if unset, ID b entity id, B[G]b decimal S[b] / G[b], D n / J n key names. One byte after a
// command's operand is skipped.
uint32_t LitWorld::overlayRun(LitActor& a, size_t pc) {
    const auto at = [&](size_t i) -> uint8_t { return i < overlayText.size() ? overlayText[i] : 0; };
    const auto num = [&](size_t& q) {  // [-]digits, a "digit" being any byte 0x30..0x3f (value & 0xf)
        int sign = 1, v = 0;
        if (at(q) == '-') sign = -1, ++q;
        while ((at(q) & 0xf0) == 0x30) v = v * 10 + (at(q++) & 0xf);
        return v * sign;
    };
    int16_t w = 0, x = -200, x0 = -200;  // line width (centring), pen, line start
    int8_t y = -60, y0 = -60;
    uint8_t pal = 0;
    bool centre = false;
    long first = -1;  // first glyph of the line
    uint8_t last = 0;  // palette of the last text glyph (the >J key name reuses it)
    uint32_t st = 0;
    const auto emit = [&](uint8_t ch, uint8_t adv, uint8_t pl) {
        if (first < 0) first = long(glyphs.size());
        glyphs.push_back({ch, kGlyph[ch], x, y, pl});
        w = int16_t(w + adv);
        x = int16_t(x + adv);
    };
    // End of a centred line: shift its glyphs left by half its width. Without a glyph the exe starts at index -1,
    // i.e. shifts every glyph of the tick (and one word before the list).
    const auto endLine = [&] {
        if (centre)
            for (size_t k = size_t(std::max(first, 0L)); k < glyphs.size(); ++k) glyphs[k].x = int16_t(glyphs[k].x - (w >> 1));
    };
    for (;; ++pc) {
        const uint8_t c = at(pc);
        if (c == 0) {
            endLine();
            return st;
        }
        if (const uint8_t g = kGlyph[c]) {
            constexpr uint8_t kIcon[8] = {5, 6, 7, 8, 10, 9, 11, 12};  // glyphs 0x67..0x6e: fixed palettes
            last = g >= 0x67 && g <= 0x6e ? kIcon[g - 0x67] : pal;
            emit(c, kAdvance[c], last);
        } else if (c == '\n') {
            endLine();
            w = 0, centre = false, first = -1;
            y = int8_t(y + 13);
            x = x0;
        } else if (c == ' ' || c == '\t') {
            const int d = c == ' ' ? 6 : 24;
            w = int16_t(w + d), x = int16_t(x + d);
        } else if (c == '>') {
            const uint8_t k = at(pc + 1), s = at(pc + 2);
            size_t q = pc + 3;
            if (k == 'C') {
                if (s == 'L') st = 0, y = y0, x = x0;  // also frees the actor's part list (none here)
                else if (s == 'X' || s == 'C') centre |= s == 'C', x = x0 = int16_t(num(q));
                else if (s == 'Y') y = y0 = int8_t(num(q));
                else if (s == 'P') pal = uint8_t(num(q));
            } else if (k == 'P') {
                st |= (s & 8 ? 0x80u : 0u) | (s & 2 ? 0x20u : 0u) | (s & 1 ? 0x10u : 0u) | (s & 4 ? 0x40u : 0u);
            } else if (k == 'W') {
                q = pc + 2;
                if (const int n = num(q)) {
                    a.timer = 0;
                    a.param = int16_t(n);
                    if (at(q + 1)) a.resume = int32_t(q);
                    endLine();
                    return 0;
                }
                if (!st) st = 0x40;
                a.param = 2;
            } else if (k == 'I') {
                if (s == 'D') a.entityId = at(pc + 3), q = pc + 4;
            } else if (k == 'B') {
                const uint8_t n = at(pc + 3);
                // S[0x2b..0x35] are u16 (title slot summaries) when GD+0x10000 == 0 (assumed: runtime shows 333).
                const uint32_t v = s == 'G' ? (n == 7 ? uint32_t(G[0xfc]) << 8 | G[7] : G[n])
                                 : n > 0x2a && n < 0x36 ? uint32_t(S[n + 1u]) << 8 | S[n] : S[n];
                const uint32_t h = (v & 0xffff) / 100, r = v + h * 0xff9c, t = (r & 0xffff) / 10;  // BCD 0x449b90
                uint32_t bcd = ((r - t * 10) & 0xffff) | t << 4 | h << 8;
                int digits = 8;
                while (!(bcd & 0xf0000000) && digits > 1) bcd <<= 4, --digits;
                for (; digits > 0; --digits, bcd <<= 4) emit(uint8_t('0' + (bcd >> 28)), kAdvance['0' + (bcd >> 28)], pal);
                q = pc + 4;
            } else if (k == 'D' || k == 'J') {
                // Key names: >D n / >DJ n the keyboard key (DIK name table 0x45e030 via 0x402250), >J n the joystick
                // button (0x403260 "BUTTON%i"); >DJ adds " /  Button <digit>" with the joystick enabled (the digit is
                // the DIK name of scan code joyMap + 1). Only glyph bytes are drawn and advance (at most 20), then +6;
                // >J keeps the last glyph's palette, >D sets it to the >CP one.
                const bool dj = k == 'D' && s == 'J';
                const uint8_t n = dj ? at(pc + 3) : s;
                std::string name;
                if (n < 8 && k == 'J') name = std::format("BUTTON{}", joyButtons[n]);
                if (n < 8 && k == 'D') name = keyNames[n];
                if (n < 8 && dj && joystick && joyButtons[n] && joyButtons[n] <= 10)
                    name += std::format(" /  Button {}", joyButtons[n] % 10);
                if (k == 'D') last = pal;
                for (size_t i = 0; i < name.size() && i < 20; ++i)
                    if (kGlyph[uint8_t(name[i])]) emit(uint8_t(name[i]), kAdvance[uint8_t(name[i])], last);
                q = k == 'J' ? pc + 2 : dj ? pc + 4 : pc + 3;
                w = int16_t(w + 6), x = int16_t(x + 6);
            } else {
                q = pc + 1;
            }
            pc = q;  // the loop's ++pc skips the byte after the operand
        }
    }
}

} // namespace bb::game
