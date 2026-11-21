#pragma once
// Host overlay (Tekken 4 port, patch 88): a game override's own 2D layer, drawn by the presenter right after the
// game's picture -- inside raylib's BeginDrawing/EndDrawing, in window pixels, before the debug panel. Host-only:
// the callback must never write guest state (online state hashes stay the same with and without it).
class PS2Runtime;

namespace ps2_host_overlay
{
    struct Frame
    {
        float x, y, w, h;      // where the game's picture sits in the window (letterboxed, scaled)
        int screenW, screenH;  // the window's size
        bool wide = false;    // the HUD occupies the middle 3/4 of a widescreen picture
    };
    using DrawFn = void (*)(PS2Runtime &runtime, const Frame &frame, void *user);
    struct Slot
    {
        DrawFn fn = nullptr;
        void *user = nullptr;
    };
    inline Slot &slot()
    {
        static Slot s;
        return s;
    }
    // One overlay; a later set() replaces it. Call before the window opens (an override's apply()).
    inline void set(DrawFn fn, void *user)
    {
        slot().fn = fn;
        slot().user = user;
    }
    inline void draw(PS2Runtime &runtime, const Frame &frame)
    {
        const Slot &s = slot();
        if (s.fn)
            s.fn(runtime, frame, s.user);
    }
}
