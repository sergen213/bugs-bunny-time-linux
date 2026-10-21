// TB menu.men parser (0x4182b0 -> 0x416e30 per section) and Savedata.dat (0x428e50 / 0x428d70); see tb_menu.h.
#include "bb/game/tb_menu.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace bb::game {

namespace {

std::string trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

// 0x416ba0: strstr(section, "Key=") (first occurrence anywhere, case-sensitive), value up to the line end, trailing
// spaces dropped.
std::string value(std::string_view sec, std::string_view key) {
    const std::string k = std::string(key) + "=";
    const size_t at = sec.find(k);
    if (at == std::string_view::npos) return {};
    std::string_view v = sec.substr(at + k.size());
    v = v.substr(0, v.find_first_of("\r\n"));
    while (!v.empty() && v.back() == ' ') v.remove_suffix(1);
    return std::string(v);
}

// Numeric fields: atoi of the value, -1 when the key is absent.
int number(std::string_view sec, std::string_view key) {
    const std::string k = std::string(key) + "=";
    const size_t at = sec.find(k);
    return at == std::string_view::npos ? -1 : std::atoi(std::string(sec.substr(at + k.size(), 16)).c_str());
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

std::string image(std::string_view sec, std::string_view key) {
    std::string v = value(sec, key);
    return v.empty() || v == "xxx" ? std::string() : v;
}

uint32_t u32(std::span<const uint8_t> b, size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24;
}

} // namespace

std::string TbMenuItem::text(int language) const {
    std::string_view t = texte;
    std::string out;
    if (t.find('~') == std::string_view::npos) {
        out = trim(t);
    } else {
        for (int i = 0; i < language && !t.empty(); ++i) {
            const size_t n = t.find('~');
            t = n == std::string_view::npos ? std::string_view() : t.substr(n + 1);
        }
        out = trim(t.substr(0, t.find('~')));
    }
    return out.find("xxx") != std::string::npos ? std::string() : out;  // 0x419c30: "xxx" is never drawn
}

std::string TbMenuPage::backdropFor(int language) const {
    if (backdrop.empty()) return {};
    return backdrop[size_t(std::clamp(language, 0, int(backdrop.size()) - 1))];
}

const TbMenuItem* TbMenu::find(int page, std::string_view name) const {
    for (const TbMenuItem& it : items)
        if (it.page == page && it.name == name) return &it;
    return nullptr;
}

TbMenu parseTbMenu(std::string_view text) {
    TbMenu m;
    bool anyPage = false;
    for (size_t at = text.find('['); at != std::string_view::npos;) {
        const size_t next = text.find('[', at + 1);
        const std::string_view sec = text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
        at = next;
        const size_t close = sec.find(']');
        if (close == std::string_view::npos) continue;
        const std::string name(sec.substr(1, std::min<size_t>(close - 1, 31)));
        const std::string_view body = sec.substr(close + 1);

        if (name.starts_with("PAGE_")) {  // page backdrop only (its x/y/With/Height/Texte are not read)
            const int p = std::atoi(name.c_str() + 5);
            if (p < 0 || p >= TbMenu::kPages) continue;
            anyPage = true;
            TbMenuPage& pg = m.pages[p];
            pg.backdrop.clear();
            for (int l = 0; l < 7; ++l) {  // Up%d of the language, "Up" when the page has no Up0
                std::string v = value(body, l == 0 ? "Up0" : "Up" + std::to_string(l));
                if (v.empty()) v = value(body, "Up");
                pg.backdrop.push_back(v);
            }
            continue;
        }

        TbMenuItem it;
        it.name = name;
        it.page = number(body, "Nopage");
        it.group = number(body, "Group");
        it.w = number(body, "With");
        it.h = number(body, "Height");
        it.x = number(body, "x");
        it.y = number(body, "y");
        it.texte = value(body, "Texte");
        it.up = image(body, "Up");
        it.down = image(body, "Down");
        const std::string type = value(body, "TypeTexte");
        const std::string align = lower(value(body, "Align"));  // _strlwr 0x480020
        const auto alignOf = [&](bool full) {
            if (align == "top") return 1;
            if (align == "left") return 2;
            if (full && align == "bottom") return 3;
            if (full && align == "right") return 4;
            if (full && align == "center") return 5;
            return 2;
        };
        if (type == "ISIMAGE") it.type = TbItemType::Image, it.align = alignOf(false);
        else if (type == "ISTEXT") it.type = TbItemType::Text, it.align = alignOf(true);
        else if (type == "ISLABEL") it.type = TbItemType::Label, it.align = alignOf(true);
        else if (type == "ISCHECK") {
            it.type = TbItemType::Check;
            it.selected = value(body, "Selected").find("TRUE") != std::string::npos;
            it.changeable = value(body, "Changeable").find("FALSE") == std::string::npos;
            it.checkType = number(body, "CheckType");
        } else if (type == "ISSLIDE") {
            it.type = TbItemType::Slide;
            it.fillX = number(body, "FillX");
            it.fillY = number(body, "FillY");
        } else if (type == "ISKEY") {
            it.type = TbItemType::Key, it.align = alignOf(true);
            it.keyId = value(body, "KeyId");
            it.linkedKey = value(body, "LinkedKey");
        } else if (type == "ISNONE") it.type = TbItemType::Blank;
        else if (type == "ISLISTBOX") it.type = TbItemType::ListBox;

        const std::string fn = value(body, "Function");
        if (fn.find("Done2") != std::string::npos) it.fn = TbFn::Done2;
        else if (fn.find("Done") != std::string::npos) it.fn = TbFn::Done;
        else if (fn.find("Cancel") != std::string::npos) it.fn = TbFn::Cancel;
        else if (const size_t c = fn.find("Call"); c != std::string::npos) {
            it.fn = TbFn::Call;
            const size_t open = fn.find('(', c);
            it.fnArg = open == std::string::npos ? 0 : std::atoi(fn.c_str() + open + 1);
        } else if (fn.find("LoadSaveGame") != std::string::npos) it.fn = TbFn::Load;
        else if (fn.find("DeleteSaveGame") != std::string::npos) it.fn = TbFn::Delete;
        it.enabled = value(body, "Enable").find("FALSE") == std::string::npos;

        if (name.starts_with("CHECKBOX_ITEM_")) { m.checkbox = std::move(it); continue; }
        if (it.page < 0 || it.page >= TbMenu::kPages) continue;  // 0x416d00(-1) = no item
        auto& groups = m.pages[it.page].groups;
        const size_t idx = m.items.size();
        auto g = std::ranges::find_if(groups, [&](const std::vector<size_t>& v) { return m.items[v.front()].group == it.group; });
        if (g == groups.end()) groups.push_back({idx});
        else g->push_back(idx);
        m.items.push_back(std::move(it));
    }
    if (!anyPage) throw std::runtime_error("Couldn't load the menu file properly, check out your data directory");
    return m;
}

std::vector<TbSaveSlot> parseTbSavedata(std::span<const uint8_t> b) {
    std::vector<TbSaveSlot> out;
    if (b.size() < 9) return out;
    const uint32_t n = u32(b, 0);
    if (n > 1000 || b.size() < 9 + size_t(n) * 0x54) return out;
    for (uint32_t i = 0; i < n; ++i) {
        const std::span<const uint8_t> r = b.subspan(9 + size_t(i) * 0x54, 0x54);
        const char* c = reinterpret_cast<const char*>(r.data() + 1);
        TbSaveSlot& s = out.emplace_back(TbSaveSlot{.slot = r[0], .text = std::string(c, strnlen(c, 0x4b)), .level = int(u32(r, 0x4c))});
        std::ranges::copy(r, s.raw.begin());
    }
    return out;
}

std::vector<uint8_t> writeTbSavedata(std::span<const TbSaveSlot> slots) {
    std::vector<uint8_t> b(9 + slots.size() * 0x54, 0);
    for (int i = 0; i < 4; ++i) b[size_t(i)] = uint8_t(slots.size() >> (8 * i));
    std::memcpy(b.data() + 4, "1.00", 5);
    for (size_t i = 0; i < slots.size(); ++i) {
        uint8_t* r = b.data() + 9 + i * 0x54;
        std::ranges::copy(slots[i].raw, r);
        r[0] = uint8_t(slots[i].slot);
        const size_t n = std::min<size_t>(slots[i].text.size(), 0x4a);
        std::memcpy(r + 1, slots[i].text.data(), n);
        r[1 + n] = 0;
        for (int k = 0; k < 4; ++k) r[0x4c + k] = uint8_t(uint32_t(slots[i].level) >> (8 * k)), r[0x50 + k] = 0;
    }
    return b;
}

} // namespace bb::game
