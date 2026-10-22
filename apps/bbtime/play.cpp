#include "play.h"

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

PadInput readPad(const KeyMap& map, SDL_Gamepad* pad) {
    PadInput in;
    const bool* key = SDL_GetKeyboardState(nullptr);
    for (const auto& [sc, bit] : map.keys)
        if (key[sc]) in.buttons |= bit;
    if (!pad) return in;
    static constexpr std::pair<SDL_GamepadButton, uint16_t> kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PadCross},         {SDL_GAMEPAD_BUTTON_EAST, PadCircle},
        {SDL_GAMEPAD_BUTTON_WEST, PadSquare},         {SDL_GAMEPAD_BUTTON_NORTH, PadTriangle},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PadL1},    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PadR1},
        {SDL_GAMEPAD_BUTTON_BACK, PadSelect},         {SDL_GAMEPAD_BUTTON_START, PadStart},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, PadUp},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PadDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PadLeft},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PadRight}};
    for (const auto& [b, bit] : kButtons)
        if (SDL_GetGamepadButton(pad, b)) in.buttons |= bit;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) in.buttons |= PadL2;
    if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) in.buttons |= PadR2;
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

Play::Snap Play::view(float a, bool interpolate) const {
    if (!interpolate) return cur;
    auto mix = [a](const V3& p, const V3& q) {
        auto m = [a](int32_t x, int32_t y) { return int32_t(std::lround(double(x) + double(y - x) * a)); };
        return V3{m(p.x, q.x), m(p.y, q.y), m(p.z, q.z)};
    };
    auto mixAngle = [a](int16_t p, int16_t q) { return int16_t(p + int16_t(std::lround(angleDiff(q, p) * a))); };
    return {mix(prev.eye, cur.eye), mix(prev.target, cur.target), mix(prev.player, cur.player), mixAngle(prev.yaw, cur.yaw),
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
