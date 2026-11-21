#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;
    class IopMemory;

    class IopCdvd
    {
    public:
        struct CompletionCallback
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            uint32_t reason = 0u;
        };

        IopCdvd(IopHost &host, IopMemory &memory, IopKernel &kernel);
        ~IopCdvd();

        IopCdvd(const IopCdvd &) = delete;
        IopCdvd &operator=(const IopCdvd &) = delete;

        void reset() noexcept;

        // Rollback: the drive's guest-visible state (host handles and the ISO index stay as they are).
        struct SavedState
        {
            uint32_t callbackAddress = 0u, callbackGp = 0u;
            std::optional<CompletionCallback> completionCallback;
            bool initialized = false;
            uint32_t mediaMode = 0u, currentLsn = 0u, lastError = 0u, streamFlag = 0u, lastReadTimeout = 0u;
            int interruptEventFlagId = 0;
        };
        void saveState(SavedState &out) const;
        void loadState(const SavedState &in);

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] std::optional<CompletionCallback> takeCompletionCallback() noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
