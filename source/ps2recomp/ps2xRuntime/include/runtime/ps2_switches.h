#pragma once

#include <atomic>

// Host-side switches: named on/off options of the runtime that a setting (environment) starts and a key can flip while
// the game runs. They only steer host work (the picture); nothing guest-visible may depend on one, so two netplay peers
// may differ in them. Tekken 4: "widescreen" (TEKKEN4_WIDESCREEN, F9), see GS::widescreenAdjust.
namespace ps2_switches
{
    // The flag of a switch, created on first use from the environment and kept for the process.
    std::atomic<bool> &flag(const char *name);

    inline void set(const char *name, bool on)
    {
        flag(name).store(on, std::memory_order_relaxed);
    }

    inline bool get(const char *name)
    {
        return flag(name).load(std::memory_order_relaxed);
    }
}

// One lookup per use site, then a relaxed load.
#define PS2_SWITCH(name)                                                  \
    ([]() -> std::atomic<bool> & {                                        \
        static std::atomic<bool> &switchFlag = ::ps2_switches::flag(name); \
        return switchFlag;                                                \
    }()                                                                   \
         .load(std::memory_order_relaxed))
