#pragma once
// Tekken 4 port (perf, t4-perf): the VU1 + GS side of a frame on a worker thread.
//
// On the PS2 the VU1 and the GS run beside the EE. Here the EE thread ran them inside its own DMA writes: every
// VIF1 transfer unpacked into VU1 memory, ran the VU1 microprograms (MSCAL/MSCNT) and pushed every GIF packet (PATH1
// XGKICKs, PATH2 DIRECTs, PATH3 DMA) through the GS frontend before the game code could go on -- about half of the
// game thread's work in a fight. With PS2X_VU1_ASYNC on (default), the EE thread only records that work (VU1 memory
// writes, microprogram starts with their TOP/ITOP, the packets' bytes) into a queue, and one worker runs it in the
// same order with the same inputs.
//
// What the EE can see stays exactly as before: the VIF1/DMA/GIF registers, MSKPATH3, PATH2 cut state and VPU_STAT are
// still computed on the EE thread at the same point; nothing the worker does reaches RDRAM, the scratchpad, the IOP or
// the EE scheduler. Its only EE-visible results are VU1 memory/registers, GS privileged registers (CSR FINISH/SIGNAL,
// SIGLBLID) and GS memory read back to the host -- and every EE access to those first waits for the queue to be empty
// (sync): GS privileged register reads/writes, EE loads/stores to VU1 memory, the GS stubs (clears, local->host
// transfers), every VBlankStart (the presenter's latch), rollback capture/restore/hash and any change of the rollback
// cuts. VU1 data the netplay hash leaves out anyway (CutVu1 frames run without VU1 at all: 0 mismatches).
// Cases this does not model run synchronously after a sync: VU1 D/T stop bits enabled, UNPACK forms that read VU1
// memory to update the ROW register (mode 2 with partial vectors), frames with rollback cuts, other threads.
#include <cstdint>
#include <functional>

namespace ps2_vu1_async
{
    // One VIF1 UNPACK with the VIF registers it reads (ps2_vif1_interpreter.cpp).
    struct Vif1Unpack
    {
        uint8_t vn = 0, vl = 0;
        bool maskEnable = false, zeroExtend = false;
        int components = 0;
        uint32_t cl = 1, wl = 1, writes = 0, sources = 0, bytesPerVector = 0, vuAddr = 0, mode = 0, mask = 0;
        uint32_t row[4]{}, col[4]{};
    };
    // Writes the UNPACK into vu1 (16 KB VU1 data memory); row = the ROW register (mode 2 updates it).
    void applyVif1Unpack(uint8_t *vu1, const Vif1Unpack &u, const uint8_t *source, uint32_t *row);

    struct Hooks
    {
        uint8_t *vu1Data = nullptr;
        uint8_t *vu1Code = nullptr;
        std::function<void()> codeModified;                                      // VU1 code generation bump
        std::function<void(uint32_t pc, uint32_t top, uint32_t itop)> mscal;     // runs the microprogram
        std::function<void(uint32_t top, uint32_t itop)> mscnt;
        std::function<void(const uint8_t *data, uint32_t size)> gif;             // the GS's packet function
    };

    // Starts the worker when PS2X_VU1_ASYNC is not 0. Called once with the bound subsystems.
    void start(const Hooks &hooks);
    // The EE executor thread calls this once (EeScheduler::run): only it queues work.
    void bindProducer() noexcept;
    bool enabled() noexcept;
    bool onWorker() noexcept;
    // On the VU1 worker with the GS worker split off: queues an XGKICK packet for the GS worker (true) -- else false
    // and the caller hands the packet to the GS itself.
    bool forwardWorkerGif(const uint8_t *data, uint32_t size);
    // True when work may be queued now: enabled, called on the producer (EE) thread, no rollback cut active.
    bool active() noexcept;
    // Waits until everything queued so far has run (no-op when disabled, idle or on the worker itself).
    void sync() noexcept;
    // sync() before an UNPACK form that reads VU1 memory on the EE thread (counted in stats().fallbacks).
    void syncForFallback() noexcept;

    // Producers (EE thread, only while active()).
    void writeVu1Data(uint32_t offset, const void *source, uint32_t bytes); // wraps at the 16 KB end
    void writeVu1Code(uint32_t offset, const void *source, uint32_t bytes);
    void mscal(uint32_t pc, uint32_t top, uint32_t itop);
    void mscnt(uint32_t top, uint32_t itop);
    void gif(const uint8_t *data, uint32_t size);
    void unpack(const Vif1Unpack &u, const uint8_t *source, uint32_t sourceBytes); // u.row = ROW before it

    struct Stats
    {
        uint64_t records = 0, bytes = 0, syncs = 0, syncWaits = 0, syncWaitNs = 0, fullWaits = 0, fallbacks = 0;
    };
    Stats stats() noexcept;
}
