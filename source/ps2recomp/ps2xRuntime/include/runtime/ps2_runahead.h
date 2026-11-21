#pragma once

#include <cstdint>

class PS2Runtime;

// Run-ahead (RetroArch style, single instance) on the rollback snapshot machinery (ps2_rollback):
// every host frame the REAL frame runs hidden (cuts: no draw, no VU1, no present; IOP live, its sound is heard),
// the machine is captured, N speculative frames run with the same input (IOP muted, the last one draws), their
// picture is latched for the presenter, and the machine is restored. The host deadlines are moved N frames
// earlier once, so the speculative picture of frame k+N is shown when frame k would have been: N frames of the
// game's own input lag less. The real timeline (what the game state becomes) is unchanged; only the input of a
// speculative frame may be one frame old.
//
// TEKKEN4_RUNAHEAD=0|1|2 (default 0 = off). Off in netplay / rollback test modes (TEKKEN4_NET, TEKKEN4_RB_MODE
// other than measure). TEKKEN4_RUNAHEAD_START=<frame> (default 600): real frames before it run normally (boot).
// TEKKEN4_RUNAHEAD_HIDDEN_CUTS: cuts of the hidden real frame (hex, default draw|vu1|present = 0x19).
// TEKKEN4_RUNAHEAD_STATS=1: every 5 s one [runahead] line (frames, capture/restore/hidden/speculative ms, fps).
// TEKKEN4_RUNAHEAD_IDLE=<hex pc,...>: idle loops the scheduler may skip (as the rollback driver does).
namespace ps2_runahead
{
    int frames();
    // PS2Runtime::run, before the game thread starts (after the game overrides installed their hooks).
    void install(PS2Runtime &runtime);
    // True while run-ahead owns the presentation latch (the presenter must not latch on vsync ticks).
    bool active() noexcept;
    // Bumped after every latch run-ahead made; the presenter copies the latched picture when it changes.
    uint64_t latchSequence() noexcept;
    // EE thread: a stub used host-side state the snapshot does not hold (the MPEG decoder): run-ahead pauses
    // until 30 real frames have passed without such a call.
    void noteHostState() noexcept;
    // A speculative frame is running: pad reads must not consume latched taps.
    bool speculative() noexcept;
}
