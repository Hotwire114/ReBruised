// Run-ahead: see runtime/ps2_runahead.h.
#include "runtime/ps2_runahead.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_rollback.h"
#include "runtime/ps2_rollback_cuts.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    int g_frames = -1;
    uint64_t g_start = 600;
    uint32_t g_hiddenCuts = ps2_rollback::CutDraw | ps2_rollback::CutVu1 | ps2_rollback::CutPresent;
    bool g_stats = false;
    PS2Runtime *g_rt = nullptr;
    std::atomic<bool> g_active{false};
    std::atomic<bool> g_spec{false};
    std::atomic<uint64_t> g_latchSeq{0};

    ps2_rollback::Snapshot g_snap;
    bool g_armed = false;
    int g_specLeft = 0;
    uint64_t g_realFrames = 0;
    uint64_t g_unsafeAt = 0; // real frame (+1) of the last noteHostState; 0 = never
    bool g_paused = false;
    constexpr uint64_t kQuietFrames = 30;
    Clock::time_point g_phaseStart{};

    struct Stats
    {
        Clock::time_point windowStart{};
        uint64_t frames = 0;
        double captureMs = 0, restoreMs = 0, hiddenMs = 0, specMs = 0, latchMs = 0;
        double maxHostMs = 0;
        Clock::time_point lastReal{};
    } g_st;

    double msSince(Clock::time_point t)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    }

    uint32_t specCuts(int left)
    {
        // Every speculative frame keeps the presenter off; only the last one (left == 1) draws.
        uint32_t cuts = ps2_rollback::CutPresent;
        if (left > 1)
            cuts |= ps2_rollback::CutDraw | ps2_rollback::CutVu1;
        return cuts;
    }

    void printStats()
    {
        if (!g_stats)
            return;
        const auto now = Clock::now();
        if (g_st.windowStart == Clock::time_point{})
        {
            g_st = Stats{};
            g_st.windowStart = now;
            return;
        }
        const double secs = std::chrono::duration<double>(now - g_st.windowStart).count();
        if (secs < 5.0)
            return;
        const double n = g_st.frames ? static_cast<double>(g_st.frames) : 1.0;
        std::fprintf(stderr,
                     "[runahead] n=%d fps=%.1f capture=%.2f restore=%.2f hidden=%.2f spec=%.2f latch=%.2f ms (avg) "
                     "host_frame_max=%.1f ms\n",
                     g_frames, g_st.frames / secs, g_st.captureMs / n, g_st.restoreMs / n, g_st.hiddenMs / n,
                     g_st.specMs / n, g_st.latchMs / n, g_st.maxHostMs);
        std::fflush(stderr);
        g_st = Stats{};
        g_st.windowStart = now;
    }

    void wrapper(const EeBoundaryInfo &info, const EeScheduler::BoundaryHook &inner)
    {
        const bool spec = g_spec.load(std::memory_order_relaxed);
        if (!info.vblank)
        {
            // Wake boundaries: only real frames reach the game's driver.
            if (!spec && inner)
                inner(info);
            return;
        }
        if (!spec)
        {
            // A real frame ended.
            if (g_armed)
                g_st.hiddenMs += msSince(g_phaseStart);
            if (inner)
                inner(info);
            ++g_realFrames;
            if (!g_armed)
            {
                if (g_realFrames < g_start)
                    return;
                // Show the speculative frames when the real ones would have been shown: move every host deadline
                // N frames earlier, once (the snapshots carry the moved deadlines from now on).
                g_rt->eeScheduler().shiftHostDeadlines(-std::chrono::microseconds(16667) * g_frames);
                g_armed = true;
                g_active.store(true, std::memory_order_release);
                std::fprintf(stderr, "[runahead] on at real frame %llu: %d frame(s), hidden cuts %x\n",
                             static_cast<unsigned long long>(g_realFrames), g_frames, g_hiddenCuts);
                std::fflush(stderr);
            }
            // Host-side state outside the snapshot (the MPEG decoder of a movie) was used lately: plain real
            // frames until it has been quiet for kQuietFrames (a restore would desync it from the guest).
            const uint64_t unsafe = g_unsafeAt;
            if (unsafe != 0 && g_realFrames < unsafe + kQuietFrames)
            {
                if (!g_paused)
                {
                    g_paused = true;
                    g_active.store(false, std::memory_order_release);
                    std::fprintf(stderr, "[runahead] paused at real frame %llu (movie)\n",
                                 static_cast<unsigned long long>(g_realFrames));
                    std::fflush(stderr);
                }
                ps2_rollback::setIopOutputMuted(false);
                ps2_rollback::setActiveCuts(0);
                return;
            }
            if (g_paused)
            {
                g_paused = false;
                g_active.store(true, std::memory_order_release);
                std::fprintf(stderr, "[runahead] resumed at real frame %llu\n", static_cast<unsigned long long>(g_realFrames));
                std::fflush(stderr);
            }
            const auto now = Clock::now();
            if (g_st.lastReal != Clock::time_point{})
                g_st.maxHostMs = std::max(g_st.maxHostMs, std::chrono::duration<double, std::milli>(now - g_st.lastReal).count());
            g_st.lastReal = now;
            ++g_st.frames;
            printStats();
            const auto t0 = Clock::now();
            ps2_rollback::capture(*g_rt, g_snap);
            g_st.captureMs += msSince(t0);
            g_specLeft = g_frames;
            g_spec.store(true, std::memory_order_release);
            ps2_rollback::setIopOutputMuted(true);
            ps2_rollback::setActiveCuts(specCuts(g_specLeft));
            g_phaseStart = Clock::now();
            return;
        }
        // A speculative frame ended.
        if (--g_specLeft > 0)
        {
            ps2_rollback::setActiveCuts(specCuts(g_specLeft));
            return;
        }
        g_st.specMs += msSince(g_phaseStart);
        // Its picture is what the presenter shows; then back to the real timeline.
        auto t0 = Clock::now();
        g_rt->gs().latchHostPresentationFrame();
        g_latchSeq.fetch_add(1, std::memory_order_acq_rel);
        g_st.latchMs += msSince(t0);
        t0 = Clock::now();
        ps2_rollback::restore(*g_rt, g_snap);
        g_st.restoreMs += msSince(t0);
        g_spec.store(false, std::memory_order_release);
        ps2_rollback::setIopOutputMuted(false);
        ps2_rollback::setActiveCuts(g_hiddenCuts);
        g_phaseStart = Clock::now();
    }

    std::vector<uint32_t> parsePcs(const char *text)
    {
        std::vector<uint32_t> pcs;
        std::string s(text ? text : "");
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t e = s.find(',', pos);
            if (e == std::string::npos)
                e = s.size();
            const std::string item = s.substr(pos, e - pos);
            if (!item.empty())
                pcs.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
            pos = e + 1;
        }
        return pcs;
    }
}

namespace ps2_runahead
{
    int frames()
    {
        if (g_frames < 0)
        {
            const char *v = std::getenv("TEKKEN4_RUNAHEAD");
            g_frames = v ? std::atoi(v) : 0;
            if (g_frames < 0)
                g_frames = 0;
            if (g_frames > 4)
                g_frames = 4;
        }
        return g_frames;
    }

    void install(PS2Runtime &runtime)
    {
        if (frames() <= 0)
            return;
        const char *net = std::getenv("TEKKEN4_NET");
        const char *rb = std::getenv("TEKKEN4_RB_MODE");
        if ((net && *net) || (rb && *rb && std::strcmp(rb, "measure") != 0))
        {
            std::fprintf(stderr, "[runahead] off: netplay / rollback mode already re-simulates (TEKKEN4_RUNAHEAD ignored)\n");
            g_frames = 0;
            return;
        }
        if (const char *v = std::getenv("TEKKEN4_RUNAHEAD_START"))
            g_start = std::strtoull(v, nullptr, 10);
        if (const char *v = std::getenv("TEKKEN4_RUNAHEAD_HIDDEN_CUTS"))
            g_hiddenCuts = static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) | ps2_rollback::CutPresent;
        if (const char *v = std::getenv("TEKKEN4_RUNAHEAD_STATS"))
            g_stats = v[0] == '1';
        g_rt = &runtime;
#ifdef PS2X_ROLLBACK_IOP_STATE
        // The IOP is part of the snapshot: speculative frames run it muted and are rolled back with it.
        ps2_rollback::setIncludeIop(true);
#endif
        EeScheduler &sched = runtime.eeScheduler();
        if (const char *idle = std::getenv("TEKKEN4_RUNAHEAD_IDLE"))
            sched.setIdleLoopPcs(parsePcs(idle));
        if (!sched.hasBoundaryHook())
            sched.setBoundaryHook([](const EeBoundaryInfo &) {}); // boundaries only exist with a hook
        sched.setBoundaryWrapper(wrapper);
        std::fprintf(stderr, "[runahead] installed: %d frame(s) from real frame %llu\n", g_frames,
                     static_cast<unsigned long long>(g_start));
        std::fflush(stderr);
    }

    bool active() noexcept
    {
        return g_active.load(std::memory_order_acquire);
    }

    uint64_t latchSequence() noexcept
    {
        return g_latchSeq.load(std::memory_order_acquire);
    }

    void noteHostState() noexcept
    {
        if (g_frames > 0)
            g_unsafeAt = g_realFrames + 1;
    }

    bool speculative() noexcept
    {
        return g_spec.load(std::memory_order_acquire);
    }
}
