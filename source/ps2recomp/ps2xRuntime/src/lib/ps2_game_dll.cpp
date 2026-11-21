// PS2X_GAME_DLL: the runtime without the generated game code. ps2EntryRunner loads ps2_game.dll from its own folder
// (or the file PS2X_GAME_DLL names) before anything runs; that DLL holds the code ps2_recomp generated from the player's
// disc and the lifted VU programs, built on the player's PC (Tekken 4: tools/build_game_dll.py). It imports what it calls
// from this executable (the exports in PS2X_GAME_DLL_DEF) and hands back its function table here.
#if defined(PS2X_GAME_DLL_HOST)
#include "ps2_runtime.h"
#include "ps2_game_dll_abi.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif

uint32_t g_ps2RecompiledFunctionTableBase = 0u;
uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction *g_ps2RecompiledFunctionTable = nullptr;

namespace
{
    // The table as the DLL filled it, before any game module replaced an entry: ps2GameDllOriginal() hands out the
    // generated function itself, so a module's wrapper can still call the game's own code.
    std::vector<PS2Runtime::RecompiledFunction> g_original;

    [[noreturn]] void fail(const std::wstring &message)
    {
        std::fwprintf(stderr, L"[game-dll] %ls\n", message.c_str());
        std::fflush(stderr);
#if defined(_WIN32)
        if (!std::getenv("PS2X_NO_MESSAGE_BOX"))
            MessageBoxW(nullptr, message.c_str(), L"Tekken 4", MB_OK | MB_ICONERROR);
#endif
        std::exit(3);
    }

    bool cpuHasSse41()
    {
#if defined(_WIN32)
        int regs[4] = {};
        __cpuid(regs, 1);
        return (regs[2] & (1 << 19)) != 0 && (regs[2] & (1 << 9)) != 0; // SSE4.1 and SSSE3
#else
        return true;
#endif
    }
}

bool ps2LoadGameDll()
{
#if defined(_WIN32)
    std::wstring path;
    if (const wchar_t *env = _wgetenv(L"PS2X_GAME_DLL"); env && *env)
    {
        path = env;
    }
    else
    {
        std::vector<wchar_t> buffer(32768);
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        path.assign(buffer.data(), n);
        const size_t slash = path.find_last_of(L"\\/");
        path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"ps2_game.dll";
    }
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            fail(L"The game code is not built yet (" + path + L" is missing).\n\nOpen the launcher and press Build.");
        fail(L"The game code could not be loaded (" + path + L", Windows error " + std::to_wstring(error) +
             L").\n\nPress Build in the launcher again. If it keeps happening, your antivirus may be blocking the file.");
    }
    using InitFn = const Ps2GameDllInfo *(*)(uint32_t);
    auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "ps2_game_dll_init"));
    const Ps2GameDllInfo *info = init ? init(PS2_GAME_DLL_ABI) : nullptr;
    if (!info || info->abi != PS2_GAME_DLL_ABI || !info->table || info->tableSlotCount == 0u)
        fail(L"The game code (" + path + L") was built for another version of the game program.\n\nPress Build in the launcher again.");
    if (info->simd && !cpuHasSse41())
        fail(L"The game code (" + path + L") was built for a processor with SSE4.1, this one does not have it.\n\n"
             L"Press Build in the launcher again on this PC.");
    g_ps2RecompiledFunctionTableBase = info->tableBase;
    g_ps2RecompiledFunctionTableEnd = info->tableEnd;
    g_ps2RecompiledFunctionTableSlotCount = info->tableSlotCount;
    g_ps2RecompiledFunctionTable = info->table;
    g_original.assign(info->table, info->table + info->tableSlotCount);
    std::fprintf(stderr, "[game-dll] loaded %ls (%u functions slots, %s)\n", path.c_str(), info->tableSlotCount,
                 info->simd ? "SSE4.1" : "SSE2");
    return true;
#else
    return false;
#endif
}

PS2Runtime::RecompiledFunction ps2GameDllOriginal(uint32_t address)
{
    const uint32_t slot = (address - g_ps2RecompiledFunctionTableBase) >> 2;
    if ((address & 3u) == 0u && address >= g_ps2RecompiledFunctionTableBase && slot < g_original.size() && g_original[slot])
        return g_original[slot];
    std::fprintf(stderr, "[game-dll] no generated function at 0x%08x\n", address);
    std::abort();
}
#endif
