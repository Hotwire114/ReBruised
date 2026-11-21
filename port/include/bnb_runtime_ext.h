// Black & Bruised additions for the game DLL (included by files tools/patch_generated.py rewrote).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>

// ---- probes: register dumps to stderr (logs\run-*.err) from generated code; config/probes.txt ----
// kernel32 through tools/gamedll/bnb_kernel32.def (build_game_dll.py links the import library).
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);

static inline void bnb_write(const char *s, unsigned long n)
{
    unsigned long done = 0;
    WriteFile(GetStdHandle((unsigned long)-12), s, n, &done, 0);
}

static inline char *bnb_hex(char *p, uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i)
        *p++ = d[(v >> (i * 4)) & 15];
    return p;
}

static inline char *bnb_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

// Dumps pc, ra, sp, v0, v1, a0-a3, s0-s3 and the first words at a0 and s0 (guest RAM). At most `limit` times per site.
#define BNB_PROBE(ctx, rdram, PC, LIMIT)                                                         \
    do                                                                                           \
    {                                                                                            \
        static int bnb_probe_count = 0;                                                          \
        if (bnb_probe_count < (LIMIT))                                                           \
        {                                                                                        \
            ++bnb_probe_count;                                                                   \
            const uint32_t bnb_v[18] = {GPR_U32(ctx, 31), GPR_U32(ctx, 29), GPR_U32(ctx, 2), GPR_U32(ctx, 3), GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 16), GPR_U32(ctx, 17), GPR_U32(ctx, 18), GPR_U32(ctx, 19), GPR_U32(ctx, 20), GPR_U32(ctx, 21), GPR_U32(ctx, 22), GPR_U32(ctx, 23), GPR_U32(ctx, 8), GPR_U32(ctx, 9)}; \
            bnb_probe_dump(bnb_v, (rdram), (PC));                                                \
        }                                                                                        \
    } while (0)

// BNB_PROBE_MEM: like BNB_PROBE, plus 8 words at GPR[REG] + OFF.
#define BNB_PROBE_MEM(ctx, rdram, PC, LIMIT, REG, OFF)                                           \
    do                                                                                           \
    {                                                                                            \
        static int bnb_probe_count = 0;                                                          \
        if (bnb_probe_count < (LIMIT))                                                           \
        {                                                                                        \
            ++bnb_probe_count;                                                                   \
            char bnb_b[160];                                                                     \
            char *bnb_p = bnb_str(bnb_b, "[bnb-mem] pc=");                                       \
            bnb_p = bnb_hex(bnb_p, (PC));                                                        \
            const uint32_t bnb_a = (GPR_U32(ctx, REG) + (uint32_t)(OFF)) & 0x01FFFFFCu;          \
            bnb_p = bnb_str(bnb_p, " @");                                                        \
            bnb_p = bnb_hex(bnb_p, bnb_a);                                                       \
            *bnb_p++ = ':';                                                                      \
            for (int bnb_i = 0; bnb_i < 8; ++bnb_i)                                              \
            {                                                                                    \
                *bnb_p++ = ' ';                                                                  \
                bnb_p = bnb_hex(bnb_p, *(const uint32_t *)((rdram) + bnb_a + bnb_i * 4));        \
            }                                                                                    \
            *bnb_p++ = '\n';                                                                     \
            bnb_write(bnb_b, (unsigned long)(bnb_p - bnb_b));                                    \
        }                                                                                        \
    } while (0)

static inline void bnb_probe_dump(const uint32_t *v, const uint8_t *rdram, uint32_t pc)
{
    static const char *const names[] = {"ra", "sp", "v0", "v1", "a0", "a1", "a2", "a3", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t0", "t1"};
    char buf[768];
    char *p = bnb_str(buf, "[bnb-probe] pc=");
    p = bnb_hex(p, pc);
    for (int i = 0; i < 18; ++i)
    {
        *p++ = ' ';
        p = bnb_str(p, names[i]);
        *p++ = '=';
        p = bnb_hex(p, v[i]);
    }
    for (int k = 0; k < 2; ++k)
    {
        const uint32_t base = (k == 0 ? v[4] : v[8]) & 0x01FFFFFFu;
        p = bnb_str(p, k == 0 ? " [a0]=" : " [s0]=");
        for (int w = 0; w < 4; ++w)
        {
            uint32_t v = 0;
            if (base + w * 4 + 4 <= 0x02000000u)
                v = *(const uint32_t *)(rdram + base + w * 4);
            p = bnb_hex(p, v);
            *p++ = w < 3 ? ',' : ' ';
        }
    }
    *p++ = '\n';
    bnb_write(buf, (unsigned long)(p - buf));
}

// COP0 Count. The runtime never advances ctx->cop0_count (Tekken 4 never read it), but Black & Bruised's frame
// limiter (0x13b5d0) busy-waits on it: it waits until Count moved 292000000 / cap since its vblank wait
// (sceGsSyncV) returned, then waits for the next vblank. Count here is the steady clock (QueryPerformanceCounter on
// Windows; the same clock as the scheduler's vblank deadlines) scaled to the EE clock (294.912 MHz). The first
// version used the 1 ms system interrupt time: frames often measured 17 ms > 16.67 and took two vblanks.
static inline uint32_t bnb_count_from_ns(int64_t ns)
{
    return (uint32_t)((unsigned __int128)(uint64_t)ns * 294912u / 1000000u);
}
static inline int64_t bnb_steady_ns(void)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline uint32_t bnb_cop0_count(void)
{
    return bnb_count_from_ns(bnb_steady_ns());
}

// MultiStream status sequence seen by the sound update before its status RPC (tools/patch_generated.py 0x1246e0).
inline uint32_t bnb_ms_seq = 0;
// BNB_STREAM_ACK=0 restores the original (stalling) behaviour, for A/B tests.
static inline bool bnb_stream_ack_on(void)
{
    static const bool on = [] { const char *e = std::getenv("BNB_STREAM_ACK"); return !(e && e[0] == '0'); }();
    return on;
}

// ---- frame rate (settings.ini "framerate", env BNB_FPS) ----

static inline int bnb_fps_setting(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char *e = getenv("BNB_FPS");
        v = e && atoi(e) > 0 ? atoi(e) : 50;
    }
    return v;
}

// Inserted at the cap setter FUN_0013afb0(a0 = frames per second; read only by the limiter 0x13b5d0). Boot sets 60;
// the PAL build's movie wrapper (0x16be00) sets 25 for a movie and 50 after it. The limiter times 292000000 / cap
// Count from the moment its vblank wait (sceGsSyncV) returned, then waits for the next vblank. At cap == vblank rate
// that left 0.17 ms (60) / 0.2 ms (50) of slack after the thread woke on the PC, so frames often took two vblanks
// (26 frames/s at 50, dips to 40 at 60; +10 % still dipped). The gameplay cap is set to twice the target: sceGsSyncV
// paces every frame at the vblank rate, as on the console (steady 60 / 50, pictures complete). Movies need two
// vblanks per frame (the FMVs are 25 fps at 50 Hz): 25 -> 28 (35.7 ms, then the vblank at 40); BNB_MOVIE_CAP overrides
// (40 hung the first movie, 2026-11-17). The movie loop decodes inside the limiter's wait (callback 0x16c070), and the
// runtime's sceMpegGetPicture paces pictures by their PTS on the real vblank clock (MPEG.cpp ntscFieldsToVblanksQ32).
// (White bands in screenshots on 2026-11-16 that looked
// like undrawn frames were another window on the desktop over the game: tools\shots.ps1 now uses PrintWindow.)
// The vblank rate follows: gameplay at the setting, movies at 50 Hz (their 25 frames per second = 2 vblanks).
void ps2SetVblankHz(int hz); // EeScheduler.cpp
static inline int bnb_movie_cap(void)
{
    static int v = -1;
    if (v < 0)
    {
        const char *e = getenv("BNB_MOVIE_CAP");
        v = e && atoi(e) > 0 ? atoi(e) : 28;
    }
    return v;
}
#define BNB_FPS_CAP(ctx)                                                                         \
    do                                                                                           \
    {                                                                                            \
        if (getenv("BNB_FPS_LOG"))                                                               \
        {                                                                                        \
            printf("[bnb-cap] set %u ra=%x\n", GPR_U32(ctx, 4), GPR_U32(ctx, 31));               \
            fflush(0);                                                                           \
        }                                                                                        \
        if (GPR_U32(ctx, 4) == 50u)                                                              \
        {                                                                                        \
            SET_GPR_S32(ctx, 4, (int32_t)(bnb_fps_setting() * 2));                               \
            ps2SetVblankHz(bnb_fps_setting());                                                   \
        }                                                                                        \
        else if (GPR_U32(ctx, 4) == 25u)                                                         \
        {                                                                                        \
            SET_GPR_S32(ctx, 4, bnb_movie_cap());                                                \
            ps2SetVblankHz(50);                                                                  \
        }                                                                                        \
    } while (0)

// BNB_FPS_LOG=1: "[bnb-fps] <game frames in the last second>" on stdout (counted at the frame limiter's entry).
static inline void bnb_frame_tick(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("BNB_FPS_LOG") != 0;
    if (!on)
        return;
    static uint32_t last = 0, frames = 0;
    const uint32_t now = bnb_cop0_count();
    ++frames;
    if (!last)
        last = now;
    else if (now - last >= 294912000u)
    {
        printf("[bnb-fps] %u\n", frames);
        fflush(0);
        frames = 0;
        last = now;
    }
}
