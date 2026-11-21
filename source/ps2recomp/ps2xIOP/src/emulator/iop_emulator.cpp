#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "core/sound_engine.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"
#include "ps2x/iop/iop_profile.h"

#include <chrono>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <type_traits>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

namespace ps2x::iop
{
    namespace
    {
        IopProfile g_iopProfile;
        // TSC <-> steady clock pair taken at the first profiled call; the report converts ticks with the ratio.
        uint64_t g_profTsc0 = 0;
        std::chrono::steady_clock::time_point g_profT0{};
        bool g_profPcs = false;
        std::vector<uint32_t> g_profPcHits; // per 64-byte block of IOP RAM

        bool readProfileOn() noexcept
        {
            const char *env = std::getenv("PS2X_IOP_PROF");
            const bool on = env && env[0] == '1';
            if (on)
            {
                g_profTsc0 = __rdtsc();
                g_profT0 = std::chrono::steady_clock::now();
                const char *pcs = std::getenv("PS2X_IOP_PROF_PCS");
                g_profPcs = pcs && pcs[0] == '1';
                if (g_profPcs)
                    g_profPcHits.assign(detail::IopMemory::RamSize / 64u, 0u);
            }
            return on;
        }

        double nsPerTick() noexcept
        {
            const uint64_t ticks = __rdtsc() - g_profTsc0;
            const double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_profT0).count());
            return ticks ? ns / static_cast<double>(ticks) : 0.0;
        }
    }

    bool iopProfileOn() noexcept
    {
        static const bool on = readProfileOn();
        return on;
    }

    IopProfile &iopProfile() noexcept
    {
        return g_iopProfile;
    }

    uint64_t iopProfileNs() noexcept
    {
        if (!iopProfileOn())
            return 0u;
        return static_cast<uint64_t>(static_cast<double>(g_iopProfile.runTicks + g_iopProfile.rpcTicks) * nsPerTick());
    }

    namespace detail
    {
        // Every 600 guest frames (614,400 IOP cycles each) of IOP time: the averages per frame since the last line.
        void iopProfileReport(uint64_t totalCycles, void (*logLine)(void *, const std::string &), void *self)
        {
            static IopProfile last{};
            static uint64_t lastCycles = 0;
            constexpr uint64_t kFrame = 614400u;
            if (totalCycles < lastCycles + 600u * kFrame)
                return;
            const IopProfile &p = g_iopProfile;
            const double frames = static_cast<double>(totalCycles - lastCycles) / static_cast<double>(kFrame);
            const double k = nsPerTick() / 1e6 / frames; // ms per frame
            char line[640];
            std::snprintf(line, sizeof(line),
                          "[iop-prof] t=%.0fs per frame: run %.3f ms (cpu %.3f, sound %.3f, sched/sif/dma/timer %.3f) imports "
                          "libsd %.3f/%.0f kernel %.3f/%.0f sifcmd %.3f/%.0f cdvd %.3f/%.0f sysclib %.3f/%.0f other %.3f/%.0f | "
                          "instr %.0f slices %.0f calls %.0f idle %.0f fast %.0f samples %.0f | rpc %.3f ms %.1f calls %.0f instr",
                          static_cast<double>(totalCycles) / 36864000.0, (p.runTicks - last.runTicks) * k,
                          (p.cpuTicks - last.cpuTicks) * k, (p.soundTicks - last.soundTicks) * k,
                          (p.sifTicks - last.sifTicks) * k, (p.importTicks[0] - last.importTicks[0]) * k,
                          (p.importCalls[0] - last.importCalls[0]) / frames, (p.importTicks[1] - last.importTicks[1]) * k,
                          (p.importCalls[1] - last.importCalls[1]) / frames, (p.importTicks[2] - last.importTicks[2]) * k,
                          (p.importCalls[2] - last.importCalls[2]) / frames, (p.importTicks[3] - last.importTicks[3]) * k,
                          (p.importCalls[3] - last.importCalls[3]) / frames, (p.importTicks[4] - last.importTicks[4]) * k,
                          (p.importCalls[4] - last.importCalls[4]) / frames, (p.importTicks[5] - last.importTicks[5]) * k,
                          (p.importCalls[5] - last.importCalls[5]) / frames,
                          (p.instructions - last.instructions) / frames, (p.slices - last.slices) / frames,
                          (p.calls - last.calls) / frames, (p.idleCalls - last.idleCalls) / frames, (p.fastCalls - last.fastCalls) / frames,
                          (p.samples - last.samples) / frames, (p.rpcTicks - last.rpcTicks) * k,
                          (p.rpcCalls - last.rpcCalls) / frames, (p.rpcInstructions - last.rpcInstructions) / frames);
            logLine(self, line);
            if (g_profPcs)
            {
                std::vector<std::pair<uint32_t, uint32_t>> hot;
                uint64_t sum = 0;
                for (uint32_t i = 0; i < g_profPcHits.size(); ++i)
                    if (g_profPcHits[i])
                    {
                        hot.emplace_back(g_profPcHits[i], i * 64u);
                        sum += g_profPcHits[i];
                    }
                std::sort(hot.begin(), hot.end(), std::greater<>());
                std::string top = "[iop-prof] hot 64-byte blocks (share of interpreted instructions):";
                for (size_t i = 0; i < hot.size() && i < 24u; ++i)
                {
                    char item[48];
                    std::snprintf(item, sizeof(item), " %05x:%.1f%%", hot[i].second, 100.0 * hot[i].first / sum);
                    top += item;
                }
                logLine(self, top);
                std::fill(g_profPcHits.begin(), g_profPcHits.end(), 0u);
            }
            last = p;
            lastCycles = totalCycles;
        }

        inline void iopProfilePc(uint32_t physicalPc) noexcept
        {
            if (g_profPcs && physicalPc < IopMemory::RamSize)
                ++g_profPcHits[physicalPc / 64u];
        }
    }
}

namespace ps2x::iop::detail
{
    // queueEeSifCommand's queue (EE side) -> serviceEeSifCommands (IOP side). One IOP per process.
    struct EeSifCommand
    {
        uint32_t cid = 0u;
        uint32_t size = 0u;
        std::array<uint8_t, 112> packet{};
    };
    std::mutex &eeSifCommandMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
    std::deque<EeSifCommand> &eeSifCommandQueue()
    {
        static std::deque<EeSifCommand> queue;
        return queue;
    }
    std::atomic<bool> &eeSifCommandPending()
    {
        static std::atomic<bool> pending{false};
        return pending;
    }

    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

        // ELF image sitting in IOP RAM (the bytes sceSifSetDma copied). IopHost::readGuest is EE RAM
        // on the runtime adapter and the test host's own buffer in unit tests, so the loader tries this first.
        bool readElfImageFromIopRam(IopMemory &memory, uint32_t address, std::vector<uint8_t> &bytes)
        {
            uint8_t hdr[52]{};
            if (!memory.readRam(address, hdr, sizeof(hdr)))
                return false;
            if (hdr[0] != 0x7Fu || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F' || hdr[4] != 1u || hdr[5] != 1u)
                return false;
            const auto ru16 = [](const uint8_t *p) -> uint16_t {
                return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8u));
            };
            const auto ru32 = [](const uint8_t *p) -> uint32_t {
                return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8u) |
                       (static_cast<uint32_t>(p[2]) << 16u) | (static_cast<uint32_t>(p[3]) << 24u);
            };
            if (ru16(hdr + 18) != 8u || ru16(hdr + 40) < 52u)
                return false;
            const uint32_t phoff = ru32(hdr + 28);
            const uint32_t shoff = ru32(hdr + 32);
            const uint16_t phentsize = ru16(hdr + 42);
            const uint16_t phnum = ru16(hdr + 44);
            const uint16_t shentsize = ru16(hdr + 46);
            const uint16_t shnum = ru16(hdr + 48);
            constexpr uint64_t kMaxImageSize = 2ull * 1024ull * 1024ull;
            uint64_t required = sizeof(hdr);
            required = std::max<uint64_t>(required, static_cast<uint64_t>(phoff) + static_cast<uint64_t>(phentsize) * phnum);
            required = std::max<uint64_t>(required, static_cast<uint64_t>(shoff) + static_cast<uint64_t>(shentsize) * shnum);
            if (required > kMaxImageSize)
                return false;
            bytes.resize(static_cast<size_t>(required));
            if (!memory.readRam(address, bytes.data(), bytes.size()))
                return false;
            constexpr uint32_t kShtNobits = 8u;
            if (shnum != 0u && shentsize >= 40u)
            {
                for (uint16_t i = 0; i < shnum; ++i)
                {
                    const size_t off = static_cast<size_t>(shoff) + static_cast<size_t>(i) * shentsize;
                    if (off + 40u > bytes.size())
                        return false;
                    const uint8_t *section = bytes.data() + off;
                    if (ru32(section + 4) != kShtNobits)
                        required = std::max<uint64_t>(required, static_cast<uint64_t>(ru32(section + 16)) + ru32(section + 20));
                }
            }
            if (phnum != 0u && phentsize >= 32u)
            {
                for (uint16_t i = 0; i < phnum; ++i)
                {
                    const size_t off = static_cast<size_t>(phoff) + static_cast<size_t>(i) * phentsize;
                    if (off + 32u > bytes.size())
                        return false;
                    const uint8_t *program = bytes.data() + off;
                    required = std::max<uint64_t>(required, static_cast<uint64_t>(ru32(program + 4)) + ru32(program + 16));
                }
            }
            if (required > kMaxImageSize)
                return false;
            bytes.resize(static_cast<size_t>(required));
            return memory.readRam(address, bytes.data(), bytes.size());
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(hostRef, memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            m_libsd = {};
            m_sound.reset();
            m_soundRoute = -1;
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            sifCmdHandlers.clear();
            sifCmdSlotBase = 0u;
            sifCmdNextSlot = 0u;
            eeSifCommandsDelivered = 0u;
            {
                std::lock_guard<std::mutex> lock(eeSifCommandMutex());
                eeSifCommandQueue().clear();
                eeSifCommandPending().store(false, std::memory_order_release);
            }
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            m_idleValid = false;
            m_idleUntil = 0;
            m_idleEpoch = 0;
            m_idleSkipCount = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        void saveStateTo(IopEmulatorState &out) const;
        void loadStateFrom(const IopEmulatorState &in);

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
            {
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                // A DMA completion can run a guest handler that readies a thread. Clearing here,
                // not inside that handler, is what stops a sealed idle skip from jumping over the IRQ.
                m_idleValid = false;
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        [[nodiscard]] bool soundRouteOn()
        {
            if (m_soundRoute < 0)
            {
                m_soundRoute = host.hostPath(HostPathKind::CdImage).empty() ? 0 : 1;
                if (m_soundRoute == 1)
                    log(LogLevel::Info, "[libsd] sound route on");
            }
            return m_soundRoute == 1;
        }

        [[nodiscard]] uint32_t stackArg(const CpuState &cpu, uint32_t slot) const
        {
            const uint32_t sp = cpu.gpr[29];
            if (sp < 16u)
                return 0u;
            return memory.read32(sp + 16u + slot * 4u);
        }

        void libsdInit()
        {
            m_libsd = {};
            for (uint32_t core = 0; core < 2u; ++core)
            {
                m_sound.initCore(core);
                m_sound.setSwitch((0x16u << 8) | core, 0xFFFFFFu); // KOFF
                for (uint32_t id : {0x18u, 0x19u, 0x1Au, 0x1Bu})
                    m_sound.setSwitch((id << 8) | core, 0xFFFFFFu); // VMIX*
                m_sound.setParam((0x08u << 8) | core, core == 0u ? 0x0FF0u : 0x0FFCu);
                for (uint32_t id = 0x09u; id <= 0x10u; ++id)
                {
                    // Both masters start at unity. step() multiplies core 0 by core 1's master, so an
                    // untouched core would mute the mix. The driver overwrites these when it wants to.
                    const bool master = id == 0x09u || id == 0x0Au;
                    uint16_t value = master ? 0x3FFFu : 0u;
                    if (core == 1u && (id == 0x0Du || id == 0x0Eu))
                        value = 0x7FFFu;
                    m_sound.setParam((id << 8) | 0x80u | core, value);
                }
                for (uint32_t voice = 0; voice < SoundEngine::VoicesPerCore; ++voice)
                {
                    const uint32_t packed = (voice << 1) | core;
                    m_sound.setParam((0x02u << 8) | packed, 0x3FFFu);
                    m_sound.setAddr((0x20u << 8) | packed, 0u);
                }
            }
            const uint8_t silentLoop[16] = {0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
                                            0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07};
            m_sound.writeSampleRam(0x5000u, silentLoop, sizeof(silentLoop));
        }

        bool dispatchLibsd(uint16_t ordinal, CpuState &cpu)
        {
            if (!soundRouteOn())
                return false;
            const auto ret = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };
            const uint32_t a0 = cpu.gpr[4];
            const uint32_t a1 = cpu.gpr[5];
            const uint32_t a2 = cpu.gpr[6];
            const uint32_t a3 = cpu.gpr[7];
            if (ordinal < m_libsdCalls.size())
                ++m_libsdCalls[ordinal];
            if (ordinal == 7u && ((a0 >> 8) & 0xFFu) == 0x15u)
                ++m_konCalls;
            if (ordinal == 7u && ((a0 >> 8) & 0xFFu) == 0x16u)
                ++m_koffCalls;
            switch (ordinal)
            {
            case 4:
                libsdInit();
                log(LogLevel::Info, "[libsd] sceSdInit");
                ret(0u);
                return true;
            case 5:
                m_sound.setParam(a0, static_cast<uint16_t>(a1));
                ret(0u);
                return true;
            case 6:
                ret(m_sound.getParam(a0));
                return true;
            case 7:
                if (((a0 >> 8) & 0xFFu) == 0x15u && a1 != 0u)
                {
                    static int keys = 0;
                    if (keys < 8)
                    {
                        ++keys;
                        std::ostringstream out;
                        out << "[libsd] KON entry=0x" << std::hex << a0 << " mask=0x" << a1;
                        log(LogLevel::Info, out.str());
                    }
                }
                m_sound.setSwitch(a0, a1);
                ret(0u);
                return true;
            case 8:
                ret(m_sound.getSwitch(a0));
                return true;
            case 9:
                m_sound.setAddr(a0, a1);
                ret(0u);
                return true;
            case 10:
                ret(m_sound.getAddr(a0));
                return true;
            case 11:
                m_sound.setCoreAttr(a0, a1);
                ret(0u);
                return true;
            case 12:
                ret(m_sound.getCoreAttr(a0));
                return true;
            case 17:
            {
                const uint32_t channel = a0 & 1u;
                const uint32_t mode = a1;
                uint32_t size = stackArg(cpu, 0);
                if (size == 0u || size > 0x200000u)
                {
                    const uint32_t viaT0 = cpu.gpr[8];
                    if (viaT0 != 0u && viaT0 <= 0x200000u)
                        size = viaT0;
                    else
                        size = 0u;
                }
                std::vector<uint8_t> data(size);
                if ((mode & 3u) == 0u)
                {
                    if (size != 0u && !memory.readRam(a2, data.data(), size))
                        data.assign(size, 0u);
                    if (size != 0u)
                        m_sound.writeSampleRam(a3, data.data(), size);
                }
                else if (size != 0u)
                {
                    m_sound.readSampleRam(a3, data.data(), size);
                    (void)memory.writeRam(a2, data.data(), size);
                }
                if ((mode & 8u) == 0u && m_libsd.transHandler[channel] != 0u)
                {
                    (void)callFunction(m_libsd.transHandler[channel], channel, m_libsd.transArg[channel], 0u, 0u,
                                       m_libsd.transGp[channel], 100000u);
                }
                static int transfers = 0;
                if (transfers < 12)
                {
                    ++transfers;
                    std::ostringstream out;
                    out << "[libsd] VoiceTrans ch=" << channel << " mode=0x" << std::hex << mode
                        << " iop=0x" << a2 << " spu=0x" << a3 << " size=" << std::dec << size;
                    log(LogLevel::Info, out.str());
                }
                ret((size + 63u) & ~63u);
                return true;
            }
            case 18:
            {
                const uint32_t channel = a0 & 1u;
                const uint32_t mode = a1;
                const uint32_t iopBuffer = a2;
                const uint32_t size = a3;
                if ((mode & 3u) == 2u)
                {
                    m_sound.stopStream(channel);
                    m_libsd.blockActive[channel] = false;
                    ret(0u);
                    return true;
                }
                const uint32_t physical = IopMemory::physicalAddress(iopBuffer);
                if (size == 0u || physical >= IopMemory::RamSize || size > IopMemory::RamSize - physical)
                {
                    ret(static_cast<uint32_t>(-100));
                    return true;
                }
                m_sound.startStream(channel, memory.ram().data() + physical, size, (mode & 0x10u) != 0u);
                m_libsd.blockActive[channel] = true;
                m_libsd.blockBuffer[channel] = iopBuffer;
                m_libsd.blockSize[channel] = size;
                std::ostringstream out;
                out << "[libsd] BlockTrans ch=" << channel << " mode=0x" << std::hex << mode
                    << " buf=0x" << iopBuffer << " size=" << std::dec << size;
                log(LogLevel::Info, out.str());
                ret(0u);
                return true;
            }
            case 19:
                ret(1u);
                return true;
            case 20:
            {
                const uint32_t channel = a0 & 1u;
                if (!m_libsd.blockActive[channel])
                {
                    ret(0u);
                    return true;
                }
                const uint32_t position = m_sound.streamPosition(channel);
                const uint32_t side = m_libsd.blockSize[channel] != 0u && position >= m_libsd.blockSize[channel] / 2u ? 1u : 0u;
                ret((side << 24) | ((m_libsd.blockBuffer[channel] + position) & 0xFFFFFFu));
                return true;
            }
            case 23:
                if (a1 != 0u)
                {
                    // sceSdEffectAttr: int core, int mode, short depth_L, short depth_R, int delay, int feedback.
                    const uint32_t core = a0 & 1u;
                    const int32_t mode = static_cast<int32_t>(memory.read32(a1 + 4u));
                    const int16_t depthL = static_cast<int16_t>(memory.read16(a1 + 8u));
                    const int16_t depthR = static_cast<int16_t>(memory.read16(a1 + 10u));
                    const int32_t delay = static_cast<int32_t>(memory.read32(a1 + 12u));
                    const int32_t feedback = static_cast<int32_t>(memory.read32(a1 + 16u));
                    m_sound.setEffectAttr(core, mode, depthL, depthR, delay, feedback);
                }
                ret(0u);
                return true;
            case 25:
            {
                static constexpr uint32_t kWorkHalfwords[10] = {0x02u, 0x4D8u, 0x3E8u, 0x908u, 0xDFCu, 0x15BCu, 0x1ED8u, 0x3008u, 0x3008u, 0x780u};
                const uint32_t core = a0 & 1u;
                const uint32_t mode = a2 < 10u ? a2 : 0u;
                const uint32_t end = 0x1FFFFFu;
                const uint32_t start = end - kWorkHalfwords[mode] * 8u + 1u;
                m_sound.setAddr((0x1Du << 8) | core, end);
                m_sound.setAddr((0x1Cu << 8) | core, start);
                m_sound.clearSampleRam(start, 64u);
                ret(0u);
                return true;
            }
            case 21:
            case 26:
            {
                const uint32_t channel = a0 & 1u;
                m_libsd.transHandler[channel] = a1;
                m_libsd.transArg[channel] = a2;
                m_libsd.transGp[channel] = cpu.gpr[28];
                ret(0u);
                return true;
            }
            case 22:
            case 27:
            {
                // [0] = sceSdSetSpu2IntrHandler(handler, arg): handler(core bits, arg).
                // [1] = sceSdSetIRQCallback(callback): callback(0), used when no handler is set (libsd).
                const uint32_t slot = ordinal == 27u ? 0u : 1u;
                m_libsd.spu2Handler[slot] = a0;
                m_libsd.spu2Arg[slot] = ordinal == 27u ? a1 : 0u;
                m_libsd.spu2Gp[slot] = cpu.gpr[28];
                std::ostringstream out;
                out << "[libsd] " << (ordinal == 27u ? "SetSpu2IntrHandler" : "SetIRQCallback") << " 0x" << std::hex << a0
                    << " arg=0x" << a1;
                log(LogLevel::Info, out.str());
                ret(0u);
                return true;
            }
            default:
            {
                static uint32_t seen = 0u;
                if (ordinal < 32u && (seen & (1u << ordinal)) == 0u)
                {
                    seen |= 1u << ordinal;
                    std::ostringstream out;
                    out << "[libsd] ordinal " << ordinal << " stubbed";
                    log(LogLevel::Info, out.str());
                }
                ret(0u);
                return true;
            }
            }
        }

        // SPU2 interrupt: libsd's handler clears the IRQ enable of the cores that fired, then calls the game's
        // handler with their bits (ps2sdk libsd Spu2Interrupt). zsiopdrv's BGM stream refills its SPU ring from it.
        void serviceSpu2Irq()
        {
            // TEKKEN4_BGM=0: no SPU2 interrupts (the state before the music fix, for A/B runs).
            static const bool off = []
            {
                const char *env = std::getenv("TEKKEN4_BGM");
                return env && env[0] == '0' && env[1] == '\0';
            }();
            const uint32_t bits = m_sound.takeIrq() & 3u;
            if (bits == 0u || off)
                return;
            for (uint32_t core = 0; core < 2u; ++core)
                if ((bits & (1u << core)) != 0u)
                    m_sound.setCoreAttr(4u | core, 0u);
            const uint32_t slot = m_libsd.spu2Handler[0] != 0u ? 0u : 1u;
            const uint32_t handler = m_libsd.spu2Handler[slot];
            ++m_spu2IrqCount;
            if (handler == 0u)
                return;
            if (m_spu2IrqCount <= 4u)
            {
                std::ostringstream out;
                out << "[libsd] SPU2 IRQ cores=" << bits << " -> 0x" << std::hex << handler;
                log(LogLevel::Info, out.str());
            }
            (void)callFunction(handler, slot == 0u ? bits : 0u, m_libsd.spu2Arg[slot], 0u, 0u, m_libsd.spu2Gp[slot],
                               100000u);
            m_idleValid = false;
        }

        // TEKKEN4_IOP_DUMP=<s1>[,<s2>...]: at those IOP seconds, write iop_ram_<s>s.bin (2 MB IOP RAM) and
        // spu_ram_<s>s.bin (2 MB sample RAM) to the working directory (debug aid for the BGM stream).
        void debugDumpRam()
        {
            // TEKKEN4_SOUND_VOICES=1 also logs, every 5 IOP seconds, the libsd calls (by ordinal), KON/KOFF writes,
            // SPU2 IRQs and EE commands delivered since the last line.
            static const bool stats = std::getenv("TEKKEN4_SOUND_VOICES") != nullptr;
            if (stats && totalCycles >= m_nextSoundStats)
            {
                m_nextSoundStats = totalCycles + 5ull * 36864000ull;
                std::ostringstream out;
                out << "[snd-stats] iop " << totalCycles / 36864000ull << " s: irq " << m_spu2IrqCount << " eecmd "
                    << eeSifCommandsDelivered << " kon " << m_konCalls << " koff " << m_koffCalls << " libsd";
                for (size_t i = 0; i < m_libsdCalls.size(); ++i)
                    if (m_libsdCalls[i] != 0u)
                        out << ' ' << i << ':' << m_libsdCalls[i];
                log(LogLevel::Info, out.str());
                m_libsdCalls.fill(0u);
                m_konCalls = m_koffCalls = 0u;
            }
            static std::vector<uint64_t> times = []
            {
                std::vector<uint64_t> list;
                if (const char *env = std::getenv("TEKKEN4_IOP_DUMP"))
                {
                    std::stringstream in(env);
                    std::string item;
                    while (std::getline(in, item, ','))
                        if (!item.empty())
                            list.push_back(std::strtoull(item.c_str(), nullptr, 10));
                }
                std::sort(list.begin(), list.end(), std::greater<uint64_t>());
                return list;
            }();
            if (times.empty() || totalCycles < times.back() * 36864000ull)
                return;
            const uint64_t at = times.back();
            (void)0;
            times.pop_back();
            const std::string suffix = std::to_string(at) + "s.bin";
            if (std::FILE *f = std::fopen(("iop_ram_" + suffix).c_str(), "wb"))
            {
                std::fwrite(memory.ram().data(), 1, kRamSize, f);
                std::fclose(f);
            }
            std::vector<uint8_t> spu(SoundEngine::RamHalfwords * 2u);
            m_sound.readSampleRam(0u, spu.data(), spu.size());
            if (std::FILE *f = std::fopen(("spu_ram_" + suffix).c_str(), "wb"))
            {
                std::fwrite(spu.data(), 1, spu.size(), f);
                std::fclose(f);
            }
            log(LogLevel::Info, "[IOP] RAM dump " + suffix);
        }

        void drainMixedAudio()
        {
            if (!soundRouteOn())
                return;
            const uint64_t mixedBefore = m_sound.mixedUpTo();
            m_sound.advanceTo(totalCycles);
            serviceSpu2Irq();
            // Tekken 4 port (perf): called after every IOP advance (the EE's checkpoints, ~100k a fight frame); only a
            // step of the mixer adds output samples, so without one there is nothing to drain (drainOutput locked and
            // returned 0: ~8% of the game thread).
            if (m_sound.mixedUpTo() == mixedBefore)
                return;
            debugDumpRam();
            int16_t block[1024 * 2];
            for (;;)
            {
                const size_t frames = m_sound.drainOutput(block, 1024u);
                if (frames == 0u)
                    break;
                if (const MixedAudioFn sink = mixedAudioFnSlot().load(std::memory_order_acquire))
                    sink(block, frames);
            }
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                    m_idleValid = false;
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                // Sound route only. Default IOP GetSreg stays the HLE that returns 0.
                // zsiopdrv's 0x8000 handler spins at sceSifGetSreg(31) until the EE's SET_SREG lands.
                if (soundRouteOn() && (call.ordinal == 6u || call.ordinal == 7u))
                {
                    const uint32_t index = cpu.gpr[4];
                    const uint32_t previous = sifSoftwareRegister(index);
                    if (call.ordinal == 7u)
                        setSifSoftwareRegister(index, cpu.gpr[5]);
                    if (call.ordinal == 6u && previous != 0u)
                    {
                        static int logged = 0;
                        if (logged < 4)
                        {
                            ++logged;
                            std::ostringstream out;
                            out << "[sifcmd] GetSreg " << index << " = 0x" << std::hex << previous;
                            log(LogLevel::Info, out.str());
                        }
                    }
                    setV0(previous);
                    return ImportDisposition::Handled;
                }
                // sceSifAddCmdHandler(cid, handler, data) / sceSifRemoveCmdHandler(cid): kept per cid so EE
                // sceSifSendCmd packets reach the module (zsiopdrv's cid 0 = the EE's answers to its data requests).
                if (soundRouteOn() && (call.ordinal == 10u || call.ordinal == 11u))
                {
                    const uint32_t cid = cpu.gpr[4];
                    if (call.ordinal == 10u)
                    {
                        sifCmdHandlers[cid] = SifCmdHandler{cpu.gpr[5], cpu.gpr[6], cpu.gpr[28]};
                        std::ostringstream out;
                        out << "[sifcmd] AddCmdHandler cid=0x" << std::hex << cid << " handler=0x" << cpu.gpr[5]
                            << " data=0x" << cpu.gpr[6];
                        log(LogLevel::Info, out.str());
                    }
                    else
                        sifCmdHandlers.erase(cid);
                    setV0(0);
                    return ImportDisposition::Handled;
                }
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "libsd") && dispatchLibsd(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
            {
                // Any timrman import can arm, stop, or move compare/overflow. GetSystemTime is thbase.
                m_idleValid = false;
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        // TEKKEN4_IOP_TRACE=<pc>[,<pc>...] (hex): logs a0-a3, ra and the cycle each time the IOP reaches one of those
        // PCs, at most TEKKEN4_IOP_TRACE_MAX (default 200) lines (debug aid for the sound driver).
        static std::vector<uint32_t> &pcTrace()
        {
            static std::vector<uint32_t> list = []
            {
                std::vector<uint32_t> pcs;
                if (const char *env = std::getenv("TEKKEN4_IOP_TRACE"))
                {
                    std::stringstream in(env);
                    std::string item;
                    while (std::getline(in, item, ','))
                        if (!item.empty())
                            pcs.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
                }
                std::sort(pcs.begin(), pcs.end());
                return pcs;
            }();
            return list;
        }

        void tracePc(const CpuState &cpu)
        {
            static int left = []
            {
                const char *env = std::getenv("TEKKEN4_IOP_TRACE_MAX");
                return env ? std::atoi(env) : 200;
            }();
            if (left <= 0 || !std::binary_search(pcTrace().begin(), pcTrace().end(), cpu.pc))
                return;
            --left;
            char line[200];
            std::snprintf(line, sizeof(line), "[iop-trace] pc=%x a0=%x a1=%x a2=%x a3=%x ra=%x t=%.3f", cpu.pc, cpu.gpr[4],
                          cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], cpu.gpr[31], static_cast<double>(totalCycles) / 36864000.0);
            log(LogLevel::Info, line);
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;
            if (!pcTrace().empty())
                tracePc(cpu);

            if (m_profiling)
                iopProfilePc(physicalAddress(cpu.pc));
            if (const auto import = imports.decode(cpu.pc))
            {
                uint64_t s0 = 0;
                if (m_profiling)
                    s0 = __rdtsc();
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                if (m_recompAuditEnabled)
                {
                    if (disposition == ImportDisposition::Handled)
                        ++m_audit.hle;
                    else if (disposition == ImportDisposition::JumpToGuest)
                        ++m_audit.guestImportJumps;
                    else
                        ++m_audit.missingImports;
                }
                if (m_profiling)
                {
                    const std::string_view lib = import->library;
                    const int slot = iequals(lib, "libsd")                                            ? 0
                                     : iequals(lib, "thbase") || iequals(lib, "threadman") || iequals(lib, "thsemap") ||
                                             iequals(lib, "thevent")
                                         ? 1
                                     : iequals(lib, "sifcmd")  ? 2
                                     : iequals(lib, "cdvdman") ? 3
                                     : iequals(lib, "sysclib") ? 4
                                                               : 5;
                    iopProfile().importTicks[slot] += __rdtsc() - s0;
                    ++iopProfile().importCalls[slot];
                }
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            if (m_recompAuditEnabled)
                ++m_audit.interpreted; // Exactly one instruction fetch/decode/execute; model charges one IOP cycle.
            const bool running = cpuCore.executeInstruction(cpu);
            // No IOP BIOS is mapped, so the vector at 0x80000080 is low RAM and a taken
            // exception spins until the RPC budget dies (sound-run-6: pc=0x80000a00).
            // On the sound route, resume past the faulting instruction and keep the driver going.
            if (cpu.exception && soundRouteOn())
            {
                const uint32_t epc = cpu.cop0[14];
                const uint32_t cause = cpu.cop0[13];
                const uint32_t code = (cause >> 2u) & 0x1Fu;
                static uint32_t lastEpc = 0xFFFFFFFFu;
                static int same = 0;
                static int logged = 0;
                if (epc == lastEpc)
                    ++same;
                else
                {
                    same = 0;
                    lastEpc = epc;
                }
                if (logged < 12 || same == 64)
                {
                    ++logged;
                    std::ostringstream out;
                    out << "[IOP] exception epc=0x" << std::hex << epc << " cause=0x" << cause
                        << " code=" << std::dec << code << " bad=0x" << std::hex << cpu.cop0[8]
                        << " insn=0x" << memory.read32(epc) << " same=" << std::dec << same;
                    log(LogLevel::Warning, out.str());
                }
                if (same >= 64)
                {
                    cpu.stopped = true;
                    return false;
                }
                const uint32_t status = cpu.cop0[12];
                cpu.cop0[12] = (status & ~0x3Fu) | ((status >> 2u) & 0xFu);
                const bool delaySlot = (cause & 0x80000000u) != 0u;
                cpu.pc = epc + (delaySlot ? 8u : 4u);
                cpu.branchPending = false;
                cpu.exception = false;
                schedulePendingDma();
                ++totalInstructions;
                ++totalCycles;
                return !cpu.stopped;
            }
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            const uint32_t ran = runCpu(cpu, budget);
            if (soundRouteOn() && ran >= budget && cpu.pc != kCallReturnSentinel && !cpu.stopped)
            {
                static int aborts = 0;
                if (aborts < 8)
                {
                    ++aborts;
                    std::ostringstream out;
                    out << "[IOP] call budget hit entry=0x" << std::hex << address << " pc=0x" << cpu.pc
                        << " ran=" << std::dec << ran;
                    log(LogLevel::Warning, out.str());
                }
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // EE sceSifSendCmd packets (queueEeSifCommand): header words as the SIF hardware delivers them
        // (size | extra size << 8, extra destination, cid), then handler(packet, data) like sifcmd's interrupt.
        void serviceEeSifCommands()
        {
            if (!eeSifCommandPending().load(std::memory_order_acquire) || servicingGuestCallbacks)
                return;
            std::deque<EeSifCommand> commands;
            {
                std::lock_guard<std::mutex> lock(eeSifCommandMutex());
                commands.swap(eeSifCommandQueue());
                eeSifCommandPending().store(false, std::memory_order_release);
            }
            m_idleValid = false;
            if (!soundRouteOn())
                return;
            if (sifCmdSlotBase == 0u)
                sifCmdSlotBase = allocate(kSifCmdSlots * kSifCmdSlotSize, 64u);
            servicingGuestCallbacks = true;
            try
            {
                for (const EeSifCommand &command : commands)
                {
                    const auto it = sifCmdHandlers.find(command.cid);
                    if (it == sifCmdHandlers.end() || it->second.function == 0u || sifCmdSlotBase == 0u)
                    {
                        static int dropped = 0;
                        if ((command.cid & 0x80000000u) == 0u && dropped < 8)
                        {
                            ++dropped;
                            std::ostringstream out;
                            out << "[sifcmd] EE cmd cid=0x" << std::hex << command.cid << " dropped (no handler)";
                            log(LogLevel::Info, out.str());
                        }
                        continue;
                    }
                    const uint32_t slot = sifCmdSlotBase + (sifCmdNextSlot++ % kSifCmdSlots) * kSifCmdSlotSize;
                    (void)memory.writeRam(slot, command.packet.data(), command.size);
                    (void)callFunction(it->second.function, slot, it->second.data, 0u, 0u, it->second.gp, 100000u);
                    if (++eeSifCommandsDelivered <= 4u)
                    {
                        uint32_t word0 = 0u, word1 = 0u;
                        std::memcpy(&word0, command.packet.data(), 4);
                        std::memcpy(&word1, command.packet.data() + 4, 4);
                        std::ostringstream out;
                        out << "[sifcmd] EE cmd cid=0x" << std::hex << command.cid << " -> handler 0x"
                            << it->second.function << " (size word 0x" << word0 << ", dest 0x" << word1 << ")";
                        log(LogLevel::Info, out.str());
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        [[nodiscard]] uint64_t instructionCount() const noexcept { return totalInstructions; }

        static void profileLog(void *self, const std::string &line)
        {
            static_cast<Impl *>(self)->log(LogLevel::Info, line);
        }

        // Tekken 4 port (t4iop): the EE advances the IOP at every checkpoint (~50,000 times a fight frame) and almost
        // every call lands inside a sealed idle window with no SPU2 sample due. Then runCycles would only move the clock:
        // serviceEeSifCommands finds nothing pending, the idle-skip branch jumps to the target and breaks, and
        // drainMixedAudio's advanceTo mixes nothing, takeIrq returns 0 and nothing is drained. This does exactly that
        // without entering the loop. Anything else (a SIF command queued, the window ending inside this step, a sample
        // due, an IRQ bit set, the sound route not decided yet) takes the full path. TEKKEN4_IOP_FAST_IDLE=0 = always
        // the full path.
        [[nodiscard]] bool fastIdleAdvance(uint64_t cycles) noexcept
        {
            if (!m_fastIdleEnabled || !m_idleCacheEnabled || !m_idleValid || kernel.schedEpoch() != m_idleEpoch)
                return false;
            const uint64_t target = totalCycles + cycles;
            if (totalCycles >= m_idleUntil || target > m_idleUntil)
                return false;
            if (m_soundRoute < 0 || eeSifCommandPending().load(std::memory_order_acquire))
                return false;
            if (m_soundRoute == 1 && (m_sound.irqPendingBits() != 0u || !m_sound.advanceIdle(target)))
                return false;
            ++m_idleSkipCount;
            ++m_fastIdleCount;
            if (m_recompAuditEnabled)
            {
                m_audit.idleSkippedCycles += cycles;
                ++m_audit.fastIdleCalls;
            }
            totalCycles = target;
            if (m_recompAuditEnabled)
                reportRecompAudit();
            return true;
        }

        // Audit counters measure host work and intentionally stay outside saved IOP state.
        // Poll the wall clock at most once per guest frame, then report about once per
        // wall second. Interpreter/HLE counters need neither atomics nor TSC reads.
        void reportRecompAudit()
        {
            if (!m_recompAuditEnabled)
                return;
            if (totalCycles >= m_auditLastPollCycle && totalCycles - m_auditLastPollCycle < 614400u)
                return;
            m_auditLastPollCycle = totalCycles;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - m_auditLastReport).count();
            if (seconds < 1.0)
                return;
            const auto delta = [this](uint64_t AuditCounters::*field) {
                return m_audit.*field - m_auditLast.*field;
            };
            const uint64_t interpreted = delta(&AuditCounters::interpreted);
            char line[800];
            std::snprintf(line, sizeof(line),
                "[recomp-audit] iop wall_s=%.3f interval_s=%.6f interpreted_calls=%llu interpreted_per_s=%.3f "
                "interpreted_model_cycles=%llu hle_calls=%llu hle_per_s=%.3f guest_import_jumps=%llu missing_imports=%llu "
                "idle_skipped_cycles=%llu fast_idle_calls=%llu full_idle_skips=%llu native_recompiled_calls=0 jit_calls=0",
                std::chrono::duration<double>(now - m_auditStarted).count(), seconds,
                static_cast<unsigned long long>(interpreted), interpreted / seconds,
                static_cast<unsigned long long>(interpreted),
                static_cast<unsigned long long>(delta(&AuditCounters::hle)), delta(&AuditCounters::hle) / seconds,
                static_cast<unsigned long long>(delta(&AuditCounters::guestImportJumps)),
                static_cast<unsigned long long>(delta(&AuditCounters::missingImports)),
                static_cast<unsigned long long>(delta(&AuditCounters::idleSkippedCycles)),
                static_cast<unsigned long long>(delta(&AuditCounters::fastIdleCalls)),
                static_cast<unsigned long long>(delta(&AuditCounters::fullIdleSkips)));
            log(LogLevel::Info, line);
            m_auditLast = m_audit;
            m_auditLastReport = now;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            if (iopProfileOn())
            {
                IopProfile &p = iopProfile();
                const uint64_t t0 = __rdtsc();
                const uint64_t instr0 = totalInstructions, cyc0 = totalCycles, mixed0 = m_sound.mixedUpTo();
                const uint64_t cpu0 = p.cpuTicks, sound0 = p.soundTicks;
                m_profiling = true;
                runCyclesImpl(cycles);
                m_profiling = false;
                const uint64_t t1 = __rdtsc();
                ++p.calls;
                if (totalInstructions == instr0)
                    ++p.idleCalls;
                p.runTicks += t1 - t0;
                p.sifTicks += (t1 - t0) - (p.cpuTicks - cpu0) - (p.soundTicks - sound0);
                p.instructions += totalInstructions - instr0;
                p.iopCycles += totalCycles - cyc0;
                p.samples += (m_sound.mixedUpTo() - mixed0) / SoundEngine::CyclesPerSample;
                iopProfileReport(totalCycles, &Impl::profileLog, this);
                if (m_recompAuditEnabled)
                    reportRecompAudit();
                return;
            }
            runCyclesImpl(cycles);
            if (m_recompAuditEnabled)
                reportRecompAudit();
        }

        void runCyclesImpl(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                serviceEeSifCommands();
                while (totalCycles < target)
                {
                    // Sealed while ready and delay queues cannot grow a runnable thread.
                    // Land on m_idleUntil and fall through so an event exactly at that cycle
                    // is serviced, matching the old jump-then-continue loop.
                    if (m_idleCacheEnabled && m_idleValid && kernel.schedEpoch() == m_idleEpoch &&
                        totalCycles < m_idleUntil)
                    {
                        const uint64_t stop = std::min(target, m_idleUntil);
                        if (stop > totalCycles)
                        {
                            ++m_idleSkipCount;
                            if (m_recompAuditEnabled)
                            {
                                m_audit.idleSkippedCycles += stop - totalCycles;
                                ++m_audit.fullIdleSkips;
                            }
                        }
                        totalCycles = stop;
                        if (totalCycles >= target)
                            break;
                    }

                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        // Horizon past this call's target, so the next small runEeCycles can skip
                        // the kernel entirely. Do not pass target into nextWakeCycle.
                        uint64_t horizon = kernel.nextWakeCycle(~uint64_t{0});
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            horizon = std::min(horizon, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            horizon = std::min(horizon, pendingGuestCallbacks.begin()->first);
                        horizon = timrman.nextEventCycle(horizon);
                        if (m_idleCacheEnabled && horizon > totalCycles)
                        {
                            m_idleUntil = horizon;
                            m_idleEpoch = kernel.schedEpoch();
                            m_idleValid = true;
                        }
                        else
                            m_idleValid = false;
                        const uint64_t idleTarget = std::max(totalCycles + 1u, std::min(target, horizon));
                        if (m_recompAuditEnabled)
                        {
                            m_audit.idleSkippedCycles += idleTarget - totalCycles;
                            ++m_audit.fullIdleSkips;
                        }
                        totalCycles = idleTarget;
                        continue;
                    }
                    m_idleValid = false;
                    const uint64_t before = totalCycles;
                    if (m_profiling)
                    {
                        const uint64_t s0 = __rdtsc();
                        runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                        iopProfile().cpuTicks += __rdtsc() - s0;
                        ++iopProfile().slices;
                    }
                    else
                        runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                if (m_profiling)
                {
                    const uint64_t s0 = __rdtsc();
                    drainMixedAudio();
                    iopProfile().soundTicks += __rdtsc() - s0;
                }
                else
                    drainMixedAudio();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            // callFunction runs only the entry. An IRX that CreateThread's its RPC server and returns
            // would otherwise still be unregistered when sceSifLoadModuleBuffer returns to the EE.
            runCycles(2000000u);
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            // The game DMAs the IRX package into IOP RAM, then passes that IOP address.
            // readGuest on the runtime host reads EE RAM (unit tests plant the image there instead).
            if (!readElfImageFromIopRam(memory, guestAddress, image) &&
                !IopModuleLoader::readElfFromGuest(host, guestAddress, image))
            {
                uint8_t mag[4]{};
                (void)memory.readRam(guestAddress, mag, sizeof(mag));
                std::ostringstream out;
                out << "[IOP] loadModuleBuffer 0x" << std::hex << guestAddress << " not an ELF, iop bytes "
                    << static_cast<unsigned>(mag[0]) << " " << static_cast<unsigned>(mag[1]) << " "
                    << static_cast<unsigned>(mag[2]) << " " << static_cast<unsigned>(mag[3]);
                log(LogLevel::Warning, out.str());
                return {true, -1, -1};
            }
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        struct LibsdState
        {
            std::array<uint32_t, 2> transHandler{};
            std::array<uint32_t, 2> transGp{};
            std::array<uint32_t, 2> transArg{};
            std::array<uint32_t, 2> spu2Handler{};
            std::array<uint32_t, 2> spu2Gp{};
            std::array<uint32_t, 2> spu2Arg{};
            std::array<bool, 2> blockActive{};
            std::array<uint32_t, 2> blockBuffer{};
            std::array<uint32_t, 2> blockSize{};
        };
        LibsdState m_libsd{};
        SoundEngine m_sound;
        uint64_t m_spu2IrqCount = 0u;
        std::array<uint32_t, 32> m_libsdCalls{};
        uint32_t m_konCalls = 0u, m_koffCalls = 0u;
        uint64_t m_nextSoundStats = 0u;
        int m_soundRoute = -1;
        // TEKKEN4_IOP_IDLE_SKIP=0 turns the idle-skip cache off (paired speed runs with one exe).
        bool m_idleCacheEnabled = []
        {
            const char *env = std::getenv("TEKKEN4_IOP_IDLE_SKIP");
            return !(env && env[0] == '0' && env[1] == '\0');
        }();
        bool m_fastIdleEnabled = []
        {
            const char *env = std::getenv("TEKKEN4_IOP_FAST_IDLE");
            return !(env && env[0] == '0' && env[1] == '\0');
        }();
        uint64_t m_fastIdleCount = 0; // host-only counter (tests), not IOP state
        bool m_idleValid = false;
        uint64_t m_idleUntil = 0;
        uint64_t m_idleEpoch = 0;
        uint64_t m_idleSkipCount = 0;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t data = 0u;
            uint32_t gp = 0u;
        };
        std::map<uint32_t, SifCmdHandler> sifCmdHandlers;
        // IOP RAM the EE command packets are handed to their handlers in (sifcmd's receive buffer on a console).
        static constexpr uint32_t kSifCmdSlots = 16u;
        static constexpr uint32_t kSifCmdSlotSize = 128u;
        uint32_t sifCmdSlotBase = 0u;
        uint32_t sifCmdNextSlot = 0u;
        uint64_t eeSifCommandsDelivered = 0u;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        bool m_profiling = false; // PS2X_IOP_PROF: inside a profiled runCycles (host-only, not IOP state)
        // Counts actual runtime work, including RPCs/callbacks and repeated work after rollback.
        // No fields below participate in forEachScalar/saveState/serialization/hash.
        const bool m_recompAuditEnabled = [] {
            const char *env = std::getenv("PS2X_RECOMP_AUDIT");
            return env && env[0] == '1' && env[1] == '\0';
        }();
        struct AuditCounters
        {
            uint64_t interpreted = 0, hle = 0, guestImportJumps = 0, missingImports = 0;
            uint64_t idleSkippedCycles = 0, fastIdleCalls = 0, fullIdleSkips = 0;
        };
        AuditCounters m_audit{}, m_auditLast{};
        const std::chrono::steady_clock::time_point m_auditStarted = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point m_auditLastReport = m_auditStarted;
        uint64_t m_auditLastPollCycle = 0;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    struct IopEmulatorState
    {
        IopMemory::SavedState memory;
        IopKernel::SavedState kernel;
        IopCdvd::SavedState cdvd;
        IopRpcBridge::SavedState rpc;
        IopIntrman::SavedState intrman;
        IopTimrman::SavedState timrman;
        IopIoman::SavedState ioman;
        IopImportRegistry::SavedState imports;
        SoundEngine::SavedState sound;
        // The emulator's own members: trivially copyable scalars as bytes (saveStateTo), containers by value.
        std::vector<uint8_t> scalars;
        std::map<int, IopEmulator::Impl::Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, IopEmulator::Impl::ScheduledGuestCallback> pendingGuestCallbacks;
        std::map<uint32_t, std::array<uint32_t, 3>> sifCmdHandlers;
        std::deque<EeSifCommand> eeSifCommands;
        bool eeSifCommandPending = false;
        std::array<uint32_t, 32> sifSoftwareRegisters{};
    };

    void IopEmulatorStateDeleter::operator()(IopEmulatorState *state) const noexcept
    {
        delete state;
    }

    namespace
    {
        template <typename T>
        void putPod(std::vector<uint8_t> &out, const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
            out.insert(out.end(), bytes, bytes + sizeof(T));
        }

        template <typename T>
        void getPod(const std::vector<uint8_t> &in, size_t &at, T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            std::memcpy(&value, in.data() + at, sizeof(T));
            at += sizeof(T);
        }

        // Every scalar member of IopEmulator::Impl that is guest state, in one fixed order (save and load).
        template <typename Impl, typename Fn>
        void forEachScalar(Impl &impl, Fn &&fn)
        {
            fn(impl.m_libsd);
            fn(impl.m_spu2IrqCount);
            fn(impl.m_libsdCalls);
            fn(impl.m_konCalls);
            fn(impl.m_koffCalls);
            fn(impl.m_nextSoundStats);
            fn(impl.m_soundRoute);
            fn(impl.m_idleValid);
            fn(impl.m_idleUntil);
            fn(impl.m_idleEpoch);
            fn(impl.m_idleSkipCount);
            fn(impl.sifCmdSlotBase);
            fn(impl.sifCmdNextSlot);
            fn(impl.eeSifCommandsDelivered);
            fn(impl.nextModuleId);
            fn(impl.moduleCursor);
            fn(impl.totalCycles);
            fn(impl.totalInstructions);
            fn(impl.eeCycleCarry);
            fn(impl.servicingDmaInterrupts);
            fn(impl.servicingGuestCallbacks);
            fn(impl.callDepth);
            fn(impl.secrMcCommandHandler);
            fn(impl.secrMcDevIdHandler);
            fn(impl.checkKelfPathCallback);
        }
    }

    void IopEmulator::Impl::saveStateTo(IopEmulatorState &out) const
    {
        memory.saveState(out.memory);
        kernel.saveState(out.kernel);
        cdvd.saveState(out.cdvd);
        rpc.saveState(out.rpc);
        intrman.saveState(out.intrman);
        timrman.saveState(out.timrman);
        ioman.saveState(out.ioman);
        imports.saveState(out.imports);
        m_sound.saveState(out.sound);
        out.scalars.clear();
        forEachScalar(*this, [&out](const auto &value) { putPod(out.scalars, value); });
        out.modules = modules;
        out.pendingDmaInterrupts = pendingDmaInterrupts;
        out.pendingGuestCallbacks = pendingGuestCallbacks;
        out.sifCmdHandlers.clear();
        for (const auto &[cid, handler] : sifCmdHandlers)
            out.sifCmdHandlers[cid] = {handler.function, handler.data, handler.gp};
        {
            std::lock_guard<std::mutex> lock(eeSifCommandMutex());
            out.eeSifCommands = eeSifCommandQueue();
            out.eeSifCommandPending = eeSifCommandPending().load(std::memory_order_acquire);
        }
        for (uint32_t i = 0; i < out.sifSoftwareRegisters.size(); ++i)
            out.sifSoftwareRegisters[i] = sifSoftwareRegister(i);
    }

    void IopEmulator::Impl::loadStateFrom(const IopEmulatorState &in)
    {
        memory.loadState(in.memory);
        kernel.loadState(in.kernel);
        cdvd.loadState(in.cdvd);
        rpc.loadState(in.rpc);
        intrman.loadState(in.intrman);
        timrman.loadState(in.timrman);
        ioman.loadState(in.ioman);
        imports.loadState(in.imports);
        m_sound.loadState(in.sound);
        size_t at = 0;
        forEachScalar(*this, [&in, &at](auto &value) { getPod(in.scalars, at, value); });
        modules = in.modules;
        pendingDmaInterrupts = in.pendingDmaInterrupts;
        pendingGuestCallbacks = in.pendingGuestCallbacks;
        sifCmdHandlers.clear();
        for (const auto &[cid, handler] : in.sifCmdHandlers)
            sifCmdHandlers[cid] = SifCmdHandler{handler[0], handler[1], handler[2]};
        {
            std::lock_guard<std::mutex> lock(eeSifCommandMutex());
            eeSifCommandQueue() = in.eeSifCommands;
            eeSifCommandPending().store(in.eeSifCommandPending, std::memory_order_release);
        }
        for (uint32_t i = 0; i < in.sifSoftwareRegisters.size(); ++i)
            setSifSoftwareRegister(i, in.sifSoftwareRegisters[i]);
        activeCpu = nullptr;
    }

    IopEmulatorStatePtr IopEmulator::newState()
    {
        return IopEmulatorStatePtr(new IopEmulatorState());
    }

    void IopEmulator::saveState(IopEmulatorState &out) const
    {
        m_impl->saveStateTo(out);
    }

    void IopEmulator::loadState(const IopEmulatorState &in)
    {
        m_impl->loadStateFrom(in);
    }

    uint64_t IopEmulator::hashState(const IopEmulatorState &state)
    {
        // Over 8-byte words: IOP RAM, SPU2 sample RAM, the scalar block, every thread's registers and state.
        uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&h](const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            size_t at = 0;
            for (; at + 8u <= size; at += 8u)
            {
                uint64_t word;
                std::memcpy(&word, bytes + at, 8u);
                h = (h ^ word) * 0x100000001b3ull;
                h ^= h >> 29;
            }
            for (; at < size; ++at)
                h = (h ^ bytes[at]) * 0x100000001b3ull;
        };
        mix(state.memory.ram.data(), state.memory.ram.size());
        mix(state.sound.ram.data(), state.sound.ram.size() * sizeof(uint16_t));
        mix(state.scalars.data(), state.scalars.size());
        mix(&state.sound.lastCycle, sizeof(state.sound.lastCycle));
        for (const auto &[id, thread] : state.kernel.threads)
        {
            mix(&id, sizeof(id));
            mix(thread.cpu.gpr.data(), sizeof(uint32_t) * thread.cpu.gpr.size());
            mix(&thread.cpu.pc, sizeof(thread.cpu.pc));
            const uint8_t threadState = static_cast<uint8_t>(thread.state);
            mix(&threadState, 1u);
            mix(&thread.wakeCycle, sizeof(thread.wakeCycle));
        }
        return h;
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (!iopCycles)
            return;
        if (m_impl->fastIdleAdvance(iopCycles))
        {
            if (iopProfileOn())
                ++iopProfile().fastCalls;
            return;
        }
        m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        if (iopProfileOn())
        {
            const uint64_t t0 = __rdtsc();
            const uint64_t instr0 = m_impl->instructionCount();
            const bool nested = m_impl->m_profiling;
            m_impl->m_profiling = true;
            RpcResult result = m_impl->rpc.handleRpc(request, *m_impl);
            m_impl->m_profiling = nested;
            IopProfile &p = iopProfile();
            p.rpcTicks += __rdtsc() - t0;
            ++p.rpcCalls;
            p.rpcInstructions += m_impl->instructionCount() - instr0;
            return result;
        }
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    void IopEmulator::setIdleSkipCache(bool enabled) noexcept
    {
        m_impl->m_idleCacheEnabled = enabled;
        if (!enabled)
            m_impl->m_idleValid = false;
    }

    void IopEmulator::setFastIdle(bool enabled) noexcept
    {
        m_impl->m_fastIdleEnabled = enabled;
    }

    uint64_t IopEmulator::testFastIdleCount() const noexcept
    {
        return m_impl->m_fastIdleCount;
    }

    void IopEmulator::testSealIdleSkip(uint64_t untilCycle) noexcept
    {
        if (!m_impl->m_idleCacheEnabled)
            return;
        m_impl->m_idleUntil = untilCycle;
        m_impl->m_idleEpoch = m_impl->kernel.schedEpoch();
        m_impl->m_idleValid = true;
    }

    uint64_t IopEmulator::testIdleSkipCount() const noexcept
    {
        return m_impl->m_idleSkipCount;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}

namespace ps2x::iop
{
    namespace
    {
        std::array<uint32_t, 32> g_sifSoftwareRegisters{};
    }

    void setSifSoftwareRegister(uint32_t index, uint32_t value) noexcept
    {
        if (index < g_sifSoftwareRegisters.size())
            g_sifSoftwareRegisters[index] = value;
    }

    uint32_t sifSoftwareRegister(uint32_t index) noexcept
    {
        return index < g_sifSoftwareRegisters.size() ? g_sifSoftwareRegisters[index] : 0u;
    }

    void queueEeSifCommand(uint32_t cid, const void *packet, uint32_t size) noexcept
    {
        if (!packet || size < 16u || size > 112u)
            return;
        try
        {
            detail::EeSifCommand command;
            command.cid = cid;
            command.size = size;
            std::memcpy(command.packet.data(), packet, size);
            std::lock_guard<std::mutex> lock(detail::eeSifCommandMutex());
            if (detail::eeSifCommandQueue().size() >= 256u) // no IOP running to take them
                detail::eeSifCommandQueue().pop_front();
            detail::eeSifCommandQueue().push_back(command);
            detail::eeSifCommandPending().store(true, std::memory_order_release);
        }
        catch (...)
        {
        }
    }
}
