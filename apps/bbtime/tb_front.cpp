// TB original front end; see tb_front.h and docs/re/TIME_BUSTERS.md §8 (addresses are bblit2.exe).
#include "tb_front.h"

#include "bb/audio.h"
#include "bb/bze.h"
#include "bb/game/pad.h"
#include "bb/game/tb_menu.h"
#include "bb/render/bmp.h"
#include "music.h"
#include "settings.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <format>
#include <map>
#include <print>

namespace fs = std::filesystem;
using namespace bb::game;

namespace bbtime {

namespace {

constexpr int kW = 640, kH = 480;

struct Rgb { uint8_t r, g, b; };
// Text colours: the RGB triples at 0x48b0d8.. (red, blue, hot blue, checked, checked hot, list selection) and the
// immediates of the draw functions (0x419c30 disabled 0x968080, 0x41a2a0 keys, 0x41adc0 slider fill 0x3300b5).
constexpr Rgb kRed{242, 31, 16}, kBlue{8, 25, 204}, kHot{9, 128, 237}, kChecked{49, 174, 241}, kCheckedHot{175, 238, 253},
              kListSel{248, 230, 0}, kGrey{128, 128, 150}, kKeyLabel{247, 176, 72}, kKeyCapture{128, 64, 255},
              kKeyHot{32, 0, 255}, kKeyOff{160, 160, 160}, kWhite{255, 255, 255}, kFill{181, 0, 51};

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

// GDI CreateFontIndirect with a positive lfHeight (cell = win ascent + descent) as the Wine oracle renders it:
// ppem = lfHeight · em / (winAscent + winDescent) truncated (30 -> 25), normally hinted glyphs blended by their 8-bit
// coverage, full colour from 240 (≈ 15/16) up (fitted to the oracle's menu text: solid pixels IoU 0.99, mean error
// 3/255 over "1 Player", local/wine/tbmenu), hinted integer advances, no kerning (DrawText ignores it).
struct Font {
    FT_Face face = nullptr;
    int ascent = 0, cell = 0;  // tmAscent, tmHeight (GetTextExtentPoint32 cy)

    bool open(FT_Library lib, const fs::path& file, int lfHeight) {
        if (FT_New_Face(lib, file.c_str(), 0, &face)) return false;
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        const int asc = os2 ? os2->usWinAscent : face->ascender, desc = os2 ? os2->usWinDescent : -face->descender;
        const int ppem = lfHeight < 0 ? -lfHeight : lfHeight * face->units_per_EM / (asc + desc);  // < 0: em height (GDI)
        ascent = int(std::lround(double(asc) * ppem / face->units_per_EM));
        cell = ascent + int(std::lround(double(desc) * ppem / face->units_per_EM));
        FT_Set_Pixel_Sizes(face, 0, FT_UInt(ppem));
        return true;
    }
    void close() { if (face) FT_Done_Face(face), face = nullptr; }

    int width(std::string_view s) const {
        int w = 0;
        for (const char c : s)
            if (!FT_Load_Char(face, uint8_t(c), FT_LOAD_DEFAULT)) w += int((face->glyph->advance.x + 32) >> 6);
        return w;
    }
    // Pen at the cell's top-left (DrawText rect top), Latin-1 text (menu.men bytes).
    void draw(bb::Texture& t, int x, int y, std::string_view s, Rgb col) const {
        const int base = y + ascent;
        for (const char c : s) {
            if (FT_Load_Char(face, uint8_t(c), FT_LOAD_DEFAULT | FT_LOAD_RENDER)) continue;
            const FT_GlyphSlot g = face->glyph;
            const FT_Bitmap& b = g->bitmap;
            for (unsigned r = 0; r < b.rows; ++r)
                for (unsigned k = 0; k < b.width; ++k) {
                    const uint8_t* row = b.buffer + r * size_t(b.pitch);  // MONO: an embedded bitmap strike (Wine's Tahoma)
                    const unsigned v = b.pixel_mode == FT_PIXEL_MODE_MONO ? (row[k / 8] >> (7 - k % 8) & 1) * 255u : row[k];
                    const unsigned a = v >= 240 ? 255 : v;
                    const int px = x + g->bitmap_left + int(k), py = base - g->bitmap_top + int(r);
                    if (!a || px < 0 || py < 0 || px >= kW || py >= kH) continue;
                    uint8_t* p = &t.rgba[(size_t(py) * kW + size_t(px)) * 4];
                    p[0] = uint8_t((p[0] * (255 - a) + col.r * a) / 255), p[1] = uint8_t((p[1] * (255 - a) + col.g * a) / 255),
                    p[2] = uint8_t((p[2] * (255 - a) + col.b * a) / 255), p[3] = 255;
                }
            x += int((g->advance.x + 32) >> 6);
        }
    }
};

// bong.wav: RIFF PCM (16-bit mono 11025 Hz in the shipped file); empty on anything else.
bb::AiffSound loadWav(const fs::path& file) {
    bb::AiffSound s;
    std::vector<uint8_t> b;
    try { b = bb::readFile(file); } catch (const std::exception&) { return s; }
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return s;
    auto u16 = [&](size_t i) { return uint16_t(b[i] | b[i + 1] << 8); };
    auto u32 = [&](size_t i) { return uint32_t(u16(i) | u16(i + 2) << 16); };
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t n = u32(at + 4);
        if (!std::memcmp(&b[at], "fmt ", 4) && n >= 16 && u16(at + 8) == 1)
            s.channels = u16(at + 10), s.rate = u32(at + 12), s.bits = u16(at + 22);
        else if (!std::memcmp(&b[at], "data", 4) && s.bits == 16) {
            const size_t len = std::min<size_t>(n, b.size() - at - 8) / 2;
            s.pcm.resize(len);
            for (size_t i = 0; i < len; ++i) s.pcm[i] = int16_t(u16(at + 8 + 2 * i));
        }
        at += 8 + n + (n & 1);
    }
    if (s.bits != 16) s.pcm.clear();
    return s;
}

struct Box {
    int x = 0, y = 0, w = 0, h = 0;
    bool in(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

// KeyId -> pad bit of the TB keyboard maps (config.pc slots of 0x415fd0: P1_/P2_ + PS1 button name).
uint16_t padBit(std::string_view id) {
    static const std::map<std::string_view, uint16_t> kBits = {
        {"up", PadUp}, {"down", PadDown}, {"left", PadLeft}, {"right", PadRight}, {"Croix", PadCross},
        {"Carre", PadSquare}, {"Rond", PadCircle}, {"Triangle", PadTriangle}, {"L1", PadL1}, {"R1", PadR1},
        {"L2", PadL2}, {"R2", PadR2}};
    const auto it = id.size() > 3 ? kBits.find(id.substr(3)) : kBits.end();
    return it == kBits.end() ? 0 : it->second;
}

// 0x41ab70: RESOLUTION_0..4 -> config.pc +0x10 (u16 w, u16 h).
constexpr std::pair<int, int> kRes[5] = {{320, 240}, {512, 384}, {640, 480}, {800, 600}, {1024, 768}};

// bblit2.exe string table 1 (caption) and 4 (text) of the WM_CLOSE box (0x42e480), one '~' segment per language
// (config.pc +0x08), Latin-1 like menu.men.
constexpr std::string_view kQuitCaption =
    "Bugs Bunny & Taz - Time Busters ~ Bugs Bunny et Taz - La Spirale du Temps ~ Bugs Bunny & Taz - Im Wettlauf gegen "
    "die Zeit ~ Bugs Bunny & Taz: La Espiral del Tiempo ~ Bugs Bunny & Taz - In Viaggio Nel Tempo ~ Pernalonga e Taz - "
    "Time Busters ~ Bugs Bunny & Taz - Op avontuur door de tijd ~ ";
constexpr std::string_view kQuitText = "Do you want to quit? ~ Veux-tu quitter ? ~ M\xf6" "chtest du abbrechen? ~ \xbf"
                                       "Deseas salir? ~ Vuoi uscire? ~ Voc\xea quer sair? ~ Wil je stoppen? ~ ";

} // namespace

struct TbFront::Impl {
    FT_Library ft = nullptr;
    Font big, list;  // 0x416850: 0x4d1dd8 (23 pt at 96 dpi -> 30 px cell), 0x4d1de0 (20 pt -> 26 px, list box)
    TbMenu menu;
    fs::path saves;
    std::map<std::string, fs::path> files;  // lower-case name -> Datas/data file (menu.men mixes case)
    std::map<std::string, bb::Texture> bmps;
    KeyMap* keys = nullptr;
    bool mute = false;
    int page = 0, language = 0;  // registry language, 0 = English
    int hot = -1, hotRow = -1;   // hovered / focused item (0x6391b0) and list row (+0xf4)
    int listSel = -1, listTop = 0;  // list box +0xfc selection, +0xec first shown row
    std::vector<TbSaveSlot> slots;
    float music = 1.0f, sfx = 1.0f, contrast = 0.5f;  // slider +0xf0 (0..1)
    int capture = -1;  // key item waiting for its key (0x4d1dd4)
    bool dirty = true;
    bb::Texture canvas;
    bb::AiffSound bong;
    // Video page = settings.ini width / height, tb_renderer, tb_details, tb_brightness (the config.pc fields).
    int resW = 0, resH = 0, renderer = 2, details = 2;
    float brightness = 1.0f;  // config.pc +0xe0 (0x639c20)
    // WM_CLOSE (0x42e480): MessageBox(string 4, string 1, MB_YESNO | MB_ICONQUESTION | MB_TOPMOST), Yes -> state 0.
    bool quitBox = false, quitExit = false;  // open; Yes leaves the app (window close) instead of the front end
    int quitFocus = 0, mx = -1, my = -1;     // focused button (0 Yes = default, 1 No); last mouse point
    Font ui, uiIcon;                         // the box's font (Wine: Tahoma 8 pt = ppem 11) and its icon's '?'
    // Attract idle (0x41c0f0, idle()): due at GetTickCount + 30000, re-armed by WM_KEYDOWN (0x6396e8) and button-down
    // messages (0x6396a4), not by mouse moves.
    uint64_t idleDue = 0;
    bool demoLogged = false;

    ~Impl() {
        big.close(), list.close(), ui.close(), uiIcon.close();
        if (ft) FT_Done_FreeType(ft);
    }

    const bb::Texture* bmp(const std::string& name) {
        if (name.empty()) return nullptr;
        const std::string key = lower(name);
        if (auto it = bmps.find(key); it != bmps.end()) return it->second.width ? &it->second : nullptr;
        bb::Texture& t = bmps[key];
        if (auto f = files.find(key); f != files.end())
            if (auto b = bb::render::loadBmp(f->second)) t = std::move(*b);
        return t.width ? &t : nullptr;
    }

    const TbMenuItem& item(int i) const { return menu.items[size_t(i)]; }
    template <class F> void forPage(F f) const {
        for (const auto& g : menu.pages[page].groups)
            for (const size_t i : g) f(int(i));
    }

    // +0x70.. rect: Up image size (0x416e30) except sliders (With/Height re-read), ISTEXT measured when 0 (0x416e30
    // line `+0x78 == 0`); a centred ISTEXT hit-tests x ± w/2 (0x41bc40).
    Box rect(const TbMenuItem& it) {
        Box b{it.x, it.y, it.w, it.h};
        if (it.type != TbItemType::Slide)
            if (const bb::Texture* t = bmp(it.up)) b.w = int(t->width), b.h = int(t->height);
        if (it.type == TbItemType::Text) {
            const std::string s = it.text(language);
            if (b.w <= 0) b.w = big.width(s);
            if (b.h <= 0) b.h = big.cell;
            if (it.align == 5) b.x -= b.w / 2;
        }
        return b;
    }
    Box checkBox(const TbMenuItem& it) {  // 0x419950: CHECKBOX_ITEM_0 (Align=left) left of the text, centred + 3
        const bb::Texture* t = bmp(menu.checkbox.up);
        const int bw = t ? int(t->width) : 16, bh = t ? int(t->height) : 16;
        return {it.x - bw - 7, it.y + it.h / 2 - bh / 2 + 3, bw + 8, bh};
    }
    Box imageLabel(const TbMenuItem& it) {  // 0x4195e0: the text centred under the image
        const Box b = rect(it);
        const int tw = big.width(it.text(language));
        return {b.x + b.w / 2 - tw / 2, b.y + b.h, tw, big.cell};
    }

    std::string family(const TbMenuItem& it, int* index) const {  // LANGUAGE_n / RESOLUTION_n / ... (0x416e30)
        for (const char* f : {"LANGUAGE_", "RESOLUTION_", "RENDERER_", "DETAILS_"})
            if (it.name.starts_with(f) && it.name.size() > std::strlen(f) && std::isdigit(uint8_t(it.name[std::strlen(f)]))) {
                *index = std::atoi(it.name.c_str() + std::strlen(f));
                return f;
            }
        return {};
    }
    // +0xa8 (checked). Language = the menu language; Video = the settings (window size, renderer, detail level); the
    // Controls pages' player / control-type checks follow the page shown (2 P1 keys, 5 P2, 6 joystick).
    bool checked(const TbMenuItem& it) const {
        int n = 0;
        const std::string f = family(it, &n);
        if (f == "LANGUAGE_") return n == language;
        if (f == "RESOLUTION_") return n < 5 && kRes[n] == std::pair{resW, resH};
        if (f == "RENDERER_") return n == (renderer == 2 ? 0 : 1);  // 0x41ab70: RENDERER_0 checked -> 2, else 1
        if (f == "DETAILS_") return n == details;
        if (it.name.starts_with("PLAYER1_CHECK")) return page != 5;
        if (it.name.starts_with("PLAYER2_CHECK")) return page == 5;
        if (it.name.starts_with("KEYB_CHECK")) return page != 6;
        if (it.name.starts_with("JOY_CHECK")) return page == 6;
        return it.selected;
    }
    // The exe also greys resolutions its display-mode probe (0x42f880, +0x24 & 0x100 / 0x200 per renderer) rejects;
    // bbtime's window takes every listed size.
    bool enabled(const TbMenuItem& it) const { return it.enabled; }

    std::string keyName(const std::string& id) const {
        if (id.starts_with("Joy_B")) {  // bbtime's fixed SDL gamepad layout (play.cpp), Xbox names
            static constexpr const char* kJoy[8] = {"A", "X", "B", "LB", "RB", "Y", "LT", "RT"};
            const int n = std::atoi(id.c_str() + 5);
            return n >= 1 && n <= 8 ? kJoy[n - 1] : "???";
        }
        const uint16_t bit = padBit(id);
        const int p = id.starts_with("P2_") ? 1 : 0;
        for (const auto& [sc, b] : keys[p].keys)
            if (b == bit) return SDL_GetScancodeName(sc);
        return "???";  // 0x48b2ac: an unassigned key
    }

    void text(const Font& f, const TbMenuItem& it, const std::string& s, Rgb col) {  // DrawText in the item rect
        const int tw = f.width(s);
        const int x = it.align == 5 ? it.x + (it.w - tw) / 2 : it.align == 4 ? it.x + it.w - tw : it.x;
        f.draw(canvas, x, it.y, s, col);
    }

    void blit(const bb::Texture* t, int x, int y) {  // BitBlt SRCCOPY
        if (!t) return;
        for (int r = 0; r < int(t->height); ++r) {
            const int py = y + r;
            if (py < 0 || py >= kH) continue;
            for (int c = 0; c < int(t->width); ++c) {
                const int px = x + c;
                if (px >= 0 && px < kW)
                    std::memcpy(&canvas.rgba[(size_t(py) * kW + size_t(px)) * 4], &t->rgba[(size_t(r) * t->width + size_t(c)) * 4], 4);
            }
        }
    }

    void fill(int x0, int y0, int x1, int y1, Rgb c) {  // FillRect
        for (int y = std::max(0, y0); y < std::min(kH, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(kW, x1); ++x) {
                uint8_t* p = &canvas.rgba[(size_t(y) * kW + size_t(x)) * 4];
                p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = 255;
            }
    }

    float* slider(const std::string& prefix) {  // 0x418bd0: <PREFIX>_SLIDER / _MIN / _MAX share the value
        if (prefix == "MUSIC") return &music;
        if (prefix == "SFX") return &sfx;
        if (prefix == "CONTRASTE") return &contrast;
        return nullptr;
    }

    void drawItem(int i, bool isHot) {
        const TbMenuItem& it = item(i);
        const std::string s = it.text(language);
        const Box b = rect(it);
        switch (it.type) {
        case TbItemType::Image: {  // 0x4195e0: Up (Down while hovered), then the label in 0x419c30 colours
            const bb::Texture* down = bmp(it.down);
            blit(isHot && down ? down : bmp(it.up), b.x, b.y);
            if (!s.empty()) {
                const Box l = imageLabel(it);
                big.draw(canvas, l.x, l.y, s, it.enabled ? kRed : kGrey);
            }
            break;
        }
        case TbItemType::Text:  // 0x4197a0: blue, hot blue while hovered; centre = on x
            if (!s.empty()) {
                const int tw = big.width(s);
                const int x = it.align == 5 ? it.x - tw / 2 : it.align == 4 ? b.x + b.w - tw : it.x;
                big.draw(canvas, x, it.y, s, isHot ? kHot : kBlue);
            }
            break;
        case TbItemType::Label:  // 0x419c30
            if (!s.empty()) text(big, it, s, it.enabled ? kRed : kGrey);
            break;
        case TbItemType::Check: {  // 0x419950
            const bool on = checked(it);
            const Box cb = checkBox(it);
            blit(bmp(on ? menu.checkbox.up : menu.checkbox.down), cb.x, cb.y);
            const Rgb col = !enabled(it) ? kGrey : isHot ? (on ? kCheckedHot : kHot) : on ? kChecked : kBlue;
            big.draw(canvas, it.x, it.y, s, col);
            break;
        }
        case TbItemType::Slide: {  // 0x419df0 label 30 px above, centred; 0x41adc0 bar + fill
            if (!s.empty()) big.draw(canvas, it.x + it.w / 2 - big.width(s) / 2, it.y - 30, s, kRed);
            blit(bmp(it.up), it.x, it.y);
            const float* v = slider(it.name.substr(0, it.name.find('_')));
            const int x0 = it.fillX - 6;
            fill(x0, it.fillY, x0 + int(float(it.w) * (v ? *v : 0.0f)), it.fillY + it.h, kFill);
            break;
        }
        case TbItemType::Key: {  // 0x41a2a0: orange label, then the key name in the item's alignment
            if (!s.empty()) {
                if (it.align == 1) big.draw(canvas, it.x + (it.w - big.width(s)) / 2, it.y - 0x17, s, kKeyLabel);
                else if (it.align == 2 || it.align == 4) big.draw(canvas, it.x, it.y, s, kKeyLabel);
            }
            if (it.keyId.empty()) break;
            const Rgb col = !it.enabled ? kKeyOff : capture == i ? kKeyCapture : isHot ? kKeyHot : kWhite;
            const std::string name = keyName(it.keyId);
            const int tw = big.width(name);
            if (it.align == 1) big.draw(canvas, it.x + (it.w - tw) / 2, it.y, name, col);
            else if (it.align == 2) big.draw(canvas, it.x, it.y, name, col);
            else if (it.align == 4) big.draw(canvas, it.x + it.w - tw, it.y, name, col);
            break;
        }
        case TbItemType::ListBox: {  // 0x419f40: 22-px rows in the small font, x + 10
            for (int r = listTop; r < int(slots.size()); ++r) {
                const int top = it.y + (r - listTop) * 22;
                if (top + 22 >= it.y + it.h) break;
                const bool rowHot = isHot && r == hotRow, sel = r == listSel;
                list.draw(canvas, it.x + 10, top, slots[size_t(r)].text, rowHot ? (sel ? kCheckedHot : kChecked) : sel ? kListSel : kHot);
            }
            break;
        }
        default: break;
        }
    }

    void compose() {
        canvas.name = "tb_front";
        canvas.width = kW, canvas.height = kH;
        canvas.rgba.assign(size_t(kW) * kH * 4, 0);
        for (size_t i = 3; i < canvas.rgba.size(); i += 4) canvas.rgba[i] = 255;
        blit(bmp(menu.pages[page].backdropFor(language)), 0, 0);  // 0x419430: page bitmap at 0,0
        forPage([&](int i) { drawItem(i, i == hot); });
        if (quitBox) drawQuit();
    }

    // The WM_CLOSE box in the Windows 9x classic MessageBox look (caption = string 1). Client layout from the Wine
    // oracle (local/wine/tbmenu/w8_quit_box_root.png: 173x82 client, icon at 11,9, text at 55, 52x26 buttons at
    // y 48); Wine without a window manager draws no caption.
    struct QuitLayout { Box win, cap, client, btn[2]; std::string caption, text; };
    QuitLayout quitLayout() const {
        QuitLayout q;
        TbMenuItem t;  // the exe's per-language '~' segments, cut like menu.men texts
        t.texte = kQuitCaption, q.caption = t.text(language);
        t.texte = kQuitText, q.text = t.text(language);
        const int cw = std::max({55 + ui.width(q.text) + 18, ui.width(q.caption) + 8, 2 * 52 + 17 + 2 * 26}), ch = 82;
        q.win = {(kW - cw - 6) / 2, (kH - ch - 24) / 2, cw + 6, ch + 24};
        q.cap = {q.win.x + 3, q.win.y + 3, cw, 18};
        q.client = {q.win.x + 3, q.win.y + 21, cw, ch};
        const int bx = q.client.x + (cw - (2 * 52 + 17)) / 2;
        q.btn[0] = {bx, q.client.y + 48, 52, 26}, q.btn[1] = {bx + 52 + 17, q.client.y + 48, 52, 26};
        return q;
    }
    void frame(const Box& b, Rgb tl, Rgb br) {  // 1-px 3-D edge
        fill(b.x, b.y, b.x + b.w, b.y + 1, tl), fill(b.x, b.y, b.x + 1, b.y + b.h, tl);
        fill(b.x, b.y + b.h - 1, b.x + b.w, b.y + b.h, br), fill(b.x + b.w - 1, b.y, b.x + b.w, b.y + b.h, br);
    }
    static Box inset(const Box& b, int d) { return {b.x + d, b.y + d, b.w - 2 * d, b.h - 2 * d}; }
    void drawQuit() {
        constexpr Rgb face{192, 192, 192}, light{223, 223, 223}, shadow{128, 128, 128}, black{0, 0, 0}, navy{0, 0, 128};
        const QuitLayout q = quitLayout();
        fill(q.win.x, q.win.y, q.win.x + q.win.w, q.win.y + q.win.h, face);
        frame(q.win, light, black), frame(inset(q.win, 1), kWhite, shadow);
        fill(q.cap.x, q.cap.y, q.cap.x + q.cap.w, q.cap.y + q.cap.h, navy);
        ui.draw(canvas, q.cap.x + 3, q.cap.y + (q.cap.h - ui.cell) / 2, q.caption, kWhite);
        const int cx = q.client.x + 27, cy = q.client.y + 25;  // IDI_QUESTION 32x32: white bubble, black rim, blue '?'
        for (int y = -16; y < 16; ++y)
            for (int x = -16; x < 16; ++x)
                if (const int d = (2 * x + 1) * (2 * x + 1) + (2 * y + 1) * (2 * y + 1); d < 31 * 31)
                    fill(cx + x, cy + y, cx + x + 1, cy + y + 1, d > 27 * 27 ? black : kWhite);
        uiIcon.draw(canvas, cx - uiIcon.width("?") / 2, cy - uiIcon.cell / 2, "?", Rgb{0, 0, 255});
        ui.draw(canvas, q.client.x + 55, q.client.y + 17, q.text, black);
        for (int k = 0; k < 2; ++k) {
            const Box& b = q.btn[k];
            const Box e = k == quitFocus ? inset(b, 1) : b;  // the focused push button is the default one
            if (k == quitFocus) frame(b, black, black);
            frame(e, kWhite, black), frame(inset(e, 1), light, shadow);
            const std::string_view s = k ? "No" : "Yes";
            const int tx = b.x + (b.w - ui.width(s)) / 2, ty = b.y + (b.h - ui.cell) / 2;
            ui.draw(canvas, tx, ty, s, black);
            fill(tx, ty + ui.ascent + 1, tx + ui.width(s.substr(0, 1)), ty + ui.ascent + 2, black);  // "&Yes" / "&No"
            if (k == quitFocus) {  // dotted focus rectangle
                const Box f = inset(b, 4);
                for (int x = f.x; x < f.x + f.w; x += 2) fill(x, f.y, x + 1, f.y + 1, black), fill(x, f.y + f.h - 1, x + 1, f.y + f.h, black);
                for (int y = f.y; y < f.y + f.h; y += 2) fill(f.x, y, f.x + 1, y + 1, black), fill(f.x + f.w - 1, y, f.x + f.w, y + 1, black);
            }
        }
    }
    void askQuit(bool exitApp) { quitBox = true, quitExit = exitApp, quitFocus = 0, capture = -1, dirty = true; }
    TbFrontAction quitAnswer(int k) {  // IDYES (6): state 0, the exe exits; IDNO: the menu stays
        quitBox = false, dirty = true;
        if (k != 0) return {};
        return {.kind = quitExit ? TbFrontAction::Exit : TbFrontAction::Quit};
    }

    // 0x41c0f0 attract: 30 s after the last key / button press the loop opens ..\data\demo<n>.avi (n = 1, 2, ... one
    // per pass, a missing file is skipped) and plays it (0x409df0) until the next key or click. The PC disc ships no
    // demo files, so the exe just keeps probing and the menu stays as it is (oracle: w8_inotify_per_second.txt, idle
    // shots unchanged); bbtime has no AVI decoder and treats a present file the same way.
    void idle() {
        if (demoLogged || SDL_GetTicks() < idleDue) return;
        demoLogged = true;
        std::println(stderr, "bbtime: TB menu idle 30 s: attract ..\\data\\demo1.avi {}",
                     files.contains("demo1.avi") ? "present, not played (no AVI decoder)" : "absent, menu stays (as the exe)");
    }

    // 0x41bc40: the first item of the page (groups in order) under the mouse; a disabled key ends its group.
    void hover(int x, int y) {
        int found = -1, row = -1;
        for (const auto& g : menu.pages[page].groups) {
            for (const size_t k : g) {
                const TbMenuItem& it = item(int(k));
                const Box b = rect(it);
                bool hit = b.in(x, y);
                if (it.type == TbItemType::Image && !it.text(language).empty()) hit = hit || imageLabel(it).in(x, y);
                if (it.type == TbItemType::Check) hit = hit || checkBox(it).in(x, y);
                if (!hit) continue;
                if (it.type == TbItemType::Key && !it.enabled) break;
                found = int(k);
                if (it.type == TbItemType::ListBox && x > it.x + 10 && x < it.x + it.w - 10) {
                    const int r = listTop + (y - it.y) / 22;
                    if (r < int(slots.size()) && (r - listTop + 1) * 22 < it.h) row = r;
                }
                break;
            }
            if (found >= 0) break;
        }
        setHot(found, row);
    }
    void setHot(int i, int row) {
        if (i != hot || row != hotRow) dirty = true;
        hot = i, hotRow = row;
    }

    void showPage(int p) {  // Call(n) 0x41a860
        if (p < 0 || p >= TbMenu::kPages || menu.pages[p].backdrop.empty()) return;
        page = p, hot = -1, hotRow = -1, capture = -1, dirty = true;
        if (p == 7) loadSlots(), listSel = -1, listTop = 0;
        // 0x419df0 draws the slider through 0x41adc0, which stores 0.25 + 1.75 v on every draw, v = 0x639c20 / 2 from
        // menu load (0x418bd0): showing the Video page turns 1.0 into 1.125 (R: peek 0x639c20, w8_capture.sh).
        if (p == 3) brightness = 0.25f + 1.75f * contrast, saveVideo();
    }
    void saveVideo() {  // 0x41a6e0 writes config.pc on leaving the menu; bbtime: settings.ini at once
        Settings st = loadSettings();
        st.width = resW, st.height = resH, st.tbRenderer = renderer, st.tbDetails = details, st.tbBrightness = brightness;
        if (const std::string err = saveSettings(st); !err.empty()) std::println(stderr, "bbtime: {}", err);
        dirty = true;
    }

    void loadSlots() {  // 0x428e50 at menu start and on the Load page
        std::vector<uint8_t> b;
        try { b = bb::readFile(saves / "Savedata.dat"); } catch (const std::exception&) {}
        slots = parseTbSavedata(b);
    }

    // 0x41b060 -> 0x41adc0: ±0.125, 0..1. The 0.05 constant meant for brightness is never used (the step is
    // compared with the 0x639c20 pointer; R: one '+' 1.125 -> 1.34375). Brightness = 0.25 + 1.75 v, no bong.
    void step(const std::string& prefix, int dir) {
        float* v = slider(prefix);
        if (!v) return;
        *v = std::clamp(*v + float(dir) * 0.125f, 0.0f, 1.0f);
        dirty = true;
        if (prefix == "CONTRASTE") { brightness = 0.25f + 1.75f * contrast, saveVideo(); return; }
        Settings st = loadSettings();
        st.musicVolume = int(std::lround(music * 100.0f)), st.sfxVolume = int(std::lround(sfx * 100.0f));
        if (const std::string err = saveSettings(st); !err.empty()) std::println(stderr, "bbtime: {}", err);
        if (bb::AudioDevice* dev = mute ? nullptr : audioDevice()) {
            dev->setMusicVolume(music), dev->setSfxVolume(sfx);
            if (!bong.pcm.empty()) dev->playSfx(bong);  // PlaySoundA("..\data\bong.wav") at the effects volume
        }
    }

    TbFrontAction load() {  // 0x41b520 -> 0x428bf0
        TbFrontAction a;
        if (listSel < 0 || listSel >= int(slots.size())) return a;
        const TbSaveSlot& s = slots[size_t(listSel)];
        std::vector<uint8_t> b;
        try { b = bb::readFile(saves / std::format("Savegame{}.dat", s.slot)); } catch (const std::exception&) {}
        if (b.size() < 0x2b0) {
            std::println(stderr, "bbtime: Could not find savegame! ({})", (saves / std::format("Savegame{}.dat", s.slot)).string());
            return a;
        }
        std::copy_n(b.begin(), 0x2b0, a.gd.begin());
        a.gd[0x93] = uint8_t(language);
        for (const size_t o : {0x14a, 0x101, 0xff, 0xfe, 0x102, 0x100, 0x103, 0x108, 0x107, 0x104, 0x105, 0x106, 0x13d}) a.gd[o] = 0;
        a.kind = TbFrontAction::Load;
        a.level = s.level;
        a.slot = s.slot;  // 0x48840c: later in-game saves go back to this file
        a.players = a.gd[0xa4] & 0x10 ? 2 : 1;
        return a;
    }

    void remove() {  // 0x41b810 -> 0x428fb0: drop the record, renumber, rewrite Savedata, shift Savegame%d.dat down
        if (listSel < 0 || listSel >= int(slots.size())) return;
        const int gone = slots[size_t(listSel)].slot;
        slots.erase(slots.begin() + listSel);
        for (size_t i = 0; i < slots.size(); ++i) slots[i].slot = int(i);
        std::error_code ec;
        if (slots.empty()) fs::remove(saves / "Savedata.dat", ec);
        else {
            const std::vector<uint8_t> b = writeTbSavedata(slots);
            std::ofstream(saves / "Savedata.dat", std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        }
        fs::remove(saves / std::format("Savegame{}.dat", gone), ec);
        for (int k = gone; k < int(slots.size()) + 1; ++k)
            fs::rename(saves / std::format("Savegame{}.dat", k + 1), saves / std::format("Savegame{}.dat", k), ec);
        listSel = -1, hotRow = -1, dirty = true;
        listTop = std::min(listTop, std::max(0, int(slots.size()) - 1));
    }

    void scroll(int dir) {  // 0x419f40 with ±1: only when the rows overflow the box
        const TbMenuItem* box = menu.find(page, "SAVEGAME_LISTBOX");
        if (!box || int(slots.size()) * 22 <= box->h) return;
        const int rows = (box->h - 1) / 22;
        listTop = std::clamp(listTop + dir, 0, std::max(0, int(slots.size()) - rows));
        dirty = true;
    }

    TbFrontAction activate(int i) {  // 0x41bf90: +0x90(item, +0x94) unless a slider, then +0x98(item, +0x9c)
        TbFrontAction a;
        if (i < 0) return a;
        const TbMenuItem& it = item(i);
        switch (it.type) {
        case TbItemType::Check:  // 0x41ab70: radio by family; non-changeable checks only run their Function
            if (int n = 0; enabled(it) && it.changeable) {
                const std::string f = family(it, &n);
                if (f == "LANGUAGE_") language = n, dirty = true;
                else if (f == "RENDERER_") renderer = n == 0 ? 2 : 1, saveVideo();
                else if (f == "DETAILS_") details = n, saveVideo();
                else if (f == "RESOLUTION_" && n < 5) {  // config.pc +0x10 -> the window size (Hor+ as before)
                    resW = kRes[n].first, resH = kRes[n].second, saveVideo();
                    a = {.kind = TbFrontAction::Resize, .width = resW, .height = resH};
                }
            }
            break;
        case TbItemType::Key:  // 0x41b1f0: wait for the key (keyboard pages; the gamepad layout is fixed here)
            if (it.enabled && !it.keyId.empty() && !it.keyId.starts_with("Joy_")) capture = i, dirty = true;
            break;
        case TbItemType::ListBox:  // 0x41b120
            if (hotRow >= 0) listSel = hotRow, dirty = true;
            break;
        default:
            if (it.name == "SAVEGAME_UP") scroll(-1);
            else if (it.name == "SAVEGAME_DOWN") scroll(1);
            else if (it.name.ends_with("_MIN")) step(it.name.substr(0, it.name.size() - 4), -1);
            else if (it.name.ends_with("_MAX")) step(it.name.substr(0, it.name.size() - 4), 1);
            break;
        }
        switch (it.fn) {
        case TbFn::Done: a.kind = TbFrontAction::Play, a.players = 1; break;  // 0x41a6e0 -> state 5, 0x48b1bc = 1
        case TbFn::Done2: a.kind = TbFrontAction::Play, a.players = 2; break;
        case TbFn::Cancel: a.kind = TbFrontAction::Quit; break;
        case TbFn::Call: showPage(it.fnArg); break;
        case TbFn::Load: a = load(); break;
        case TbFn::Delete: remove(); break;
        case TbFn::None: break;
        }
        return a;
    }

    bool focusable(const TbMenuItem& it) const {
        if (it.fn != TbFn::None || it.type == TbItemType::ListBox) return true;
        if (it.type == TbItemType::Check) return enabled(it) && it.changeable;
        if (it.type == TbItemType::Key) return it.enabled && !it.keyId.empty() && !it.keyId.starts_with("Joy_");
        return it.name == "SAVEGAME_UP" || it.name == "SAVEGAME_DOWN" || it.name.ends_with("_MIN") || it.name.ends_with("_MAX");
    }

    void nav(int dx, int dy) {
        if (capture >= 0) return;
        if (hot >= 0 && item(hot).type == TbItemType::ListBox && dy && !slots.empty()) {  // rows first
            const int r = hotRow + dy;
            if (hotRow >= 0 && r >= 0 && r < int(slots.size())) {
                const int rows = (item(hot).h - 1) / 22;
                listTop = std::clamp(listTop, r - rows + 1, r);
                setHot(hot, r);
                dirty = true;
                return;
            }
        }
        const auto centre = [&](int i) {
            const Box b = rect(item(i));
            return std::pair{b.x + b.w / 2, b.y + b.h / 2};
        };
        int best = -1;
        long bestScore = 0;
        forPage([&](int i) {
            if (i == hot || !focusable(item(i))) return;
            if (hot < 0) { if (best < 0) best = i; return; }
            const auto [hx, hy] = centre(hot);
            const auto [cx, cy] = centre(i);
            const long along = long(cx - hx) * dx + long(cy - hy) * dy, across = std::abs(dx ? cy - hy : cx - hx);
            if (along <= 0) return;
            if (const long score = along + 2 * across; best < 0 || score < bestScore) best = i, bestScore = score;
        });
        if (best < 0) return;
        setHot(best, item(best).type == TbItemType::ListBox && !slots.empty() ? std::max(listSel, 0) : -1);
        if (hotRow >= 0) listTop = std::min(listTop, hotRow);
    }
};

TbFront::TbFront() : d(std::make_unique<Impl>()) {}
TbFront::~TbFront() = default;

std::string TbFront::open(const fs::path& tbRoot, const fs::path& saves, KeyMap (&keys)[2], bool mute) {
    Impl& m = *d;
    m.keys = keys, m.mute = mute, m.saves = saves;
    m.page = 0, m.hot = -1, m.hotRow = -1, m.capture = -1, m.dirty = true, m.quitBox = false;
    m.idleDue = SDL_GetTicks() + 30000, m.demoLogged = false;  // 0x41c0f0 arms the attract timer at menu start
    m.loadSlots();
    if (!m.files.empty()) return {};  // already loaded: language / volumes / keys persist for the session
    std::error_code ec;
    fs::path data;
    for (const auto& e : fs::directory_iterator(tbRoot, ec))
        if (lower(e.path().filename().string()) == "datas") data = e.path();
    for (const auto& e : fs::directory_iterator(data, ec))
        if (lower(e.path().filename().string()) == "data") data = e.path();
    for (const auto& e : fs::directory_iterator(data, ec)) m.files[lower(e.path().filename().string())] = e.path();
    if (!m.files.contains("menu.men")) return std::format("no Datas/data/menu.men under {}", tbRoot.string());
    try {
        const std::vector<uint8_t> b = bb::readFile(m.files["menu.men"]);
        m.menu = parseTbMenu(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    } catch (const std::exception& e) {
        m.files.clear();
        return e.what();
    }
    if (FT_Init_FreeType(&m.ft) || !m.files.contains("bradybun.ttf") || !m.big.open(m.ft, m.files["bradybun.ttf"], 30) ||
        !m.list.open(m.ft, m.files["bradybun.ttf"], 26)) {
        m.files.clear();
        return "cannot open Datas/data/bradybun.ttf (FreeType)";
    }
    m.bong = loadWav(m.files["bong.wav"]);
    const Settings st = loadSettings();
    m.music = float(st.musicVolume) / 100.0f, m.sfx = float(st.sfxVolume) / 100.0f;
    m.resW = st.width, m.resH = st.height, m.renderer = st.tbRenderer, m.details = st.tbDetails;
    m.brightness = st.tbBrightness, m.contrast = st.tbBrightness * 0.5f;  // 0x418bd0: slider = 0x639c20 · 0.5
    // MessageBox font: Wine's Tahoma (what the oracle draws), else a common sans; the menu's own TTF as last resort.
    for (const fs::path& f : {fs::path("/usr/share/wine/fonts/tahoma.ttf"), fs::path("/usr/share/fonts/TTF/DejaVuSans.ttf"),
                              fs::path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
                              fs::path("/usr/share/fonts/liberation/LiberationSans-Regular.ttf"),
                              fs::path("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"), m.files["bradybun.ttf"]}) {
        m.ui.close(), m.uiIcon.close();
        if (m.ui.open(m.ft, f, -11) && m.uiIcon.open(m.ft, f, -24)) break;
    }
    return {};
}

void TbFront::mouse(int x, int y) {
    d->mx = x, d->my = y;
    if (d->capture < 0 && !d->quitBox) d->hover(x, y);
}
TbFrontAction TbFront::click(bool mouse) {
    Impl& m = *d;
    if (m.quitBox) {  // a click answers only on a button; Enter / ✕ answers the focused one
        if (!mouse) return m.quitAnswer(m.quitFocus);
        const Impl::QuitLayout q = m.quitLayout();
        for (int k = 0; k < 2; ++k)
            if (q.btn[k].in(m.mx, m.my)) return m.quitAnswer(k);
        return {};
    }
    return m.capture >= 0 ? TbFrontAction{} : m.activate(m.hot);
}
void TbFront::nav(int dx, int dy) {
    if (!d->quitBox) d->nav(dx, dy);
    else if (dx || dy) d->quitFocus ^= 1, d->dirty = true;  // two buttons: any arrow moves the focus
}
bool TbFront::capturing() const { return d->capture >= 0 || d->quitBox; }
void TbFront::askQuit(bool exitApp) { d->askQuit(exitApp); }
void TbFront::input() { d->idleDue = SDL_GetTicks() + 30000, d->demoLogged = false; }

TbFrontAction TbFront::back() {
    Impl& m = *d;
    if (m.quitBox) return {};  // MB_YESNO has no cancel: Esc does nothing
    if (m.capture >= 0) { m.capture = -1, m.dirty = true; return {}; }
    if (m.page == 0) { m.askQuit(false); return {}; }  // bbtime's Esc on page 0 = the window's close (WM_CLOSE)
    int target = -1;  // the page's Back image (Call(n))
    m.forPage([&](int i) {
        if (target < 0 && m.item(i).name.starts_with("BACK") && m.item(i).fn == TbFn::Call) target = m.item(i).fnArg;
    });
    m.showPage(target);
    return {};
}

// Quit box: Y / N answer, Enter / Space the focused button, arrows / Tab move the focus.
// 0x41ba20: the key replaces the item's binding; another action of the same player holding it loses it ("???").
TbFrontAction TbFront::captureKey(SDL_Scancode sc) {
    Impl& m = *d;
    if (m.quitBox) {
        if (sc == SDL_SCANCODE_Y || sc == SDL_SCANCODE_N) return m.quitAnswer(sc == SDL_SCANCODE_N);
        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER || sc == SDL_SCANCODE_SPACE) return m.quitAnswer(m.quitFocus);
        if (sc == SDL_SCANCODE_TAB || (sc >= SDL_SCANCODE_RIGHT && sc <= SDL_SCANCODE_UP)) m.quitFocus ^= 1, m.dirty = true;
        return {};
    }
    if (m.capture < 0) return {};
    const TbMenuItem& it = m.item(m.capture);
    m.capture = -1, m.dirty = true;
    if (sc == SDL_SCANCODE_ESCAPE) return {};
    const uint16_t bit = padBit(it.keyId);
    auto& keys = m.keys[it.keyId.starts_with("P2_") ? 1 : 0].keys;
    std::erase_if(keys, [&](const auto& k) { return k.first == sc && k.second != bit; });
    const auto own = std::ranges::find_if(keys, [&](const auto& k) { return k.second == bit; });
    if (own != keys.end()) own->first = sc;
    else keys.emplace_back(sc, bit);
    return {};
}

const bb::Texture* TbFront::frame() {
    d->idle();
    if (!d->dirty) return nullptr;
    d->dirty = false;
    d->compose();
    return &d->canvas;
}

bb::Texture ttfText(const fs::path& ttf, std::string_view s, int lfHeight) {
    bb::Texture t;
    FT_Library lib = nullptr;
    Font f;
    if (FT_Init_FreeType(&lib)) return t;
    if (f.open(lib, ttf, lfHeight)) {
        t.width = uint32_t(std::max(1, f.width(s) + lfHeight / 4)), t.height = uint32_t(f.cell);  // + italic overhang
        t.rgba.assign(size_t(t.width) * t.height * 4, 0);
        int x = 0;
        for (const char c : s) {
            if (FT_Load_Char(f.face, uint8_t(c), FT_LOAD_DEFAULT | FT_LOAD_RENDER)) continue;
            const FT_GlyphSlot g = f.face->glyph;
            for (unsigned r = 0; r < g->bitmap.rows; ++r)
                for (unsigned k = 0; k < g->bitmap.width; ++k) {
                    const int px = x + g->bitmap_left + int(k), py = f.ascent - g->bitmap_top + int(r);
                    if (px < 0 || py < 0 || px >= int(t.width) || py >= int(t.height)) continue;
                    uint8_t* p = &t.rgba[(size_t(py) * t.width + size_t(px)) * 4];
                    p[0] = p[1] = p[2] = 255, p[3] = std::max(p[3], g->bitmap.buffer[r * size_t(g->bitmap.pitch) + k]);
                }
            x += int((g->advance.x + 32) >> 6);
        }
    }
    f.close();
    FT_Done_FreeType(lib);
    return t;
}

} // namespace bbtime
