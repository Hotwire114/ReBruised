#include "runtime/ps2_pad.h"
#include "runtime/ps2_input_latency.h"
#include "runtime/ps2_host_pad.h"
#include "runtime/ps2_pad_bindings.h"
#include "ps2_host_backend.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readBound(int player, unsigned flags, uint8_t *data, size_t size)
{
    if (!data || size < 32 || player < 0 || player >= ps2_pad_bindings::kPlayers)
        return false;
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    const std::shared_ptr<const ps2_pad_bindings::Config> cfg = ps2_pad_bindings::current();
    const ps2_pad_bindings::PlayerCfg &c = cfg->p[player];
    uint16_t btns = 0xFFFFu;
    if ((flags & kBoundPad) != 0 && c.pad)
    {
        ps2_host_pad::State pad;
        if (ps2_host_pad::readPlayer(player, pad))
        {
            btns &= static_cast<uint16_t>(~pad.buttons);
            data[4] = pad.rx;
            data[5] = pad.ry;
            data[6] = pad.lx;
            data[7] = pad.ly;
        }
    }
    if ((flags & kBoundKeys) != 0 && (c.keyboard || (flags & kBoundForceKeys) != 0))
    {
        // Posted keys from the window thread and the physical keyboard, at pad-read time (patch 31).
        std::vector<uint16_t> vks, bits;
        for (const auto &kb : ps2_pad_bindings::flatKeys(c.keyMap, player))
        {
            vks.push_back(kb.first);
            bits.push_back(kb.second);
        }
        uint16_t held = 0;
        if (ps2_input_latency::sampleKeys(vks.data(), bits.data(), vks.size(), held))
            btns &= static_cast<uint16_t>(~held);
    }
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    // A bindings file that changes something: this player's devices and buttons as the file says.
    if (!ps2_pad_bindings::current()->isDefault())
        return readBound(0, kBoundPad | kBoundKeys, data, size);

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // The first host controller (ps2_host_pad: XInput read right now, else raylib's latest poll).
    ps2_host_pad::State pad;
    if (ps2_host_pad::read(0, pad))
    {
        btns &= static_cast<uint16_t>(~pad.buttons);
        data[4] = pad.rx;
        data[5] = pad.ry;
        data[6] = pad.lx;
        data[7] = pad.ly;
    }

    // The keyboard always works, also with a gamepad plugged in:
    // D-pad = WASD, Cross/Circle/Square/Triangle = Down/Right/Left/Up arrows
    // (or X/C/Z/V), L1/R1 = Q/E, L2/R2 = LeftShift/RightShift, Start = Enter, Select = Tab.
    // Fast path: the window thread publishes posted keys and this thread samples
    // the physical keyboard. Legacy keeps raylib's once-per-frame IsKeyDown.
    uint16_t hostKeys = 0;
    if (ps2_input_latency::sampleKeyboard(hostKeys))
    {
        btns &= ~hostKeys;
    }
    else
    {
        if (IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

#if defined(_WIN32) && !defined(PLATFORM_ANDROID)
// raylib builds GLFW in (rglfw.c); a GUID starting with "xinput" in hex is a pad GLFW reads through XInput.
extern "C" const char *glfwGetJoystickGUID(int jid);
static bool glfwPadIsXInput(int id)
{
    const char *guid = glfwGetJoystickGUID(id);
    return guid && std::strncmp(guid, "78696e707574", 12) == 0;
}
#else
static bool glfwPadIsXInput(int) { return false; }
#endif

void PSPadBackend::publishHostPads()
{
    ps2_host_pad::RaylibPad pads[4];
    int n = 0;
    if (IsWindowReady())
    {
        for (int i = 0; i < 4; ++i)
        {
            if (!IsGamepadAvailable(i))
                continue;
            ps2_host_pad::RaylibPad &p = pads[n++];
            p.id = i;
            p.xinput = glfwPadIsXInput(i);
            const char *name = GetGamepadName(i);
            std::snprintf(p.name, sizeof(p.name), "%s", name ? name : "?");
            uint16_t b = 0;
            const auto down = [i](int button)
            { return IsGamepadButtonDown(i, button); };
            // raylib's positional names: RIGHT_FACE_DOWN = A / Cross, ... (same as before)
            if (down(GAMEPAD_BUTTON_LEFT_FACE_UP))
                b |= PAD_UP;
            if (down(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                b |= PAD_DOWN;
            if (down(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                b |= PAD_LEFT;
            if (down(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
                b |= PAD_RIGHT;
            if (down(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                b |= PAD_CROSS;
            if (down(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
                b |= PAD_CIRCLE;
            if (down(GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
                b |= PAD_SQUARE;
            if (down(GAMEPAD_BUTTON_RIGHT_FACE_UP))
                b |= PAD_TRIANGLE;
            if (down(GAMEPAD_BUTTON_LEFT_TRIGGER_1))
                b |= PAD_L1;
            if (down(GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
                b |= PAD_R1;
            if (down(GAMEPAD_BUTTON_LEFT_TRIGGER_2))
                b |= PAD_L2;
            if (down(GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
                b |= PAD_R2;
            if (down(GAMEPAD_BUTTON_MIDDLE_RIGHT))
                b |= PAD_START;
            if (down(GAMEPAD_BUTTON_MIDDLE_LEFT))
                b |= PAD_SELECT;
            if (down(GAMEPAD_BUTTON_LEFT_THUMB))
                b |= PAD_L3;
            if (down(GAMEPAD_BUTTON_RIGHT_THUMB))
                b |= PAD_R3;
            p.buttons = b;
            p.lx = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_X);
            p.ly = GetGamepadAxisMovement(i, GAMEPAD_AXIS_LEFT_Y);
            p.rx = GetGamepadAxisMovement(i, GAMEPAD_AXIS_RIGHT_X);
            p.ry = GetGamepadAxisMovement(i, GAMEPAD_AXIS_RIGHT_Y);
        }
    }
    ps2_host_pad::publishRaylib(pads, n);
}
