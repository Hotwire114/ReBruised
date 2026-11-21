#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        explicit IopIoman(IopHost &host, IopMemory &memory) noexcept;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;
        };

        struct File
        {
            bool open = false;
            uint64_t position = 0;
            uint64_t base = 0;
            uint64_t size = 0;
        };

        [[nodiscard]] bool soundRoute() const;
        bool ensureImage();
        bool cdromOpen(IopCpuState &cpu);
        bool cdromClose(IopCpuState &cpu);
        bool cdromRead(IopCpuState &cpu);
        bool cdromSeek(IopCpuState &cpu);

        IopHost &m_host;
        IopMemory &m_memory;
        std::vector<Device> m_devices;
        std::vector<File> m_files;
        uint64_t m_image = 0;

    public:
        // The disc image handle (m_image) is a host resource: it stays as it is.
        struct SavedState
        {
            std::vector<Device> devices;
            std::vector<File> files;
        };
        void saveState(SavedState &out) const { out.devices = m_devices; out.files = m_files; }
        void loadState(const SavedState &in) { m_devices = in.devices; m_files = in.files; }
    };
}
