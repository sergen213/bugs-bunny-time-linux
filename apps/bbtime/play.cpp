#include "play.h"

#include "music.h"

#include <cmath>
#include <format>

namespace bbtime {

using namespace bb::game;

KeyMap makeKeyMap(const std::map<std::string, std::string>& bindings, std::vector<std::string>* warnings) {
    static const std::map<std::string_view, uint16_t> kBits = {
        {"up", PadUp},          {"down", PadDown},         {"left", PadLeft},     {"right", PadRight},
        {"camera_left", PadL2}, {"camera_right", PadR2},   {"roll", PadL1},       {"action", PadR1},
        {"look", PadTriangle},  {"crouch", PadCircle},     {"jump", PadCross},    {"kick", PadSquare},
        {"select", PadSelect},  {"start", PadStart}};
    KeyMap m;
    for (const auto& [action, keys] : bindings) {
        const auto bit = kBits.find(action);
        if (bit == kBits.end()) continue;
        for (size_t pos = 0; pos <= keys.size();) {
            size_t end = keys.find(',', pos);
            if (end == keys.npos) end = keys.size();
            const std::string name = keys.substr(pos, end - pos);
            pos = end + 1;
            if (name.empty()) continue;
            const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                if (warnings) warnings->push_back(std::format("bind.{}: unknown key '{}'", action, name));
                continue;
            }
            m.keys.emplace_back(sc, bit->second);
        }
    }
    m.keys.emplace_back(SDL_SCANCODE_SPACE, PadCross);
    m.keys.emplace_back(SDL_SCANCODE_LCTRL, PadSquare);
    return m;
}

namespace {
// DIK codes of bugs.exe's table 0x45e030 with their SDL scancode (keyboard capture C94, settings bind.*).
constexpr std::pair<uint8_t, SDL_Scancode> kDik[] = {
    {1, SDL_SCANCODE_ESCAPE}, {2, SDL_SCANCODE_1}, {3, SDL_SCANCODE_2}, {4, SDL_SCANCODE_3}, {5, SDL_SCANCODE_4},
    {6, SDL_SCANCODE_5}, {7, SDL_SCANCODE_6}, {8, SDL_SCANCODE_7}, {9, SDL_SCANCODE_8}, {10, SDL_SCANCODE_9},
    {11, SDL_SCANCODE_0}, {12, SDL_SCANCODE_MINUS}, {13, SDL_SCANCODE_EQUALS}, {14, SDL_SCANCODE_BACKSPACE},
    {15, SDL_SCANCODE_TAB}, {16, SDL_SCANCODE_Q}, {17, SDL_SCANCODE_W}, {18, SDL_SCANCODE_E}, {19, SDL_SCANCODE_R},
    {20, SDL_SCANCODE_T}, {21, SDL_SCANCODE_Y}, {22, SDL_SCANCODE_U}, {23, SDL_SCANCODE_I}, {24, SDL_SCANCODE_O},
    {25, SDL_SCANCODE_P}, {26, SDL_SCANCODE_LEFTBRACKET}, {27, SDL_SCANCODE_RIGHTBRACKET}, {28, SDL_SCANCODE_RETURN},
    {29, SDL_SCANCODE_LCTRL}, {30, SDL_SCANCODE_A}, {31, SDL_SCANCODE_S}, {32, SDL_SCANCODE_D}, {33, SDL_SCANCODE_F},
    {34, SDL_SCANCODE_G}, {35, SDL_SCANCODE_H}, {36, SDL_SCANCODE_J}, {37, SDL_SCANCODE_K}, {38, SDL_SCANCODE_L},
    {39, SDL_SCANCODE_SEMICOLON}, {40, SDL_SCANCODE_APOSTROPHE}, {41, SDL_SCANCODE_GRAVE}, {42, SDL_SCANCODE_LSHIFT},
    {43, SDL_SCANCODE_BACKSLASH}, {44, SDL_SCANCODE_Z}, {45, SDL_SCANCODE_X}, {46, SDL_SCANCODE_C}, {47, SDL_SCANCODE_V},
    {48, SDL_SCANCODE_B}, {49, SDL_SCANCODE_N}, {50, SDL_SCANCODE_M}, {51, SDL_SCANCODE_COMMA}, {52, SDL_SCANCODE_PERIOD},
    {53, SDL_SCANCODE_SLASH}, {54, SDL_SCANCODE_RSHIFT}, {55, SDL_SCANCODE_KP_MULTIPLY}, {58, SDL_SCANCODE_CAPSLOCK},
    {59, SDL_SCANCODE_F1}, {60, SDL_SCANCODE_F2}, {61, SDL_SCANCODE_F3}, {62, SDL_SCANCODE_F4}, {63, SDL_SCANCODE_F5},
    {64, SDL_SCANCODE_F6}, {65, SDL_SCANCODE_F7}, {66, SDL_SCANCODE_F8}, {67, SDL_SCANCODE_F9}, {68, SDL_SCANCODE_F10},
    {69, SDL_SCANCODE_NUMLOCKCLEAR}, {70, SDL_SCANCODE_SCROLLLOCK}, {71, SDL_SCANCODE_KP_7}, {72, SDL_SCANCODE_KP_8},
    {73, SDL_SCANCODE_KP_9}, {74, SDL_SCANCODE_KP_MINUS}, {75, SDL_SCANCODE_KP_4}, {76, SDL_SCANCODE_KP_5},
    {77, SDL_SCANCODE_KP_6}, {78, SDL_SCANCODE_KP_PLUS}, {79, SDL_SCANCODE_KP_1}, {80, SDL_SCANCODE_KP_2},
    {81, SDL_SCANCODE_KP_3}, {82, SDL_SCANCODE_KP_0}, {83, SDL_SCANCODE_KP_PERIOD}, {87, SDL_SCANCODE_F11},
    {88, SDL_SCANCODE_F12}, {156, SDL_SCANCODE_KP_ENTER}, {157, SDL_SCANCODE_RCTRL}, {181, SDL_SCANCODE_KP_DIVIDE},
    {183, SDL_SCANCODE_PRINTSCREEN}, {184, SDL_SCANCODE_RALT}, {199, SDL_SCANCODE_HOME}, {200, SDL_SCANCODE_UP},
    {201, SDL_SCANCODE_PAGEUP}, {203, SDL_SCANCODE_LEFT}, {205, SDL_SCANCODE_RIGHT}, {207, SDL_SCANCODE_END},
    {208, SDL_SCANCODE_DOWN}, {209, SDL_SCANCODE_PAGEDOWN}, {210, SDL_SCANCODE_INSERT}, {211, SDL_SCANCODE_DELETE}};

// Gamepad button index n = DirectInput "BUTTON<n>" of the LiT joyMap (6 / 7: the right / left trigger), chosen so the
// exe's default joyMap gives bbtime's layout (✕ south, ○ east, □ west, △ north, L1/R1 shoulders, L2/R2 triggers).
constexpr SDL_GamepadButton kJoyButtons[16] = {
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_INVALID,
    SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1};

bool joyHeld(SDL_Gamepad* pad, int n) {
    if (n == 6 || n == 7)
        return SDL_GetGamepadAxis(pad, n == 6 ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    return n >= 0 && n < 16 && SDL_GetGamepadButton(pad, kJoyButtons[n]);
}
} // namespace

uint8_t dikOf(SDL_Scancode sc) {
    for (const auto& [d, s] : kDik)
        if (s == sc) return d;
    return 0;
}

SDL_Scancode scancodeOfDik(uint8_t dik) {
    for (const auto& [d, s] : kDik)
        if (d == dik) return s;
    return SDL_SCANCODE_UNKNOWN;
}

std::array<bool, 256> dikHeld() {
    std::array<bool, 256> h{};
    const bool* key = SDL_GetKeyboardState(nullptr);
    for (const auto& [d, s] : kDik) h[d] = key[s];
    return h;
}

PadInput readPad(const KeyMap& map, SDL_Gamepad* pad) {
    PadInput in;
    const bool* key = SDL_GetKeyboardState(nullptr);
    for (const auto& [sc, bit] : map.keys)
        if (key[sc]) in.buttons |= bit;
    // C94 capture: with a letter key held, the lowest scan code held (the exe scans its key state from 1).
    bool letter = false;
    for (const auto& [d, s] : kDik) letter |= ((d >= 16 && d <= 25) || (d >= 30 && d <= 38) || (d >= 44 && d <= 50)) && key[s];
    if (letter)
        for (const auto& [d, s] : kDik)
            if (key[s] && (in.key == 0 || d < in.key)) in.key = d;
    if (!pad) return in;  // joyButton 0xff: no joystick (0x403210)
    in.joyButton = 0xfe;
    for (int n = 15; n >= 0; --n)
        if (joyHeld(pad, n)) in.joyButton = uint8_t(n);
    static constexpr uint16_t kJoyBits[8] = {PadSquare, PadCircle, PadTriangle, PadCross, PadL1, PadR2, PadR1, PadL2};
    for (size_t n = 0; n < 8; ++n)
        if (joyHeld(pad, map.joy[n])) in.buttons |= kJoyBits[n];
    static constexpr std::pair<SDL_GamepadButton, uint16_t> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_BACK, PadSelect},         {SDL_GAMEPAD_BUTTON_START, PadStart},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PadUp},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadLeft},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadRight}};
    for (const auto& [b, bit] : kButtons)
        if (SDL_GetGamepadButton(pad, b)) in.buttons |= bit;
    // Analog report (type 0x73): stick bytes 0..255, 0x80 neutral, up = small (SDL: up = negative).
    in.analog = true;
    in.lx = uint8_t((int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) + 32768) >> 8);
    in.ly = uint8_t((int(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)) + 32768) >> 8);
    return in;
}

std::optional<std::vector<PadInput>> parseAutoplay(std::string_view s) {
    std::vector<PadInput> out;
    for (size_t pos = 0; pos < s.size();) {
        size_t end = s.find(',', pos);
        if (end == s.npos) end = s.size();
        const std::string_view seg = s.substr(pos, end - pos);
        pos = end + 1;
        const size_t star = seg.find('*');
        if (star == seg.npos) return std::nullopt;
        PadInput in;
        for (char c : seg.substr(0, star)) {
            switch (c) {
            case 'U': in.buttons |= PadUp; break;
            case 'D': in.buttons |= PadDown; break;
            case 'L': in.buttons |= PadLeft; break;
            case 'R': in.buttons |= PadRight; break;
            case 'X': in.buttons |= PadCross; break;
            case 'S': in.buttons |= PadSquare; break;
            case 'C': in.buttons |= PadCircle; break;
            case 'T': in.buttons |= PadTriangle; break;
            case '1': in.buttons |= PadL1; break;
            case '2': in.buttons |= PadL2; break;
            case '3': in.buttons |= PadR1; break;
            case '4': in.buttons |= PadR2; break;
            case 'H': in.help = true; break;  // LiT F1 (help screen toggle)
            case 'P': in.buttons |= PadStart; break;  // Start (LiT: the Enter key)
            case 'E': in.esc = true; break;           // LiT Esc (title C96)
            case '-': break;
            default: return std::nullopt;
            }
        }
        int n = 0;
        for (char c : seg.substr(star + 1)) {
            if (c < '0' || c > '9') return std::nullopt;
            n = n * 10 + (c - '0');
        }
        if (n <= 0) return std::nullopt;
        out.insert(out.end(), size_t(n), in);
    }
    if (out.empty()) return std::nullopt;
    return out;
}

glm::vec3 toEngine(const V3& v) { return {float(v.x), -float(v.y), -float(v.z)}; }

V3 lerpRoot(const V3& p, const V3& q, float a, int32_t jump) {
    if (std::abs(q.x - p.x) + std::abs(q.z - p.z) > jump || std::abs(q.y - p.y) > std::max(1000, jump)) return q;
    auto m = [a](int32_t x, int32_t y) { return int32_t(std::lround(double(x) + double(y - x) * a)); };
    return {m(p.x, q.x), m(p.y, q.y), m(p.z, q.z)};
}

int16_t lerpAngle(int16_t p, int16_t q, float a) { return int16_t(p + int16_t(std::lround(angleDiff(q, p) * a))); }

void Play::tick(const bb::game::PadInput& in) {
    prev = cur;
    prevActors.resize(world.actors.size());
    for (size_t i = 0; i < world.actors.size(); ++i) {
        const LitActor& a = world.actors[i];
        prevActors[i] = {a.animState, a.pos, {a.rot[0], a.yaw, a.rot[2]}};
    }
    prevPlayerAnim = world.p.animState;
    prevShadows = world.shadows();
    world.tick(in);
    cur = snap();
}

Play::Snap Play::view(float a, bool interpolate) const {
    if (!interpolate) return cur;
    auto mix = [a](const V3& p, const V3& q) {
        auto m = [a](int32_t x, int32_t y) { return int32_t(std::lround(double(x) + double(y - x) * a)); };
        return V3{m(p.x, q.x), m(p.y, q.y), m(p.z, q.z)};
    };
    auto mixAngle = [a](int16_t p, int16_t q) { return int16_t(p + int16_t(std::lround(angleDiff(q, p) * a))); };
    // A camera cut / teleport is shown at once, not smeared across the tick (LS01: Bugs and the eye jump 7000 at tick 22).
    const bool cut = std::abs(cur.eye.x - prev.eye.x) + std::abs(cur.eye.y - prev.eye.y) + std::abs(cur.eye.z - prev.eye.z) > 3 * kLitJump;
    return {cut ? cur.eye : mix(prev.eye, cur.eye), cut ? cur.target : mix(prev.target, cur.target), lerpRoot(prev.player, cur.player, a, kLitJump),
            mixAngle(prev.yaw, cur.yaw),
            mixAngle(prev.lean, cur.lean),
            {mixAngle(prev.parent[0], cur.parent[0]), mixAngle(prev.parent[1], cur.parent[1]), mixAngle(prev.parent[2], cur.parent[2])}};
}

bb::render::Camera Play::camera(float a, bool interpolate) const {
    const Snap v = view(a, interpolate);
    bb::render::Camera c;
    c.pos = toEngine(v.eye);
    const glm::vec3 d = toEngine(v.target) - c.pos;
    c.yaw = std::atan2(-d.x, -d.z);
    c.pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
    return c;
}

KeyMap tbKeyMap(int player) {
    KeyMap m;
    if (player == 0)
        m.keys = {{SDL_SCANCODE_UP, PadUp},     {SDL_SCANCODE_DOWN, PadDown}, {SDL_SCANCODE_RIGHT, PadRight},
                  {SDL_SCANCODE_LEFT, PadLeft}, {SDL_SCANCODE_A, PadL1},      {SDL_SCANCODE_D, PadL2},
                  {SDL_SCANCODE_S, PadR1},      {SDL_SCANCODE_F, PadR2},      {SDL_SCANCODE_W, PadTriangle},
                  {SDL_SCANCODE_Q, PadCircle},  {SDL_SCANCODE_E, PadCross},   {SDL_SCANCODE_R, PadSquare},
                  {SDL_SCANCODE_TAB, PadSelect}, {SDL_SCANCODE_SPACE, PadCross}, {SDL_SCANCODE_LCTRL, PadSquare}};
    else
        m.keys = {{SDL_SCANCODE_KP_8, PadUp},       {SDL_SCANCODE_KP_5, PadDown},      {SDL_SCANCODE_KP_6, PadRight},
                  {SDL_SCANCODE_KP_4, PadLeft},     {SDL_SCANCODE_L, PadL1},           {SDL_SCANCODE_SEMICOLON, PadL2},
                  {SDL_SCANCODE_K, PadR1},          {SDL_SCANCODE_APOSTROPHE, PadR2},  {SDL_SCANCODE_O, PadTriangle},
                  {SDL_SCANCODE_I, PadCircle},      {SDL_SCANCODE_P, PadCross},        {SDL_SCANCODE_LEFTBRACKET, PadSquare},
                  {SDL_SCANCODE_RCTRL, PadSelect},  {SDL_SCANCODE_RSHIFT, PadStart}};
    return m;
}

TbPlay::Snap TbPlay::view(float a, bool interpolate) const {
    if (!interpolate) return cur;
    auto mix = [a](const V3& p, const V3& q) {
        auto m = [a](int32_t x, int32_t y) { return int32_t(std::lround(double(x) + double(y - x) * a)); };
        return V3{m(p.x, q.x), m(p.y, q.y), m(p.z, q.z)};
    };
    auto mixAngle = [a](int16_t p, int16_t q) { return int16_t(p + int16_t(std::lround(angleDiff(q, p) * a))); };
    Snap s;
    s.eye = mix(prev.eye, cur.eye);
    s.look = mix(prev.look, cur.look);
    for (size_t i = 0; i < 2; ++i) {
        // a teleport (call friend, respawn) is not smeared across the screen
        const bool jump = std::abs(cur.pos[i].x - prev.pos[i].x) + std::abs(cur.pos[i].z - prev.pos[i].z) > 400;
        s.pos[i] = jump ? cur.pos[i] : mix(prev.pos[i], cur.pos[i]);
        s.yaw[i] = mixAngle(prev.yaw[i], cur.yaw[i]);
        s.roll[i] = mixAngle(prev.roll[i], cur.roll[i]);
    }
    return s;
}

void TbPlay::tick(const bb::game::PadInput& p1, const bb::game::PadInput& p2) {
    if (world.S[0x22] == 2) world.speechAudio = speech && !tbSpeechFile(speechRoot, world.GD[0x93], world.speechClip).empty();
    prev = cur;
    for (size_t k = 0; k < 2; ++k) prevAnim[k] = world.actors[k].anim;
    std::map<const TbActor*, ActorPrev> ents;
    for (const TbActor& e : world.entities) ents[&e] = {e.anim, e.pos, {e.pitch, e.yaw, e.roll}};
    prevEntities = std::move(ents);
    prevShadows = world.shadows;
    world.tick(p1, p2);
    cur = snap();
}

bb::render::Camera TbPlay::camera(float a, bool interpolate) const {
    const Snap v = view(a, interpolate);
    bb::render::Camera c;
    c.pos = toEngine(v.eye);
    const glm::vec3 d = toEngine(v.look) - c.pos;
    c.yaw = std::atan2(-d.x, -d.z);
    c.pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
    return c;
}

} // namespace bbtime
