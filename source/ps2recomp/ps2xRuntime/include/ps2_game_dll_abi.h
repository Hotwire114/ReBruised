// The contract between the prebuilt runtime (PS2X_GAME_DLL_HOST) and the game DLL built on the player's PC from the
// generated code (PS2X_GAME_DLL_VIEW). Bump PS2_GAME_DLL_ABI whenever the DLL view (ps2_runtime.h, the VU lifter
// headers) or this struct changes: an old DLL is then refused with a message instead of crashing.
#ifndef PS2_GAME_DLL_ABI_H
#define PS2_GAME_DLL_ABI_H

#include <cstdint>

#define PS2_GAME_DLL_ABI 1u

struct R5900Context;
class PS2Runtime;

struct Ps2GameDllInfo
{
    uint32_t abi;
    uint32_t simd;           // 1 = built with SSE4.1, 0 = SSE2 only
    uint32_t tableBase;
    uint32_t tableEnd;
    uint32_t tableSlotCount;
    void (**table)(uint8_t *, R5900Context *, PS2Runtime *);
};

#endif
