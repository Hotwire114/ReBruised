// Tekken 4 port (perf, t4-perf): VU1 + GS work of the EE thread on worker threads, in order (ps2_vu1_async.h).
//
// EE thread --(VU1 queue)--> VU1 worker: VU1 memory writes, microprograms; GIF packets are forwarded
//                             --(GS queue)--> GS worker: every GIF packet (PATH1 XGKICKs and the EE's PATH2/3) in the
//                                             order the EE thread would have processed them.
// With one worker (PS2X_GS_SPLIT=0, or fewer than 3 logical processors) the VU1 worker runs the GS packets itself.
#include "runtime/ps2_vu1_async.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_rollback_cuts.h"
#include "runtime/ps2_perf_stats.h"
#include "ThreadNaming.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <immintrin.h>
#ifdef _WIN32
#include <intrin.h>
extern "C" __declspec(dllimport) int __stdcall GetProcessAffinityMask(void *process, unsigned long long *processMask,
                                                                      unsigned long long *systemMask);
extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
#endif

namespace ps2_vu1_async
{
    namespace
    {
        enum Type : uint32_t
        {
            kWrap = 0x57524150u, // rest of the ring up to its end is padding
            kVu1Data = 1,
            kVu1Code = 2,
            kMscal = 3,
            kMscnt = 4,
            kGif = 5,
            kUnpack = 6, // payload: Vif1Unpack, then the source bytes
        };

        struct Header
        {
            uint32_t type;
            uint32_t bytes; // payload
            uint32_t a, b, c;
            uint32_t mxcsr;
        };
        static_assert(sizeof(Header) == 24, "record header");

        constexpr uint64_t kCapacity = 32ull << 20; // bytes; a power of two
        constexpr uint32_t kMaxPayload = 8u << 20;   // larger packets run synchronously

        inline uint64_t align8(uint64_t v) { return (v + 7u) & ~7ull; }

        // Spin budgets (pause iterations) before a thread sleeps: short on PCs with few logical processors, where a
        // spinning thread takes the processor the presenter, the GL thread or the other side needs.
        int g_workerSpin = 2000;
        int g_syncSpin = 4000;

        // One single-producer / single-consumer ring of records.
        struct Queue
        {
            std::unique_ptr<uint8_t[]> ring;
            alignas(64) std::atomic<uint64_t> head{0}; // written by the producer
            alignas(64) std::atomic<uint64_t> tail{0}; // written by the consumer
            alignas(64) std::atomic<bool> consumerSleeping{false};
            std::atomic<bool> producerWaiting{false}; // the producer waits for room
            std::atomic<bool> drainerWaiting{false};  // the EE thread waits for the queue to empty (sync)
            uint64_t fullWaits = 0, records = 0, bytes = 0; // producer side

            uint8_t *reserve(uint32_t type, uint32_t payload, uint32_t a, uint32_t b, uint32_t c)
            {
                const uint64_t need = align8(sizeof(Header) + payload);
                uint64_t h = head.load(std::memory_order_relaxed);
                uint64_t phys = h & (kCapacity - 1u);
                const uint64_t pad = kCapacity - phys < need ? kCapacity - phys : 0u;
                for (;;)
                {
                    if (h - tail.load(std::memory_order_acquire) + pad + need <= kCapacity)
                        break;
                    ++fullWaits;
                    producerWaiting.store(true, std::memory_order_seq_cst);
                    const uint64_t t = tail.load(std::memory_order_seq_cst);
                    if (h - t + pad + need > kCapacity)
                        tail.wait(t, std::memory_order_seq_cst);
                    producerWaiting.store(false, std::memory_order_relaxed);
                }
                if (pad)
                {
                    if (pad >= sizeof(uint32_t))
                    {
                        const uint32_t wrap = kWrap;
                        std::memcpy(ring.get() + phys, &wrap, sizeof wrap);
                    }
                    phys = 0u;
                }
                const Header hd{type, payload, a, b, c, _mm_getcsr()};
                std::memcpy(ring.get() + phys, &hd, sizeof hd);
                ++records;
                bytes += need;
                return ring.get() + phys + sizeof(Header);
            }

            void publish(uint32_t payload)
            {
                uint64_t h = head.load(std::memory_order_relaxed);
                const uint64_t phys = h & (kCapacity - 1u);
                const uint64_t need = align8(sizeof(Header) + payload);
                if (kCapacity - phys < need)
                    h += kCapacity - phys;
                head.store(h + need, std::memory_order_seq_cst);
                if (consumerSleeping.load(std::memory_order_seq_cst))
                    head.notify_one();
            }

            void put(uint32_t type, const void *payload, uint32_t bytesIn, uint32_t a, uint32_t b, uint32_t c)
            {
                uint8_t *dst = reserve(type, bytesIn, a, b, c);
                if (bytesIn)
                    std::memcpy(dst, payload, bytesIn);
                publish(bytesIn);
            }

            // Waits (producer side) until the consumer has run everything published so far; returns the wait in ns.
            uint64_t drain()
            {
                const uint64_t h = head.load(std::memory_order_acquire);
                uint64_t t = tail.load(std::memory_order_acquire);
                if (t == h)
                    return 0u;
                const uint64_t t0 = ps2_perf_stats::nowNs();
                for (int i = 0; i < g_syncSpin && t != h; ++i)
                {
                    _mm_pause();
                    t = tail.load(std::memory_order_acquire);
                }
                while (t != h)
                {
                    drainerWaiting.store(true, std::memory_order_seq_cst);
                    t = tail.load(std::memory_order_seq_cst);
                    if (t != h)
                        tail.wait(t, std::memory_order_seq_cst);
                    t = tail.load(std::memory_order_acquire);
                }
                drainerWaiting.store(false, std::memory_order_relaxed);
                return ps2_perf_stats::nowNs() - t0;
            }

            // Consumer loop: calls run(header, payload) for every record, in order.
            template <typename Run>
            [[noreturn]] void consume(Run &&run)
            {
                uint64_t t = tail.load(std::memory_order_relaxed);
                for (;;)
                {
                    uint64_t h = head.load(std::memory_order_acquire);
                    if (h == t)
                    {
                        for (int i = 0; i < g_workerSpin && h == t; ++i)
                        {
                            _mm_pause();
                            h = head.load(std::memory_order_acquire);
                        }
                        if (h == t)
                        {
                            consumerSleeping.store(true, std::memory_order_seq_cst);
                            h = head.load(std::memory_order_seq_cst);
                            if (h == t)
                                head.wait(t, std::memory_order_seq_cst);
                            consumerSleeping.store(false, std::memory_order_relaxed);
                            continue;
                        }
                    }
                    while (t != h)
                    {
                        const uint64_t phys = t & (kCapacity - 1u);
                        if (kCapacity - phys < sizeof(Header))
                        {
                            t += kCapacity - phys;
                            continue;
                        }
                        Header hd;
                        std::memcpy(&hd, ring.get() + phys, sizeof hd);
                        if (hd.type == kWrap)
                        {
                            t += kCapacity - phys;
                            continue;
                        }
                        run(hd, ring.get() + phys + sizeof(Header));
                        t += align8(sizeof(Header) + hd.bytes);
                        tail.store(t, std::memory_order_seq_cst);
                        if (producerWaiting.load(std::memory_order_seq_cst) || drainerWaiting.load(std::memory_order_seq_cst))
                            tail.notify_all();
                    }
                }
            }
        };

        struct State
        {
            Queue vu1; // EE -> VU1 worker
            Queue gs;  // VU1 worker -> GS worker (split mode)
            bool split = false;
            Hooks hooks;
            Stats stats; // EE side
        };

        State *g_s = nullptr;
        bool g_enabled = false;
        thread_local bool t_vu1Worker = false;
        thread_local bool t_gsWorker = false;
        thread_local bool t_producer = false;
        bool g_foreignWarned = false;

        inline void applyMxcsr(uint32_t want, uint32_t &have)
        {
            if (want != have)
            {
                _mm_setcsr(want);
                have = want;
            }
        }

        void vu1WorkerMain()
        {
            t_vu1Worker = true;
            ThreadNaming::SetCurrentThreadName("VU1Worker");
            uint32_t mxcsr = _mm_getcsr();
            State &s = *g_s;
            s.vu1.consume([&](const Header &h, const uint8_t *payload)
                          {
                              applyMxcsr(h.mxcsr, mxcsr);
                              Hooks &k = s.hooks;
                              switch (h.type)
                              {
                              case kVu1Data:
                                  std::memcpy(k.vu1Data + h.a, payload, h.bytes);
                                  break;
                              case kVu1Code:
                                  std::memcpy(k.vu1Code + h.a, payload, h.bytes);
                                  if (k.codeModified)
                                      k.codeModified();
                                  break;
                              case kMscal:
                                  k.mscal(h.a, h.b, h.c);
                                  break;
                              case kMscnt:
                                  k.mscnt(h.b, h.c);
                                  break;
                              case kGif:
                                  if (s.split)
                                      s.gs.put(kGif, payload, h.bytes, h.a, 0u, 0u); // keeps its place behind earlier XGKICKs
                                  else
                                  {
                                      GifPathScope scope(static_cast<GifPathId>(h.a));
                                      k.gif(payload, h.bytes);
                                  }
                                  break;
                              case kUnpack:
                              {
                                  Vif1Unpack u;
                                  std::memcpy(&u, payload, sizeof u);
                                  applyVif1Unpack(k.vu1Data, u, payload + sizeof u, u.row);
                                  break;
                              }
                              default:
                                  break;
                              } });
        }

        void gsWorkerMain()
        {
            t_gsWorker = true;
            ThreadNaming::SetCurrentThreadName("GSFrontWorker");
            uint32_t mxcsr = _mm_getcsr();
            State &s = *g_s;
            s.gs.consume([&](const Header &h, const uint8_t *payload)
                         {
                             applyMxcsr(h.mxcsr, mxcsr);
                             if (h.type == kGif)
                             {
                                 GifPathScope scope(static_cast<GifPathId>(h.a));
                                 s.hooks.gif(payload, h.bytes);
                             } });
        }

        // Logical processors this process may run on (its affinity mask; the whole PC when that cannot be read).
        unsigned usableProcessors()
        {
#ifdef _WIN32
            unsigned long long processMask = 0, systemMask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask)
                return static_cast<unsigned>(__popcnt64(processMask));
#endif
            const unsigned n = std::thread::hardware_concurrency();
            return n ? n : 1u;
        }
    }

    void start(const Hooks &hooks)
    {
        if (g_s)
        {
            sync();
            g_s->hooks = hooks;
            return;
        }
        const char *env = std::getenv("PS2X_VU1_ASYNC");
        g_enabled = !(env && std::strcmp(env, "0") == 0);
        if (!g_enabled)
        {
            std::fprintf(stderr, "[vu1-async] off (PS2X_VU1_ASYNC=0): VU1 and the GS frontend run on the EE thread\n");
            return;
        }
        const unsigned processors = usableProcessors();
        if (processors < 8u)
        {
            g_workerSpin = 64;
            g_syncSpin = 256;
        }
        g_s = new State();
        g_s->vu1.ring.reset(new uint8_t[kCapacity]);
        const char *splitEnv = std::getenv("PS2X_GS_SPLIT");
        g_s->split = splitEnv ? std::strcmp(splitEnv, "0") != 0 : processors >= 3u;
        if (g_s->split)
            g_s->gs.ring.reset(new uint8_t[kCapacity]);
        g_s->hooks = hooks;
        std::thread(vu1WorkerMain).detach();
        if (g_s->split)
            std::thread(gsWorkerMain).detach();
        std::fprintf(stderr,
                     "[vu1-async] on: VU1 microprograms %s run on their own thread%s (%u logical processors, spin %d/%d)\n",
                     g_s->split ? "and GS packets" : "and GS packets", g_s->split ? "s" : "", processors, g_workerSpin,
                     g_syncSpin);
    }

    void bindProducer() noexcept { t_producer = true; }
    bool enabled() noexcept { return g_enabled; }
    bool onWorker() noexcept { return t_vu1Worker || t_gsWorker; }

    bool forwardWorkerGif(const uint8_t *data, uint32_t size)
    {
        if (!t_vu1Worker || !g_s || !g_s->split)
            return false;
        g_s->gs.put(kGif, data, size, gifCurrentPath(), 0u, 0u);
        return true;
    }

    bool active() noexcept
    {
        if (!g_enabled)
            return false;
        if (!t_producer)
        {
            if (!onWorker() && !g_foreignWarned)
            {
                g_foreignWarned = true;
                std::fprintf(stderr, "[vu1-async] VU1/GS work from a thread other than the EE thread: run synchronously\n");
            }
            return false;
        }
        if (ps2_rollback::activeCuts() != 0u)
        {
            sync();
            return false;
        }
        return true;
    }

    void syncForFallback() noexcept
    {
        if (g_s && t_producer)
            ++g_s->stats.fallbacks;
        sync();
    }

    void sync() noexcept
    {
        // Only the producer waits: other threads (presenter, debug panel) already ran beside the GS work before.
        if (!g_enabled || !t_producer || !g_s)
            return;
        State &s = *g_s;
        ++s.stats.syncs;
        uint64_t waited = s.vu1.drain(); // the VU1 worker forwards GS packets before it moves past them
        if (s.split)
            waited += s.gs.drain();
        if (waited)
        {
            ++s.stats.syncWaits;
            s.stats.syncWaitNs += waited;
        }
    }

    void writeVu1Data(uint32_t offset, const void *source, uint32_t bytes)
    {
        constexpr uint32_t kSize = 0x4000u;
        const uint8_t *src = static_cast<const uint8_t *>(source);
        offset &= kSize - 1u;
        while (bytes)
        {
            const uint32_t n = bytes < kSize - offset ? bytes : kSize - offset;
            g_s->vu1.put(kVu1Data, src, n, offset, 0u, 0u);
            src += n;
            bytes -= n;
            offset = 0u;
        }
    }

    void writeVu1Code(uint32_t offset, const void *source, uint32_t bytes) { g_s->vu1.put(kVu1Code, source, bytes, offset, 0u, 0u); }
    void mscal(uint32_t pc, uint32_t top, uint32_t itop) { g_s->vu1.put(kMscal, nullptr, 0u, pc, top, itop); }
    void mscnt(uint32_t top, uint32_t itop) { g_s->vu1.put(kMscnt, nullptr, 0u, 0u, top, itop); }

    void gif(const uint8_t *data, uint32_t size)
    {
        if (size > kMaxPayload)
        {
            syncForFallback();
            g_s->hooks.gif(data, size);
            return;
        }
        g_s->vu1.put(kGif, data, size, gifCurrentPath(), 0u, 0u);
    }

    void unpack(const Vif1Unpack &u, const uint8_t *source, uint32_t sourceBytes)
    {
        if (sourceBytes > kMaxPayload)
        {
            syncForFallback();
            uint32_t row[4];
            std::memcpy(row, u.row, sizeof row);
            applyVif1Unpack(g_s->hooks.vu1Data, u, source, row);
            return;
        }
        uint8_t *dst = g_s->vu1.reserve(kUnpack, static_cast<uint32_t>(sizeof u) + sourceBytes, 0u, 0u, 0u);
        std::memcpy(dst, &u, sizeof u);
        std::memcpy(dst + sizeof u, source, sourceBytes);
        g_s->vu1.publish(static_cast<uint32_t>(sizeof u) + sourceBytes);
    }

    Stats stats() noexcept
    {
        if (!g_s)
            return {};
        Stats out = g_s->stats;
        out.records = g_s->vu1.records;
        out.bytes = g_s->vu1.bytes;
        out.fullWaits = g_s->vu1.fullWaits;
        return out;
    }
}
