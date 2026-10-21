// TB front end (GDI title menu, WinMain state 2 -> 0x41c0f0): `Datas/data/menu.men` as parsed by 0x4182b0 / 0x416e30
// and the save list of `..\bin\Savedata.dat` (0x428e50). Model only; drawing and input are apps/bbtime/tb_front.cpp.
// Format and behaviour: docs/re/TIME_BUSTERS.md §8.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bb::game {

// Item type (+0x6c), TypeTexte keyword.
enum class TbItemType : int8_t { None = -1, Text = 0, Check = 1, Slide = 2, Label = 3, Image = 4, Key = 5, Blank = 6, ListBox = 7 };
// Function= (+0x90/+0x94, +0x98/+0x9c): Done / Done2 start 1 / 2 players (0x41a6e0), Call(n) shows page n (0x41a860),
// Cancel quits (0x41a6e0 arg 0), LoadSaveGame / DeleteSaveGame act on the page's list box (0x41b520 / 0x41b810).
enum class TbFn : int8_t { None, Done, Done2, Cancel, Call, Load, Delete };

struct TbMenuItem {
    std::string name;                // [SECTION] (+4, 31 chars)
    int page = -1, group = -1;       // Nopage / Group
    TbItemType type = TbItemType::None;
    int x = 0, y = 0, w = 0, h = 0;  // x y With Height; Up image size overrides w/h (except ISSLIDE: With/Height)
    int fillX = -1, fillY = -1;      // ISSLIDE fill origin
    std::string up, down;            // BMP file names, empty for "xxx"
    std::string texte;               // raw Texte= (all languages, '~'-separated)
    int align = 2;                   // +0x88: 1 top, 2 left (default), 3 bottom, 4 right, 5 center
    bool enabled = true;             // +0xac, Enable=FALSE -> 0 (every section of the shipped file sets Enable)
    bool selected = false;           // Selected=TRUE (check)
    bool changeable = true;
    int checkType = -1;
    std::string keyId, linkedKey;    // ISKEY
    TbFn fn = TbFn::None;
    int fnArg = 0;                   // Call(n)

    std::string text(int language) const;  // Texte for a language (0x416a60), "" for xxx
};

struct TbMenuPage {
    std::vector<std::string> backdrop;           // Up0..Up6 per language (a page without them: Up for all)
    std::vector<std::vector<size_t>> groups;     // item indices, groups in order of first appearance (0x416d00)
    std::string backdropFor(int language) const;
};

struct TbMenu {
    static constexpr int kPages = 10;   // 0x638ee0: 10 page slots of 9 ints
    std::vector<TbMenuItem> items;
    TbMenuPage pages[kPages];
    TbMenuItem checkbox;                // CHECKBOX_ITEM_0: the box images of every ISCHECK
    const TbMenuItem* find(int page, std::string_view name) const;
};

// Throws std::runtime_error on input without a single page (the exe's "Couldn't load the menu file properly").
TbMenu parseTbMenu(std::string_view text);

// Savedata.dat (0x428e50 / 0x428d70): u32 count, "1.00\0", count records of 0x54 bytes: u8 slot (the %d of
// Savegame%d.dat, renumbered 0..n-1 on delete), char text[0x4b] "<date>, <time>, <percent>%", u32 level (GD+0) at
// +0x4c, +0x50 the in-memory next pointer (written, never read). docs/formats/SAVE_TB.md.
struct TbSaveSlot {
    int slot = 0;
    std::string text;
    int level = 0;
    // The record as read: the writer lays slot / text + NUL / level over it, so the bytes after the text keep what
    // the original left there (0xcd heap fill of a new record, the tail of a longer old text). +0x50 is written 0.
    std::array<uint8_t, 0x54> raw{};
};
std::vector<TbSaveSlot> parseTbSavedata(std::span<const uint8_t> bytes);  // empty if short / malformed
std::vector<uint8_t> writeTbSavedata(std::span<const TbSaveSlot> slots);

} // namespace bb::game
