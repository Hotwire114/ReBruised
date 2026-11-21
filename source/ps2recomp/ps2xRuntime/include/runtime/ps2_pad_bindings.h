#pragma once

// Pad bindings: which physical input is which PS2 button, per player (docs/INPUT.md of the Tekken 4 port,
// "Pad bindings"). Header only, no windows.h, no raylib: ps2_host_pad.cpp (XInput), ps2_input_latency.cpp
// (keyboard) and ps2_pad.cpp include it, and tools/pad_bindings_test.cpp compiles it on its own.
//
// The file (TEKKEN4_CONTROLS=<path>, else controls.ini in the working directory = the save folder; re-read
// within ~1 s when it changes). No file, an empty file, or a file that only says `device = auto` and has no
// binding lines = the fixed layout of patches 29/60 exactly (isDefault(): every caller keeps its old code).
//
//   [player1]                 ; player1 = pad port 1, player2 = port 2
//   device = auto             ; auto | none | keyboard | xinput | xinput:0..3, joined with +  (keyboard+xinput:1)
//   [player1.pad]             ; XInput bindings; a button not listed keeps its default
//   SQUARE = X                ; TARGET = SOURCE SOURCE ... ; `none` = unbound
//   UP = DUP LUP
//   [player1.keys]            ; keyboard bindings, same rules (player 2 has no default keys)
//   SQUARE = LEFT Z NUM0
//
// TARGET: SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE, or the Tekken names
// 1 2 3 4 (= SQUARE TRIANGLE CROSS CIRCLE; LP RP LK RK).
// Pad SOURCE: A B X Y LB RB LT RT BACK START LS RS DUP DDOWN DLEFT DRIGHT LUP LDOWN LLEFT LRIGHT RUP RDOWN
// RLEFT RRIGHT. Key SOURCE: A..Z 0..9 F1..F12 UP DOWN LEFT RIGHT ENTER TAB SPACE ESC BACKSPACE LSHIFT RSHIFT
// LCTRL RCTRL LALT RALT NUM0..NUM9 NUMADD NUMSUB NUMMUL NUMDIV NUMDEC SEMICOLON COMMA PERIOD SLASH MINUS
// EQUALS GRAVE LBRACKET RBRACKET BACKSLASH APOSTROPHE INSERT DELETE HOME END PGUP PGDN CAPSLOCK.
//
// Bindings only choose which input presses which PS2 button. The sticks' analog bytes, the game state and
// netplay's input format (PS2 button bits, active-low like the pad packet) are untouched.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ps2_pad_bindings
{
    constexpr int kButtons = 16;
    constexpr int kPlayers = 2;

    // Index == bit position of the PS2 pad word (active-high here): SELECT = 0x0001 ... SQUARE = 0x8000.
    inline const char *const kButtonNames[kButtons] = {"SELECT", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT",
                                                       "L2", "R2", "L1", "R1", "TRIANGLE", "CIRCLE", "CROSS", "SQUARE"};
    enum : int
    {
        B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1,
        B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE
    };

    inline std::string upper(std::string s)
    {
        for (char &c : s)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return s;
    }

    inline std::string trim(const std::string &s)
    {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
            ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
            --b;
        return s.substr(a, b - a);
    }

    // PS2 button index of a target name, or -1.
    inline int buttonIndex(const std::string &name)
    {
        const std::string n = upper(name);
        for (int i = 0; i < kButtons; ++i)
            if (n == kButtonNames[i])
                return i;
        if (n == "1" || n == "LP")
            return B_SQUARE;
        if (n == "2" || n == "RP")
            return B_TRIANGLE;
        if (n == "3" || n == "LK")
            return B_CROSS;
        if (n == "4" || n == "RK")
            return B_CIRCLE;
        return -1;
    }

    // ------------------------------------------------------------------------------------ XInput sources
    enum SrcKind : uint8_t
    {
        SK_BUTTON = 1, // arg = XINPUT_GAMEPAD_* wButtons mask
        SK_LT = 2,
        SK_RT = 3,
        SK_AXIS = 4 // arg 0..7 = LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT
    };
    struct Src
    {
        uint8_t kind = 0;
        uint16_t arg = 0;
        bool operator==(const Src &o) const { return kind == o.kind && arg == o.arg; }
    };

    constexpr uint16_t X_DUP = 0x0001, X_DDOWN = 0x0002, X_DLEFT = 0x0004, X_DRIGHT = 0x0008, X_START = 0x0010,
                       X_BACK = 0x0020, X_LS = 0x0040, X_RS = 0x0080, X_LB = 0x0100, X_RB = 0x0200, X_A = 0x1000,
                       X_B = 0x2000, X_X = 0x4000, X_Y = 0x8000;
    constexpr int kTriggerThreshold = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD
    constexpr float kDirOn = 0.5f;        // stick direction: on past half way ...
    constexpr float kDirOff = 0.4f;       // ... off again only back inside 40 %

    inline const char *const kAxisNames[8] = {"LUP", "LDOWN", "LLEFT", "LRIGHT", "RUP", "RDOWN", "RLEFT", "RRIGHT"};
    struct NamedButton
    {
        const char *name;
        uint16_t mask;
    };
    inline const NamedButton kXButtons[] = {{"A", X_A},     {"B", X_B},       {"X", X_X},         {"Y", X_Y},
                                            {"LB", X_LB},   {"RB", X_RB},     {"BACK", X_BACK},   {"START", X_START},
                                            {"LS", X_LS},   {"RS", X_RS},     {"DUP", X_DUP},     {"DDOWN", X_DDOWN},
                                            {"DLEFT", X_DLEFT}, {"DRIGHT", X_DRIGHT}};

    inline bool parsePadSource(const std::string &name, Src &out)
    {
        const std::string n = upper(name);
        for (const NamedButton &b : kXButtons)
            if (n == b.name)
            {
                out = Src{SK_BUTTON, b.mask};
                return true;
            }
        if (n == "LT")
        {
            out = Src{SK_LT, 0};
            return true;
        }
        if (n == "RT")
        {
            out = Src{SK_RT, 0};
            return true;
        }
        for (int i = 0; i < 8; ++i)
            if (n == kAxisNames[i])
            {
                out = Src{SK_AXIS, static_cast<uint16_t>(i)};
                return true;
            }
        return false;
    }

    inline std::string padSourceName(const Src &s)
    {
        if (s.kind == SK_BUTTON)
            for (const NamedButton &b : kXButtons)
                if (b.mask == s.arg)
                    return b.name;
        if (s.kind == SK_LT)
            return "LT";
        if (s.kind == SK_RT)
            return "RT";
        if (s.kind == SK_AXIS && s.arg < 8)
            return kAxisNames[s.arg];
        return "?";
    }

    // One XInput sample in XInput's own units (y up).
    struct RawPad
    {
        uint16_t buttons = 0;
        uint8_t lt = 0, rt = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    inline float axisNorm(int16_t v)
    {
        const float f = static_cast<float>(v) / 32767.0f;
        return (std::min)(1.0f, (std::max)(-1.0f, f));
    }

    // The 8 stick directions held (bit i = kAxisNames[i]), with hysteresis against `prev` (the previous result of
    // the same reader). Same thresholds and axes as the fixed layout's stick -> d-pad.
    inline uint32_t axisBits(const RawPad &r, uint32_t prev)
    {
        const float lx = axisNorm(r.lx), ly = -axisNorm(r.ly), rx = axisNorm(r.rx), ry = -axisNorm(r.ry);
        const float v[8] = {-ly, ly, -lx, lx, -ry, ry, -rx, rx};
        uint32_t out = 0;
        for (int i = 0; i < 8; ++i)
            if (v[i] > kDirOn || (((prev >> i) & 1u) != 0 && v[i] > kDirOff))
                out |= 1u << i;
        return out;
    }

    // ------------------------------------------------------------------------------------ keys
    struct NamedKey
    {
        const char *name;
        uint16_t vk;
    };
    // The first name of a key is the one written back.
    inline const NamedKey kKeyNames[] = {
        {"UP", 0x26}, {"DOWN", 0x28}, {"LEFT", 0x25}, {"RIGHT", 0x27}, {"SPACE", 0x20}, {"ENTER", 0x0D},
        {"RETURN", 0x0D}, {"TAB", 0x09}, {"ESC", 0x1B}, {"ESCAPE", 0x1B}, {"BACKSPACE", 0x08}, {"LSHIFT", 0xA0},
        {"RSHIFT", 0xA1}, {"LCTRL", 0xA2}, {"RCTRL", 0xA3}, {"LALT", 0xA4}, {"RALT", 0xA5}, {"CAPSLOCK", 0x14},
        {"INSERT", 0x2D}, {"DELETE", 0x2E}, {"HOME", 0x24}, {"END", 0x23}, {"PGUP", 0x21}, {"PGDN", 0x22},
        {"NUM0", 0x60}, {"NUM1", 0x61}, {"NUM2", 0x62}, {"NUM3", 0x63}, {"NUM4", 0x64}, {"NUM5", 0x65},
        {"NUM6", 0x66}, {"NUM7", 0x67}, {"NUM8", 0x68}, {"NUM9", 0x69}, {"NUMADD", 0x6B}, {"NUMSUB", 0x6D},
        {"NUMMUL", 0x6A}, {"NUMDIV", 0x6F}, {"NUMDEC", 0x6E}, {"SEMICOLON", 0xBA}, {"EQUALS", 0xBB},
        {"COMMA", 0xBC}, {"MINUS", 0xBD}, {"PERIOD", 0xBE}, {"SLASH", 0xBF}, {"GRAVE", 0xC0}, {"LBRACKET", 0xDB},
        {"BACKSLASH", 0xDC}, {"RBRACKET", 0xDD}, {"APOSTROPHE", 0xDE}, {"QUOTE", 0xDE},
        {"F1", 0x70}, {"F2", 0x71}, {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75}, {"F7", 0x76},
        {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F11", 0x7A}, {"F12", 0x7B}};

    inline uint16_t parseKey(const std::string &name)
    {
        const std::string n = upper(name);
        if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9')))
            return static_cast<uint16_t>(n[0]); // VK_A..VK_Z and VK_0..VK_9 are the ASCII codes
        for (const NamedKey &k : kKeyNames)
            if (n == k.name)
                return k.vk;
        return 0;
    }

    inline std::string keyName(uint16_t vk)
    {
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return std::string(1, static_cast<char>(vk));
        for (const NamedKey &k : kKeyNames)
            if (k.vk == vk)
                return k.name;
        return "?";
    }

    // ------------------------------------------------------------------------------------ maps
    // A button's sources; `set[b]` says the file listed that button (an empty list = unbound), otherwise the
    // built-in default stays.
    struct PadMap
    {
        std::vector<Src> src[kButtons];
        bool set[kButtons] = {};
        bool any() const { return std::any_of(std::begin(set), std::end(set), [](bool b) { return b; }); }
    };
    struct KeyMap
    {
        std::vector<uint16_t> vk[kButtons];
        bool set[kButtons] = {};
        bool any() const { return std::any_of(std::begin(set), std::end(set), [](bool b) { return b; }); }
    };

    // The fixed layout (patch 60): positional face buttons, bumpers, triggers, Back = Select, sticks click, the
    // d-pad and the left stick (the stick part only while stickDpad is on).
    inline std::vector<Src> defaultPadSources(int b)
    {
        switch (b)
        {
        case B_UP: return {{SK_BUTTON, X_DUP}, {SK_AXIS, 0}};
        case B_DOWN: return {{SK_BUTTON, X_DDOWN}, {SK_AXIS, 1}};
        case B_LEFT: return {{SK_BUTTON, X_DLEFT}, {SK_AXIS, 2}};
        case B_RIGHT: return {{SK_BUTTON, X_DRIGHT}, {SK_AXIS, 3}};
        case B_START: return {{SK_BUTTON, X_START}};
        case B_SELECT: return {{SK_BUTTON, X_BACK}};
        case B_L3: return {{SK_BUTTON, X_LS}};
        case B_R3: return {{SK_BUTTON, X_RS}};
        case B_L1: return {{SK_BUTTON, X_LB}};
        case B_R1: return {{SK_BUTTON, X_RB}};
        case B_L2: return {{SK_LT, 0}};
        case B_R2: return {{SK_RT, 0}};
        case B_CROSS: return {{SK_BUTTON, X_A}};
        case B_CIRCLE: return {{SK_BUTTON, X_B}};
        case B_SQUARE: return {{SK_BUTTON, X_X}};
        case B_TRIANGLE: return {{SK_BUTTON, X_Y}};
        default: return {};
        }
    }

    // Player 1's fixed keyboard (patch 29/31, ps2_input_latency.cpp asyncPadMask); player 2 has none.
    inline std::vector<uint16_t> defaultKeys(int player, int b)
    {
        if (player != 0)
            return {};
        switch (b)
        {
        case B_UP: return {'W'};
        case B_DOWN: return {'S'};
        case B_LEFT: return {'A'};
        case B_RIGHT: return {'D'};
        case B_CROSS: return {0x28, 'X', 0x20};
        case B_CIRCLE: return {0x27, 'C', 0x1B};
        case B_SQUARE: return {0x25, 'Z', 0x60};
        case B_TRIANGLE: return {0x26, 'V', 0x61};
        case B_L1: return {'Q'};
        case B_R1: return {'E'};
        case B_L2: return {0xA0};
        case B_R2: return {0xA1};
        case B_START: return {0x0D};
        case B_SELECT: return {0x09};
        default: return {};
        }
    }

    inline const std::vector<Src> &padSources(const PadMap &m, int b, std::vector<Src> &scratch)
    {
        if (m.set[b])
            return m.src[b];
        scratch = defaultPadSources(b);
        return scratch;
    }

    // PS2 buttons (active-high word) held on a pad. `axis` in = the previous axisBits, out = the new ones.
    // stickDpad: whether the default d-pad sources include the left stick (TEKKEN4_PAD_STICK_DPAD).
    inline uint16_t evalPad(const PadMap &m, const RawPad &r, bool stickDpad, uint32_t &axis)
    {
        axis = axisBits(r, axis);
        uint16_t out = 0;
        std::vector<Src> scratch;
        for (int b = 0; b < kButtons; ++b)
        {
            const bool fixed = !m.set[b];
            for (const Src &s : padSources(m, b, scratch))
            {
                bool on = false;
                switch (s.kind)
                {
                case SK_BUTTON: on = (r.buttons & s.arg) != 0; break;
                case SK_LT: on = r.lt > kTriggerThreshold; break;
                case SK_RT: on = r.rt > kTriggerThreshold; break;
                case SK_AXIS: on = ((axis >> s.arg) & 1u) != 0 && (!fixed || stickDpad); break;
                default: break;
                }
                if (on)
                {
                    out = static_cast<uint16_t>(out | (1u << b));
                    break;
                }
            }
        }
        return out;
    }

    // (virtual key, PS2 bit) pairs of a player's keyboard; the list the key readers poll.
    inline std::vector<std::pair<uint16_t, uint16_t>> flatKeys(const KeyMap &m, int player)
    {
        std::vector<std::pair<uint16_t, uint16_t>> out;
        for (int b = 0; b < kButtons; ++b)
        {
            const std::vector<uint16_t> keys = m.set[b] ? m.vk[b] : defaultKeys(player, b);
            for (uint16_t vk : keys)
                out.emplace_back(vk, static_cast<uint16_t>(1u << b));
        }
        return out;
    }

    // ------------------------------------------------------------------------------------ the config
    struct PlayerCfg
    {
        bool autoDevice = true; // device = auto: today's order (player 1: keyboard + first pad; player 2: second pad)
        bool keyboard = false;  // reads the keyboard
        bool pad = false;       // reads an XInput pad (and, for `auto`, the DirectInput ones raylib sees)
        int xslot = -1;         // pinned XInput slot, -1 = the next free one in order
        PadMap padMap;
        KeyMap keyMap;
    };

    struct Config
    {
        PlayerCfg p[kPlayers];
        Config()
        {
            p[0].keyboard = true;
            p[0].pad = true;
            p[1].pad = true;
        }
        bool isDefault() const
        {
            for (const PlayerCfg &c : p)
                if (!c.autoDevice || c.padMap.any() || c.keyMap.any())
                    return false;
            return true;
        }
        // Slots pinned by a player (bit mask over XInput slots 0..3).
        unsigned pinnedMask() const
        {
            unsigned m = 0;
            for (const PlayerCfg &c : p)
                if (c.pad && c.xslot >= 0)
                    m |= 1u << c.xslot;
            return m;
        }
    };

    inline std::vector<std::string> splitWords(const std::string &s)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n')
            {
                if (!cur.empty())
                    out.push_back(cur), cur.clear();
            }
            else
                cur.push_back(c);
        }
        if (!cur.empty())
            out.push_back(cur);
        return out;
    }

    inline void parseDevice(const std::string &value, int player, PlayerCfg &c, std::vector<std::string> *warn, int line)
    {
        std::string v = value;
        for (char &ch : v)
            if (ch == '+')
                ch = ' ';
        const std::vector<std::string> tokens = splitWords(v);
        PlayerCfg fresh;
        fresh.keyboard = false;
        fresh.pad = false;
        fresh.autoDevice = false;
        bool understood = !tokens.empty();
        for (const std::string &t : tokens)
        {
            const std::string u = upper(t);
            if (u == "AUTO")
            {
                fresh.autoDevice = true;
                fresh.keyboard = player == 0;
                fresh.pad = true;
            }
            else if (u == "NONE")
            {
            }
            else if (u == "KEYBOARD")
                fresh.keyboard = true;
            else if (u == "XINPUT")
                fresh.pad = true;
            else if (u.size() == 8 && u.compare(0, 7, "XINPUT:") == 0 && u[7] >= '0' && u[7] <= '3')
            {
                fresh.pad = true;
                fresh.xslot = u[7] - '0';
            }
            else
            {
                understood = false;
                if (warn)
                    warn->push_back("line " + std::to_string(line) + ": unknown device '" + t + "'");
            }
        }
        if (!understood)
            return; // keep auto
        if (fresh.autoDevice)
        {
            // `auto` is exactly the default order; anything joined to it is ignored.
            c.autoDevice = true;
            c.keyboard = player == 0;
            c.pad = true;
            c.xslot = -1;
            return;
        }
        c.autoDevice = false;
        c.keyboard = fresh.keyboard;
        c.pad = fresh.pad;
        c.xslot = fresh.xslot;
    }

    // Parses a file's text. Unknown lines and sources are skipped with a warning; the rest still applies.
    inline Config parse(const std::string &text, std::vector<std::string> *warn = nullptr)
    {
        Config cfg;
        std::istringstream in(text);
        std::string raw;
        int line = 0;
        int player = -1;
        enum Part { NONE, DEVICE, PAD, KEYS } part = NONE;
        bool first = true;
        while (std::getline(in, raw))
        {
            ++line;
            if (first)
            {
                first = false;
                if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF && static_cast<unsigned char>(raw[1]) == 0xBB &&
                    static_cast<unsigned char>(raw[2]) == 0xBF)
                    raw.erase(0, 3);
            }
            const size_t cut = raw.find_first_of(";#");
            if (cut != std::string::npos)
                raw.erase(cut);
            const std::string s = trim(raw);
            if (s.empty())
                continue;
            if (s.front() == '[')
            {
                const size_t end = s.find(']');
                std::string name = end == std::string::npos ? "" : upper(trim(s.substr(1, end - 1)));
                player = -1;
                part = NONE;
                if (name.size() >= 7 && name.compare(0, 6, "PLAYER") == 0 && (name[6] == '1' || name[6] == '2'))
                {
                    player = name[6] - '1';
                    const std::string rest = name.substr(7);
                    if (rest.empty())
                        part = DEVICE;
                    else if (rest == ".PAD")
                        part = PAD;
                    else if (rest == ".KEYS")
                        part = KEYS;
                    else
                        player = -1;
                }
                if (part == NONE && name != "GENERAL" && warn)
                    warn->push_back("line " + std::to_string(line) + ": unknown section '" + s + "'");
                continue;
            }
            const size_t eq = s.find('=');
            if (eq == std::string::npos || part == NONE)
            {
                if (part != NONE && warn)
                    warn->push_back("line " + std::to_string(line) + ": not 'name = value'");
                continue;
            }
            const std::string key = trim(s.substr(0, eq)), value = trim(s.substr(eq + 1));
            PlayerCfg &c = cfg.p[player];
            if (part == DEVICE)
            {
                if (upper(key) == "DEVICE")
                    parseDevice(value, player, c, warn, line);
                continue;
            }
            const int b = buttonIndex(key);
            if (b < 0)
            {
                if (warn)
                    warn->push_back("line " + std::to_string(line) + ": unknown button '" + key + "'");
                continue;
            }
            if (part == PAD)
            {
                c.padMap.set[b] = true;
                c.padMap.src[b].clear();
                for (const std::string &w : splitWords(value))
                {
                    if (upper(w) == "NONE")
                        continue;
                    Src src;
                    if (parsePadSource(w, src))
                    {
                        if (std::find(c.padMap.src[b].begin(), c.padMap.src[b].end(), src) == c.padMap.src[b].end())
                            c.padMap.src[b].push_back(src);
                    }
                    else if (warn)
                        warn->push_back("line " + std::to_string(line) + ": unknown pad button '" + w + "'");
                }
            }
            else
            {
                c.keyMap.set[b] = true;
                c.keyMap.vk[b].clear();
                for (const std::string &w : splitWords(value))
                {
                    if (upper(w) == "NONE")
                        continue;
                    const uint16_t vk = parseKey(w);
                    if (vk != 0)
                    {
                        if (std::find(c.keyMap.vk[b].begin(), c.keyMap.vk[b].end(), vk) == c.keyMap.vk[b].end())
                            c.keyMap.vk[b].push_back(vk);
                    }
                    else if (warn)
                        warn->push_back("line " + std::to_string(line) + ": unknown key '" + w + "'");
                }
            }
        }
        return cfg;
    }

    inline std::string describe(const Config &cfg)
    {
        std::string out;
        for (int i = 0; i < kPlayers; ++i)
        {
            const PlayerCfg &c = cfg.p[i];
            out += "P" + std::to_string(i + 1) + ": ";
            if (c.autoDevice)
                out += "auto";
            else if (!c.keyboard && !c.pad)
                out += "none";
            else
            {
                if (c.keyboard)
                    out += "keyboard";
                if (c.keyboard && c.pad)
                    out += "+";
                if (c.pad)
                    out += c.xslot >= 0 ? "xinput:" + std::to_string(c.xslot) : std::string("xinput");
            }
            int padLines = 0, keyLines = 0;
            for (int b = 0; b < kButtons; ++b)
                padLines += c.padMap.set[b], keyLines += c.keyMap.set[b];
            out += " (" + std::to_string(padLines) + " pad, " + std::to_string(keyLines) + " key bindings)";
            out += i + 1 < kPlayers ? "; " : "";
        }
        return out;
    }

    // ------------------------------------------------------------------------------------ the live file
    // current() is cheap enough for a 1 kHz poll thread: a thread-local copy, refreshed when the generation moves.
    // The file is looked at most once a second (size + time stamp; no read while it is unchanged).
    struct Live
    {
        std::mutex mu;
        std::shared_ptr<const Config> cfg = std::make_shared<const Config>();
        std::atomic<uint64_t> gen{1};
        std::atomic<int64_t> nextCheckMs{0};
        std::string path;
        bool pathKnown = false;
        bool havePath = false;
        bool loadedOnce = false;
        uintmax_t size = 0;
        long long stamp = 0;
        bool existed = false;
        int periodMs = 1000;
    };

    inline Live &live()
    {
        static Live l;
        return l;
    }

    inline int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Under live().mu.
    inline void reloadLocked(Live &l, bool log)
    {
        std::error_code ec;
        const bool exists = l.havePath && std::filesystem::is_regular_file(l.path, ec);
        uintmax_t size = 0;
        long long stamp = 0;
        if (exists)
        {
            size = std::filesystem::file_size(l.path, ec);
            stamp = static_cast<long long>(std::filesystem::last_write_time(l.path, ec).time_since_epoch().count());
        }
        if (l.loadedOnce && exists == l.existed && size == l.size && stamp == l.stamp)
            return;
        std::shared_ptr<const Config> next;
        std::vector<std::string> warn;
        if (exists)
        {
            std::ifstream f(l.path, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            next = std::make_shared<const Config>(parse(ss.str(), &warn));
        }
        else
            next = std::make_shared<const Config>();
        l.loadedOnce = true;
        l.existed = exists;
        l.size = size;
        l.stamp = stamp;
        l.cfg = next;
        l.gen.fetch_add(1, std::memory_order_acq_rel);
        if (log && (exists || l.gen.load() > 2))
        {
            std::fprintf(stderr, "[pad] controls: %s %s -- %s\n", exists ? "loaded" : "removed, fixed layout again",
                         l.path.c_str(), next->isDefault() ? "default layout" : describe(*next).c_str());
            for (const std::string &w : warn)
                std::fprintf(stderr, "[pad] controls: %s\n", w.c_str());
            std::fflush(stderr);
        }
    }

    inline void initPathLocked(Live &l)
    {
        if (l.pathKnown)
            return;
        l.pathKnown = true;
        const char *env = std::getenv("TEKKEN4_CONTROLS");
        std::string p = env ? env : "controls.ini";
        if (p == "0" || p == "off")
            l.havePath = false;
        else
        {
            l.path = p;
            l.havePath = true;
        }
        if (const char *per = std::getenv("TEKKEN4_CONTROLS_POLL_MS"))
            l.periodMs = (std::max)(10, std::atoi(per));
    }

    inline std::shared_ptr<const Config> current()
    {
        Live &l = live();
        thread_local std::shared_ptr<const Config> mine;
        thread_local uint64_t mineGen = 0;
        const int64_t now = nowMs();
        if (now >= l.nextCheckMs.load(std::memory_order_relaxed) || mineGen == 0)
        {
            std::unique_lock<std::mutex> lock(l.mu, std::defer_lock);
            if (mineGen == 0)
                lock.lock(); // first use on this thread: wait for the loader
            else
                (void)lock.try_lock();
            if (lock.owns_lock() && (now >= l.nextCheckMs.load(std::memory_order_relaxed) || !l.loadedOnce))
            {
                initPathLocked(l);
                l.nextCheckMs.store(now + l.periodMs, std::memory_order_relaxed);
                reloadLocked(l, true);
            }
        }
        const uint64_t g = l.gen.load(std::memory_order_acquire);
        if (g != mineGen || !mine)
        {
            std::lock_guard<std::mutex> lock(l.mu);
            mine = l.cfg;
            mineGen = l.gen.load(std::memory_order_acquire);
        }
        return mine;
    }

    // Tests: read this file now (and again on every call), without the one-second rate limit.
    inline void useFileForTest(const std::string &path, int periodMs = 10)
    {
        Live &l = live();
        std::lock_guard<std::mutex> lock(l.mu);
        l.pathKnown = true;
        l.path = path;
        l.havePath = !path.empty();
        l.periodMs = periodMs;
        l.nextCheckMs.store(0);
        l.loadedOnce = false;
    }
}
