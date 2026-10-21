// TB front end (docs/re/TIME_BUSTERS.md §8): the shipped Datas/data/menu.men parsed as 0x4182b0 does (pages, groups,
// positions, types, functions, per-language texts), and the Savedata.dat layout round trip. Needs BBTIME_DATA
// (dir containing tb/) for the real file; 77 = skipped.
#include "bb/bze.h"
#include "bb/game/tb_menu.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace bb::game;
namespace fs = std::filesystem;

int main() {
    const std::vector<TbSaveSlot> slots = {{0, "10/20/26, 23:41:07, 12%", 4}, {1, "x", 51}};
    const std::vector<uint8_t> raw = writeTbSavedata(slots);
    assert(raw.size() == 9 + 2 * 0x54 && raw[0] == 2 && raw[4] == '1' && raw[8] == 0);
    const std::vector<TbSaveSlot> back = parseTbSavedata(raw);
    assert(back.size() == 2 && back[0].text == slots[0].text && back[1].slot == 1 && back[1].level == 51);
    assert(parseTbSavedata(std::span(raw).first(20)).empty());

    const char* data = std::getenv("BBTIME_DATA");
    const fs::path file = data ? fs::path(data) / "tb/Datas/data/menu.men" : fs::path();
    if (file.empty() || !fs::exists(file)) {
        std::puts("skip: no BBTIME_DATA/tb/Datas/data/menu.men");
        return 77;
    }
    const std::vector<uint8_t> bytes = bb::readFile(file);
    const TbMenu m = parseTbMenu(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    // 121 items on pages 0-7 and 9 (no page 8), in groups by first appearance
    assert(m.items.size() == 121);
    const int counts[TbMenu::kPages] = {5, 5, 24, 18, 8, 24, 20, 7, 0, 10};
    const size_t groups[TbMenu::kPages] = {1, 1, 3, 4, 1, 3, 3, 2, 0, 2};
    for (int p = 0; p < TbMenu::kPages; ++p) {
        size_t n = 0;
        for (const auto& g : m.pages[p].groups) n += g.size();
        assert(int(n) == counts[p] && m.pages[p].groups.size() == groups[p]);
    }
    assert(m.pages[0].backdropFor(0) == "english.bmp" && m.pages[0].backdropFor(1) == "french.bmp" &&
           m.pages[0].backdropFor(6) == "dutch.bmp");
    assert(m.pages[1].backdropFor(3) == "pc03.bmp" && m.pages[7].backdropFor(0) == "pc07.bmp");
    assert(m.checkbox.up == "coche1.bmp" && m.checkbox.down == "coche2.bmp" && m.checkbox.align == 2);

    // main page: x y, images, function targets, texts (EN / FR / DE)
    struct Main { const char* name; int x, y; const char* up; TbFn fn; int arg; const char* en; const char* fr; };
    const Main main[] = {{"ONE_PLAYER", 33, 177, "Play1.bmp", TbFn::Done, 0, "1 Player", "1 Joueur"},
                         {"TWO_PLAYER", 93, 323, "Play2.bmp", TbFn::Done2, 0, "2 Players", "2 Joueurs"},
                         {"CONFIG", 383, 366, "config.bmp", TbFn::Call, 1, "Configuration", "Configuration"},
                         {"LOAD", 221, 377, "load.bmp", TbFn::Call, 7, "Load Game", "Charger une partie"},
                         {"QUIT", 504, 337, "quit.bmp", TbFn::Cancel, 0, "Quit", "Quitter"}};
    for (size_t i = 0; i < 5; ++i) {
        const TbMenuItem& it = m.items[m.pages[0].groups[0][i]];
        assert(it.name == main[i].name && it.x == main[i].x && it.y == main[i].y && it.up == main[i].up);
        assert(it.type == TbItemType::Image && it.fn == main[i].fn && it.fnArg == main[i].arg && it.enabled);
        assert(it.text(0) == main[i].en && it.text(1) == main[i].fr);
    }
    assert(m.find(0, "QUIT")->text(2) == "Ende" && m.find(0, "QUIT")->text(5).empty());  // index 5 is "xxx"

    // other pages: key items, sliders, check groups, list box, centred label
    const TbMenuItem* camg = m.find(2, "CAMG_P1");
    assert(camg && camg->type == TbItemType::Key && camg->keyId == "P1_L2" && camg->align == 4 &&
           camg->linkedKey == "FIRSTVIEW_P1_KEY1" && camg->w == 350 && camg->h == 25);
    assert(!m.find(2, "FIRSTVIEW_P1")->enabled && m.find(2, "PLAYER2_CHECK_P1")->fn == TbFn::Call &&
           m.find(2, "PLAYER2_CHECK_P1")->fnArg == 5 && !m.find(2, "PLAYER2_CHECK_P1")->changeable);
    const TbMenuItem* music = m.find(4, "MUSIC_SLIDER");
    assert(music && music->type == TbItemType::Slide && music->fillX == 190 && music->fillY == 233 && music->w == 274);
    assert(m.find(7, "SAVEGAME_LISTBOX")->type == TbItemType::ListBox && m.find(7, "LOADGAME")->fn == TbFn::Load &&
           m.find(7, "DELETE_LOADGAME")->fn == TbFn::Delete && m.find(7, "SELECTSAVEGAME")->align == 5);
    assert(m.find(9, "LANGUAGE_3")->text(0) == "Espa\xf1ol" && m.find(9, "LANGUAGE_0")->group == 1);
    std::puts("tb_menu ok");
    return 0;
}
