#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    // Rollback: a saved IOP (CPU, kernel, IOP RAM, SPU2, timers, SIF/RPC bookkeeping of the emulated IOP).
    // The HLE services (mcserv, dbcman, HLE libsd) and the module manager are boot-time state and not in it.
    class IopSnapshot
    {
    public:
        IopSnapshot();
        ~IopSnapshot();
        IopSnapshot(IopSnapshot &&) noexcept;
        IopSnapshot &operator=(IopSnapshot &&) noexcept;
        struct Data;
        std::unique_ptr<Data> data;
    };

    class IopSubsystem
    {
    public:
        explicit IopSubsystem(IopHost &host);
        ~IopSubsystem();

        IopSubsystem(const IopSubsystem &) = delete;
        IopSubsystem &operator=(const IopSubsystem &) = delete;
        IopSubsystem(IopSubsystem &&) noexcept;
        IopSubsystem &operator=(IopSubsystem &&) noexcept;

        void reset();

        [[nodiscard]] ModuleLoadResult loadModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
        [[nodiscard]] bool stopModule(int32_t moduleId, int32_t *result = nullptr);
        void runEeCycles(uint64_t eeCycles) noexcept;

        [[nodiscard]] RpcAbi selectRpcAbi(const RpcAbiRequest &request) const;
        [[nodiscard]] bool canBindRpc(uint32_t sid) const noexcept;
        [[nodiscard]] RpcResult handleRpc(const RpcRequest &request);
        void onSifTransfer(const SifTransfer &transfer);

        // Physical IOP RAM access shared by the emulator, SIF DMA, and HLE services. Addresses are IOP addresses.
        [[nodiscard]] uint32_t allocateMemory(uint32_t size, uint32_t alignment = 16u);
        [[nodiscard]] bool freeMemory(uint32_t address);
        [[nodiscard]] bool readMemory(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeMemory(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroMemory(uint32_t address, size_t size);
        [[nodiscard]] bool isMemoryRange(uint32_t address, size_t size) const;

        [[nodiscard]] DebugSnapshot debugSnapshot() const;

        // Rollback: only while no IOP code runs (between two runEeCycles/handleRpc calls).
        void saveState(IopSnapshot &out) const;
        void loadState(const IopSnapshot &in);
        [[nodiscard]] static uint64_t hashState(const IopSnapshot &state);
        // While set, the SPU2 mixes but hands nothing to the host audio output (re-simulated frames).
        static void setSoundOutputMuted(bool muted) noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
