#pragma once
// Black & Bruised: controller button prompts. The game draws its PS2 button symbols (cross, circle, square,
// triangle, L1...) as sprites from cells of a font atlas. A game port registers the atlas (a fingerprint of one cell)
// and its button cells; the GS backend then marks every sprite that samples such a cell (screen box in the frame
// target, button, alpha) and publishes a frame's marks when the game copies that frame to the display buffer. The
// port's host overlay (ps2_host_overlay.h) draws the player's controller glyphs over the published marks. Host-only:
// nothing here touches guest state.
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2_button_prompts
{
    enum Button : uint8_t { Cross, Circle, Square, Triangle, L1, R1, L2, R2, Count };

    struct Cell { float u0, v0, u1, v1; Button button; };   // texel box (min/max) in the atlas
    struct Atlas
    {
        uint32_t signature = 0;                              // FNV-1a of the raw texels of the signature box
        int sigU0 = 0, sigV0 = 0, sigU1 = 0, sigV1 = 0;      // texel box hashed for the signature
        std::vector<Cell> cells;
    };
    struct Mark { float x0, y0, x1, y1; Button button; float alpha; };

    struct State
    {
        std::mutex mutex;
        bool enabled = false;
        bool log = false;
        std::vector<Atlas> atlases;
        std::vector<std::pair<uint32_t, Mark>> collecting;   // (frame buffer page, mark)
        std::vector<Mark> published;
        uint64_t publishSerial = 0;
    };
    inline State &state()
    {
        static State s;
        return s;
    }
    // port: before the game starts
    inline void registerAtlas(const Atlas &a)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.atlases.push_back(a);
        s.enabled = true;
    }
    inline bool enabled() { return state().enabled; }
    // GS backend
    inline void add(uint32_t fbp, const Mark &m)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.collecting.size() < 512)
            s.collecting.emplace_back(fbp, m);
    }
    inline bool hasMarksFor(uint32_t fbp)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (const auto &e : s.collecting)
            if (e.first == fbp) return true;
        return false;
    }
    // the frame in `fbp` is being shown: its marks become the published set
    inline void publish(uint32_t fbp)
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<Mark> out;
        std::vector<std::pair<uint32_t, Mark>> keep;
        for (const auto &e : s.collecting)
            (e.first == fbp ? out.push_back(e.second) : keep.push_back(e));
        s.collecting.swap(keep);
        s.published.swap(out);
        ++s.publishSerial;
    }
    // the frame has no button symbols any more (its copy happened without marks)
    inline void publishEmpty()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.published.clear();
        ++s.publishSerial;
    }
    // presenter
    inline std::vector<Mark> latest()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.published;
    }
}
