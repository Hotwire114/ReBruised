#include "runtime/ps2_perf_stats.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_rollback_cuts.h"
#include "runtime/ps2_vu1_async.h"

#include <cstdio>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2_perf_stats
{
    Counters g_counters;
    static std::atomic<uint64_t> g_logic[17]{};
    void noteGameLogic(uint32_t scene) noexcept
    {
        if (frameTrace()) g_logic[scene < 16 ? scene : 16].fetch_add(1, std::memory_order_relaxed);
    }

    bool frameTrace() noexcept
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_FRAME_TRACE"); return v && *v == '1'; }();
        return on;
    }

    namespace
    {
        std::thread g_reporter;
        std::atomic<bool> g_stop{false};
        std::mutex g_reporterMutex;
        std::condition_variable g_reporterCv;
        int seconds()
        {
            static const int value = []
            {
                const char *env = std::getenv("PS2X_PERF_STATS");
                // PS2X_PERF_STATS=<seconds to report>; 1 (the natural "on") = until the program exits, not one line.
                const int n = env && *env ? std::atoi(env) : 0;
                const char *probe = std::getenv("TEKKEN4_PRESENT_LOG");
                const char *hashProbe = std::getenv("TEKKEN4_VBLANK_HASH");
                return n == 1 || frameTrace() || (probe && *probe == '1') || (hashProbe && *hashProbe == '1') ? 1000000 : n;
            }();
            return value;
        }
    }

    bool enabled() noexcept
    {
        static const bool on = seconds() > 0;
        return on;
    }

    namespace
    {
        struct HitchSnap
        {
            uint64_t eeGif = 0, pubWait = 0, drainWait = 0, worker = 0, texDec = 0, rtUp = 0, drains = 0, hostWait = 0;
        };
        struct FrameEvent
        {
            char kind = 0; // p/g: present/guest; q/c: legacy QPC/display-copy probe
            uint64_t t = 0, tick = 0;
            uint32_t intervalUs = 0;
            HitchSnap then{}, now{};
            PresentWork work{};
            uint64_t vb = 0, latch = 0, swap = 0, frequency = 0;
            uint32_t fbp = 0;
            double afterVblankMs = 0;
        };
        constexpr size_t kFrameCapacity = 16384; // bounded; overflow is explicitly reported
        struct FrameLog
        {
            std::mutex mutex;
            std::vector<uint32_t> presentUs, vblankUs;
            std::vector<FrameEvent> events;
            uint64_t lastPresentNs = 0, lastVblankNs = 0, lastTick = 0, dropped = 0;
            uint32_t repeats = 0, skipped = 0;
            bool haveTick = false;
        };
        FrameLog g_frames;
        void enqueue(const FrameEvent &event) // g_frames.mutex held; never allocate on a game thread
        {
            if (g_frames.events.size() < g_frames.events.capacity())
                g_frames.events.push_back(event);
            else
                ++g_frames.dropped;
        }

        std::atomic<uint64_t> g_t0Ns{0};
        double hitchMs()
        {
            static const double v = []
            {
                const char *e = std::getenv("PS2X_PERF_HITCH_MS");
                return e && *e ? std::atof(e) : 20.0;
            }();
            return v;
        }
        HitchSnap takeHitchSnap(uint64_t hostWait = 0)
        {
            const Counters &c = g_counters;
            const auto r = [](const std::atomic<uint64_t> &a) { return a.load(std::memory_order_relaxed); };
            return HitchSnap{r(c.eeGifNs), r(c.gsPublishWaitNs), r(c.gsDrainWaitNs), r(c.gsWorkerBusyNs),
                             r(c.texDecodes), r(c.rtUploads), r(c.gsDrains), hostWait};
        }
        void reportHitch(const char *kind, uint64_t nowNs_, uint32_t intervalUs, const HitchSnap &then, const HitchSnap &now)
        {
            const auto ms = [](uint64_t a, uint64_t b) { return static_cast<double>(b - a) / 1e6; };
            std::printf("[hitch] %s t=%.3f dt=%.1f eegif=%.1f pubwait=%.1f hostwait=%.1f drainwait=%.1f worker=%.1f texdec=%llu rtup=%llu drains=%llu\n",
                        kind, static_cast<double>(nowNs_ - g_t0Ns.load(std::memory_order_relaxed)) / 1e9, intervalUs / 1000.0,
                        ms(then.eeGif, now.eeGif), ms(then.pubWait, now.pubWait), ms(then.hostWait, now.hostWait), ms(then.drainWait, now.drainWait),
                        ms(then.worker, now.worker), static_cast<unsigned long long>(now.texDec - then.texDec),
                        static_cast<unsigned long long>(now.rtUp - then.rtUp), static_cast<unsigned long long>(now.drains - then.drains));
        }
        HitchSnap g_presentSnap, g_vblankSnap;

        std::string joinUs(const std::vector<uint32_t> &v)
        {
            std::string s;
            char buf[16];
            for (uint32_t us : v)
            {
                std::snprintf(buf, sizeof(buf), s.empty() ? "%u" : ",%u", us / 100u); // 0.1 ms units
                s += buf;
            }
            return s;
        }
    }

    void notePresent(uint64_t shownTick, PresentWork work) noexcept
    {
        if (!enabled()) return;
        const uint64_t t = nowNs();
        const HitchSnap snap = takeHitchSnap();
        std::lock_guard lock(g_frames.mutex);
        const uint32_t us = g_frames.lastPresentNs ? static_cast<uint32_t>((t - g_frames.lastPresentNs) / 1000u) : 0u;
        if (us && g_frames.presentUs.size() < g_frames.presentUs.capacity())
            g_frames.presentUs.push_back(us);
        if (frameTrace() || (hitchMs() > 0.0 && us > hitchMs() * 1000.0))
            enqueue(FrameEvent{'p', t, shownTick, us, g_presentSnap, snap, work});
        g_presentSnap = snap;
        g_frames.lastPresentNs = t;
        if (g_frames.haveTick)
        {
            if (shownTick == g_frames.lastTick) ++g_frames.repeats;
            else if (shownTick > g_frames.lastTick + 1u)
                g_frames.skipped += static_cast<uint32_t>(shownTick - g_frames.lastTick - 1u);
        }
        g_frames.lastTick = shownTick;
        g_frames.haveTick = true;
    }

    void noteVblank(uint64_t hostWaitNs, uint64_t tick) noexcept
    {
        if (!enabled()) return;
        const uint64_t t = nowNs();
        const HitchSnap snap = takeHitchSnap(hostWaitNs);
        std::lock_guard lock(g_frames.mutex);
        const uint32_t us = g_frames.lastVblankNs ? static_cast<uint32_t>((t - g_frames.lastVblankNs) / 1000u) : 0u;
        if (us && g_frames.vblankUs.size() < g_frames.vblankUs.capacity())
            g_frames.vblankUs.push_back(us);
        if (frameTrace() || (hitchMs() > 0.0 && us > hitchMs() * 1000.0))
            enqueue(FrameEvent{'g', t, tick, us, g_vblankSnap, snap});
        g_vblankSnap = snap;
        g_frames.lastVblankNs = t;
    }

    void notePresentQpc(uint64_t tick, uint64_t vb, uint64_t latch, uint64_t swap, uint64_t frequency) noexcept
    {
        if (!enabled()) return;
        FrameEvent event{}; event.kind = 'q'; event.tick = tick;
        event.vb = vb; event.latch = latch; event.swap = swap; event.frequency = frequency;
        std::lock_guard lock(g_frames.mutex);
        enqueue(event);
    }

    void noteDisplayCopy(uint64_t tick, uint32_t fbp, double afterVblankMs) noexcept
    {
        if (!enabled()) return;
        FrameEvent event{}; event.kind = 'c'; event.tick = tick;
        event.fbp = fbp; event.afterVblankMs = afterVblankMs;
        std::lock_guard lock(g_frames.mutex);
        enqueue(event);
    }

    void stop()
    {
        g_stop.store(true, std::memory_order_release);
        g_reporterCv.notify_all();
        if (g_reporter.joinable()) g_reporter.join();
    }

    void start(PS2Runtime &runtime)
    {
        static std::atomic<bool> started{false};
        if (!enabled() || started.exchange(true))
            return;
        // VU1 / VIF1 host time come from the rollback profile counters (two clock reads per VU1 run / VIF1 packet).
        if (const char *vu = std::getenv("PS2X_PERF_STATS_VU"); vu && *vu == '1')
            ps2_rollback::g_hostProfile.on = true;
        PS2Runtime *rt = &runtime;
        g_t0Ns.store(nowNs(), std::memory_order_relaxed);
        g_frames.presentUs.reserve(kFrameCapacity);
        g_frames.vblankUs.reserve(kFrameCapacity);
        g_frames.events.reserve(kFrameCapacity);
        g_reporter = std::thread([rt]()
                    {
            struct Snap
            {
                uint64_t t, hostWait, eeGif, pubWait, pubWaits, drainWait, drains, worker, batches, draws, texDec, rowDec, verHits, rtUp,
                    latches, vu1, vif1;
            };
            const auto take = [rt]()
            {
                const Counters &c = g_counters;
                const auto r = [](const std::atomic<uint64_t> &a) { return a.load(std::memory_order_relaxed); };
                return Snap{nowNs(), rt->eeScheduler().hostWaitNanoseconds(), r(c.eeGifNs), r(c.gsPublishWaitNs),
                            r(c.gsPublishWaits), r(c.gsDrainWaitNs), r(c.gsDrains), r(c.gsWorkerBusyNs), r(c.glBatches),
                            r(c.glDraws), r(c.texDecodes), r(c.texRowDecodes), r(c.texVersionHits), r(c.rtUploads), r(c.latches),
                            ps2_rollback::g_hostProfile.vu1Ns, ps2_rollback::g_hostProfile.vif1Ns};
            };
            const auto ms = [](uint64_t a, uint64_t b) { return static_cast<double>(b - a) / 1e6; };
            Snap last = take();
            uint64_t lastBy[kDrainKinds] = {};
            uint64_t lastLogic[17] = {};
            const uint64_t t0 = g_t0Ns.load(std::memory_order_relaxed);
            std::vector<uint32_t> pres, vbl;
            std::vector<FrameEvent> events;
            pres.reserve(kFrameCapacity); vbl.reserve(kFrameCapacity); events.reserve(kFrameCapacity);
            std::FILE *trace = frameTrace() ? std::fopen("frame_trace.csv", "w") : nullptr;
            if (frameTrace() && !trace) std::fprintf(stderr, "[frame-trace] ERROR opening frame_trace.csv\n");
            if (trace)
            {
                std::fprintf(trace, "# steady_clock nanoseconds; deltas overlap across threads, not an exclusive CPU breakdown\n");
                std::fprintf(trace, "kind,t_ns,t_s,tick,requested_tick,interval_us,upload_ns,draw_ns,swap_ns,shared_wait_ns,present_cycles,shared_sequence,shared_fresh,shared_tick,shared_render_sequence,ee_gif_ns,gs_pubwait_ns,gs_drainwait_ns,gs_worker_ns,texdec,rtup,drains,hostwait_ns\n");
            }
            for (int s = 0; s < seconds(); ++s)
            {
                {
                    std::unique_lock lock(g_reporterMutex);
                    g_reporterCv.wait_for(lock, std::chrono::seconds(1), [] { return g_stop.load(std::memory_order_acquire); });
                }
                const Snap now = take();
                const double wall = ms(last.t, now.t);
                // EE busy = wall time minus the scheduler's host pacing waits (the game thread is idle there)
                std::printf("[perf] t=%.1f vsyncs=%llu ee_busy=%.1f gif=%.1f gs_pubwait=%.1f/%llu gs_drain=%.1f/%llu "
                            "gs_worker=%.1f batches=%llu draws=%llu texdec=%llu rowdec=%llu texver=%llu rtup=%llu vu1=%.1f vif1=%.1f\n",
                            static_cast<double>(now.t - t0) / 1e9,
                            static_cast<unsigned long long>(now.latches - last.latches), wall - ms(last.hostWait, now.hostWait),
                            ms(last.eeGif, now.eeGif), ms(last.pubWait, now.pubWait),
                            static_cast<unsigned long long>(now.pubWaits - last.pubWaits), ms(last.drainWait, now.drainWait),
                            static_cast<unsigned long long>(now.drains - last.drains), ms(last.worker, now.worker),
                            static_cast<unsigned long long>(now.batches - last.batches),
                            static_cast<unsigned long long>(now.draws - last.draws),
                            static_cast<unsigned long long>(now.texDec - last.texDec),
                            static_cast<unsigned long long>(now.rowDec - last.rowDec),
                            static_cast<unsigned long long>(now.verHits - last.verHits),
                            static_cast<unsigned long long>(now.rtUp - last.rtUp), ms(last.vu1, now.vu1), ms(last.vif1, now.vif1));
                if (frameTrace())
                    for (unsigned scene = 0; scene < 17; ++scene)
                    {
                        const uint64_t n = g_logic[scene].load(std::memory_order_relaxed);
                        if (n != lastLogic[scene])
                            std::printf("[logic] t=%.3f scene=%u frames=%llu wall_ms=%.3f\n",
                                static_cast<double>(now.t - t0) / 1e9, scene,
                                static_cast<unsigned long long>(n - lastLogic[scene]), wall);
                        lastLogic[scene] = n;
                    }
                std::string by;
                for (size_t k = 0; k < kDrainKinds; ++k)
                {
                    const uint64_t n = g_counters.gsDrainBy[k].load(std::memory_order_relaxed);
                    if (n != lastBy[k])
                        by += std::string(by.empty() ? "" : ",") + kDrainNames[k] + ":" + std::to_string(n - lastBy[k]);
                    lastBy[k] = n;
                }
                if (!by.empty())
                    std::printf("[perf] t=%.1f drains_by=%s\n", static_cast<double>(now.t - t0) / 1e9, by.c_str());
                {
                    static ps2_vu1_async::Stats lastAsync{};
                    const ps2_vu1_async::Stats a = ps2_vu1_async::stats();
                    if (a.records != lastAsync.records || a.syncs != lastAsync.syncs)
                        std::printf("[perf] t=%.1f vu1_async records=%llu kb=%llu syncs=%llu waits=%llu wait_ms=%.1f full=%llu fallbacks=%llu\n",
                                    static_cast<double>(now.t - t0) / 1e9,
                                    static_cast<unsigned long long>(a.records - lastAsync.records),
                                    static_cast<unsigned long long>((a.bytes - lastAsync.bytes) >> 10),
                                    static_cast<unsigned long long>(a.syncs - lastAsync.syncs),
                                    static_cast<unsigned long long>(a.syncWaits - lastAsync.syncWaits),
                                    static_cast<double>(a.syncWaitNs - lastAsync.syncWaitNs) / 1e6,
                                    static_cast<unsigned long long>(a.fullWaits - lastAsync.fullWaits),
                                    static_cast<unsigned long long>(a.fallbacks - lastAsync.fallbacks));
                    lastAsync = a;
                }
                {
                    uint32_t rep = 0, skip = 0;
                    uint64_t dropped = 0;
                    {
                        std::lock_guard lock(g_frames.mutex);
                        pres.clear(); vbl.clear(); events.clear();
                        pres.swap(g_frames.presentUs);
                        events.swap(g_frames.events);
                        dropped = g_frames.dropped;
                        g_frames.dropped = 0;
                        vbl.swap(g_frames.vblankUs);
                        rep = g_frames.repeats;
                        skip = g_frames.skipped;
                        g_frames.repeats = g_frames.skipped = 0;
                    }
                    for (const FrameEvent &e : events)
                    {
                        if (e.kind == 'q')
                        {
                            std::fprintf(stderr, "[present] tick=%llu vb=%llu latch=%llu swap=%llu qpc_freq=%llu\n",
                                static_cast<unsigned long long>(e.tick), static_cast<unsigned long long>(e.vb),
                                static_cast<unsigned long long>(e.latch), static_cast<unsigned long long>(e.swap),
                                static_cast<unsigned long long>(e.frequency));
                            continue;
                        }
                        if (e.kind == 'c')
                        {
                            std::fprintf(stderr, "[dispcopy] tick=%llu fbp=%x after_vblank_ms=%.2f\n",
                                static_cast<unsigned long long>(e.tick), e.fbp, e.afterVblankMs);
                            continue;
                        }
                        if (hitchMs() > 0.0 && e.intervalUs > hitchMs() * 1000.0)
                            reportHitch(e.kind == 'p' ? "present" : "guest", e.t, e.intervalUs, e.then, e.now);
                        if (trace)
                            std::fprintf(trace, "%c,%llu,%.9f,%llu,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                                e.kind, static_cast<unsigned long long>(e.t), static_cast<double>(e.t - t0) / 1e9,
                                static_cast<unsigned long long>(e.tick), static_cast<unsigned long long>(e.work.requestedTick), e.intervalUs,
                                static_cast<unsigned long long>(e.work.uploadNs), static_cast<unsigned long long>(e.work.drawNs),
                                static_cast<unsigned long long>(e.work.swapNs), static_cast<unsigned long long>(e.work.sharedWaitNs),
                                static_cast<unsigned long long>(e.work.cycles),
                                static_cast<unsigned long long>(e.work.sharedSequence), static_cast<unsigned long long>(e.work.sharedFresh),
                                static_cast<unsigned long long>(e.work.sharedTick), static_cast<unsigned long long>(e.work.sharedRenderSequence),
                                static_cast<unsigned long long>(e.now.eeGif - e.then.eeGif),
                                static_cast<unsigned long long>(e.now.pubWait - e.then.pubWait), static_cast<unsigned long long>(e.now.drainWait - e.then.drainWait),
                                static_cast<unsigned long long>(e.now.worker - e.then.worker), static_cast<unsigned long long>(e.now.texDec - e.then.texDec),
                                static_cast<unsigned long long>(e.now.rtUp - e.then.rtUp), static_cast<unsigned long long>(e.now.drains - e.then.drains),
                                static_cast<unsigned long long>(e.now.hostWait - e.then.hostWait));
                    }
                    if (dropped)
                    {
                        std::fprintf(stderr, "[frame-trace] DROPPED events=%llu (trace incomplete)\n", static_cast<unsigned long long>(dropped));
                        if (trace) std::fprintf(trace, "# DROPPED events=%llu\n", static_cast<unsigned long long>(dropped));
                    }
                    if (trace) std::fflush(trace);
                    // intervals in 0.1 ms: p = presents (window swaps), g = guest vblanks; rep/skip = guest frames
                    // shown twice / never shown (tools/perf_smooth.py)
                    std::printf("[frames] t=%.1f rep=%u skip=%u p=%s g=%s\n", static_cast<double>(now.t - t0) / 1e9, rep,
                                skip, joinUs(pres).c_str(), joinUs(vbl).c_str());
                }
                std::fflush(stdout);
                last = now;
                if (g_stop.load(std::memory_order_acquire)) break;
            }
            if (trace) std::fclose(trace);
        });
    }
}
