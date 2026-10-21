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
    enum Kind { None, Play, Load, Quit } kind = None;
    int players = 1;                  // Play (Done / Done2), Load (GD+0xa4 & 0x10)
    int level = 0;                    // Load: Savedata record level (GD+0)
    std::array<uint8_t, 0x300> gd{};  // Load: Savegame%d.dat (0x2b0 bytes) after the 0x428bf0 resets
};

class TbFront {
public:
    TbFront();
    ~TbFront();
    // tbRoot contains Datas/; saves = Savedata.dat / Savegame%d.dat directory; keys = the TB keyboard maps
    // (Controls pages edit them in place). Returns an error text (empty = ok). Shows page 0.
    std::string open(const std::filesystem::path& tbRoot, const std::filesystem::path& saves, KeyMap (&keys)[2], bool mute);
    void mouse(int x, int y);  // canvas pixel, (-1, -1) = outside
    TbFrontAction click();     // WM_LBUTTONDOWN on the hovered item; also Enter / ✕ on the focused one
    TbFrontAction back();      // Esc: the page's Back item, Quit on page 0
    void nav(int dx, int dy);  // arrows / d-pad: move the focus (list box: rows first)
    bool capturing() const;    // a Controls key waits for its new key
    void captureKey(SDL_Scancode sc);  // Esc cancels
    // The canvas when it changed since the last call (else nullptr).
    const bb::Texture* frame();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace bbtime
