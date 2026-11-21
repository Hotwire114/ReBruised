// Host game controllers: see runtime/ps2_host_pad.h. No raylib here (windows.h and raylib.h clash);
// ps2_pad.cpp reads raylib's gamepads on the window thread and hands them over with publishRaylib.
#include "runtime/ps2_host_pad.h"
#include "runtime/ps2_pad_bindings.h"
#include "runtime/ps2_input_latency.h"
#include "runtime/ps2_runahead.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h> // types and constants only: the functions are loaded at run time

namespace
{
    constexpr uint16_t kSelect = 0x0001u;
    constexpr uint16_t kL3 = 0x0002u;
    constexpr uint16_t kR3 = 0x0004u;
    constexpr uint16_t kStart = 0x0008u;
    constexpr uint16_t kUp = 0x0010u;
    constexpr uint16_t kRight = 0x0020u;
    constexpr uint16_t kDown = 0x0040u;
    constexpr uint16_t kLeft = 0x0080u;
    constexpr uint16_t kL2 = 0x0100u;
    constexpr uint16_t kR2 = 0x0200u;
    constexpr uint16_t kL1 = 0x0400u;
    constexpr uint16_t kR1 = 0x0800u;
    constexpr uint16_t kTriangle = 0x1000u;
    constexpr uint16_t kCircle = 0x2000u;
    constexpr uint16_t kCross = 0x4000u;
    constexpr uint16_t kSquare = 0x8000u;

    constexpr int kXSlots = 4;
    constexpr int kRaylibPads = 4;
    constexpr float kDeadzone = 0.24f; // ~XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE (7849 / 32767)
    constexpr float kDirOn = 0.5f;     // stick -> d-pad: on past half way ...
    constexpr float kDirOff = 0.4f;    // ... off again only back inside 40 %

    using GetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);

    uint64_t g_freq = 1;
    uint64_t g_startQpc = 0; // process start
    std::once_flag g_initOnce;
    bool g_raw = false;
    bool g_stickDpad = true;
    bool g_stats = false;
    GetStateFn g_getState = nullptr;
    const char *g_xinputDll = "none";

    uint64_t qpc()
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<uint64_t>(c.QuadPart);
    }

    double usSince(uint64_t from, uint64_t to)
    {
        return to > from ? static_cast<double>(to - from) * 1e6 / static_cast<double>(g_freq) : 0.0;
    }

    struct XSlot
    {
        std::atomic<bool> connected{false};
        std::atomic<uint16_t> rises{0}; // poll thread: buttons that went down since the last pad read
        // poll thread only
        uint16_t pollPrev = 0;
        uint16_t pollDirs = 0;
        uint32_t pollAxis = 0;   // pad bindings: stick directions held (ps2_pad_bindings::axisBits)
        uint64_t pollGen = 0;    // the bindings generation pollPrev was computed under
        uint64_t lastProbe = 0;
        // pad-read side only (under g_readMu)
        uint16_t gameDirs = 0;
        uint32_t gameAxis = 0;
        uint16_t lastReported = 0;
    };
    XSlot g_x[kXSlots];

    struct RPad
    {
        bool present = false;
        bool xinput = false;
        char name[64]{};
        ps2_host_pad::State st{};
        uint16_t prevButtons = 0;
        uint16_t rises = 0;
        uint16_t dirs = 0;
        uint16_t lastReported = 0;
        uint64_t qpc = 0; // when the window thread published it (= right after raylib's poll)
    };
    std::mutex g_snapMu;
    RPad g_r[kRaylibPads];

    std::mutex g_readMu;
    std::atomic<bool> g_pollStop{false};

    // Stats (TEKKEN4_PAD_STATS=1), under g_readMu except the poll thread's atomics.
    struct Stats
    {
        uint64_t windowStart = 0;
        uint32_t reads = 0, xiCalls = 0, snapReads = 0, taps = 0;
        double xiUsSum = 0, xiUsMax = 0, ageUsSum = 0, ageUsMax = 0;
    } g_st;
    std::atomic<uint32_t> g_pollWakes{0};
    std::atomic<uint32_t> g_pollGapMaxUs{0};

    // ---- scripted test pad (TEKKEN4_PAD_TEST) ----
    struct TestSpec
    {
        double start = 40.0;
        int count = 24;
        int periodMs = 400;
        int holdMs = 70;
        bool stick = false;
    } g_test;
    bool g_testOn = false;
    std::atomic<uint16_t> g_fakeButtons{0}; // XInput wButtons
    std::atomic<int16_t> g_fakeLY{0};
    std::atomic<DWORD> g_fakePacket{1};

    DWORD WINAPI fakeGetState(DWORD slot, XINPUT_STATE *s)
    {
        if (slot != 0)
            return ERROR_DEVICE_NOT_CONNECTED;
        std::memset(s, 0, sizeof(*s));
        s->dwPacketNumber = g_fakePacket.load(std::memory_order_acquire);
        s->Gamepad.wButtons = g_fakeButtons.load(std::memory_order_acquire);
        s->Gamepad.sThumbLY = g_fakeLY.load(std::memory_order_acquire);
        return ERROR_SUCCESS;
    }

    float axisNorm(SHORT v)
    {
        return std::clamp(static_cast<float>(v) / 32767.0f, -1.0f, 1.0f);
    }

    uint8_t axisByte(float v)
    {
        v = std::clamp(v, -1.0f, 1.0f);
        return static_cast<uint8_t>(std::lround((v + 1.0f) * 127.5f));
    }

    // Radial deadzone, rescaled so the stick still reaches full deflection.
    void stickBytes(float x, float y, uint8_t &bx, uint8_t &by)
    {
        const float m = std::sqrt(x * x + y * y);
        if (m < kDeadzone)
        {
            x = 0.0f;
            y = 0.0f;
        }
        else
        {
            const float s = std::min(1.0f, (m - kDeadzone) / (1.0f - kDeadzone)) / m;
            x *= s;
            y *= s;
        }
        bx = axisByte(x);
        by = axisByte(y);
    }

    // Left stick -> d-pad, per axis with hysteresis (diagonals come out of both axes).
    uint16_t stickDirs(float x, float y, uint16_t prev)
    {
        uint16_t d = 0;
        const auto on = [prev](float v, uint16_t bit)
        { return v > kDirOn || ((prev & bit) != 0 && v > kDirOff); };
        if (on(-x, kLeft))
            d |= kLeft;
        if (on(x, kRight))
            d |= kRight;
        if (on(-y, kUp))
            d |= kUp;
        if (on(y, kDown))
            d |= kDown;
        return d;
    }

    // Same layout as the keyboard (patch 29) by position: A/B/X/Y = Cross/Circle/Square/Triangle,
    // LB/RB = L1/R1, LT/RT = L2/R2, Back = Select, Start = Start, LS/RS = L3/R3.
    ps2_host_pad::State fromXInput(const XINPUT_GAMEPAD &g, uint16_t &dirs)
    {
        ps2_host_pad::State s;
        const WORD w = g.wButtons;
        uint16_t b = 0;
        if (w & XINPUT_GAMEPAD_DPAD_UP)
            b |= kUp;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN)
            b |= kDown;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT)
            b |= kLeft;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT)
            b |= kRight;
        if (w & XINPUT_GAMEPAD_START)
            b |= kStart;
        if (w & XINPUT_GAMEPAD_BACK)
            b |= kSelect;
        if (w & XINPUT_GAMEPAD_LEFT_THUMB)
            b |= kL3;
        if (w & XINPUT_GAMEPAD_RIGHT_THUMB)
            b |= kR3;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER)
            b |= kL1;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER)
            b |= kR1;
        if (w & XINPUT_GAMEPAD_A)
            b |= kCross;
        if (w & XINPUT_GAMEPAD_B)
            b |= kCircle;
        if (w & XINPUT_GAMEPAD_X)
            b |= kSquare;
        if (w & XINPUT_GAMEPAD_Y)
            b |= kTriangle;
        if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            b |= kL2;
        if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
            b |= kR2;
        const float lx = axisNorm(g.sThumbLX), ly = -axisNorm(g.sThumbLY);
        const float rx = axisNorm(g.sThumbRX), ry = -axisNorm(g.sThumbRY);
        stickBytes(lx, ly, s.lx, s.ly);
        stickBytes(rx, ry, s.rx, s.ry);
        if (g_stickDpad)
        {
            dirs = stickDirs(lx, ly, dirs);
            b |= dirs;
        }
        s.buttons = b;
        return s;
    }

    namespace pb = ps2_pad_bindings;

    pb::RawPad toRaw(const XINPUT_GAMEPAD &g)
    {
        pb::RawPad r;
        r.buttons = g.wButtons;
        r.lt = g.bLeftTrigger;
        r.rt = g.bRightTrigger;
        r.lx = g.sThumbLX;
        r.ly = g.sThumbLY;
        r.rx = g.sThumbRX;
        r.ry = g.sThumbRY;
        return r;
    }

    // fromXInput with the player's [playerN.pad] bindings (map != nullptr). The analog bytes are the same either way.
    ps2_host_pad::State fromXInputMapped(const XINPUT_GAMEPAD &g, const pb::PadMap *map, uint16_t &dirs, uint32_t &axis)
    {
        ps2_host_pad::State s = fromXInput(g, dirs);
        if (map)
            s.buttons = pb::evalPad(*map, toRaw(g), g_stickDpad, axis);
        return s;
    }

    // Which pad the player reads: a pinned XInput slot, or the next free one in order (slots nobody pinned, the
    // auto players taking them in player order), then the DirectInput pads raylib sees (`raylib` = its index among
    // them). Same order as read(nth) when no slot is pinned.
    struct Resolved
    {
        int slot = -1;
        int raylib = -1;
    };

    Resolved resolvePlayer(const pb::Config &cfg, int player)
    {
        Resolved r;
        const pb::PlayerCfg &c = cfg.p[player];
        if (!c.pad)
            return r;
        if (c.xslot >= 0)
        {
            if (g_x[c.xslot].connected.load(std::memory_order_acquire))
                r.slot = c.xslot;
            return r;
        }
        int rank = 0;
        for (int q = 0; q < player; ++q)
            if (cfg.p[q].pad && cfg.p[q].xslot < 0)
                ++rank;
        const unsigned pinned = cfg.pinnedMask();
        int seen = 0;
        for (int i = 0; i < kXSlots; ++i)
        {
            if ((pinned >> i) & 1u || !g_x[i].connected.load(std::memory_order_acquire))
                continue;
            if (seen == rank)
            {
                r.slot = i;
                return r;
            }
            ++seen;
        }
        r.raylib = rank - seen;
        return r;
    }

    // The player whose pad is XInput slot `slot` (-1: nobody reads it).
    int playerOfSlot(const pb::Config &cfg, int slot)
    {
        for (int p = 0; p < pb::kPlayers; ++p)
            if (resolvePlayer(cfg, p).slot == slot)
                return p;
        return -1;
    }

    ps2_host_pad::State fromRaylib(const ps2_host_pad::RaylibPad &p, uint16_t &dirs)
    {
        ps2_host_pad::State s;
        s.buttons = p.buttons;
        if (g_raw)
        {
            // The old path: raw axes, no deadzone, no stick -> d-pad.
            s.lx = axisByte(p.lx);
            s.ly = axisByte(p.ly);
            s.rx = axisByte(p.rx);
            s.ry = axisByte(p.ry);
            return s;
        }
        stickBytes(p.lx, p.ly, s.lx, s.ly);
        stickBytes(p.rx, p.ry, s.rx, s.ry);
        if (g_stickDpad)
        {
            dirs = stickDirs(p.lx, p.ly, dirs);
            s.buttons |= dirs;
        }
        return s;
    }

    void waitTimer(HANDLE timer, unsigned ms)
    {
        if (!timer)
        {
            Sleep(ms);
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000LL;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, ms + 50u);
        else
            Sleep(ms);
    }

    // 1 ms: latches the presses a pad read could miss; probes empty XInput slots once a second.
    void pollThread()
    {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                              TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        uint64_t last = qpc();
        while (!g_pollStop.load(std::memory_order_relaxed))
        {
            const uint64_t now = qpc();
            const uint32_t gap = static_cast<uint32_t>(usSince(last, now));
            last = now;
            g_pollWakes.fetch_add(1, std::memory_order_relaxed);
            uint32_t prevMax = g_pollGapMaxUs.load(std::memory_order_relaxed);
            while (gap > prevMax && !g_pollGapMaxUs.compare_exchange_weak(prevMax, gap, std::memory_order_relaxed))
            {
            }
            bool any = false;
            const std::shared_ptr<const pb::Config> cfg = pb::current();
            const uint64_t cfgGen = pb::live().gen.load(std::memory_order_acquire);
            for (int i = 0; i < kXSlots; ++i)
            {
                XSlot &x = g_x[i];
                XINPUT_STATE xs{};
                // Pad bindings: presses are latched in the terms of the player who reads this slot.
                const pb::PadMap *map = nullptr;
                if (!cfg->isDefault())
                {
                    const int player = playerOfSlot(*cfg, i);
                    if (player >= 0 && cfg->p[player].padMap.any())
                        map = &cfg->p[player].padMap;
                }
                if (x.connected.load(std::memory_order_acquire))
                {
                    if (g_getState(static_cast<DWORD>(i), &xs) == ERROR_SUCCESS)
                    {
                        any = true;
                        const uint16_t b = fromXInputMapped(xs.Gamepad, map, x.pollDirs, x.pollAxis).buttons;
                        if (x.pollGen != cfgGen)
                        {
                            x.pollGen = cfgGen; // other bindings: no press out of the change itself
                            x.pollPrev = b;
                        }
                        const uint16_t rise = static_cast<uint16_t>(b & ~x.pollPrev);
                        if (rise)
                            x.rises.fetch_or(rise, std::memory_order_acq_rel);
                        x.pollPrev = b;
                    }
                    else
                    {
                        x.connected.store(false, std::memory_order_release);
                        x.lastProbe = now;
                        std::fprintf(stderr, "[pad] XInput slot %d disconnected\n", i);
                        std::fflush(stderr);
                    }
                }
                else if (now - x.lastProbe >= g_freq)
                {
                    x.lastProbe = now;
                    if (g_getState(static_cast<DWORD>(i), &xs) == ERROR_SUCCESS)
                    {
                        x.pollPrev = fromXInputMapped(xs.Gamepad, map, x.pollDirs, x.pollAxis).buttons;
                        x.pollGen = cfgGen;
                        x.connected.store(true, std::memory_order_release);
                        any = true;
                        std::fprintf(stderr, "[pad] XInput slot %d connected\n", i);
                        std::fflush(stderr);
                    }
                }
            }
            waitTimer(timer, any ? 1u : 50u);
        }
        if (timer)
            CloseHandle(timer);
    }

    void parseTest(const char *spec)
    {
        std::string s(spec);
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t e = s.find(',', pos);
            if (e == std::string::npos)
                e = s.size();
            const std::string kv = s.substr(pos, e - pos);
            pos = e + 1;
            const size_t eq = kv.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "start")
                g_test.start = std::atof(v.c_str());
            else if (k == "count")
                g_test.count = std::atoi(v.c_str());
            else if (k == "period")
                g_test.periodMs = std::atoi(v.c_str());
            else if (k == "hold")
                g_test.holdMs = std::atoi(v.c_str());
            else if (k == "what")
                g_test.stick = v == "stick";
        }
    }

    void sleepUntilQpc(uint64_t target)
    {
        for (;;)
        {
            const uint64_t now = qpc();
            if (now >= target)
                return;
            const double leftMs = static_cast<double>(target - now) * 1000.0 / static_cast<double>(g_freq);
            if (leftMs > 3.0)
                Sleep(static_cast<DWORD>(leftMs - 2.0));
            else
                YieldProcessor();
        }
    }

    // Presses d-pad Down / Up (or the left stick down / up) on the fake XInput pad 0, at random sub-frame
    // offsets like tools\input_lag.ps1 does with keys. t1 of each press = the moment the fake pad changes.
    void testThread()
    {
        std::mt19937 rng(12345u);
        std::uniform_int_distribution<int> jitterUs(0, 16666);
        const uint64_t base = g_startQpc + static_cast<uint64_t>(g_test.start * static_cast<double>(g_freq));
        for (int i = 0; i < g_test.count; ++i)
        {
            const uint64_t at = base + static_cast<uint64_t>(i) * g_freq * static_cast<uint64_t>(g_test.periodMs) / 1000u +
                                static_cast<uint64_t>(jitterUs(rng)) * g_freq / 1000000u;
            sleepUntilQpc(at);
            const bool down = (i % 2) == 0;
            const uint16_t bit = down ? kDown : kUp;
            ps2_input_latency::openTestPress(0x10000u | bit, bit);
            if (g_test.stick)
                g_fakeLY.store(down ? static_cast<int16_t>(-32767) : static_cast<int16_t>(32767), std::memory_order_release);
            else
                g_fakeButtons.store(down ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_UP, std::memory_order_release);
            g_fakePacket.fetch_add(1, std::memory_order_acq_rel);
            sleepUntilQpc(qpc() + g_freq * static_cast<uint64_t>(g_test.holdMs) / 1000u);
            g_fakeButtons.store(0, std::memory_order_release);
            g_fakeLY.store(0, std::memory_order_release);
            g_fakePacket.fetch_add(1, std::memory_order_acq_rel);
        }
        std::fprintf(stderr, "[pad-test] done: %d presses\n", g_test.count);
        std::fflush(stderr);
    }

    bool envIs(const char *name, const char *value)
    {
        const char *v = std::getenv(name);
        return v && std::strcmp(v, value) == 0;
    }

    void init()
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = static_cast<uint64_t>(f.QuadPart);
        {
            // Process start in QPC units, so TEKKEN4_PAD_TEST times match the capture scripts' "seconds after start".
            FILETIME created{}, exited{}, kernel{}, user{}, nowFt{};
            const uint64_t nowQ = qpc();
            GetSystemTimePreciseAsFileTime(&nowFt);
            uint64_t ageQ = 0;
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
            {
                const uint64_t c = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                const uint64_t n = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
                if (n > c)
                    ageQ = (n - c) * g_freq / 10000000u;
            }
            g_startQpc = nowQ > ageQ ? nowQ - ageQ : 0;
        }
        g_raw = envIs("TEKKEN4_PAD_BACKEND", "raylib");
        g_stickDpad = !envIs("TEKKEN4_PAD_STICK_DPAD", "0");
        g_stats = envIs("TEKKEN4_PAD_STATS", "1");
        const char *test = std::getenv("TEKKEN4_PAD_TEST");
        g_testOn = test && *test;
        if (g_testOn)
        {
            parseTest(test);
            g_getState = fakeGetState;
            g_xinputDll = "test";
        }
        else if (!g_raw)
        {
            static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
            for (const char *dll : dlls)
            {
                if (HMODULE m = LoadLibraryA(dll))
                {
                    g_getState = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
                    if (g_getState)
                    {
                        g_xinputDll = dll;
                        break;
                    }
                }
            }
        }
        if (g_getState && !g_raw)
        {
            pb::current(); // loads the bindings file now, so the poll thread starts on the right generation
            const uint64_t gen0 = pb::live().gen.load(std::memory_order_acquire);
            const uint64_t now = qpc();
            for (int i = 0; i < kXSlots; ++i)
            {
                XINPUT_STATE xs{};
                g_x[i].lastProbe = now;
                if (g_getState(static_cast<DWORD>(i), &xs) == ERROR_SUCCESS)
                {
                    g_x[i].pollPrev = fromXInput(xs.Gamepad, g_x[i].pollDirs).buttons;
                    g_x[i].pollGen = gen0;
                    g_x[i].connected.store(true, std::memory_order_release);
                    std::fprintf(stderr, "[pad] XInput slot %d connected\n", i);
                }
            }
            std::thread(pollThread).detach();
        }
        std::fprintf(stderr, "[pad] backend=%s xinput=%s stick_dpad=%d stats=%d test=%d\n",
                     g_raw ? "raylib" : "direct", g_xinputDll, g_stickDpad ? 1 : 0, g_stats ? 1 : 0, g_testOn ? 1 : 0);
        std::fflush(stderr);
        if (g_testOn)
            std::thread(testThread).detach();
    }

    void ensureInit()
    {
        std::call_once(g_initOnce, init);
    }

    // Under g_readMu.
    void maybePrintStats(uint64_t now)
    {
        if (!g_stats)
            return;
        if (g_st.windowStart == 0)
        {
            g_st.windowStart = now;
            return;
        }
        if (now - g_st.windowStart < 5u * g_freq)
            return;
        const uint32_t wakes = g_pollWakes.exchange(0, std::memory_order_relaxed);
        const uint32_t gapMax = g_pollGapMaxUs.exchange(0, std::memory_order_relaxed);
        const double secs = static_cast<double>(now - g_st.windowStart) / static_cast<double>(g_freq);
        std::fprintf(stderr,
                     "[pad-stats] backend=%s reads/s=%.1f xinput_calls=%u xinput_us avg=%.1f max=%.1f "
                     "raylib_reads=%u raylib_age_ms avg=%.2f max=%.2f taps=%u poll/s=%.0f poll_gap_max_ms=%.2f\n",
                     g_raw ? "raylib" : "direct", g_st.reads / secs, g_st.xiCalls,
                     g_st.xiCalls ? g_st.xiUsSum / g_st.xiCalls : 0.0, g_st.xiUsMax, g_st.snapReads,
                     g_st.snapReads ? g_st.ageUsSum / g_st.snapReads / 1000.0 : 0.0, g_st.ageUsMax / 1000.0, g_st.taps,
                     wakes / secs, gapMax / 1000.0);
        std::fflush(stderr);
        g_st = Stats{};
        g_st.windowStart = now;
    }

    // Under g_readMu.
    bool readXInput(int slot, ps2_host_pad::State &out, uint64_t now, const pb::PadMap *map = nullptr)
    {
        XSlot &x = g_x[slot];
        // Always fresh: XInputGetState costs ~10 us (TEKKEN4_PAD_STATS), and a cached sample could be one
        // taken just before the press (the game reads port 0 twice per frame in menus).
        XINPUT_STATE xs{};
        const uint64_t t0 = qpc();
        const DWORD r = g_getState(static_cast<DWORD>(slot), &xs);
        const double us = usSince(t0, qpc());
        ++g_st.xiCalls;
        g_st.xiUsSum += us;
        g_st.xiUsMax = std::max(g_st.xiUsMax, us);
        if (r != ERROR_SUCCESS)
        {
            // The poll thread logs it and probes the slot again.
            x.connected.store(false, std::memory_order_release);
            return false;
        }
        ps2_host_pad::State st = fromXInputMapped(xs.Gamepad, map, x.gameDirs, x.gameAxis);
        // A press that started and ended between two reads: held for exactly this one read.
        // A speculative run-ahead frame only looks: the real frame after it consumes the taps.
        const bool spec = ps2_runahead::speculative();
        const uint16_t rises = spec ? x.rises.load(std::memory_order_acquire) : x.rises.exchange(0, std::memory_order_acq_rel);
        const uint16_t taps = static_cast<uint16_t>(rises & ~st.buttons & ~x.lastReported);
        if (taps)
        {
            ++g_st.taps;
            st.buttons |= taps;
        }
        if (!spec)
            x.lastReported = st.buttons;
        out = st;
        return true;
    }

    bool directXInput()
    {
        return !g_raw && g_getState != nullptr;
    }

    // The k-th raylib pad that is not read through XInput here. Under g_snapMu.
    RPad *nthRaylib(int k)
    {
        for (RPad &p : g_r)
        {
            if (!p.present)
                continue;
            if (p.xinput && directXInput())
                continue;
            if (k == 0)
                return &p;
            --k;
        }
        return nullptr;
    }

    int connectedXInputBefore(int nth, int &slotOut)
    {
        int seen = 0;
        for (int i = 0; i < kXSlots; ++i)
        {
            if (!g_x[i].connected.load(std::memory_order_acquire))
                continue;
            if (seen == nth)
            {
                slotOut = i;
                return seen;
            }
            ++seen;
        }
        slotOut = -1;
        return seen;
    }
}

namespace ps2_host_pad
{
    bool rawRaylibOnly()
    {
        ensureInit();
        return g_raw;
    }

    bool testActive()
    {
        ensureInit();
        return g_testOn;
    }

    void publishRaylib(const RaylibPad *pads, int count)
    {
        ensureInit();
        RaylibPad fake{};
        if (g_raw && g_testOn)
        {
            // The old path with the scripted pad: it becomes visible exactly when raylib would have polled it
            // (GLFW reads XInput inside PollInputEvents on this thread).
            XINPUT_STATE xs{};
            fakeGetState(0, &xs);
            uint16_t noDirs = 0;
            const bool keep = g_stickDpad;
            g_stickDpad = false;
            fake.buttons = fromXInput(xs.Gamepad, noDirs).buttons;
            g_stickDpad = keep;
            fake.id = 0;
            fake.ly = -axisNorm(xs.Gamepad.sThumbLY);
            std::snprintf(fake.name, sizeof(fake.name), "%s", "test pad");
            pads = &fake;
            count = 1;
        }
        else if (g_testOn)
        {
            count = 0; // the scripted pad is the only one: real pads stay out of a measurement
        }
        const uint64_t now = qpc();
        bool seen[kRaylibPads]{};
        std::lock_guard<std::mutex> lock(g_snapMu);
        for (int i = 0; i < count; ++i)
        {
            const RaylibPad &p = pads[i];
            if (p.id < 0 || p.id >= kRaylibPads)
                continue;
            RPad &r = g_r[p.id];
            seen[p.id] = true;
            if (!r.present || r.xinput != p.xinput)
            {
                std::fprintf(stderr, "[pad] raylib gamepad %d '%s' connected (%s)\n", p.id, p.name,
                             p.xinput ? (directXInput() ? "XInput: read directly" : "XInput") : "DirectInput");
                std::fflush(stderr);
                r = RPad{};
                r.present = true;
                r.xinput = p.xinput;
                std::snprintf(r.name, sizeof(r.name), "%s", p.name);
            }
            r.st = fromRaylib(p, r.dirs);
            r.rises = static_cast<uint16_t>(r.rises | (r.st.buttons & ~r.prevButtons));
            r.prevButtons = r.st.buttons;
            r.qpc = now;
        }
        for (int i = 0; i < kRaylibPads; ++i)
        {
            if (g_r[i].present && !seen[i])
            {
                std::fprintf(stderr, "[pad] raylib gamepad %d '%s' disconnected\n", i, g_r[i].name);
                std::fflush(stderr);
                g_r[i] = RPad{};
            }
        }
    }

    namespace
    {
        // Under g_readMu: the k-th raylib pad (the DirectInput ones, XInput pads read directly are skipped).
        bool readRaylib(int k, State &out, uint64_t now)
        {
            std::lock_guard<std::mutex> snap(g_snapMu);
            RPad *p = nthRaylib(k);
            if (!p)
                return false;
            State st = p->st;
            if (!g_raw)
            {
                const uint16_t taps = static_cast<uint16_t>(p->rises & ~st.buttons & ~p->lastReported);
                if (taps)
                {
                    ++g_st.taps;
                    st.buttons |= taps;
                }
                if (!ps2_runahead::speculative())
                {
                    p->rises = 0;
                    p->lastReported = st.buttons;
                }
            }
            const double age = usSince(p->qpc, now);
            ++g_st.reads;
            ++g_st.snapReads;
            g_st.ageUsSum += age;
            g_st.ageUsMax = std::max(g_st.ageUsMax, age);
            out = st;
            return true;
        }
    }

    bool read(int nth, State &out)
    {
        ensureInit();
        if (nth < 0)
            return false;
        std::lock_guard<std::mutex> lock(g_readMu);
        const uint64_t now = qpc();
        maybePrintStats(now);
        int k = nth;
        if (directXInput())
        {
            int slot = -1;
            const int before = connectedXInputBefore(nth, slot);
            if (slot >= 0)
            {
                if (readXInput(slot, out, now))
                {
                    ++g_st.reads;
                    return true;
                }
                return false;
            }
            k = nth - before;
        }
        return readRaylib(k, out, now);
    }

    bool readPlayer(int player, State &out)
    {
        ensureInit();
        if (player < 0 || player >= pb::kPlayers)
            return false;
        const std::shared_ptr<const pb::Config> cfg = pb::current();
        if (cfg->isDefault())
            return read(player, out);
        if (!cfg->p[player].pad)
            return false;
        std::lock_guard<std::mutex> lock(g_readMu);
        const uint64_t now = qpc();
        maybePrintStats(now);
        const Resolved r = resolvePlayer(*cfg, player);
        if (r.slot >= 0)
        {
            const pb::PadMap *map = cfg->p[player].padMap.any() ? &cfg->p[player].padMap : nullptr;
            if (readXInput(r.slot, out, now, map))
            {
                ++g_st.reads;
                return true;
            }
            return false;
        }
        return r.raylib >= 0 && readRaylib(r.raylib, out, now);
    }

    bool playerPresent(int player)
    {
        ensureInit();
        if (player < 0 || player >= pb::kPlayers)
            return false;
        const std::shared_ptr<const pb::Config> cfg = pb::current();
        if (cfg->isDefault())
            return present(player);
        const pb::PlayerCfg &c = cfg->p[player];
        if (c.keyboard)
            return true;
        if (!c.pad)
            return false;
        const Resolved r = resolvePlayer(*cfg, player);
        if (r.slot >= 0)
            return true;
        if (r.raylib < 0)
            return false;
        std::lock_guard<std::mutex> snap(g_snapMu);
        return nthRaylib(r.raylib) != nullptr;
    }

    bool present(int nth)
    {
        ensureInit();
        if (nth < 0)
            return false;
        int k = nth;
        if (directXInput())
        {
            int slot = -1;
            const int before = connectedXInputBefore(nth, slot);
            if (slot >= 0)
                return true;
            k = nth - before;
        }
        std::lock_guard<std::mutex> snap(g_snapMu);
        return nthRaylib(k) != nullptr;
    }
}

#else

namespace ps2_host_pad
{
    // Other platforms: raylib only (the old path).
    namespace
    {
        std::mutex g_mu;
        RaylibPad g_pads[4];
        int g_count = 0;
    }
    bool rawRaylibOnly() { return true; }
    bool testActive() { return false; }
    void publishRaylib(const RaylibPad *pads, int count)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_count = std::min(count, 4);
        for (int i = 0; i < g_count; ++i)
            g_pads[i] = pads[i];
    }
    bool read(int nth, State &out)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (nth < 0 || nth >= g_count)
            return false;
        const RaylibPad &p = g_pads[nth];
        const auto b = [](float v)
        { return static_cast<uint8_t>(std::lround((std::clamp(v, -1.0f, 1.0f) + 1.0f) * 127.5f)); };
        out.buttons = p.buttons;
        out.lx = b(p.lx);
        out.ly = b(p.ly);
        out.rx = b(p.rx);
        out.ry = b(p.ry);
        return true;
    }
    bool present(int nth)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        return nth >= 0 && nth < g_count;
    }
    bool readPlayer(int player, State &out) { return read(player, out); }
    bool playerPresent(int player) { return present(player); }
}

#endif
