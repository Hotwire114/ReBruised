/* Scripted XInput for unattended test runs of the Black & Bruised port.
 *
 * Placed next to ps2EntryRunner.exe as xinput1_4.dll. Without BNB_PAD_SCRIPT / BNB_PAD_LIVE it forwards every call
 * to the system's real xinput1_4.dll, so normal controllers keep working.
 *
 *   BNB_PAD_SCRIPT = "t:mask[:dur];t:mask[:dur];..."   t = seconds after process start, mask = XInput wButtons hex,
 *                                                       dur = hold seconds (default 0.15). Slot 0 only.
 *   BNB_PAD_LIVE   = path of a text file holding one hex mask; re-read every 50 ms and OR-ed in (slot 0).
 *   Mask bits: Up 1, Down 2, Left 4, Right 8, Start 10, Back 20, LB 100, RB 200, A 1000, B 2000, X 4000, Y 8000;
 *   LT 10000, RT 20000 (mapped to the analog triggers).
 *
 * Build: tools\xinput_script\build.cmd (llvm-mingw clang, plain C).
 */
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XGAMEPAD;
typedef struct { DWORD dwPacketNumber; XGAMEPAD Gamepad; } XSTATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XGAMEPAD Gamepad; XVIBRATION Vibration; } XCAPS;

typedef DWORD(WINAPI *GetStateFn)(DWORD, XSTATE *);
typedef DWORD(WINAPI *SetStateFn)(DWORD, XVIBRATION *);
typedef DWORD(WINAPI *GetCapsFn)(DWORD, DWORD, XCAPS *);
typedef void(WINAPI *EnableFn)(BOOL);

#define MAX_EVENTS 512
static struct { double t, dur; DWORD mask; } g_ev[MAX_EVENTS];
static int g_nev;
static int g_mode = -1; /* -1 not initialised, 0 forward, 1 scripted */
static double g_start;
static LARGE_INTEGER g_freq;
static char g_live[MAX_PATH];
static DWORD g_liveMask, g_packet = 1, g_lastMask = 0xFFFFFFFF;
static double g_liveRead;
static HMODULE g_real;
static GetStateFn r_get;
static SetStateFn r_set;
static GetCapsFn r_caps;
static EnableFn r_enable;
static CRITICAL_SECTION g_cs;

static double now_s(void)
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)g_freq.QuadPart;
}

static void init(void)
{
    if (g_mode >= 0)
        return;
    EnterCriticalSection(&g_cs);
    if (g_mode < 0) {
        QueryPerformanceFrequency(&g_freq);
        /* process age, so script times are "seconds after the game started" */
        FILETIME c, e, k, u, n;
        double age = 0.0;
        GetSystemTimeAsFileTime(&n);
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            ULONGLONG cc = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            ULONGLONG nn = ((ULONGLONG)n.dwHighDateTime << 32) | n.dwLowDateTime;
            if (nn > cc) age = (double)(nn - cc) / 1e7;
        }
        g_start = now_s() - age;
        const char *s = getenv("BNB_PAD_SCRIPT");
        const char *l = getenv("BNB_PAD_LIVE");
        if ((s && *s) || (l && *l)) {
            g_mode = 1;
            if (l) strncpy(g_live, l, MAX_PATH - 1);
            if (s) {
                const char *p = s;
                while (*p && g_nev < MAX_EVENTS) {
                    double t = strtod(p, (char **)&p);
                    if (*p != ':') break;
                    DWORD m = strtoul(p + 1, (char **)&p, 16);
                    double d = 0.15;
                    if (*p == ':') d = strtod(p + 1, (char **)&p);
                    g_ev[g_nev].t = t; g_ev[g_nev].mask = m; g_ev[g_nev].dur = d; g_nev++;
                    while (*p == ';' || *p == ' ') p++;
                }
            }
            fprintf(stderr, "[bnb-pad] scripted XInput: %d events%s%s\n", g_nev, g_live[0] ? ", live file " : "", g_live);
        } else {
            char path[MAX_PATH];
            UINT len = GetSystemDirectoryA(path, MAX_PATH);
            strcpy(path + len, "\\xinput1_4.dll");
            g_real = LoadLibraryA(path);
            if (g_real) {
                r_get = (GetStateFn)GetProcAddress(g_real, "XInputGetState");
                r_set = (SetStateFn)GetProcAddress(g_real, "XInputSetState");
                r_caps = (GetCapsFn)GetProcAddress(g_real, "XInputGetCapabilities");
                r_enable = (EnableFn)GetProcAddress(g_real, "XInputEnable");
            }
            g_mode = 0;
        }
    }
    LeaveCriticalSection(&g_cs);
}

static DWORD scripted_mask(void)
{
    double t = now_s() - g_start;
    DWORD m = 0;
    for (int i = 0; i < g_nev; i++)
        if (t >= g_ev[i].t && t < g_ev[i].t + g_ev[i].dur) m |= g_ev[i].mask;
    if (g_live[0]) {
        if (t - g_liveRead > 0.05) {
            g_liveRead = t;
            FILE *f = fopen(g_live, "r");
            if (f) {
                unsigned long v = 0;
                if (fscanf(f, "%lx", &v) == 1) g_liveMask = (DWORD)v;
                fclose(f);
            }
        }
        m |= g_liveMask;
    }
    return m;
}

__declspec(dllexport) DWORD WINAPI XInputGetState(DWORD slot, XSTATE *s)
{
    init();
    if (g_mode == 0) return r_get ? r_get(slot, s) : ERROR_DEVICE_NOT_CONNECTED;
    if (slot != 0 || !s) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&g_cs);
    DWORD m = scripted_mask();
    if (m != g_lastMask) {
        g_packet++;
        g_lastMask = m;
        if (getenv("BNB_PAD_TRACE"))
            fprintf(stderr, "[bnb-pad] t=%.2f mask=%04lx\n", now_s() - g_start, (unsigned long)m), fflush(stderr);
    }
    memset(s, 0, sizeof(*s));
    s->dwPacketNumber = g_packet;
    s->Gamepad.wButtons = (WORD)(m & 0xFFFF);
    s->Gamepad.bLeftTrigger = (m & 0x10000) ? 255 : 0;
    s->Gamepad.bRightTrigger = (m & 0x20000) ? 255 : 0;
    LeaveCriticalSection(&g_cs);
    return ERROR_SUCCESS;
}

__declspec(dllexport) DWORD WINAPI XInputSetState(DWORD slot, XVIBRATION *v)
{
    init();
    if (g_mode == 0) return r_set ? r_set(slot, v) : ERROR_DEVICE_NOT_CONNECTED;
    return slot == 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

__declspec(dllexport) DWORD WINAPI XInputGetCapabilities(DWORD slot, DWORD flags, XCAPS *c)
{
    init();
    if (g_mode == 0) return r_caps ? r_caps(slot, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
    if (slot != 0 || !c) return ERROR_DEVICE_NOT_CONNECTED;
    memset(c, 0, sizeof(*c));
    c->Type = 1;    /* XINPUT_DEVTYPE_GAMEPAD */
    c->SubType = 1; /* XINPUT_DEVSUBTYPE_GAMEPAD */
    return ERROR_SUCCESS;
}

__declspec(dllexport) void WINAPI XInputEnable(BOOL on)
{
    init();
    if (g_mode == 0 && r_enable) r_enable(on);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_cs);
    return TRUE;
}
