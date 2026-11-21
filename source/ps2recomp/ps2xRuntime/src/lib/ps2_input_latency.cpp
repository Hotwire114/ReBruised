// Compiled into the GS GL translation unit (that file already includes windows.h
// and does not include raylib). src/lib/*.cpp is an explicit CMake list; this
// file is #included from gs/gpu/gs_gl.cpp so it links without a CMake edit.
#include "runtime/ps2_input_latency.h"
#include "runtime/ps2_perf_stats.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    constexpr uint16_t kUp = 0x0010u;
    constexpr uint16_t kDown = 0x0040u;
    constexpr uint16_t kLeft = 0x0080u;
    constexpr uint16_t kRight = 0x0020u;
    constexpr uint16_t kStart = 0x0008u;
    constexpr uint16_t kSelect = 0x0001u;
    constexpr uint16_t kSquare = 0x8000u;
    constexpr uint16_t kCross = 0x4000u;
    constexpr uint16_t kCircle = 0x2000u;
    constexpr uint16_t kTriangle = 0x1000u;
    constexpr uint16_t kR1 = 0x0800u;
    constexpr uint16_t kL1 = 0x0400u;
    constexpr uint16_t kR2 = 0x0200u;
    constexpr uint16_t kL2 = 0x0100u;

    constexpr int kSlots = 32;

    struct Press
    {
        uint32_t seq = 0;
        uint32_t vk = 0;
        uint16_t bit = 0;
        uint64_t t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0;
        uint64_t tRead = 0; // first pad read after t1, pressed or not (t2 - tRead = reads that missed it)
        uint64_t tickAtT2 = 0;
        uint64_t hashBefore = 0;
        uint64_t hashAtT5 = 0;
        int stage = 0; // 1 t1, 2 t2, 3 t3, 4 t4, 5 printed
        int latchesAfterT2 = 0;
    };

    std::atomic<uint16_t> g_posted{0};
    std::atomic<uint64_t> g_postedVk[4]{}; // pad bindings: every virtual key the window thread saw go down (L/R split)
    std::atomic<uintptr_t> g_hwnd{0};
    std::atomic<uint16_t> g_asyncCache{0};
    std::atomic<uint64_t> g_asyncCacheQpc{0};
    std::atomic<uint64_t> g_lastHash{0};
    WNDPROC g_oldProc = nullptr;
    bool g_timerPeriod = false;
    bool g_latched = false;

    // Frame-chain instrumentation.
    constexpr int kTickRing = 256;
    std::atomic<uint64_t> g_vbQpc[kTickRing]{};
    std::atomic<uint64_t> g_padTick{0};
    std::atomic<uint16_t> g_padMask{0};
    uint64_t g_latchTick = 0;
    uint64_t g_latchQpc = 0;

    std::mutex g_pressMu;
    Press g_press[kSlots];
    uint32_t g_seq = 0;

    bool envIs1(const char *name)
    {
        const char *v = std::getenv(name);
        return v && v[0] == '1';
    }

    uint64_t qpc()
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<uint64_t>(c.QuadPart);
    }

    uint64_t qpcFreq()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }

    // Same keys as PSPadBackend::readState. Letters match the VK the capture
    // script posts; arrows and Enter are the other bindings.
    uint16_t vkToPad(WPARAM vk)
    {
        switch (vk)
        {
        case 'W':
            return kUp;
        case 'S':
            return kDown;
        case 'A':
            return kLeft;
        case 'D':
            return kRight;
        case VK_DOWN:
        case 'X':
        case VK_SPACE:
            return kCross;
        case VK_RIGHT:
        case 'C':
        case VK_ESCAPE:
            return kCircle;
        case VK_LEFT:
        case 'Z':
        case VK_NUMPAD0:
            return kSquare;
        case VK_UP:
        case 'V':
        case VK_NUMPAD1:
            return kTriangle;
        case 'Q':
            return kL1;
        case 'E':
            return kR1;
        case VK_LSHIFT:
        case VK_SHIFT:
            return kL2;
        case VK_RSHIFT:
            return kR2;
        case VK_RETURN:
            return kStart;
        case VK_TAB:
            return kSelect;
        default:
            return 0;
        }
    }

    uint16_t asyncPadMask()
    {
        uint16_t m = 0;
        const auto down = [](int vk)
        { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down('W'))
            m = static_cast<uint16_t>(m | kUp);
        if (down('S'))
            m = static_cast<uint16_t>(m | kDown);
        if (down('A'))
            m = static_cast<uint16_t>(m | kLeft);
        if (down('D'))
            m = static_cast<uint16_t>(m | kRight);
        if (down(VK_DOWN) || down('X') || down(VK_SPACE))
            m = static_cast<uint16_t>(m | kCross);
        if (down(VK_RIGHT) || down('C') || down(VK_ESCAPE))
            m = static_cast<uint16_t>(m | kCircle);
        if (down(VK_LEFT) || down('Z') || down(VK_NUMPAD0))
            m = static_cast<uint16_t>(m | kSquare);
        if (down(VK_UP) || down('V') || down(VK_NUMPAD1))
            m = static_cast<uint16_t>(m | kTriangle);
        if (down('Q'))
            m = static_cast<uint16_t>(m | kL1);
        if (down('E'))
            m = static_cast<uint16_t>(m | kR1);
        if (down(VK_LSHIFT))
            m = static_cast<uint16_t>(m | kL2);
        if (down(VK_RSHIFT))
            m = static_cast<uint16_t>(m | kR2);
        if (down(VK_RETURN))
            m = static_cast<uint16_t>(m | kStart);
        if (down(VK_TAB))
            m = static_cast<uint16_t>(m | kSelect);
        return m;
    }

    void printPress(const Press &p)
    {
        std::fprintf(stderr,
                     "[input-lag] seq=%u vk=%u t1=%llu t2=%llu t3=%llu t4=%llu t5=%llu before=%llu hash=%llu read=%llu\n",
                     p.seq, p.vk,
                     static_cast<unsigned long long>(p.t1),
                     static_cast<unsigned long long>(p.t2),
                     static_cast<unsigned long long>(p.t3),
                     static_cast<unsigned long long>(p.t4),
                     static_cast<unsigned long long>(p.t5),
                     static_cast<unsigned long long>(p.hashBefore),
                     static_cast<unsigned long long>(p.hashAtT5),
                     static_cast<unsigned long long>(p.tRead));
        std::fflush(stderr);
    }

    void openPress(uint32_t vk, uint16_t bit)
    {
        std::lock_guard<std::mutex> lock(g_pressMu);
        Press *slot = nullptr;
        for (Press &p : g_press)
        {
            if (p.stage == 0 || p.stage == 5)
            {
                slot = &p;
                break;
            }
        }
        if (!slot)
            slot = &g_press[0];
        *slot = Press{};
        slot->seq = ++g_seq;
        slot->vk = vk;
        slot->bit = bit;
        slot->t1 = qpc();
        slot->hashBefore = g_lastHash.load(std::memory_order_relaxed);
        slot->stage = 1;
    }

    // The window gets VK_SHIFT / VK_CONTROL / VK_MENU for either side; the scan code and the extended bit tell which.
    uint32_t sideVk(WPARAM wp, LPARAM lp)
    {
        if (wp == VK_SHIFT)
            return ((lp >> 16) & 0xFF) == 0x36 ? VK_RSHIFT : VK_LSHIFT;
        if (wp == VK_CONTROL)
            return (lp & (static_cast<LPARAM>(1) << 24)) ? VK_RCONTROL : VK_LCONTROL;
        if (wp == VK_MENU)
            return (lp & (static_cast<LPARAM>(1) << 24)) ? VK_RMENU : VK_LMENU;
        return static_cast<uint32_t>(wp);
    }

    LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)
        {
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            const bool repeat = down && ((lp & (static_cast<LPARAM>(1) << 30)) != 0);
            const uint32_t sideKey = sideVk(wp, lp);
            if (sideKey < 256)
            {
                if (down)
                    g_postedVk[sideKey >> 6].fetch_or(uint64_t{1} << (sideKey & 63), std::memory_order_release);
                else
                    g_postedVk[sideKey >> 6].fetch_and(~(uint64_t{1} << (sideKey & 63)), std::memory_order_release);
            }
            const uint16_t bit = vkToPad(wp);
            if (bit != 0)
            {
                if (down)
                    g_posted.fetch_or(bit, std::memory_order_release);
                else
                    g_posted.fetch_and(static_cast<uint16_t>(~bit), std::memory_order_release);
                if (down && !repeat && ps2_input_latency::enabled())
                    openPress(static_cast<uint32_t>(wp), bit);
            }
        }
        if (g_oldProc)
            return CallWindowProcW(g_oldProc, hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void setTimerPeriod(bool on)
    {
        const auto begin = reinterpret_cast<UINT(WINAPI *)(UINT)>(GetProcAddress(GetModuleHandleA("winmm.dll"), "timeBeginPeriod"));
        const auto end = reinterpret_cast<UINT(WINAPI *)(UINT)>(GetProcAddress(GetModuleHandleA("winmm.dll"), "timeEndPeriod"));
        HMODULE winmm = nullptr;
        auto beginFn = begin;
        auto endFn = end;
        if (!beginFn || !endFn)
        {
            winmm = LoadLibraryA("winmm.dll");
            beginFn = reinterpret_cast<UINT(WINAPI *)(UINT)>(GetProcAddress(winmm, "timeBeginPeriod"));
            endFn = reinterpret_cast<UINT(WINAPI *)(UINT)>(GetProcAddress(winmm, "timeEndPeriod"));
        }
        if (on && beginFn && !g_timerPeriod)
        {
            if (beginFn(1) == 0)
                g_timerPeriod = true;
        }
        else if (!on && endFn && g_timerPeriod)
        {
            endFn(1);
            g_timerPeriod = false;
        }
        (void)winmm;
    }

    uint64_t hashImage(const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        // 16x16 block-mean luma, quantised. A menu cursor moves several blocks.
        // A one-line interlace shift does not, which a per-pixel sample did.
        uint64_t h = 14695981039346656037ull;
        if (!rgba || width < 16u || height < 16u)
            return 0;
        for (uint32_t y = 0; y + 16u <= height; y += 16u)
        {
            for (uint32_t x = 0; x + 16u <= width; x += 16u)
            {
                uint32_t sum = 0;
                for (uint32_t by = 0; by < 16u; by += 2u)
                {
                    const uint8_t *row = rgba + (static_cast<size_t>(y + by) * width + x) * 4u;
                    for (uint32_t bx = 0; bx < 16u; bx += 2u)
                    {
                        const uint8_t *p = row + static_cast<size_t>(bx) * 4u;
                        sum += (static_cast<unsigned>(p[0]) * 3u + static_cast<unsigned>(p[1]) * 4u +
                                static_cast<unsigned>(p[2])) >>
                               3;
                    }
                }
                const unsigned q = (sum / 64u) >> 4;
                h ^= q;
                h *= 1099511628211ull;
            }
        }
        return h == 0 ? 1 : h;
    }
}

namespace ps2_input_latency
{
    bool enabled()
    {
        static const bool on = envIs1("TEKKEN4_INPUT_LAG");
        return on;
    }

    bool legacy()
    {
        static const bool on = envIs1("TEKKEN4_INPUT_LEGACY");
        return on;
    }

    bool presentOnTick()
    {
        static const bool tick = []
        {
            const char *v = std::getenv("TEKKEN4_INPUT_PRESENT");
            return v && std::strcmp(v, "tick") == 0;
        }();
        return tick;
    }

    void attachWindow(void *hwnd)
    {
        if (!hwnd)
            return;
        if (legacy() && !enabled())
            return;
        auto window = static_cast<HWND>(hwnd);
        g_hwnd.store(reinterpret_cast<uintptr_t>(window), std::memory_order_release);
        if (!legacy())
            setTimerPeriod(true);
        if (ps2_perf_stats::enabled())
            std::fprintf(stderr, "[present-config] timeBeginPeriod_1ms=%s legacy=%d (request status, not wake precision)\n",
                g_timerPeriod ? "success" : "inactive", legacy() ? 1 : 0);
        if (!g_oldProc)
        {
            g_oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProc)));
        }
        if (enabled())
        {
            std::fprintf(stderr, "[input-lag] enabled legacy=%d present=%s qpc_freq=%llu\n",
                         legacy() ? 1 : 0, presentOnTick() ? "tick" : "paced",
                         static_cast<unsigned long long>(qpcFreq()));
            std::fflush(stderr);
        }
    }

    void detachWindow()
    {
        const uintptr_t raw = g_hwnd.exchange(0, std::memory_order_acq_rel);
        if (raw && g_oldProc)
        {
            SetWindowLongPtrW(reinterpret_cast<HWND>(raw), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldProc));
            g_oldProc = nullptr;
        }
        setTimerPeriod(false);
    }

    namespace
    {
        // Auto-reset event the EE thread sets at every guest VBlankStart: the window thread wakes for the present
        // right away instead of at the next 1 ms poll (present jitter).
        HANDLE tickEvent()
        {
            static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return event;
        }
    }

    void notifyTick()
    {
        if (const HANDLE event = tickEvent())
            SetEvent(event);
    }

    void waitMessage(unsigned timeoutMs)
    {
        const HANDLE event = tickEvent();
        MsgWaitForMultipleObjects(event ? 1u : 0u, event ? &event : nullptr, FALSE, timeoutMs, QS_ALLINPUT);
    }

    bool sampleKeyboard(uint16_t &padDownMask)
    {
        if (legacy())
            return false;
        const uintptr_t raw = g_hwnd.load(std::memory_order_acquire);
        if (raw == 0)
            return false;
        uint16_t mask = g_posted.load(std::memory_order_acquire);
        const uint64_t now = qpc();
        const uint64_t cachedAt = g_asyncCacheQpc.load(std::memory_order_acquire);
        const uint64_t freq = qpcFreq();
        const uint64_t budget = freq / 5000u; // 200 us
        uint16_t asyncMask = g_asyncCache.load(std::memory_order_acquire);
        if (cachedAt == 0 || now - cachedAt >= budget)
        {
            const bool focused = GetForegroundWindow() == reinterpret_cast<HWND>(raw);
            asyncMask = focused ? asyncPadMask() : 0;
            g_asyncCache.store(asyncMask, std::memory_order_release);
            g_asyncCacheQpc.store(now, std::memory_order_release);
        }
        padDownMask = static_cast<uint16_t>(mask | asyncMask);
        return true;
    }

    bool sampleKeys(const uint16_t *vks, const uint16_t *bits, size_t n, uint16_t &padDownMask)
    {
        if (legacy())
            return false;
        const uintptr_t raw = g_hwnd.load(std::memory_order_acquire);
        if (raw == 0)
            return false;
        const bool focused = GetForegroundWindow() == reinterpret_cast<HWND>(raw);
        uint16_t mask = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t vk = vks[i];
            if (vk >= 256)
                continue;
            const bool posted = ((g_postedVk[vk >> 6].load(std::memory_order_acquire) >> (vk & 63)) & 1u) != 0;
            if (posted || (focused && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0))
                mask = static_cast<uint16_t>(mask | bits[i]);
        }
        padDownMask = mask;
        return true;
    }

    void notePad(uint16_t pressedMask, uint64_t vsyncTick)
    {
        if (vblankHash())
        {
            if (g_padTick.load(std::memory_order_relaxed) != vsyncTick)
                g_padMask.store(0, std::memory_order_relaxed);
            g_padMask.fetch_or(pressedMask, std::memory_order_relaxed);
            g_padTick.store(vsyncTick, std::memory_order_release);
        }
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(g_pressMu);
        const uint64_t now = qpc();
        for (Press &p : g_press)
        {
            if (p.stage == 1 && p.tRead == 0)
                p.tRead = now;
            if (p.stage == 1 && (pressedMask & p.bit) != 0)
            {
                p.t2 = now;
                p.tickAtT2 = vsyncTick;
                p.stage = 2;
            }
        }
    }

    bool presentLog()
    {
        static const bool on = envIs1("TEKKEN4_PRESENT_LOG");
        return on;
    }

    bool vblankHash()
    {
        static const bool on = envIs1("TEKKEN4_VBLANK_HASH");
        return on;
    }

    void noteVBlank(uint64_t newTick)
    {
        g_vbQpc[newTick % kTickRing].store(qpc(), std::memory_order_release);
    }

    void noteDisplayCopy(uint64_t tick, uint32_t destFbp)
    {
        if (!vblankHash() && !presentLog())
            return;
        const uint64_t vb = g_vbQpc[tick % kTickRing].load(std::memory_order_acquire);
        const uint64_t now = qpc();
        const double ms = vb && now > vb ? static_cast<double>(now - vb) * 1000.0 / static_cast<double>(qpcFreq()) : -1.0;
        ps2_perf_stats::noteDisplayCopy(tick, destFbp, ms);
    }

    double msSinceVBlank(uint64_t tick)
    {
        const uint64_t vb = g_vbQpc[tick % kTickRing].load(std::memory_order_acquire);
        const uint64_t now = qpc();
        if (vb == 0 || now < vb)
            return -1.0;
        return static_cast<double>(now - vb) * 1000.0 / static_cast<double>(qpcFreq());
    }

    uint16_t padMaskOfTick(uint64_t tick)
    {
        return g_padTick.load(std::memory_order_acquire) == tick ? g_padMask.load(std::memory_order_acquire) : 0;
    }

    uint64_t hashExact(const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        uint64_t h = 14695981039346656037ull;
        const size_t n = static_cast<size_t>(width) * height;
        const uint32_t *px = reinterpret_cast<const uint32_t *>(rgba);
        for (size_t i = 0; rgba && i < n; ++i)
        {
            h ^= px[i] & 0x00FFFFFFu; // alpha is not shown
            h *= 1099511628211ull;
        }
        return h;
    }

    void openTestPress(uint32_t tag, uint16_t bit)
    {
        if (enabled())
            openPress(tag, bit);
    }

    void onLatchBegin(uint64_t vsyncTick)
    {
        g_latched = true;
        g_latchTick = vsyncTick;
        g_latchQpc = qpc();
        if (!enabled())
            return;
        std::lock_guard<std::mutex> lock(g_pressMu);
        const uint64_t now = qpc();
        for (Press &p : g_press)
        {
            if (p.stage == 2)
            {
                p.t3 = now;
                p.stage = 3;
            }
        }
    }

    void onLatchPixels(const uint8_t *rgba, uint32_t width, uint32_t height)
    {
        if (!enabled())
            return;
        g_lastHash.store(hashImage(rgba, width, height), std::memory_order_relaxed);
    }

    void onSwapReturned()
    {
        if (presentLog() && g_latched)
        {
            const uint64_t now = qpc();
            ps2_perf_stats::notePresentQpc(g_latchTick,
                g_vbQpc[g_latchTick % kTickRing].load(std::memory_order_acquire), g_latchQpc, now, qpcFreq());
        }
        if (!enabled())
        {
            g_latched = false;
            return;
        }
        const bool latched = g_latched;
        g_latched = false;
        const uint64_t now = qpc();
        const uint64_t hash = g_lastHash.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_pressMu);
        for (Press &p : g_press)
        {
            if (p.stage == 0 || p.stage == 5)
                continue;
            if (p.stage == 3 && latched)
            {
                p.t4 = now;
                p.stage = 4;
            }
            // Pixels hashed for this swap were latched in onLatchBegin. That
            // latch is only after the pad read once t3 is set. A swap that
            // was already in flight when the key arrived must not become t5.
            if (latched && p.t3 != 0 && p.t5 == 0 && p.hashBefore != 0 && hash != 0 && hash != p.hashBefore)
            {
                p.t5 = now;
                p.hashAtT5 = hash;
            }
            if (latched)
                ++p.latchesAfterT2;
            const bool done = p.t4 != 0 && p.t5 != 0;
            const bool gaveUp = p.latchesAfterT2 > 45;
            if ((done || gaveUp) && p.stage < 5)
            {
                p.stage = 5;
                printPress(p);
            }
        }
    }
}

#else

namespace ps2_input_latency
{
    bool enabled() { return false; }
    bool legacy() { return false; }
    bool presentOnTick() { return false; }
    void attachWindow(void *) {}
    void detachWindow() {}
    void waitMessage(unsigned) {}
    void notifyTick() {}
    bool sampleKeyboard(uint16_t &) { return false; }
    bool sampleKeys(const uint16_t *, const uint16_t *, size_t, uint16_t &) { return false; }
    void notePad(uint16_t, uint64_t) {}
    void openTestPress(uint32_t, uint16_t) {}
    bool presentLog() { return false; }
    bool vblankHash() { return false; }
    void noteVBlank(uint64_t) {}
    uint16_t padMaskOfTick(uint64_t) { return 0; }
    double msSinceVBlank(uint64_t) { return -1.0; }
    uint64_t hashExact(const uint8_t *, uint32_t, uint32_t) { return 0; }
    void noteDisplayCopy(uint64_t, uint32_t) {}
    void onLatchBegin(uint64_t) {}
    void onLatchPixels(const uint8_t *, uint32_t, uint32_t) {}
    void onSwapReturned() {}
}

#endif
