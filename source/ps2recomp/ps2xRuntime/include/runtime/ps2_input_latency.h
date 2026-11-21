#pragma once

#include <cstddef>
#include <cstdint>

// Host input-latency path. Instrumentation (QPC log, frame hash) runs only when
// TEKKEN4_INPUT_LAG=1. TEKKEN4_INPUT_LEGACY=1 keeps the old once-per-host-frame
// raylib poll. The fast path samples posted keys from the window thread and
// physical keys with GetAsyncKeyState at pad-read time.
namespace ps2_input_latency
{
    bool enabled();
    bool legacy();
    // "paced" (default): present on a 16.667 ms clock.
    // "tick" (TEKKEN4_INPUT_PRESENT=tick): present when the guest vsync tick
    // changes. Measured slower (readback lands while the GS is still drawing).
    // Ignored when legacy() is set.
    bool presentOnTick();

    void attachWindow(void *hwnd);
    void detachWindow();

    // Window thread. Returns after a queued input message, a guest vblank
    // (notifyTick) or timeoutMs.
    void waitMessage(unsigned timeoutMs);
    // EE thread, at every guest VBlankStart: wakes waitMessage.
    void notifyTick();

    // Game thread. Active-high pad bits. False: caller uses raylib IsKeyDown.
    bool sampleKeyboard(uint16_t &padDownMask);

    // Game thread. Pad bindings: the keys of a player's [playerN.keys] map. n pairs (virtual key, active-high PS2
    // bit); padDownMask gets the bits of the pairs whose key is down (posted by the window thread, or physically
    // down while the game window has the focus). False: no window yet / legacy mode, caller reads no keyboard.
    bool sampleKeys(const uint16_t *vks, const uint16_t *bits, size_t n, uint16_t &padDownMask);

    // Game thread, after scePadRead wrote the pad packet. pressedMask is active-high.
    void notePad(uint16_t pressedMask, uint64_t vsyncTick);

    // Frame-chain instrumentation (docs/INPUT.md of the Tekken 4 port):
    // TEKKEN4_PRESENT_LOG=1: one [present] line per presented guest frame: the guest tick shown, host QPC of that
    //   tick's VBlankStart, of the latch and of the swap return (host-added time = swap - vblank).
    // TEKKEN4_VBLANK_HASH=1: at every VBlankStart the game thread latches the displayed picture and logs
    //   [vblank] shown=<tick> hash=<exact pixel hash> pad=<port-0 buttons read during that tick>; two
    //   deterministic runs that differ in one press give the game's own frames of lag.
    bool presentLog();
    bool vblankHash();
    // Game thread, at VBlankStart before the tick advances (tick = the frame whose picture was just shown).
    void noteVBlank(uint64_t newTick);
    uint16_t padMaskOfTick(uint64_t tick);
    // Host ms since that tick's VBlankStart (-1: unknown).
    double msSinceVBlank(uint64_t tick);
    uint64_t hashExact(const uint8_t *rgba, uint32_t width, uint32_t height);
    // GS frontend (EE thread): the game's full-screen copy into the display buffer was submitted (frame complete).
    void noteDisplayCopy(uint64_t tick, uint32_t destFbp);

    // A scripted press (ps2_host_pad's TEKKEN4_PAD_TEST): t1 = now, timed like a key from here on.
    void openTestPress(uint32_t tag, uint16_t bit);

    // Window thread, around the latch that actually calls GS present.
    void onLatchBegin(uint64_t vsyncTick);
    void onLatchPixels(const uint8_t *rgba, uint32_t width, uint32_t height);
    void onSwapReturned();
}
