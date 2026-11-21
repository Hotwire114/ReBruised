#pragma once

// Rollback phase 2 (docs/ROLLBACK_PHASE2.md of the Tekken 4 port): what a re-simulated frame leaves out, and the
// gate that keeps the IOP (sound driver, SPU2) outside the rollback state. Nothing here is active unless a game
// module sets cuts or installs a gate; the default runtime path only reads a zero word / a null pointer.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

struct R5900Context;

namespace ps2x::iop
{
    struct RpcRequest;
    struct RpcResult;
}

namespace ps2_vu1_async
{
    void sync() noexcept;
}

namespace ps2_rollback
{
    // Host-only work dropped while a frame is re-simulated. Everything that writes guest-visible state (RDRAM,
    // VU memories, DMA/VIF/GIF registers, GS privileged registers incl. CSR SIGNAL/FINISH, VU1 runs) still runs.
    enum Cut : uint32_t
    {
        CutDraw = 1u << 0,    // GS: assembled primitives are not built nor submitted to the raster backend
        CutUpload = 1u << 1,  // GS: host->local image data is not written to VRAM (local->local/host still served)
        CutIop = 1u << 2,     // IOP frozen: no IOP cycles, sound RPCs answered by the gate (see IopGate)
        CutPresent = 1u << 3, // the presenter keeps showing the last live frame (no latch on vsync ticks)
        CutVu1 = 1u << 4,     // EXPERIMENT: VU1 microprograms (MSCAL) not run; VU1 data/regs then differ
    };

    // Host time per subsystem on the executor thread (rollback profiling; off unless a driver turns it on).
    struct HostProfile
    {
        bool on = false;
        bool hashVu1Data = true; // false: hashState leaves VU1 data memory out (CutVu1 experiments)
        uint64_t vu1Ns = 0, vif1Ns = 0, iopNs = 0, vu1Runs = 0;
    };
    extern HostProfile g_hostProfile;

    class ProfileScope
    {
    public:
        explicit ProfileScope(uint64_t &slot) noexcept : m_slot(slot), m_on(g_hostProfile.on)
        {
            if (m_on)
                m_start = std::chrono::steady_clock::now();
        }
        ~ProfileScope()
        {
            if (m_on)
                m_slot += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count());
        }
        ProfileScope(const ProfileScope &) = delete;
        ProfileScope &operator=(const ProfileScope &) = delete;

    private:
        uint64_t &m_slot;
        bool m_on;
        std::chrono::steady_clock::time_point m_start{};
    };

    extern std::atomic<uint32_t> g_activeCuts;

    inline uint32_t activeCuts() noexcept
    {
        return g_activeCuts.load(std::memory_order_relaxed);
    }

    inline bool cutActive(Cut cut) noexcept
    {
        return (g_activeCuts.load(std::memory_order_relaxed) & cut) != 0u;
    }

    inline void setActiveCuts(uint32_t cuts) noexcept
    {
        // work queued for the VU1 worker ran under the cuts it was queued with
        if (cuts != g_activeCuts.load(std::memory_order_relaxed))
            ps2_vu1_async::sync();
        g_activeCuts.store(cuts, std::memory_order_relaxed);
    }

    // Everything the IOP does to the EE goes through PS2IopHostAdapter (RDRAM writes, SIF commands) or comes back
    // from a synchronous RPC. A gate sees all of it on live frames (to log it), and answers in its place while
    // CutIop is active (the IOP does not run, so a re-simulated frame neither repeats sound commands nor advances
    // the sound clock; the logged replies are written back instead).
    class IopGate
    {
    public:
        virtual ~IopGate() = default;

        // Live (IOP running): effects on EE state, in order. data == nullptr: zero fill.
        virtual void guestWrite(uint32_t address, const uint8_t *data, size_t size) = 0;
        virtual void sifCommand(uint32_t commandId, const uint8_t *packet, size_t size) = 0;
        virtual void rpcBegin(const ps2x::iop::RpcRequest &request) = 0;
        virtual void rpcEnd(const ps2x::iop::RpcRequest &request, const ps2x::iop::RpcResult &result) = 0;

        // CutIop active: called instead of advancing the IOP (eeCycle = EE cycle after the advance) ...
        virtual void frozenAdvance(uint64_t eeCycle) = 0;
        // ... instead of an RPC into the IOP. forward() runs the call for real (IOP state changes).
        virtual ps2x::iop::RpcResult frozenRpc(const ps2x::iop::RpcRequest &request,
                                               const std::function<ps2x::iop::RpcResult()> &forward) = 0;
        // ... when the EE writes IOP memory (sceSifSetDma etc.): return true to let the write through.
        virtual bool frozenIopWrite(uint32_t address, size_t size) = 0;
    };

    void setIopGate(IopGate *gate) noexcept;
    IopGate *iopGate() noexcept;
}
