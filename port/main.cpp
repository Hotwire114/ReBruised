#include "ps2_runtime.h"
#include "games_database.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <sstream>
#include <thread>
#include "runtime/ee_scheduler.h"
#include "ps2_stubs.h"
#include "disc_setup.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#include <psapi.h>
extern "C" __declspec(dllimport) int __stdcall MessageBoxA(void *, const char *, const char *, unsigned int);

#include "rebruised_report.h"

// ---- problem reports (rebruised_report.h) ----
// The project root (the folder holding settings.ini) and the files the launcher's REPORT A PROBLEM page reads.
static char g_bnbSessionFile[MAX_PATH * 2] = {};
static char g_bnbCrashFile[MAX_PATH * 2] = {};
static char g_bnbLauncherExe[MAX_PATH * 2] = {};
static char g_bnbRoot[MAX_PATH * 2] = {};

static void bnbRemoveSessionFile()
{
    if (g_bnbSessionFile[0])
        DeleteFileA(g_bnbSessionFile);
}

// Opens the launcher on its report page (the game is going down); detached, never waits.
static void bnbOpenReportPage()
{
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0 || !g_bnbLauncherExe[0] || std::getenv("BNB_NO_REPORT_PAGE"))
        return;
    char cmd[MAX_PATH * 3];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" --report", g_bnbLauncherExe);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(g_bnbLauncherExe, cmd, nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr,
                       g_bnbRoot[0] ? g_bnbRoot : nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static bool bnbFatalCode(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case 0xC0000409u: // fast fail / stack buffer overrun
    case 0xC0000374u: // heap corruption
        return true;
    default:
        return false;
    }
}

// Unhandled crash (after the vectored reporter below printed it): note it for the report and open the launcher's report
// page. The window is gone a moment later.
static LONG WINAPI bnbUnhandledFilter(PEXCEPTION_POINTERS info)
{
    if (info && info->ExceptionRecord && g_bnbCrashFile[0])
    {
        if (FILE *f = std::fopen(g_bnbCrashFile, "a"))
        {
            std::fprintf(f, "unhandled exception 0x%08lx at %p\n", info->ExceptionRecord->ExceptionCode,
                         info->ExceptionRecord->ExceptionAddress);
            std::fclose(f);
        }
    }
    bnbRemoveSessionFile();
    bnbOpenReportPage();
    return EXCEPTION_CONTINUE_SEARCH;
}

// Fatal exceptions: print the faulting address and a captured stack as module+offset (stderr and logs\crash.txt), then
// let the normal crash handling continue. tools\symbolize.ps1 turns the BlackAndBruised.exe offsets into function names.
static LONG CALLBACK bnbCrashReporter(PEXCEPTION_POINTERS info)
{
    if (!info || !info->ExceptionRecord || !bnbFatalCode(info->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    FILE *note = g_bnbCrashFile[0] ? std::fopen(g_bnbCrashFile, "a") : nullptr;
    auto describe = [](void *addr, char *out, size_t n)
    {
        HMODULE mod = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(addr), &mod) && mod)
            GetModuleFileNameA(mod, path, MAX_PATH);
        const char *base = std::strrchr(path, '\\');
        std::snprintf(out, n, "%p %s+0x%llx", addr, base ? base + 1 : path,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
    };
    char line[512];
    const auto *rec = info->ExceptionRecord;
    describe(rec->ExceptionAddress, line, sizeof(line));
    for (FILE *out : {stderr, note})
        if (out)
            std::fprintf(out, "[bnb-crash] exception 0x%08lx at %s (%s 0x%llx) thread %lu\n", rec->ExceptionCode, line,
                         rec->NumberParameters > 0 && rec->ExceptionInformation[0] ? "write" : "read",
                         rec->NumberParameters > 1 ? static_cast<unsigned long long>(rec->ExceptionInformation[1]) : 0ull,
                         GetCurrentThreadId());
#if defined(_M_X64) || defined(__x86_64__)
    // Walk the faulting context with RtlVirtualUnwind (works for the frames that have unwind data).
    CONTEXT ctx = *info->ContextRecord;
    for (int i = 0; i < 40 && ctx.Rip; ++i)
    {
        describe(reinterpret_cast<void *>(ctx.Rip), line, sizeof(line));
        std::fprintf(stderr, "[bnb-crash]   #%02d %s\n", i, line);
        if (note)
            std::fprintf(note, "[bnb-crash]   #%02d %s\n", i, line);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn)
        {
            ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
#endif
    std::fflush(stderr);
    if (note)
        std::fclose(note);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Session start: logs\session.txt (removed at a normal exit) and the crash note path for the runtime (BNB_CRASH_FILE:
// EeScheduler appends its [ee-crash] report there). root = the folder holding settings.ini.
static void bnbStartSession(const std::filesystem::path &root)
{
    std::error_code ec;
    const std::filesystem::path logs = root / "logs";
    std::filesystem::create_directories(logs, ec);
    std::snprintf(g_bnbRoot, sizeof(g_bnbRoot), "%s", root.string().c_str());
    std::snprintf(g_bnbSessionFile, sizeof(g_bnbSessionFile), "%s", (logs / REBRUISED_SESSION_FILE).string().c_str());
    std::snprintf(g_bnbCrashFile, sizeof(g_bnbCrashFile), "%s", (logs / REBRUISED_CRASH_FILE).string().c_str());
    char exe[MAX_PATH * 2] = {};
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    std::snprintf(g_bnbLauncherExe, sizeof(g_bnbLauncherExe), "%s",
                  (std::filesystem::path(exe).parent_path() / "BlackAndBruisedLauncher.exe").string().c_str());
    if (!std::filesystem::exists(g_bnbLauncherExe, ec))
        g_bnbLauncherExe[0] = 0;
    DeleteFileA(g_bnbCrashFile); // a new session: the last crash note was for the last one
    if (FILE *f = std::fopen(g_bnbSessionFile, "w"))
    {
        const std::time_t now = std::time(nullptr);
        char when[64];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "pid=%lu\nstarted=%s\nversion=%s\n", GetCurrentProcessId(), when, REBRUISED_VERSION);
        std::fclose(f);
    }
    _putenv_s("BNB_CRASH_FILE", g_bnbCrashFile);
    SetUnhandledExceptionFilter(bnbUnhandledFilter);
}

// The game is ending: a normal close removes the session file; when the runtime stopped on a guest crash (it wrote
// logs\crash.txt) the launcher opens on its report page.
static void bnbEndSession()
{
    std::error_code ec;
    const bool crashed = g_bnbCrashFile[0] && std::filesystem::exists(g_bnbCrashFile, ec);
    bnbRemoveSessionFile();
    if (crashed)
        bnbOpenReportPage();
}
#endif
#include <exception>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

void bnbInstallButtonPrompts(); // button_prompts.cpp

// disc_setup.cpp: per-user data folder
std::string bnbLocalAppData()
{
    const char *e = std::getenv("LOCALAPPDATA");
    return e ? e : "";
}

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    // ---- PC settings: settings.ini (written by the launcher, BlackAndBruised-Launcher.pyw; hand-editable) ----
    // "key = value" lines, '#' comments. Each key becomes the runtime option below unless that environment variable
    // is already set (scripts and the command line win). Read before anything in the runtime looks at the options.
    void setDefaultEnv(const char *name, const std::string &value)
    {
        if (value.empty() || std::getenv(name))
            return;
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 0);
#endif
    }

    std::string trimLower(std::string s, bool lower = true)
    {
        const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        if (lower)
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Returns the folder of the settings.ini it read (the project root; the current folder without one).
    std::filesystem::path loadSettings(const char *argv0)
    {
        std::vector<std::filesystem::path> candidates;
        if (const char *e = std::getenv("BNB_SETTINGS"); e && *e)
            candidates.emplace_back(e);
        candidates.push_back(std::filesystem::current_path() / "settings.ini");
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
        for (int up = 0; up < 4 && !dir.empty(); ++up, dir = dir.parent_path())
            candidates.push_back(dir / "settings.ini");
        std::filesystem::path file;
        for (const auto &c : candidates)
            if (std::filesystem::is_regular_file(c, ec))
            {
                file = c;
                break;
            }
        if (file.empty())
        {
            std::cerr << "[settings] no settings.ini (defaults)" << std::endl;
            return std::filesystem::current_path();
        }
        std::ifstream in(file);
        std::map<std::string, std::string> kv;
        for (std::string line; std::getline(in, line);)
        {
            line = line.substr(0, line.find('#'));
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
                kv[trimLower(line.substr(0, eq))] = trimLower(line.substr(eq + 1));
        }
        const auto get = [&](const char *k) { const auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; };
        const auto onOff = [](const std::string &v) { return v == "1" || v == "on" || v == "true" || v == "yes" ? "1" : v.empty() ? "" : "0"; };

        const std::string fps = get("framerate");
        if (fps == "60")
            setDefaultEnv("BNB_FPS", "60"); // the vblank rate follows in game (bnb_runtime_ext.h BNB_FPS_CAP)
        else if (!fps.empty())
            setDefaultEnv("BNB_FPS", "50");
        setDefaultEnv("TEKKEN4_INTERNAL_RES", get("resolution"));
        setDefaultEnv("TEKKEN4_AA", get("antialiasing"));
        setDefaultEnv("TEKKEN4_ANISO", get("anisotropic"));
        if (const std::string d = get("display"); !d.empty())
            setDefaultEnv("TEKKEN4_FULLSCREEN", d == "borderless" || d == "fullscreen" ? "borderless" : d == "exclusive" ? "exclusive" : "0");
        setDefaultEnv("TEKKEN4_WINDOW_FILTER", get("scaling"));
        setDefaultEnv("TEKKEN4_WIDESCREEN", onOff(get("widescreen")));
        // menus widescreen with the fights unless widescreen_menus = off
        setDefaultEnv("PS2X_WS_MENUS", get("widescreen_menus") == "off" ? "0" : "1");
        setDefaultEnv("TEKKEN4_HUD", get("hud"));
        setDefaultEnv("TEKKEN4_SOUND", onOff(get("sound")));
        setDefaultEnv("TEKKEN4_AUDIO_BUFFER_MS", get("audio_latency")); // ms of sound buffered ahead (20..150)
        setDefaultEnv("BNB_MOVIE_TIMEOUT", get("movie_timeout"));
        setDefaultEnv("PS2X_DEBUG_PANEL", onOff(get("debug_panel")));
        setDefaultEnv("BNB_FPS_LOG", get("show_fps") == "1" || get("show_fps") == "on" ? "1" : "");
        setDefaultEnv("BNB_PROMPTS", get("launcher_glyphs"));
        setDefaultEnv("BNB_ISO", get("disc_image")); // the player's own disc image (launcher: GAME DISC) // controller glyphs in the game too (button_prompts.cpp)
        std::cerr << "[settings] " << file.string() << ":";
        for (const auto &[k, v] : kv)
            std::cerr << " " << k << "=" << v;
        std::cerr << std::endl;
        return std::filesystem::absolute(file, ec).parent_path();
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
        // ReBruised: no game files ship with the program; the launcher takes them from the player's own disc image
        // (port/disc_setup.h). Without a set-up disc the game does not start.
        {
            const char *iso = std::getenv("BNB_ISO");
            if (iso && *iso && disc_setup::ready(iso))
                return disc_setup::elfPath();
#if defined(_WIN32)
            MessageBoxA(nullptr,
                        "ReBruised needs your own copy of Black & Bruised (PAL, SLES-51620).\n\n"
                        "Start BlackAndBruised-Launcher and choose GAME DISC to select your disc image.",
                        "ReBruised", 0x30u /* MB_ICONWARNING */);
            bnbRemoveSessionFile();
#endif
            std::_Exit(2);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, bnbCrashReporter);
#endif
    const std::filesystem::path bnbRootDir = loadSettings(argc > 0 ? argv[0] : nullptr);
#if defined(_WIN32)
    bnbStartSession(bnbRootDir);
#endif
    bnbInstallButtonPrompts();
    // defaults when neither settings.ini nor the environment says otherwise
    setDefaultEnv("BNB_FPS", "60");
    setDefaultEnv("TEKKEN4_SOUND", "1");          // the game's own sound driver on the emulated IOP
    // Sound latency: the runtime buffered 64 ms ahead of the device (52-75 ms measured in fights); 40 ms keeps fights
    // free of underruns (TEKKEN4_AUDIO_STATS) with ~25 ms less delay from a punch to its sound. 2026-11-17: 60 ms,
    // since the playback rate now follows the averaged level (ps2_audio.cpp) and the fight mix arrives in ~30 ms bursts.
    setDefaultEnv("PS2X_AUDIO_DEFAULT_BUFFER_MS", "60");
    setDefaultEnv("PS2X_DEBUG_PANEL", "0");       // F1 opens it
    // (BNB_MOVIE_TIMEOUT: off by default; movies end at their PSS end code, PS2X_MPEG_END_ON_PROGRAM_END)
    setDefaultEnv("PS2X_GS_THREADS", "auto");
    setDefaultEnv("PS2X_MPEG_END_ON_PROGRAM_END", "1");
    setDefaultEnv("PS2X_CD_SPEED", "8"); // disc streams 8x the PS2 drive: no multi-second load stalls // movies end at their PSS end code (MPEG.cpp sceMpegIsEnd)
    setDefaultEnv("PS2X_GS_BACKEND", "gl");
    // A fight sends ~35 GS queue chunks (64 KB) per frame; with the runtime's 8 the game thread waited for the GS
    // worker ~300-450 ms per second (lock-step). 64 chunks: no waits, 60 fps in fights.
    setDefaultEnv("PS2X_GS_QUEUE_CHUNKS", "64");
#if defined(PS2X_GAME_DLL_HOST)
    // The generated game code is in ps2_game.dll next to this program (PS2X_GAME_DLL): load it before anything runs.
    if (!ps2LoadGameDll())
        return 3;
#endif

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        (void)normalizedId;
        std::string windowTitle = "Black & Bruised - native PC recompilation";

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        // BNB_SKIP_MOVIES=1 (testing): sceMpegIsEnd (0x1091C0) says every movie has ended, so the movie loop
        // (0x16c0a0) stops at once.
        if (const char *skip = std::getenv("BNB_SKIP_MOVIES"); skip && skip[0] == '1')
        {
            runtime.replaceFunction(0x001091C0u, [](uint8_t *, R5900Context *ctx, PS2Runtime *)
            {
                setReturnS32(ctx, 1);
                ctx->pc = getRegU32(ctx, 31);
            });
            std::cerr << "[bnb] movies skipped (BNB_SKIP_MOVIES=1)" << std::endl;
        }
        // BNB_MOVIE_TIMEOUT=<seconds>: the runtime's sceMpegIsEnd, but a movie that has played longer than this ends.
        // Workaround: one of the intro FMVs sometimes decodes to a white picture and never reaches its end.
        else if (const char *limit = std::getenv("BNB_MOVIE_TIMEOUT"); limit && std::atoi(limit) > 0)
        {
            static const double timeoutSec = std::atoi(limit);
            runtime.replaceFunction(0x001091C0u, [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
            {
                using clock = std::chrono::steady_clock;
                static clock::time_point movieStart{}, lastCall{};
                const clock::time_point now = clock::now();
                if (lastCall == clock::time_point{} || now - lastCall > std::chrono::seconds(2))
                    movieStart = now; // a new movie (the loop polls IsEnd every frame)
                lastCall = now;
                const uint32_t ra = getRegU32(ctx, 31);
                ps2_stubs::sceMpegIsEnd(rdram, ctx, rt);
                if (static_cast<int32_t>(getRegU32(ctx, 2)) == 0 &&
                    std::chrono::duration<double>(now - movieStart).count() > timeoutSec)
                {
                    std::cerr << "[bnb] movie timeout: ending it" << std::endl;
                    setReturnS32(ctx, 1);
                }
                ctx->pc = ra;
            });
            std::cerr << "[bnb] movie timeout " << timeoutSec << " s" << std::endl;
        }

        // Black & Bruised streams by absolute disc sectors (sceCdSt* from its own TOC), which the extracted files
        // cannot map: give the CD layer the player's ISO. argv[2], else BNB_ISO, else <disc folder>\..\BnB.iso.
        {
            std::filesystem::path iso;
            if (argc > 2)
                iso = argv[2];
            else if (const char *env = std::getenv("BNB_ISO"); env && *env)
                iso = env;
            else
                iso = pathObj.parent_path().parent_path() / "BnB.iso";
            std::error_code ec;
            if (std::filesystem::is_regular_file(iso, ec))
            {
                auto paths = PS2Runtime::getIoPaths();
                paths.cdImage = std::filesystem::absolute(iso, ec);
                PS2Runtime::setIoPaths(paths);
                std::cerr << "[bnb] disc image: " << paths.cdImage.string() << std::endl;
            }
            else
            {
                std::cerr << "[bnb] no disc image at " << iso.string() << " (streamed audio/data will fail)" << std::endl;
            }
        }

        // BNB_WATCHDOG=<seconds>: every N seconds print guest vsyncs, EE cycle and every thread's pc/ra/sp/state
        // (stderr). The debug panel's thread view needs mouse input; this works in unattended runs.
        std::jthread watchdog;
        if (const char *wd = std::getenv("BNB_WATCHDOG"); wd && std::atoi(wd) > 0)
        {
            const int every = std::atoi(wd);
            watchdog = std::jthread([&runtime, every](std::stop_token stop)
            {
                static const char *const st[] = {"RUN", "READY", "WAIT", "WAITSUSP", "SUSP", "DORMANT"};
                static const char *const why[] = {"-", "sleep", "sema", "evflag"};
                const auto t0 = std::chrono::steady_clock::now();
                while (!stop.stop_requested())
                {
                    for (int i = 0; i < every * 10 && !stop.stop_requested(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                    std::ostringstream o;
                    o << "[bnb-wd] t=" << secs << "s vsync=" << runtime.eeScheduler().currentVSyncTick();
                    // The thread list reads the scheduler without its lock: a long run crashed in this thread (read
                    // of address -1 while copying the snapshot). Only with BNB_WATCHDOG_THREADS=1, for short runs.
                    static const bool threads = std::getenv("BNB_WATCHDOG_THREADS") != nullptr;
                    if (!threads)
                    {
                        std::cerr << o.str() << "\n" << std::flush;
                        continue;
                    }
                    const auto snap = runtime.eeScheduler().snapshot();
                    o << " cycle=" << snap.eeCycle << " running=" << snap.runningThreadId << std::hex;
                    for (const auto &t : snap.threads)
                    {
                        if (t.status == EeThreadStatus::Dormant)
                            continue;
                        o << " | T" << std::dec << t.id << std::hex << " " << st[static_cast<int>(t.status)];
                        if (t.waitReason != EeWaitReason::None)
                            o << ":" << why[static_cast<int>(t.waitReason)] << std::dec << t.waitId << std::hex;
                        o << " pc=" << t.pc << " ra=" << t.ra << " pri=" << std::dec << t.currentPriority << std::hex;
                    }
                    for (const auto &s : snap.semaphores)
                        if (s.waiters)
                            o << " | S" << std::dec << s.id << " cnt=" << s.count << " waiters=" << s.waiters << std::hex;
                    o << "\n";
                    std::cerr << o.str() << std::flush;
                }
            });
        }

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
#if defined(_WIN32)
        bnbEndSession();
#endif
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
#if defined(_WIN32)
    if (FILE *f = g_bnbCrashFile[0] ? std::fopen(g_bnbCrashFile, "a") : nullptr)
    {
        std::fprintf(f, "the game stopped with a fatal error (see play.err: [main] fatal exception)\n");
        std::fclose(f);
    }
    bnbEndSession();
#endif
    std::_Exit(1);
}
