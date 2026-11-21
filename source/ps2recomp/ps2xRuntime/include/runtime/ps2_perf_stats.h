#pragma once
// Per-second performance counters (PS2X_PERF_STATS=<seconds>): one "[perf]" line per second on stdout with the frame
// rate as presented, the EE (game) thread's busy time, the GS worker's queue back-pressure and drain waits, the GL
// worker's busy time and the GL batch / draw / texture counts. Off by default; when off every counter site is one
// relaxed load of a static bool.
#include <atomic>
#include <chrono>
#include <cstdint>

class PS2Runtime;

#ifdef _WIN32
// Declared like <windows.h> does (BOOL, HANDLE, PULONG64), so a translation unit may include both.
extern "C" __declspec(dllimport) int __stdcall QueryThreadCycleTime(void *ThreadHandle, unsigned long long *CycleTime);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
#endif

namespace ps2_perf_stats
{
    // Processor cycles the calling thread has run so far (not the time it waited or was preempted): a cost measure
    // that other programs on the PC do not inflate. 0 where the OS has no such counter.
    inline uint64_t threadCycles() noexcept
    {
#ifdef _WIN32
        unsigned long long cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        return cycles;
#else
        return 0;
#endif
    }

    bool enabled() noexcept;
    bool frameTrace() noexcept;
    // Sound-driver frame call from the game loop; host-only counts by scene.
    void noteGameLogic(uint32_t scene) noexcept;

    struct Counters
    {
        std::atomic<uint64_t> eeGifNs{0};          // GS packet processing on the EE thread (GS::processGIFPacket)
        std::atomic<uint64_t> gsPublishWaitNs{0};  // EE waits for room in the GS worker queue (back-pressure)
        std::atomic<uint64_t> gsPublishWaits{0};
        std::atomic<uint64_t> gsDrainWaitNs{0};    // waits until the GS worker has run everything queued (sync points)
        std::atomic<uint64_t> gsDrains{0};
        std::atomic<uint64_t> gsWorkerBusyNs{0};   // GS worker executing queued commands (GL calls included)
        std::atomic<uint64_t> glBatches{0};
        std::atomic<uint64_t> glDraws{0};          // primitives
        std::atomic<uint64_t> texDecodes{0};
        std::atomic<uint64_t> texRowDecodes{0};
        std::atomic<uint64_t> texVersionHits{0};   // index textures taken back from an earlier decode of the same bytes
        std::atomic<uint64_t> rtUploads{0};
        std::atomic<uint64_t> latches{0};          // presenter latches (one per vsync tick)
        std::atomic<uint64_t> gsDrainBy[16]{};     // GS worker round trips by caller (kDrainNames)
    };
    extern Counters g_counters;
    constexpr size_t kDrainKinds = 16;
    inline const char *const kDrainNames[kDrainKinds] = {"other", "init", "reset", "flush", "sync", "present", "clear", "l2h",
                                                         "readvram", "snapshot", "xfersnap", "state", "", "", "", ""};

    inline uint64_t nowNs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline void add(std::atomic<uint64_t> &counter, uint64_t value = 1u) noexcept
    {
        counter.fetch_add(value, std::memory_order_relaxed);
    }

    // Adds the scope's wall time to a counter when the stats are on.
    class Timer
    {
    public:
        explicit Timer(std::atomic<uint64_t> &slot) noexcept : m_slot(slot), m_start(enabled() ? nowNs() : 0u) {}
        ~Timer()
        {
            if (m_start)
                m_slot.fetch_add(nowNs() - m_start, std::memory_order_relaxed);
        }
        Timer(const Timer &) = delete;
        Timer &operator=(const Timer &) = delete;

    private:
        std::atomic<uint64_t> &m_slot;
        uint64_t m_start;
    };

    // Starts the reporter thread (once) when PS2X_PERF_STATS is set.
    void start(PS2Runtime &runtime);
    // Join and flush the reporter while runtime is still alive.
    void stop();

    // Frame pacing ("[frames]" line per second when the stats are on): the window thread notes every present with the
    // guest vsync tick it shows (interval between swaps; a tick shown twice = repeat, ticks never shown = skipped), the
    // EE thread every VBlankStart it processes (interval between guest frames in host time).
    struct PresentWork
    {
        uint64_t uploadNs = 0, drawNs = 0, swapNs = 0, sharedWaitNs = 0, cycles = 0;
        uint64_t sharedSequence = 0, sharedFresh = 0, sharedTick = 0, sharedRenderSequence = 0, requestedTick = 0;
    };
    void notePresent(uint64_t shownTick, PresentWork work = {}) noexcept;
    // Legacy QPC probes are queued too: no file I/O on the presentation/EE threads.
    void notePresentQpc(uint64_t tick, uint64_t vb, uint64_t latch, uint64_t swap, uint64_t frequency) noexcept;
    void noteDisplayCopy(uint64_t tick, uint32_t fbp, double afterVblankMs) noexcept;
    void noteVblank(uint64_t hostWaitNs = 0, uint64_t tick = 0) noexcept; // hostWaitNs: the scheduler's pacing-wait total so far
}

// Counts one event in g_counters.<name> when the stats are on (an expression, usable after a comma).
#define PS2_PERF_COUNT(name) (::ps2_perf_stats::enabled() ? static_cast<void>(::ps2_perf_stats::g_counters.name.fetch_add(1u, std::memory_order_relaxed)) : static_cast<void>(0))
