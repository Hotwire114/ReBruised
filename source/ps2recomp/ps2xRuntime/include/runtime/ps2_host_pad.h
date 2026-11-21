#pragma once

#include <cstdint>

// Host game controllers for the PS2 pad ports.
//
// Direct backend (default on Windows): XInput pads (Xbox pads, XInput sticks, DS4Windows/Steam virtual pads)
// are read by this module itself, at pad-read time on the game thread (a fresh XInputGetState, the same
// moment patch 31 samples the keyboard with GetAsyncKeyState). A 1 ms poll thread only latches presses
// that start and end between two pad reads, so a tap shorter than a frame is still seen once.
// Other controllers (DirectInput: DualShock 4 / DualSense without DS4Windows, generic pads) come from
// raylib/GLFW: the window thread publishes a snapshot right after each raylib input poll.
// The left stick also presses the d-pad (8-way, past half way); sticks get a radial deadzone.
//
// TEKKEN4_PAD_BACKEND=raylib: the old path (every pad from raylib's snapshot, no XInput, no stick -> d-pad).
// TEKKEN4_PAD_STICK_DPAD=0: the left stick no longer presses the d-pad.
// TEKKEN4_PAD_STATS=1: every 5 s one [pad-stats] line (reads, XInput call cost, snapshot age, taps).
// TEKKEN4_PAD_TEST=start=<s>,count=<n>,period=<ms>,hold=<ms>,what=dpad|stick: a scripted XInput pad 0
// (fake XInputGetState, no real pad needed) presses d-pad Down/Up from <s> s after start; with
// TEKKEN4_INPUT_LAG=1 each press is timed like a key (ps2_input_latency, vk = 0x10000 | bit).
namespace ps2_host_pad
{
    // Active-high PS2 bits (SELECT = 0x0001 ... SQUARE = 0x8000); axes as PS2 bytes (0x80 centre, 0 = left/up).
    struct State
    {
        uint16_t buttons = 0;
        uint8_t rx = 0x80;
        uint8_t ry = 0x80;
        uint8_t lx = 0x80;
        uint8_t ly = 0x80;
    };

    // One raylib (GLFW) gamepad as the window thread saw it right after a poll.
    struct RaylibPad
    {
        int id = -1;          // raylib gamepad index
        bool xinput = false;  // GLFW reads it through XInput (the direct backend reads those itself)
        uint16_t buttons = 0; // active-high PS2 bits from raylib's buttons (L2/R2 from raylib's trigger buttons)
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f; // -1..1, +y = down
        char name[64]{};
    };

    bool rawRaylibOnly(); // TEKKEN4_PAD_BACKEND=raylib
    bool testActive();    // TEKKEN4_PAD_TEST set

    // Window thread, right after raylib polled input (PollInputEvents / EndDrawing).
    void publishRaylib(const RaylibPad *pads, int count);

    // Game thread (scePadRead, netplay frame sample). nth = 0-based host controller; false: none there.
    bool read(int nth, State &out);
    bool present(int nth);

    // Pad bindings (ps2_pad_bindings.h, TEKKEN4_CONTROLS): player 0 / 1 = pad port 1 / 2.
    // readPlayer: the pad this player's device choice names (a pinned XInput slot, or the next free one in order
    // like read()), its buttons through the player's [playerN.pad] map; false: no pad there. Without a bindings
    // file this is read(player).
    // playerPresent: the player has something to read: a keyboard, or the pad above.
    bool readPlayer(int player, State &out);
    bool playerPresent(int player);
}
