// TB original front end (GDI title menu 0x41c0f0 over Datas/data/menu.men, docs/re/TIME_BUSTERS.md §8): the 640x480
// page composed on the CPU like the exe's BitBlt / DrawText (Brady Bunch TTF through FreeType, 1-bit like GDI),
// mouse hover / click as the window procedure 0x42e480 routes them (0x41bc40 / 0x41bf90), keyboard and gamepad
// focus on top (not in the original). The app scales the canvas into a 4:3 viewport.
#pragma once
#include "bb/scene.h"
#include "play.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace bbtime {

struct TbFrontAction {
    // Quit: back to the collection menu (page-0 Quit, or Yes in the quit box opened by Esc); Exit: Yes in the box a
    // window close opened (the exe's WM_CLOSE -> state 0); Resize: Video page resolution (window size).
    enum Kind { None, Play, Load, Quit, Exit, Resize } kind = None;
    int players = 1;                  // Play (Done / Done2), Load (GD+0xa4 & 0x10)
    int level = 0;                    // Load: Savedata record level (GD+0)
    std::array<uint8_t, 0x300> gd{};  // Load: Savegame%d.dat (0x2b0 bytes) after the 0x428bf0 resets
    int width = 0, height = 0;        // Resize
    int slot = -1;                    // Load: the record's Savegame%d.dat number (exe 0x48840c)
};

class TbFront {
public:
    TbFront();
    ~TbFront();
    // tbRoot contains Datas/; saves = Savedata.dat / Savegame%d.dat directory; keys = the TB keyboard maps
    // (Controls pages edit them in place). Returns an error text (empty = ok). Shows page 0.
    std::string open(const std::filesystem::path& tbRoot, const std::filesystem::path& saves, KeyMap (&keys)[2], bool mute);
    void mouse(int x, int y);  // canvas pixel, (-1, -1) = outside
    // WM_LBUTTONDOWN (mouse) on the hovered item / quit-box button; else Enter / ✕ on the focused one.
    TbFrontAction click(bool mouse = false);
    TbFrontAction back();      // Esc: the page's Back item; on page 0 the quit box (as WM_CLOSE)
    void nav(int dx, int dy);  // arrows / d-pad: move the focus (list box: rows first)
    bool capturing() const;    // raw keys wanted: a Controls key waits for its new key, or the quit box is open
    TbFrontAction captureKey(SDL_Scancode sc);  // key capture (Esc cancels) / quit box keys
    void askQuit(bool exitApp);  // WM_CLOSE (0x42e480): "Do you want to quit?" Yes / No
    void input();              // a key or button press: re-arms the 30 s attract timer (0x41c0f0)
    // The canvas when it changed since the last call (else nullptr).
    const bb::Texture* frame();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// One line of a TTF (the Brady Bunch menu font) as white text, alpha = coverage; lfHeight as TbFront's fonts (cell px).
bb::Texture ttfText(const std::filesystem::path& ttf, std::string_view s, int lfHeight);

} // namespace bbtime
