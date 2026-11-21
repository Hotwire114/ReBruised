#pragma once

// Rollback support (phase 0/1, docs/ROLLBACK_PLAN.md of the Tekken 4 port): a full copy of the emulated machine
// at a frame boundary (EeScheduler boundary hook), its restore, and a state hash. Nothing here runs unless a game
// module installs the boundary hook; the default runtime is unchanged.
//
// What a snapshot holds: RDRAM 32 MB, scratchpad, VU0/VU1 code+data, EeScheduler (threads, contexts, kernel
// objects, deadlines, cycle counters), PS2Memory device state (DMA/VIF/GIF/MFIFO/timers), the VU0/VU1 interpreters,
// the guest heap bookkeeping of the runtime, the pad stub ports. NOT held (output side or outside the EE): GS VRAM
// and GS frontend/backend state, IOP (CPU, RAM, SPU2, SoundEngine), host audio, presenter.

#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "ps2x/iop/iop_subsystem.h"

#include <cstdint>
#include <memory>
#include <vector>

class PS2Runtime;

// Netplay (Tekken 4 phase 3): the IOP can be part of a snapshot (setIncludeIop), so a re-simulated frame runs the
// IOP like a live one and both peers' IOPs only ever hear the timeline that sticks. Game modules test this macro.
#define PS2X_ROLLBACK_IOP_STATE 1
// Tekken 4 phase 3b: incremental RDRAM copies (rdramCopyStats).
#define PS2X_ROLLBACK_RDRAM_INCREMENTAL 1

namespace ps2_rollback
{
    struct Snapshot
    {
        std::vector<uint8_t> rdram;
        std::vector<uint8_t> scratchpad;
        std::vector<uint8_t> vu0Code, vu0Data, vu1Code, vu1Data;
        EeScheduler::SavedState scheduler;
        PS2DeviceState devices;
        std::unique_ptr<VU1Interpreter> vu0, vu1;
        R5900Context cpuContext;
        std::vector<uint8_t> runtimeState; // guest heap + async callback stacks (PS2RollbackAccess)
        std::vector<uint8_t> padState;
        uint32_t sifSendCmdId = 1;
        uint64_t rdramSeq = 0; // incremental RDRAM copies: the write-watch sequence this copy was filled at
        ps2x::iop::IopSnapshot iop; // filled only while includeIop()
        bool hasIop = false;
        bool valid = false;
    };

    // Off by default (phase 2: the IOP stays outside and is frozen/gated in re-simulated frames).
    void setIncludeIop(bool on) noexcept;
    bool includeIop() noexcept;
    // Hash of the IOP part of a snapshot (0 when it has none).
    uint64_t hashIop(const Snapshot &snapshot);
    // Re-simulated frames with the IOP running: the SPU2 output of those frames is not played.
    void setIopOutputMuted(bool muted) noexcept;

    // Incremental RDRAM copies (RDRAM under a write watch, Tekken 4 netplay): pages copied per capture/restore;
    // verifyMismatchPages counts pages that differed after a copy (PS2X_RB_WW_VERIFY=1; must stay 0).
    struct RdramCopyStats
    {
        uint64_t captures = 0, capturePages = 0, restores = 0, restorePages = 0, verifyMismatchPages = 0;
    };
    RdramCopyStats rdramCopyStats() noexcept;
    bool rdramIncremental() noexcept;

    // Both at a boundary only (inside the EeScheduler boundary hook, executor thread).
    void capture(PS2Runtime &runtime, Snapshot &out);
    void restore(PS2Runtime &runtime, const Snapshot &in);

    // Byte range excluded from the RDRAM hash (physical address).
    struct MaskRange
    {
        uint32_t address = 0;
        uint32_t size = 0;
    };

    struct StateHash
    {
        uint64_t total = 0;      // everything below combined
        uint64_t rdram = 0;      // RDRAM with masks applied
        uint64_t other = 0;      // scratchpad + VU memories + scheduler digest + device state digest
        uint64_t scheduler = 0;  // scheduler digest alone
        uint32_t regions[32]{};  // RDRAM per 1 MB (masked), folded to 32 bits
    };

    // Hashes the current machine state. pageHashes (optional) receives 8192 per-4 KB-page hashes of RDRAM.
    StateHash hashState(PS2Runtime &runtime, const std::vector<MaskRange> &masks,
                        std::vector<uint64_t> *pageHashes = nullptr);

    uint64_t hashBytes(const uint8_t *data, size_t size, uint64_t seed = 0);
}
