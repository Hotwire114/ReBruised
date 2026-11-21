#pragma once

// Tekken 4 port (t4iop): where the emulated IOP spends the EE thread's time. Off unless PS2X_IOP_PROF=1 (one
// relaxed load per runCycles otherwise). Counters are written by the thread that runs the IOP only.
//   PS2X_IOP_PROF=1          count + time (TSC) every IOP advance; a "[iop-prof]" line every 600 guest frames
//   PS2X_IOP_PROF_PCS=1      also a histogram of the interpreted PCs (top functions/loops in the report)
//   PS2X_RECOMP_AUDIT=1      independent exact interpreter/HLE counters, about once per wall second.
//     interpreted_calls counts only cpuCore.executeInstruction, including RPC/callback work;
//     interpreted_model_cycles is identical because this runtime charges one cycle per instruction,
//     not a hardware-accurate R3000 cycle count. HLE calls, guest import jumps and missing imports
//     are separate; idle_skipped_cycles covers both sealed-idle and no-runnable-thread advances.
//     Counts are host-only, do not rewind during rollback, and never enter save/hash state.
//     Existing [iop-prof] instr includes HLE import steps and must not be used as this exact count.

#include <cstdint>

namespace ps2x::iop
{
    struct IopProfile
    {
        uint64_t calls = 0;        // runCycles calls (EE checkpoints that advanced the IOP)
        uint64_t idleCalls = 0;    // ... that ran no guest instruction
        uint64_t fastCalls = 0;    // EE advances taken by the idle fast path (not counted in calls)
        uint64_t runTicks = 0;     // whole runCycles
        uint64_t cpuTicks = 0;     // runCpu slices (interpreter + HLE imports called from it)
        uint64_t importTicks[6]{}; // HLE imports: libsd, kernel (thbase/thsemap/thevent), sifcmd, cdvdman, sysclib, other
        uint64_t importCalls[6]{};
        uint64_t soundTicks = 0;   // SPU2 mix + IRQ service + host drain (drainMixedAudio)
        uint64_t sifTicks = 0;     // EE->IOP SIF commands, DMA interrupts, guest callbacks, timers outside slices
        uint64_t rpcTicks = 0;     // synchronous EE->IOP RPCs (the server function runs on the EE thread)
        uint64_t rpcCalls = 0;
        uint64_t rpcInstructions = 0;
        uint64_t slices = 0;
        uint64_t instructions = 0;
        uint64_t samples = 0;      // stereo samples mixed
        uint64_t iopCycles = 0;
    };

    [[nodiscard]] bool iopProfileOn() noexcept;
    [[nodiscard]] IopProfile &iopProfile() noexcept;
    // Whole runCycles + RPC time so far in nanoseconds (0 while off): the rollback frame log's iop column.
    [[nodiscard]] uint64_t iopProfileNs() noexcept;
}
