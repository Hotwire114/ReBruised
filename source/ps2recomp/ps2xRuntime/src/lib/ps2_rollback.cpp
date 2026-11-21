#include "runtime/ps2_rollback.h"
#include "runtime/ps2_rollback_cuts.h"
#include "runtime/ps2_vu1_async.h"

#include "ps2_runtime.h"
#include "Kernel/Stubs/Pad.h"
#include "Kernel/Syscalls/RPC.h"
#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Private runtime members a snapshot needs (friend of PS2Runtime).
class PS2RollbackAccess
{
public:
    static void save(PS2Runtime &runtime, std::vector<uint8_t> &out)
    {
        std::lock_guard<std::mutex> heapLock(runtime.m_guestHeapMutex);
        std::lock_guard<std::mutex> stackLock(runtime.m_asyncCallbackStackMutex);
        out.clear();
        auto put = [&out](const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            out.insert(out.end(), bytes, bytes + size);
        };
        const uint32_t blocks = static_cast<uint32_t>(runtime.m_guestHeapBlocks.size());
        put(&blocks, sizeof(blocks));
        for (const auto &block : runtime.m_guestHeapBlocks)
        {
            put(&block.addr, sizeof(block.addr));
            put(&block.size, sizeof(block.size));
            const uint8_t isFree = block.free ? 1u : 0u;
            put(&isFree, 1u);
        }
        put(&runtime.m_guestHeapBase, sizeof(uint32_t));
        put(&runtime.m_guestHeapEnd, sizeof(uint32_t));
        put(&runtime.m_guestHeapLimit, sizeof(uint32_t));
        put(&runtime.m_guestHeapSuggestedBase, sizeof(uint32_t));
        const uint8_t configured = runtime.m_guestHeapConfigured ? 1u : 0u;
        put(&configured, 1u);
        put(&runtime.m_asyncCallbackStackFloor, sizeof(uint32_t));
        put(&runtime.m_asyncCallbackStackTop, sizeof(uint32_t));
    }

    static bool saveIop(PS2Runtime &runtime, ps2x::iop::IopSnapshot &out)
    {
        if (!runtime.m_iopSubsystem)
            return false;
        runtime.m_iopSubsystem->saveState(out);
        return true;
    }

    static void loadIop(PS2Runtime &runtime, const ps2x::iop::IopSnapshot &in)
    {
        if (runtime.m_iopSubsystem)
            runtime.m_iopSubsystem->loadState(in);
    }

    static void load(PS2Runtime &runtime, const std::vector<uint8_t> &in)
    {
        std::lock_guard<std::mutex> heapLock(runtime.m_guestHeapMutex);
        std::lock_guard<std::mutex> stackLock(runtime.m_asyncCallbackStackMutex);
        size_t at = 0;
        auto get = [&in, &at](void *data, size_t size)
        {
            std::memcpy(data, in.data() + at, size);
            at += size;
        };
        uint32_t blocks = 0;
        get(&blocks, sizeof(blocks));
        runtime.m_guestHeapBlocks.resize(blocks);
        for (auto &block : runtime.m_guestHeapBlocks)
        {
            get(&block.addr, sizeof(block.addr));
            get(&block.size, sizeof(block.size));
            uint8_t isFree = 0;
            get(&isFree, 1u);
            block.free = isFree != 0u;
        }
        get(&runtime.m_guestHeapBase, sizeof(uint32_t));
        get(&runtime.m_guestHeapEnd, sizeof(uint32_t));
        get(&runtime.m_guestHeapLimit, sizeof(uint32_t));
        get(&runtime.m_guestHeapSuggestedBase, sizeof(uint32_t));
        uint8_t configured = 0;
        get(&configured, 1u);
        runtime.m_guestHeapConfigured = configured != 0u;
        get(&runtime.m_asyncCallbackStackFloor, sizeof(uint32_t));
        get(&runtime.m_asyncCallbackStackTop, sizeof(uint32_t));
    }
};

namespace ps2_rollback
{
    namespace
    {
        void copyIn(std::vector<uint8_t> &dst, const uint8_t *src, size_t size)
        {
            dst.resize(size);
            std::memcpy(dst.data(), src, size);
        }

        // Tekken 4 port (netplay): incremental RDRAM copies. With RDRAM under a Windows write watch
        // (PS2Memory::rdramWriteWatch), every capture/restore first collects the pages written since the last one and
        // stamps them with a new sequence number; a snapshot remembers the sequence it was filled at, so filling it
        // again copies only the pages stamped later (written since -- by the game or by a restore), and a restore
        // copies back only those. Pages not written since the snapshot are equal to it by construction.
        // PS2X_RB_INCREMENTAL=0: full copies; PS2X_RB_WW_VERIFY=1: compare every copy with the whole memory.
        constexpr size_t kWwPage = 4096u;
        constexpr size_t kWwPages = PS2_RAM_SIZE / kWwPage;
        struct WriteTracker
        {
            bool init = false, on = false, verify = false;
            uint64_t seq = 1;
            std::vector<uint64_t> stamp; // per page: the sequence of its last known write
            std::vector<void *> addrs;
            RdramCopyStats stats;
        };
        WriteTracker g_ww;

        bool trackerOn(PS2Memory &memory)
        {
            if (!g_ww.init)
            {
                g_ww.init = true;
                const char *inc = std::getenv("PS2X_RB_INCREMENTAL");
                const char *ver = std::getenv("PS2X_RB_WW_VERIFY");
                g_ww.on = memory.rdramWriteWatch() && !(inc && *inc == '0');
                g_ww.verify = ver && *ver == '1';
                g_ww.stamp.assign(kWwPages, 1u);
                g_ww.addrs.resize(kWwPages);
#ifdef _WIN32
                if (g_ww.on)
                    ResetWriteWatch(memory.getRDRAM(), PS2_RAM_SIZE);
#endif
            }
            return g_ww.on;
        }

        // Pages written since the last call get a new stamp; returns that stamp (the current sequence).
        uint64_t collectWrites(PS2Memory &memory)
        {
            ++g_ww.seq;
#ifdef _WIN32
            uint8_t *base = memory.getRDRAM();
            ULONG_PTR count = static_cast<ULONG_PTR>(g_ww.addrs.size());
            DWORD granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, PS2_RAM_SIZE, g_ww.addrs.data(), &count, &granularity) != 0)
            {
                std::fill(g_ww.stamp.begin(), g_ww.stamp.end(), g_ww.seq); // (never seen) everything counts as written
                return g_ww.seq;
            }
            for (ULONG_PTR i = 0; i < count; ++i)
            {
                const size_t page = static_cast<size_t>(static_cast<uint8_t *>(g_ww.addrs[i]) - base) / kWwPage;
                if (page < kWwPages)
                    g_ww.stamp[page] = g_ww.seq;
            }
#endif
            return g_ww.seq;
        }

        void verifyRdram(const uint8_t *mem, const std::vector<uint8_t> &copy)
        {
            for (size_t page = 0; page < kWwPages; ++page)
                if (std::memcmp(mem + page * kWwPage, copy.data() + page * kWwPage, kWwPage) != 0)
                    ++g_ww.stats.verifyMismatchPages;
        }

        void captureRdram(PS2Memory &memory, Snapshot &out)
        {
            const uint8_t *mem = memory.getRDRAM();
            if (!trackerOn(memory) || out.rdram.size() != PS2_RAM_SIZE || out.rdramSeq == 0u)
            {
                if (g_ww.on)
                    collectWrites(memory);
                copyIn(out.rdram, mem, PS2_RAM_SIZE);
                out.rdramSeq = g_ww.on ? g_ww.seq : 0u;
                ++g_ww.stats.captures;
                g_ww.stats.capturePages += kWwPages;
                return;
            }
            const uint64_t now = collectWrites(memory);
            uint8_t *dst = out.rdram.data();
            size_t copied = 0;
            for (size_t page = 0; page < kWwPages; ++page)
            {
                if (g_ww.stamp[page] <= out.rdramSeq)
                    continue;
                size_t end = page + 1u; // a run of pages in one memcpy
                while (end < kWwPages && g_ww.stamp[end] > out.rdramSeq)
                    ++end;
                std::memcpy(dst + page * kWwPage, mem + page * kWwPage, (end - page) * kWwPage);
                copied += end - page;
                page = end - 1u;
            }
            out.rdramSeq = now;
            ++g_ww.stats.captures;
            g_ww.stats.capturePages += copied;
            if (g_ww.verify)
                verifyRdram(mem, out.rdram);
        }

        void restoreRdram(PS2Memory &memory, const Snapshot &in)
        {
            uint8_t *mem = memory.getRDRAM();
            if (!trackerOn(memory) || in.rdramSeq == 0u)
            {
                std::memcpy(mem, in.rdram.data(), PS2_RAM_SIZE);
                if (g_ww.on)
                {
                    const uint64_t now = collectWrites(memory);
                    std::fill(g_ww.stamp.begin(), g_ww.stamp.end(), now);
                }
                ++g_ww.stats.restores;
                g_ww.stats.restorePages += kWwPages;
                return;
            }
            const uint64_t now = collectWrites(memory);
            size_t copied = 0;
            for (size_t page = 0; page < kWwPages; ++page)
            {
                if (g_ww.stamp[page] <= in.rdramSeq)
                    continue;
                size_t end = page + 1u;
                while (end < kWwPages && g_ww.stamp[end] > in.rdramSeq)
                    ++end;
                std::memcpy(mem + page * kWwPage, in.rdram.data() + page * kWwPage, (end - page) * kWwPage);
                // the restore wrote these pages: they may differ from every snapshot filled before now
                for (size_t q = page; q < end; ++q)
                    g_ww.stamp[q] = now;
                copied += end - page;
                page = end - 1u;
            }
            ++g_ww.stats.restores;
            g_ww.stats.restorePages += copied;
            if (g_ww.verify)
                verifyRdram(mem, in.rdram);
        }

        constexpr uint64_t kPrime1 = 0x9E3779B185EBCA87ull;
        constexpr uint64_t kPrime2 = 0xC2B2AE3D27D4EB4Full;

        inline uint64_t rotl(uint64_t value, int bits)
        {
            return (value << bits) | (value >> (64 - bits));
        }

        inline uint64_t mixFinal(uint64_t h)
        {
            h ^= h >> 33;
            h *= kPrime2;
            h ^= h >> 29;
            h *= kPrime1;
            h ^= h >> 32;
            return h;
        }
    }

    uint64_t hashBytes(const uint8_t *data, size_t size, uint64_t seed)
    {
        // Four independent lanes over 8-byte words (xxHash64-like rounds), then a tail. Not cryptographic; only
        // has to tell two machine states apart.
        uint64_t lane[4] = {seed + kPrime1, seed ^ kPrime2, seed + 0x165667B19E3779F9ull, seed - kPrime1};
        size_t at = 0;
        for (; at + 32u <= size; at += 32u)
        {
            uint64_t word[4];
            std::memcpy(word, data + at, 32u);
            for (int i = 0; i < 4; ++i)
            {
                lane[i] = rotl(lane[i] + word[i] * kPrime2, 31) * kPrime1;
            }
        }
        uint64_t h = rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18) + size;
        for (; at < size; ++at)
        {
            h = (h ^ data[at]) * kPrime1;
        }
        return mixFinal(h);
    }

    void capture(PS2Runtime &runtime, Snapshot &out)
    {
        ps2_vu1_async::sync();
        PS2Memory &memory = runtime.memory();
        captureRdram(memory, out);
        copyIn(out.scratchpad, memory.getScratchpad(), PS2_SCRATCHPAD_SIZE);
        copyIn(out.vu0Code, memory.getVU0Code(), PS2_VU0_CODE_SIZE);
        copyIn(out.vu0Data, memory.getVU0Data(), PS2_VU0_DATA_SIZE);
        copyIn(out.vu1Code, memory.getVU1Code(), PS2_VU1_CODE_SIZE);
        copyIn(out.vu1Data, memory.getVU1Data(), PS2_VU1_DATA_SIZE);
        runtime.eeScheduler().saveState(out.scheduler);
        memory.saveDeviceState(out.devices);
        if (!out.vu0)
        {
            out.vu0 = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
        }
        if (!out.vu1)
        {
            out.vu1 = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU1);
        }
        *out.vu0 = runtime.vu0();
        *out.vu1 = runtime.vu1();
        out.cpuContext = runtime.cpu();
        PS2RollbackAccess::save(runtime, out.runtimeState);
        out.padState = ps2_stubs::savePadState();
        out.sifSendCmdId = ps2_syscalls::sifSendCmdNextId();
        out.hasIop = false;
        if (includeIop())
            out.hasIop = PS2RollbackAccess::saveIop(runtime, out.iop);
        out.valid = true;
    }

    void restore(PS2Runtime &runtime, const Snapshot &in)
    {
        ps2_vu1_async::sync();
        if (!in.valid)
        {
            return;
        }
        PS2Memory &memory = runtime.memory();
        restoreRdram(memory, in);
        std::memcpy(memory.getScratchpad(), in.scratchpad.data(), PS2_SCRATCHPAD_SIZE);
        if (std::memcmp(memory.getVU0Code(), in.vu0Code.data(), PS2_VU0_CODE_SIZE) != 0)
        {
            std::memcpy(memory.getVU0Code(), in.vu0Code.data(), PS2_VU0_CODE_SIZE);
            memory.markVU0CodeModified();
        }
        std::memcpy(memory.getVU0Data(), in.vu0Data.data(), PS2_VU0_DATA_SIZE);
        if (std::memcmp(memory.getVU1Code(), in.vu1Code.data(), PS2_VU1_CODE_SIZE) != 0)
        {
            std::memcpy(memory.getVU1Code(), in.vu1Code.data(), PS2_VU1_CODE_SIZE);
            memory.markVU1CodeModified();
        }
        std::memcpy(memory.getVU1Data(), in.vu1Data.data(), PS2_VU1_DATA_SIZE);
        runtime.eeScheduler().loadState(in.scheduler);
        memory.loadDeviceState(in.devices);
        runtime.vu0() = *in.vu0;
        runtime.vu1() = *in.vu1;
        runtime.cpu() = in.cpuContext;
        PS2RollbackAccess::load(runtime, in.runtimeState);
        ps2_stubs::loadPadState(in.padState);
        ps2_syscalls::setSifSendCmdNextId(in.sifSendCmdId);
        if (in.hasIop)
            PS2RollbackAccess::loadIop(runtime, in.iop);
    }

    RdramCopyStats rdramCopyStats() noexcept
    {
        return g_ww.stats;
    }

    bool rdramIncremental() noexcept
    {
        return g_ww.on;
    }

    StateHash hashState(PS2Runtime &runtime, const std::vector<MaskRange> &masks, std::vector<uint64_t> *pageHashes)
    {
        ps2_vu1_async::sync();
        constexpr uint32_t kPage = 4096u;
        constexpr uint32_t kPages = PS2_RAM_SIZE / kPage;
        PS2Memory &memory = runtime.memory();
        const uint8_t *rdram = memory.getRDRAM();

        // Pages touched by a mask are hashed from a copy with the masked bytes zeroed.
        std::vector<MaskRange> sorted = masks;
        std::sort(sorted.begin(), sorted.end(), [](const MaskRange &a, const MaskRange &b)
                  { return a.address < b.address; });
        size_t maskIndex = 0;
        alignas(64) uint8_t page[kPage];

        StateHash result{};
        uint64_t regionAccumulator[32]{};
        uint64_t rdramAccumulator = 0;
        if (pageHashes)
        {
            pageHashes->resize(kPages);
        }
        for (uint32_t p = 0; p < kPages; ++p)
        {
            const uint32_t start = p * kPage;
            const uint32_t end = start + kPage;
            while (maskIndex < sorted.size() && sorted[maskIndex].address + sorted[maskIndex].size <= start)
            {
                ++maskIndex;
            }
            const uint8_t *source = rdram + start;
            if (maskIndex < sorted.size() && sorted[maskIndex].address < end)
            {
                std::memcpy(page, source, kPage);
                for (size_t m = maskIndex; m < sorted.size() && sorted[m].address < end; ++m)
                {
                    const uint32_t from = std::max(start, sorted[m].address);
                    const uint32_t to = std::min(end, sorted[m].address + sorted[m].size);
                    if (to > from)
                    {
                        std::memset(page + (from - start), 0, to - from);
                    }
                }
                source = page;
            }
            const uint64_t h = hashBytes(source, kPage, p);
            if (pageHashes)
            {
                (*pageHashes)[p] = h;
            }
            regionAccumulator[p >> 8] = mixFinal(regionAccumulator[p >> 8] ^ h) + p;
            rdramAccumulator = mixFinal(rdramAccumulator ^ h) + p;
        }
        for (int r = 0; r < 32; ++r)
        {
            result.regions[r] = static_cast<uint32_t>(regionAccumulator[r] ^ (regionAccumulator[r] >> 32));
        }
        result.rdram = rdramAccumulator;

        std::vector<uint8_t> digest;
        digest.reserve(64 * 1024);
        runtime.eeScheduler().appendStateDigest(digest);
        result.scheduler = hashBytes(digest.data(), digest.size(), 1);

        uint64_t other = result.scheduler;
        other = mixFinal(other ^ hashBytes(memory.getScratchpad(), PS2_SCRATCHPAD_SIZE, 2));
        other = mixFinal(other ^ hashBytes(memory.getVU0Data(), PS2_VU0_DATA_SIZE, 3));
        if (g_hostProfile.hashVu1Data)
            other = mixFinal(other ^ hashBytes(memory.getVU1Data(), PS2_VU1_DATA_SIZE, 4));
        other = mixFinal(other ^ hashBytes(memory.getVU0Code(), PS2_VU0_CODE_SIZE, 5));
        other = mixFinal(other ^ hashBytes(memory.getVU1Code(), PS2_VU1_CODE_SIZE, 6));
        // Device registers (fixed-size part only; the pending vectors are expected empty at a boundary).
        PS2DeviceState devices;
        memory.saveDeviceState(devices);
        std::vector<uint8_t> device;
        auto put = [&device](const void *data, size_t size)
        {
            const auto *bytes = static_cast<const uint8_t *>(data);
            device.insert(device.end(), bytes, bytes + size);
        };
        put(devices.dma, sizeof(devices.dma));
        put(&devices.vif0, sizeof(devices.vif0));
        put(&devices.vif1, sizeof(devices.vif1));
        for (const auto &timer : devices.timers)
        {
            put(&timer.count, sizeof(timer.count));
            put(&timer.mode, sizeof(timer.mode));
            put(&timer.compare, sizeof(timer.compare));
        }
        put(&devices.mfifoWaitingChannel, sizeof(devices.mfifoWaitingChannel));
        const uint32_t sizes[5] = {static_cast<uint32_t>(devices.vif1Carry.size()),
                                   static_cast<uint32_t>(devices.pendingGif.size()),
                                   static_cast<uint32_t>(devices.pendingVif1.size()),
                                   static_cast<uint32_t>(devices.completedDmacCauses.size()),
                                   static_cast<uint32_t>(devices.path3MaskedFifo.size())};
        put(sizes, sizeof(sizes));
        other = mixFinal(other ^ hashBytes(device.data(), device.size(), 7));
        const uint32_t sifId = ps2_syscalls::sifSendCmdNextId();
        other = mixFinal(other ^ hashBytes(reinterpret_cast<const uint8_t *>(&sifId), sizeof(sifId), 8));
        result.other = other;
        result.total = mixFinal(result.rdram ^ rotl(result.other, 17));
        return result;
    }
}

namespace ps2_rollback
{
    std::atomic<uint32_t> g_activeCuts{0u};
    HostProfile g_hostProfile;

    namespace
    {
        IopGate *g_iopGate = nullptr;
        std::atomic<bool> g_includeIop{false};
    }

    void setIncludeIop(bool on) noexcept
    {
        g_includeIop.store(on, std::memory_order_relaxed);
    }

    bool includeIop() noexcept
    {
        return g_includeIop.load(std::memory_order_relaxed);
    }

    uint64_t hashIop(const Snapshot &snapshot)
    {
        return snapshot.hasIop ? ps2x::iop::IopSubsystem::hashState(snapshot.iop) : 0u;
    }

    void setIopOutputMuted(bool muted) noexcept
    {
        ps2x::iop::IopSubsystem::setSoundOutputMuted(muted);
    }

    void setIopGate(IopGate *gate) noexcept
    {
        g_iopGate = gate;
    }

    IopGate *iopGate() noexcept
    {
        return g_iopGate;
    }
}
