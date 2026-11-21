#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // Pad bindings (ps2_pad_bindings.h): the 32-byte pad packet of `player` (0 / 1) from its configured devices.
    // flags: 1 the pad part, 2 the keyboard part, 4 the keyboard even when the player's device choice leaves it out
    // (TEKKEN4_NET_PAD=-1). readState() takes this path for port 0 once a bindings file changes anything.
    enum : unsigned { kBoundPad = 1u, kBoundKeys = 2u, kBoundForceKeys = 4u };
    bool readBound(int player, unsigned flags, uint8_t *data, size_t size);

    // Window thread, right after raylib polled input: hands raylib's gamepads to ps2_host_pad.
    void publishHostPads();
};

#endif
