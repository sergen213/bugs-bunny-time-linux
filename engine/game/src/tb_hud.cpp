// TB screen fade 0x464ef0, overlay text (S30 0x4680b0, interpreter 0x466d50, class 31 0x450d00, deferred runs
// 0x4681c0), the HUD (class 6 entity 0x463170 with 0x462430 / 0x461840 / 0x461a00 / 0x461b20 / 0x462d70 / 0x462820 /
// 0x463080) and the model camera node 0x405b50. Docs: GAMEPLAY_TB.md §10, RUNTIME_REFERENCE.md §13.
#include "bb/game/tb_world.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace bb::game {
namespace {

// 0x4cee40: glyph (1-based font record) per script byte; 0x4cef40: pen advance per byte.
constexpr uint8_t kGlyph[256] = {
    0x91, 0x8f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8d, 0x8c, 0x85, 0x87, 0x00, 0x00,
    0x8e, 0x8f, 0x90, 0x91, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x3f, 0x40, 0x00, 0x00, 0x41, 0x82, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x4b, 0x4c, 0x00, 0x4d, 0x00, 0x4e,
    0x00, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6b, 0x67, 0x68, 0x69, 0x6a,
    0x6d, 0x6e, 0x00, 0x00, 0x84, 0x83, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x00, 0x00, 0x4f, 0x75, 0x76, 0x77, 0x00, 0x00, 0x78, 0x00, 0x00,
    0x00, 0x79, 0x00, 0x7a, 0x7b, 0x7c, 0x7d, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x7f, 0x00, 0x00, 0x6f,
    0x54, 0x62, 0x52, 0x55, 0x53, 0x00, 0x00, 0x56, 0x59, 0x51, 0x57, 0x58, 0x5c, 0x63, 0x5b, 0x5a,
    0x00, 0x66, 0x5f, 0x64, 0x5d, 0x00, 0x5e, 0x00, 0x00, 0x61, 0x65, 0x60, 0x50, 0x00, 0x00, 0x00};
constexpr uint8_t kAdvance[256] = {
    0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2b, 0x32, 0x13, 0x13, 0x13, 0x13,
    0x0e, 0x12, 0x0c, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x04, 0x00, 0x00, 0x0b, 0x09, 0x04, 0x05, 0x05, 0x0c, 0x0b, 0x04, 0x06, 0x05, 0x07,
    0x0e, 0x08, 0x0b, 0x0a, 0x0a, 0x0a, 0x0c, 0x09, 0x0c, 0x0d, 0x03, 0x04, 0x00, 0x04, 0x00, 0x08,
    0x00, 0x0d, 0x0b, 0x0a, 0x0c, 0x0a, 0x0a, 0x0d, 0x0c, 0x04, 0x0b, 0x0b, 0x0b, 0x0f, 0x0c, 0x0e,
    0x0b, 0x0e, 0x0b, 0x0c, 0x0c, 0x0c, 0x0c, 0x0f, 0x0b, 0x0b, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x0d, 0x0b, 0x0a, 0x0c, 0x0a, 0x0a, 0x0d, 0x0c, 0x04, 0x0b, 0x0b, 0x0b, 0x0f, 0x0c, 0x0e,
    0x0b, 0x0e, 0x0b, 0x0c, 0x0c, 0x0c, 0x0c, 0x0f, 0x0b, 0x0b, 0x0b, 0x07, 0x07, 0x07, 0x07, 0x04,
    0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x42, 0x11, 0x11, 0x11, 0x11,
    0x1c, 0x1c, 0x00, 0x00, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x00, 0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x00, 0x00, 0x06, 0x00, 0x00,
    0x00, 0x0b, 0x00, 0x0e, 0x0e, 0x0e, 0x0e, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x08,
    0x0d, 0x0d, 0x0d, 0x0d, 0x0d, 0x00, 0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x06, 0x06, 0x06, 0x06,
    0x00, 0x09, 0x0e, 0x0e, 0x0e, 0x00, 0x0e, 0x00, 0x00, 0x0c, 0x0c, 0x0c, 0x0c, 0x00, 0x00, 0x00};
// 0x4cf040: font records drawn untinted (button icons; 0x4660c0 sets the tint flag on every other record).
constexpr uint8_t kUntinted[] = {0x6c, 0x6b, 0x67, 0x68, 0x69, 0x6a, 0x6d, 0x6e, 0x84, 0x83, 0x86, 0x88};

// HUD tables (exe initial data; 0x4cebc0 and the bar records are rewritten at run time, copied per world here).
constexpr uint16_t kHudTemplate[15] = {32, 36, 37, 36, 37, 37, 41, 42, 43, 44, 44, 46, 47, 38, 49};  // 0x4ced18
constexpr int16_t kHudModelPos[15][3] = {  // 0x4ceb64: view translation x, y (screen offset), z (depth)
    {-25, -80, 1800}, {-220, 80, 1200}, {215, 80, 1600}, {-220, -80, 1200}, {215, -82, 1600}, {18, -35, 2000},
    {85, -36, 2500},  {85, -36, 2500},  {85, -36, 2500}, {85, -36, 2500},   {85, -36, 2500},  {148, -36, 2600},
    {208, -36, 3000}, {208, -36, 2600}, {0, 82, 1200}};
constexpr uint8_t kHudRot[15] = {11, 0, 1, 2, 3, 4, 5, 5, 5, 5, 5, 6, 7, 8, 11};  // 0x4cee18 (11 = fixed)
constexpr int16_t kHudSpin[10][3] = {{0, -100, 0}, {0, 100, 0}, {0, -90, 0}, {12, 64, 0}, {16, 128, 0},
                                     {0, 68, 0},   {26, 48, 22}, {0, -80, 0}, {70, 18, 0}, {0, 0, 0}};  // 0x4cedc8
constexpr uint8_t kHudFrame0[32] = {13, 12, 14, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0,  0,  0,  0,  0, 0, 10, 0, 0, 0, 17, 0, 0, 16, 19, 20};  // 0x4ced38
constexpr int16_t kDigitW[10] = {29, 15, 27, 23, 24, 28, 26, 27, 26, 23};  // 0x4ceb58
struct Desc { int16_t x, y; int8_t first, n; };                            // 0x4cec40 + 6 * k
constexpr Desc kDesc[6] = {{5, -98, 6, 2}, {-200, 70, 8, 3}, {176, 70, 11, -4}, {-200, -92, 15, 3}, {176, -92, 18, -4},
                           {200, 78, 23, -3}};

uint16_t be16(const std::vector<uint8_t>& d, size_t p) { return p + 2 <= d.size() ? uint16_t(d[p] << 8 | d[p + 1]) : 0; }

}  // namespace

// ------------------------------------------------------------------------------------------------ fade
void TbWorld::fadeTick() {  // 0x464ef0
    Fade& f = fade;
    if (!f.mode) {
        f.dark = S[9] ? 0xe00 : 0;  // paused: dimmed (the 0x4d2764 menu case is not ported)
        return;
    }
    if (f.mode & 4) {
        f.mode = 1, f.speed = 0x100, f.level = 0, f.dark = 0x1000;
    } else if (f.mode & 8) {
        f.mode = 2, f.speed = 0x100, f.level = 0x1000, f.dark = 0;
    } else if (S[0x23] == 0) {  // 0x4d24c3
        if (f.mode & 2) {
            if (f.level <= 0) {
                f.level = 0, f.dark = 0x1000;
                if (f.mode & 0x40) f.mode = 0x20;  // a flash ends; a plain fade-out holds black
            } else {
                f.dark = int16_t(0x1000 - f.level);
                f.level = int16_t(f.level - f.speed);
            }
        } else if (f.mode & 1) {
            if (f.level >= 0x1000) {
                f.level = 0x1000, f.dark = 0;
                f.mode = f.mode & 0x40 ? uint8_t((f.mode & 0xfe) | 2) : 0x10;
            } else {
                f.dark = int16_t(0x1000 - f.level);
                f.level = int16_t(f.level + f.speed);
            }
        }
    }
    if (f.mode & 0x30) f.mode = 0, f.dark = 0;
}

std::array<uint8_t, 4> TbWorld::fadeOverlay() const {
    // 0x464ef0 scales the level light colours (0x4883e0..) and the vertex brightness 0x488250 = (0x1000 - dark) >> 4;
    // the black veil stands in (as LiT). TB has no flash colour: the 0x40 flash of S52 is a dark-light-dark blink.
    const int lit = 0x1000 - fade.dark;
    return {0, 0, 0, uint8_t(255 - std::min(255, lit >> 4))};
}

// ------------------------------------------------------------------------------------------------ overlay text
uint32_t TbWorld::textRun(TbActor* e, uint16_t script, int16_t prio) {  // 0x466d50
    if (prio > 0) {  // deferred to the end of the tick; only the lowest priority value runs (up to 5 scripts)
        if (prio < textQueuePrio_) textQueue_.clear(), textQueuePrio_ = prio;
        if (prio == textQueuePrio_ && textQueue_.size() < 5) textQueue_.push_back({e ? e->uid : UINT32_MAX, script});
        if (e) e->stateTicks = 0;
        return 0;
    }
    const std::vector<uint8_t>& d = textBlock_;
    if (size_t(script) * 2 + 2 > d.size()) return 0;
    size_t p = size_t(d[script * 2u] | d[script * 2u + 1] << 8);
    if (++blink_ >= 0x1e) blink_ = 0;
    const uint8_t pal9 = blink_ > 0xf ? 1 : 0;  // 0x4cf074 = 0x4cf054 (yellow) / 0x4cf050 (white)
    int32_t width = 0, xBase = 0, yBase = 0, orgX = 0, orgY = 0;
    int32_t align = 0, minX = 10000, maxX = -10000, minY = 10000, bottom = -10000;
    int16_t pal = 0, pal2 = 0;
    int8_t cmpVal = -1;
    int cmpMode = 0;
    uint16_t cmpIdx = 0, bitOp = 0, bit = 0;
    uint32_t status = 0x40;
    int minDigits = 0;
    std::string number;
    bool menu = false, fitBox = false;
    std::optional<std::array<int16_t, 4>> box;
    // Pen of the last emission (0x466d50 locals 0x68 / 0x64): set before each op's text even when it is skipped.
    int32_t penX = 0, penY = 0;
    const size_t runStart = glyphs.size();
    size_t lineStart = runStart, lineCount = 0;
    auto shiftLine = [&](int a) {  // right / centre alignment of the current line by its width
        const int32_t w = a == 0 ? 0 : a == 1 ? int16_t(width) >> 1 : width;
        for (size_t i = lineStart; i < glyphs.size(); ++i) glyphs[i].x = int16_t(glyphs[i].x - w);
        return w;
    };
    auto bounds = [&](int32_t w) {  // grow the text's bounding box by the current line
        if (lineStart < glyphs.size()) minX = std::min<int32_t>(minX, glyphs[lineStart].x);
        maxX = std::max(maxX, penX - w);
        minY = std::min(minY, penY);
    };
    auto finishLine = [&](int a) {  // 0x46702c: align the line, grow the box, start a new line
        if (!lineCount) return;
        bounds(shiftLine(a));
        bottom += 0xd;
        width = 0, lineCount = 0, lineStart = glyphs.size();
    };
    auto emit = [&](std::string_view s, int32_t x, int32_t y, int16_t pl) {  // 0x466810
        const int32_t x0 = x;
        for (const char ch : s) {
            const uint8_t b = uint8_t(ch);
            if (const uint8_t g = kGlyph[b]) {
                const bool plain = std::ranges::contains(kUntinted, uint8_t(g - 1));
                glyphs.push_back({b, g, int16_t(x), int8_t(std::clamp(y, -128, 127)), uint8_t(plain ? 5 : pl == 9 ? pal9 : pl)});
                ++lineCount;
                x += kAdvance[b];  // advance x 0x639abc (1.0 at 640x480 and above)
            } else if (b == ' ') {
                x += 6;
            } else if (b == '\t') {
                x += 0x18;
            }
        }
        return x - x0;
    };
    // Ops 0x11 (timed: the last line also grows the box, bottom >= pen y + 16) / 0x18 (end: aligned only): this
    // run's off-screen glyphs dropped, then the box.
    auto finish = [&](int a, bool timed) {
        const int32_t w = shiftLine(a);
        int32_t bot = bottom;
        if (timed) bounds(w), bot = std::max(bottom, penY + 0x10);
        glyphs.erase(std::remove_if(glyphs.begin() + std::ptrdiff_t(runStart), glyphs.end(),
                                    [](const OverlayGlyph& g) { return g.x <= -0x120 || g.x >= 0x100; }),
                     glyphs.end());
        if (box) {
            const auto& b = *box;
            textBoxes.push_back(fitBox ? std::array<int16_t, 4>{int16_t(minX - b[0]), int16_t(minY - b[1]),
                                                               int16_t(maxX - minX + b[0] + b[2]), int16_t(bot - minY + b[1] + b[3])}
                                       : b);
        }
        if (menu) unsupported.insert("text menu 0x1c (0x4668f0)");
    };
    for (int guard = 0; guard < 4096 && p + 3 <= d.size(); ++guard) {
        const uint8_t op = d[p];
        const size_t len = be16(d, p + 1), start = p;
        size_t q = p + 3;
        const int prevAlign = align;
        switch (op) {
        case 1: yBase += 0xd; finishLine(prevAlign); break;
        case 2: if (e) e->id = be16(d, q); q += 2; break;
        case 3: xBase = int16_t(be16(d, q)), q += 2; finishLine(prevAlign); break;
        case 4: yBase = int16_t(be16(d, q)), q += 2; finishLine(prevAlign); break;
        case 5: pal = int8_t(d[q]) == 0x45 ? 9 : int8_t(d[q]), q += 1; break;
        case 6: status = 0x40; break;
        case 7: case 8: case 9:
            xBase = int16_t(be16(d, q)), q += 2;
            align = op == 7 ? 0 : op == 8 ? 1 : 2;
            finishLine(prevAlign);
            break;
        case 10: cmpVal = int8_t(d[q]), q += 1; break;
        case 11: pal2 = int8_t(d[q]) == 0x45 ? 9 : int8_t(d[q]), q += 1; break;
        case 12: case 13: cmpMode = op == 12 ? 1 : 2, cmpIdx = be16(d, q), q += 2; break;
        case 14: orgX = orgY = 0, q += 1; break;  // origins 0x4d59a0[n]: written only by the pause panels (zero here)
        case 15:  // panel page n of the pause screen: x = n * 800 + scroll (scroll 0: page 0 only on screen)
            orgY = 0, orgX = d[q] * 800, q += 1;
            if (orgX > 0x960) orgX -= 4000;
            break;
        case 16:
            if (d[q] & 8) status |= 0x80;
            if (d[q] & 2) status |= 0x20;
            if (d[q] & 1) status |= 0x10;
            if (d[q] & 4) status |= 0x40;
            q += 1;
            break;
        case 0x11: {
            const int16_t n = int16_t(be16(d, q));
            q += 2;
            if (n) {
                if (e) e->stateTicks = 0, e->textTime = n;
                finish(align, true);
                return 0;
            }
            if (e) e->textTime = 2;
            break;
        }
        case 0x12: case 0x13: {
            const uint16_t i = be16(d, q);
            q += 2;
            uint32_t v;
            if (op == 0x12) v = (i & 1) == 0 && i < 0x31 ? uint32_t(GD[0x58u + i] | GD[0x59u + i] << 8) : GD[(0x58u + i) % GD.size()];
            else v = (i & 1) == 0 && i > 0x53 && i < 0x5f ? uint32_t(S[i] | S[i + 1u] << 8) : S[i % S.size()];
            number = std::format("{:0{}}", v % 10000, std::clamp(minDigits, 1, 4));  // at most 4 digits
            break;
        }
        case 0x14: unsupported.insert("text key name 0x14"); q += 1; break;
        case 0x15: case 0x16:
            box = {int16_t(be16(d, q)), int16_t(be16(d, q + 2)), int16_t(be16(d, q + 4)), int16_t(be16(d, q + 6))};
            fitBox = op == 0x15;
            q += 8;
            break;
        case 0x1a: case 0x1b: bitOp = op, bit = be16(d, q), q += 2; break;
        case 0x1c: menu = true, q += 1; break;  // multi-page text box (0x4668f0 / 0x466b20): not ported
        case 0x1d: minDigits = int8_t(d[q]), q += 1; break;
        default: break;  // 0 / 0x17 / 0x19 (second list layer, drawn with the first) / > 0x1d: text only
        }
        if (op == 0x18) {
            finish(prevAlign, false);
            return status;
        }
        int16_t pl = pal;
        if (cmpMode == 1 && int8_t(S[cmpIdx % S.size()]) == cmpVal) pl = pal2;
        if (cmpMode == 2 && int8_t(GD[(0x58u + cmpIdx) % GD.size()]) == cmpVal) pl = pal2;
        if (bitOp) {
            pl = pal;
            const bool set = (500u + (bit >> 3) < GD.size()) && (GD[500u + (bit >> 3)] >> (bit & 7) & 1);
            if (set != (bitOp == 0x1a)) pl = -1;
        }
        penX = width + xBase + orgX, penY = yBase + orgY;
        if (pl == -1 || orgX < -0x1ff || orgX > 0x1ff || penY < -0x83 || penY > 0x77 || menu) {
            number.clear();
        } else {
            if (!number.empty()) width += emit(number, penX, penY, pl), number.clear();
            const size_t end = start + len;
            if (q < end && end <= d.size()) {
                width += emit(std::string_view(reinterpret_cast<const char*>(d.data() + q), end - q), width + xBase + orgX, penY, pl);
                penX = width + xBase + orgX;
            }
        }
        if (!len) break;
        p = start + len;
    }
    return 0;
}

uint32_t TbWorld::textSpawn(uint16_t script, int16_t prio) {  // 0x4680b0 (new entity)
    TbActor* e = spawn(0x18, {}, 0);
    if (!e) return 0;
    e->textScript = uint16_t(script | 0x8000), e->textPrio = prio, e->textTime = 0;
    const uint32_t st = textRun(e, script, prio);
    if (prio > 0 && size_t(script) * 2 + 2 <= textBlock_.size()) {  // display time from the script's op 0x11
        e->stateTicks = 0;
        size_t p = size_t(textBlock_[script * 2u] | textBlock_[script * 2u + 1] << 8);
        for (int guard = 0; guard < 4096 && p + 5 <= textBlock_.size() && textBlock_[p] != 0x11; ++guard) {
            const size_t len = be16(textBlock_, p + 1);
            if (!len) break;
            p += len;
        }
        if (p + 5 <= textBlock_.size() && textBlock_[p] == 0x11) e->textTime = int16_t(be16(textBlock_, p + 3));
    }
    return (st & 0x80 ? 8u : 0u) | (st & 0x20 ? 2u : 0u) | (st & 0x10 ? 1u : 0u) | (st & 0x40 ? 4u : 0u);
}

void TbWorld::textTick(TbActor& a) {  // 0x450d00
    if (a.textTime > 0 && a.stateTicks == uint16_t(a.textTime)) {
        a.f20 |= 0x2000;  // no continuation page (+0x17c is only set by the unported multi-page op 0x1c)
        return;
    }
    if (a.textScript & 0x8000) {  // the spawner drew it this tick
        a.textScript &= 0x7fff;
        return;
    }
    const uint16_t keep = a.stateTicks;
    textRun(&a, a.textScript, a.textPrio);
    a.stateTicks = keep;
}

void TbWorld::textFlush() {  // 0x4681c0
    const std::vector<TextJob> jobs = std::exchange(textQueue_, {});
    textQueuePrio_ = 32000;
    for (const TextJob& j : jobs) {
        TbActor* e = j.uid == UINT32_MAX ? nullptr : entity(int32_t(j.uid));
        const uint16_t keep = e ? e->stateTicks : 0;
        textRun(e, j.script, 0);
        if (e) e->stateTicks = keep;
    }
}

// ------------------------------------------------------------------------------------------------ HUD
bool TbWorld::slideStep(Slide& s) {  // 0x461840
    int8_t acc = s.acc, decel = s.decel, v = s.vel;
    int32_t tgt, other;
    bool back, over;
    if (s.flags & 4) tgt = s.in, other = s.out, back = s.flags & 0x10, over = s.flags & 0x40;
    else if (s.flags & 8) tgt = s.out, other = s.in, back = s.flags & 0x20, over = s.flags & 0x80;
    else return false;
    int16_t* axis = s.flags & 1 ? &s.cur[0] : s.flags & 2 ? &s.cur[1] : nullptr;
    if (!axis) return false;
    int32_t pos = *axis;
    if (int16_t(pos) == int16_t(tgt) && v == 0) {
        s.flags &= ~0x100;
        return false;
    }
    bool neg = false;
    if (int16_t(pos) > int16_t(tgt) || (int16_t(pos) == int16_t(tgt) && v < 0)) tgt = -tgt, v = int8_t(-v), other = -other, pos = -pos, neg = true;
    if (int16_t(pos) == int16_t(other) && v == 0) {
        v = back ? int8_t(-s.back) : s.maxv;  // leaving the far end: full speed (or reversed)
        s.flags |= 0x300;
    } else {
        int8_t add = acc;
        if (v < 0 && (int16_t(other) > int16_t(tgt) || int16_t(other) < int16_t(pos))) add = decel, decel = acc;
        v = int8_t(v + add);
        const int32_t t = int16_t(tgt), x = int16_t(pos);
        if (x + v > t) {
            v = int8_t(v - int8_t(uint32_t(((v - t + x) << 12) / v * decel) >> 12));
            if (v + x < t) v = int8_t(int8_t(pos) - int8_t(tgt));
        }
        if (v > s.maxv) v = s.maxv;
    }
    pos = int16_t(pos + v);
    if (int16_t(pos) >= int16_t(tgt) && (!over || v < s.acc)) {
        v = 0, pos = tgt;
        if (s.flags & 8) s.flags &= ~0x200;
    }
    if (neg) pos = -pos, v = int8_t(-v);
    s.vel = v;
    *axis = int16_t(pos);
    return true;
}

void TbWorld::hudSetSprite(size_t i, uint8_t frame, const std::array<int16_t, 4>* clip) {  // 0x461a00
    HudSprite& sp = hudSprites[i];
    const auto wh = frame < hudFrames_.size() ? hudFrames_[frame] : std::array<int16_t, 2>{1, 1};
    int16_t w = int16_t(wh[0] - 1), h = int16_t(wh[1] - 1), ox = 0, oy = 0;
    if (clip) w = std::min(w, (*clip)[2]), h = std::min(h, (*clip)[3]), ox = (*clip)[0], oy = (*clip)[1];
    sp = {frame, true, int16_t(sp.x + ox), int16_t(sp.y + oy), w, h, ox, oy};
}

void TbWorld::hudMove(size_t i, int16_t x, int16_t y) { hudSprites[i].x = x, hudSprites[i].y = y; }

void TbWorld::hudDigits(uint32_t value, size_t di, const Slide& at) {  // 0x461b20
    const Desc& c = kDesc[di];
    const bool right = c.n < 0;
    const int n = std::abs(c.n);
    int count = 0;
    for (uint32_t p10 = 1; count < n; ++count, p10 *= 10) {
        if (value < p10 && p10 != 1) break;
        const uint32_t dgt = value % (p10 * 10) / p10;
        hudSetSprite(size_t(c.first + count), uint8_t(dgt));
        hud_.pos[size_t(c.first + count)] = {kDigitW[dgt], c.y};
    }
    int16_t x = c.x;
    if (right) {
        for (int k = 0; k < count; ++k) {
            auto& ps = hud_.pos[size_t(c.first + k)];
            x = int16_t(x - ps[0]);
            ps[0] = x;
            hudMove(size_t(c.first + k), int16_t(x + at.cur[0]), int16_t(ps[1] + at.cur[1]));
        }
    } else {
        for (int k = count - 1; k >= 0; --k) {
            auto& ps = hud_.pos[size_t(c.first + k)];
            const int16_t w = ps[0];
            ps[0] = x;
            hudMove(size_t(c.first + k), int16_t(x + at.cur[0]), int16_t(ps[1] + at.cur[1]));
            x = int16_t(x + w);
        }
    }
    for (int k = count; k < n; ++k) hudSprites[size_t(c.first + k)].shown = false;
}

void TbWorld::hudCounter(Slide& s, size_t slot, size_t di, uint32_t value, bool changed) {  // 0x462d70
    HudItem& m = hud[slot];
    uint16_t& timer = hud_.timer[slot];
    if (!(hud_.cea & 2)) {
        if (changed || (hud_.cea & 1)) s.flags = uint16_t((s.flags & ~8) | 4), timer = hud_.cea & 1 ? 2 : 0x3c;
    } else {
        timer = 0;
    }
    const bool moved = slideStep(s);
    if (!moved && timer && (s.flags & 0x200)) --timer;
    if (!timer) s.flags = uint16_t((s.flags & ~4) | 8);
    const Desc& c = kDesc[di];
    if (!(s.flags & 0x200)) {
        if (m.shown) {
            m.shown = false;
            for (int k = 0; k < std::abs(c.n); ++k) hudSprites[size_t(c.first + k)].shown = false;
        }
        return;
    }
    if (!m.shown) changed = true, m.shown = true;
    else if (!changed && !moved) return;
    m.x = int16_t(kHudModelPos[slot][0] + s.cur[0]), m.y = int16_t(kHudModelPos[slot][1] + s.cur[1]);
    if (changed) {
        hudDigits(value, di, s);
        return;
    }
    for (int k = 0; k < std::abs(c.n); ++k) {
        const size_t i = size_t(c.first + k);
        if (hudSprites[i].shown) hudMove(i, int16_t(hud_.pos[i][0] + s.cur[0]), int16_t(hud_.pos[i][1] + s.cur[1]));
    }
}

void TbWorld::hudHealth(size_t hi, Slide& s, int32_t value) {  // 0x462820
    HealthRec& h = hud_.health[hi];
    bool first = false, panelIcon = false, changed = false;
    const int16_t target = int16_t(value * h.scale / std::max<int>(1, h.max));
    if (target != h.shown) {
        if (h.shown == -1) h.shown = target, first = changed = true;
        else h.shown = int16_t(h.shown + (h.shown < target ? 1 : -1)), changed = true;
    }
    const TbActor& F = actors[size_t(focus())];
    if ((hud_.focusId == 2 && h.panel == 1) || (hud_.focusId == 3 && h.panel == 3)) panelIcon = true;
    bool icon = true;  // the call-friend icon frame logic below (LAB_00462904)
    if (hud_.focusId == F.id) {
        icon = panelIcon;
    } else if (!panelIcon) {
        hud_.focusId = uint8_t(F.id);
        changed = panelIcon = true;
        hud_.iconX = int16_t((hud_.focusId != 2 ? 0x12a : 0) - 0xa0);
    }
    if (icon) {
        const TbActor& P = actors[size_t(1 - focus())];
        const bool ready = !GD[0x9f] && !(S[3] & 3) && airTicks < 3 && !(F.f24 & 0x40) && !(P.f20 & 0x2000000) && cooldown > 0x2c;
        if (ready) {
            if (hud_.callBusy) hud_.callBusy = 0, hud_.callTicks = 1;
            else if (hud_.callTicks < 5) ++hud_.callTicks;
        } else {
            if (!hud_.callBusy) hud_.callBusy = 1, hud_.callTicks = 1;
            else if (hud_.callTicks < 5) ++hud_.callTicks;
        }
        if (hud_.callTicks == 4) hudSetSprite(26, ready ? 0x11 : 0x12);
    }
    bool trig = false;
    if (h.link < 0) {
        trig = changed && !first;
    } else if (changed && !first) {
        h.flags |= 2, trig = true;
    } else if (HealthRec& l = hud_.health[size_t(h.link)]; l.flags & 2) {
        l.flags &= ~2, changed = true, trig = !first;
    } else {
        trig = false;
    }
    if (!(hud_.cea & 2)) {
        if (trig || (hud_.cea & 1)) s.flags = uint16_t((s.flags & ~8) | 4), h.timer = hud_.cea & 1 ? 2 : 0x3c;
    } else {
        h.timer = 0;
    }
    const bool moved = slideStep(s);
    if (!moved && h.timer && (s.flags & 0x200)) --h.timer;
    if (!h.timer) s.flags = uint16_t((s.flags & ~4) | 8);
    HudSprite &bar = hudSprites[h.bar], &panel = hudSprites[h.panel], &ic = hudSprites[26];
    if (panelIcon && (moved || changed)) {
        if (!(s.flags & 0x200)) ic.shown = false;
        else hudMove(26, int16_t(hud_.iconX + s.cur[0]), int16_t(hud_.iconY + s.cur[1]));
    }
    if (!(s.flags & 0x200)) {
        panel.shown = bar.shown = false;
        if (panelIcon) ic.shown = false;
        return;
    }
    panel.shown = true;
    if (h.shown) bar.shown = true;
    if (panelIcon) ic.shown = true;
    if (!changed && !moved && (h.flags & 4)) return;
    hudMove(h.panel, int16_t(hud_.pos[h.panel][0] + s.cur[0]), int16_t(hud_.pos[h.panel][1] + s.cur[1]));
    if (changed || !(h.flags & 4)) {
        if (h.shown == 0) {
            bar.shown = false;
        } else {
            const std::array<int16_t, 4> clip{int16_t(h.flags & 1 ? h.scale - h.shown : 0), 0, h.shown, 200};
            hudSetSprite(h.bar, h.frame, &clip);
            hud_.pos[h.bar] = {int16_t(hud_.pos[h.panel][0] + int8_t(h.dx)), int16_t(hud_.pos[h.panel][1] + int8_t(h.dy))};
            if (h.flags & 1) hud_.pos[h.bar][0] = int16_t(hud_.pos[h.bar][0] + h.scale - h.shown);
        }
        h.flags |= 4;
    }
    hudMove(h.bar, int16_t(hud_.pos[h.bar][0] + s.cur[0]), int16_t(hud_.pos[h.bar][1] + s.cur[1]));
}

void TbWorld::hudSetup() {  // 0x462430 (HUD sheet holder template 0x1f; the second sheet, template 0x28, not ported)
    GD[0xac] &= 0xfe;
    for (size_t k = 0; k < hud.size(); ++k)
        hud[k] = {.tmpl = kHudTemplate[k], .x = kHudModelPos[k][0], .y = kHudModelPos[k][1], .z = kHudModelPos[k][2]};
    constexpr Slide kSlide[11] = {
        {{0, -120}, 0, -120, 0x62, 36, 0, 5, 9, 13}, {{0, -120}, 0, -120, 0x62, 36, 0, 5, 9, 13},
        {{0, -120}, 0, -120, 0x62, 28, 0, 5, 10, 11}, {{-120, 0}, 0, -120, 0x61, 32, 0, 6, 11, 8},
        {{120, 0}, 0, 120, 0x61, 34, 0, 5, 12, 9},    {{-120, 0}, 0, -120, 0x61, 30, 0, 5, 11, 7},
        {{120, 0}, 0, 120, 0x61, 32, 0, 6, 12, 8},    {{0, 120}, 0, 120, 0x62, 26, 0, 4, 11, 6},
        {{160, 0}, 0, 160, 0x61, 34, 0, 5, 12, 9},    {{0, -120}, 0, -120, 0x62, 36, 0, 5, 9, 13},
        {{0, -120}, 0, -120, 0x62, 36, 0, 5, 9, 13}};
    hud_ = {};
    std::ranges::copy(kSlide, hud_.slide.begin());
    // 0x4ced78.. bars: Bugs, Taz, S[6] (sheet 0x28), Bugs S[0x6d], Taz S[0x6e]; linked pairs share a panel.
    hud_.health = {{{0, 1, -1, 0, 0, 0x39, 0x16, 6, 0x50, 0x0d, 3}, {2, 3, -1, 0, 1, 3, 0x18, 6, 0x50, 0x0e, 4},
                    {0x1b, 0x1c, -1, 0, 1, 5, 0x14, 0x80, 0x58, 1, -1}, {4, 1, -1, 0, 0, 0x39, 0x1f, 0xf0, 0x50, 0x0d, 0},
                    {5, 3, -1, 0, 1, 3, 0x21, 0xf0, 0x50, 0x0e, 1}}};
    for (size_t k = 0; k < 10; ++k) hud_.spd[k] = {kHudSpin[k][0], kHudSpin[k][1], kHudSpin[k][2]};
    const int16_t pos0[][3] = {{1, -244, -102}, {3, 132, -104}, {22, 200, 78}, {26, -160, -104}, {28, 112, 45},
                               {29, -16, -16},  {30, -16, -16}, {31, -20, 80}};  // 0x4cebc0 non-zero entries
    for (const auto& p : pos0) hud_.pos[size_t(p[0])] = {p[1], p[2]};
    for (size_t i = 0; i < hudSprites.size(); ++i) {
        hudSprites[i] = {};
        if (kHudFrame0[i]) hudSetSprite(i, kHudFrame0[i]), hudMove(i, hud_.pos[i][0], hud_.pos[i][1]);
        hudSprites[i].shown = false;  // +6 = 0x8010
    }
    S[0x6d] = S[0x6e] = 0;
    hud_.init = true;
}

void TbWorld::hudTick() {  // 0x463170 (class 6 entity, first in the list)
    const int level = int(GD[0]);
    if (!hud_.init && (S[4] & 0x10)) return;
    if (level == 0 || level == 0x5d) return;  // title / credits screens (0x462150 / 0x4622d0): not ported
    if (S[4] & 0x10) {  // cut-scene mode: only finish moving what is still on screen
        const bool any = std::ranges::any_of(hudSprites, [](const HudSprite& s) { return s.shown; }) ||
                         std::ranges::any_of(hud, [](const HudItem& h) { return h.shown; });
        if (!any) return;
    }
    if (uint16_t(GD[0x58] | GD[0x59] << 8) > 100) GD[0x58] = 100, GD[0x59] = 0;
    if (restart) return;
    if (!hud_.init) hudSetup();
    if (hudFrames_.empty()) return;  // no HUD sheet in this level (0x4d5d74 == 0)
    // 0x461db0: S[4] mode changes; the pause-screen panels (S[4] 2/4/8, 0x461db0 slides) are not ported.
    if (S[4] != hud_.cea) {
        if ((S[4] & 0xe) || (hud_.cea & 2)) unsupported.insert("hud panel S[4]");
        hud_.cea = S[4];
    }
    hudSprites[29].shown = false;  // snipe-mode cross-hair: look mode (0x4d4fd4 & 2) is not simulated
    if (!(GD[0xac] & 1)) {
        hudSprites[31].shown = false;
        hud[14].shown = false;
    } else if (!S[9]) {  // 0x463080 stopwatch: S[0x75] seconds, one tick of 30 per second
        if (S[0x75] != hud_.watchPrev) hud_.watchTicks = 0;
        hudSprites[31].shown = true;
        hud[14].shown = true;
        if (S[0x75]) ++hud_.watchTicks;
        if (hud_.watchTicks > 0x1d) hud_.watchTicks = 0, --S[0x75];
        hud[14].rot = {0, 0, int16_t((int16_t(S[0x75] * 0x1e - hud_.watchTicks) * 0x1000) / 0x708)};
        hud[14].x = 0, hud[14].y = 82, hud[14].z = 1200;  // 0x4cebb8 (= the slot 14 table entry)
        hud_.watchPrev = S[0x75];
    }
    const uint8_t keep = hud_.cea;
    hud_.cea &= 0xfd;
    const int8_t lives = int8_t(GD[0xa0]);
    hudCounter(hud_.slide[2], 0, 0, uint8_t(lives), hud_.lives != lives && hud_.lives != -1);
    hud_.cea = keep;
    hud_.lives = lives;
    if (GD[0x9e] && !(S[4] & 0x10)) hud_.cea |= 1;
    for (size_t p = 0; p < 2; ++p) {  // Bugs (bars 0, 3) and Taz (1, 4): health and the S[0x6d] / S[0x6e] bar
        const TbActor& pl = actors[p];
        hudHealth(p, hud_.slide[p], pl.hp);
        hudHealth(p + 3, hud_.slide[p + 9], S[0x6d + p]);
        const uint8_t fl = flash[p];
        if ((fl & (p ? 0x80 : 0x40)) && hudSprites[hud_.health[p].panel].shown && (fl & 0x3c) < 0x3c)
            hudSprites[hud_.health[p].bar].shown = fl & 4;
    }
    uint8_t s7 = S[7];
    if (s7 != hud_.s7 && (hud_.s7 = s7) != 0 && S[6]) hud_.health[2].shown = -1;
    if (s7 && !S[6] && hud_.health[2].shown == 0) s7 = 0, hud_.s7 = 0;
    if (!(keep & 3) && s7) hud_.cea |= 1;
    else hud_.cea &= 0xfc;
    if ((s7 || (hud_.cea & 1) || (hud_.slide[8].flags & 0x200)) && hud_.health[2].bar)
        unsupported.insert("hud S[6] bar (sheet template 0x28)");
    const uint16_t v1 = uint16_t(GD[0x58] | GD[0x59] << 8);
    if (hud_.slot1 != v1 && hud_.slot1 != 0xffff) hud_.spd[0][1] = -600;
    hud_.cea = keep;
    hudCounter(hud_.slide[3], 1, 1, v1, hud_.slot1 != v1 && hud_.slot1 != 0xffff);
    if (hud_.spd[0][1] < -100) hud_.spd[0][1] = int16_t(hud_.spd[0][1] + 10);
    hud_.slot1 = v1;
    const uint8_t gi = GD[0xa9];  // the level's collectible counter (ARCA01: 2 = GD+0x5a)
    const uint16_t v2 = uint16_t(GD[(0x58u + gi) % GD.size()] | GD[(0x59u + gi) % GD.size()] << 8);
    if (hud_.slot2 != v2 && hud_.slot2 != 0xffff) hud_.spd[1][1] = 600;
    hudCounter(hud_.slide[4], 2, 2, v2, hud_.slot2 != v2 && hud_.slot2 != 0xffff);
    hud_.slot2 = v2;
    if (hud_.spd[1][1] > 100) hud_.spd[1][1] = int16_t(hud_.spd[1][1] - 10);
    if (hud_.cea & 0x62) unsupported.insert("hud era selector");
    else for (size_t k : {5u, 6u, 7u, 8u, 9u, 11u, 12u, 13u}) hud[k].shown = false;
    for (size_t k = 0; k < 10; ++k)
        for (size_t c = 0; c < 3; ++c) hud_.ang[k][c] = int16_t(hud_.ang[k][c] + hud_.spd[k][c]);
    for (size_t k = 0; k < hud.size(); ++k)
        if (kHudRot[k] != 11 && hud[k].shown)
            hud[k].rot = {hud_.ang[kHudRot[k]][0], hud_.ang[kHudRot[k]][1], hud_.ang[kHudRot[k]][2]};
}

// ------------------------------------------------------------------------------------------------ camera node
void TbWorld::cameraNodes() {  // 0x405b50 (load-mode frames): a part with flag 0x40 is the camera
    for (TbActor* a : order()) {
        if (!a->active) continue;
        const std::vector<TodPart>& parts = a->anim.parts;
        for (size_t i = 0; i < parts.size(); ++i) {
            if ((parts[i].flags & 0x8000) || !(parts[i].flags & 0x40)) continue;
            auto world = [&](double z) {  // the part's coordinate chain applied to (0, 0, z), then the entity root
                double v[3] = {0, 0, z};
                for (int32_t k = int32_t(i), guard = 0; k >= 0 && size_t(k) < parts.size() && guard < 64; ++guard) {
                    const TodPart& t = parts[size_t(k)];
                    const double x = v[0], y = v[1], w = v[2];
                    for (int r = 0; r < 3; ++r) v[r] = t.m[size_t(r * 3)] * x + t.m[size_t(r * 3 + 1)] * y + t.m[size_t(r * 3 + 2)] * w + t.trans[size_t(r)];
                    k = t.parent;
                }
                const V3 r = rotY(a->yaw, int32_t(std::lround(v[0])), int32_t(std::lround(v[1])), int32_t(std::lround(v[2])));
                return V3{a->pos.x + r.x, a->pos.y + r.y, a->pos.z + r.z};
            };
            camScript.flags |= 0x100;
            cam.eye = world(0);
            cam.look = world(1000);
        }
    }
}

}  // namespace bb::game
