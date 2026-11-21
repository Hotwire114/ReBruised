#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop::detail
{
    // Rollback (Tekken 4 netplay, docs/ROLLBACK_PHASE3.md of the port): the whole guest-visible IOP -- CPU and
    // kernel, IOP RAM, SPU2, timers, interrupts, SIF/RPC bookkeeping -- as a value that can be put back.
    struct IopEmulatorState;
    struct IopEmulatorStateDeleter
    {
        void operator()(IopEmulatorState *state) const noexcept;
    };
    using IopEmulatorStatePtr = std::unique_ptr<IopEmulatorState, IopEmulatorStateDeleter>;

    class IopEmulator
    {
    public:
        explicit IopEmulator(IopHost &host);
        ~IopEmulator();

        IopEmulator(const IopEmulator &) = delete;
        IopEmulator &operator=(const IopEmulator &) = delete;

        void reset();
        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result);
        void runEeCycles(uint64_t eeCycles) noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        [[nodiscard]] bool hasRpcServer(uint32_t sid) const noexcept;
        void onSifTransfer(const SifTransfer &transfer);

        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] uint64_t cycles() const noexcept;

        // Only between two runEeCycles/handleRpc calls (no guest code running).
        [[nodiscard]] static IopEmulatorStatePtr newState();
        void saveState(IopEmulatorState &out) const;
        void loadState(const IopEmulatorState &in);
        [[nodiscard]] static uint64_t hashState(const IopEmulatorState &state);
        // When set, runCycles skips kernel entry while no ready thread can appear before a sealed cycle.
        // Tests turn it off to lock the same wake cycle, and reseal to prove one invalidation.
        void setIdleSkipCache(bool enabled) noexcept;
        void testSealIdleSkip(uint64_t untilCycle) noexcept;
        [[nodiscard]] uint64_t testIdleSkipCount() const noexcept;
        // Tekken 4 port (t4iop): the idle fast path of runEeCycles (TEKKEN4_IOP_FAST_IDLE). Tests run it on and off.
        void setFastIdle(bool enabled) noexcept;
        [[nodiscard]] uint64_t testFastIdleCount() const noexcept;
        [[nodiscard]] uint64_t instructions() const noexcept;
        [[nodiscard]] uint32_t loadedModuleCount() const noexcept;
        [[nodiscard]] uint32_t threadCount() const noexcept;
        [[nodiscard]] uint32_t rpcServerCount() const noexcept;

    private:
        class Impl;
        friend struct IopEmulatorState;
        std::unique_ptr<Impl> m_impl;
    };
}
