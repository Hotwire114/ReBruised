// Ported from God-Of-War-Native-Port origin/runtime ps2xIOP/src/emulator/core/sound_engine.h (GPL-3.0).
// SPU2 mixer used at the LIBSD import boundary. Reverb is the SPU recurrence (psx-spx) on libsd's presets.
#pragma once

// GOW-Port: native sound engine (replaces the register-level SPU2 model). The game's sound driver (989snd) reaches the
// sound hardware only through the LIBSD API; those calls are native (src/native/iop_native_overrides.cpp) and land here
// as typed voice / core state. No SPU2 registers and no IOP DMA controller exist: transfers are copies into the sample
// RAM. Mixing: 2 cores x 24 ADPCM voices (pitch with linear interpolation, ADSR envelope), dry mix L/R per voice,
// master volume, plus the core input stream (LIBSD block transfer, "ADMA") at the stream volume. 48 kHz stereo output.
// DSP algorithms as in PCSX2 (pcsx2/SPU2) and psx-spx (ADPCM / ADSR are the same as on the PS1). Reverb runs at
// 24 kHz from the libsd effect presets. docs/native-port.md (step 3), docs/sound.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ps2x::iop::detail
{
    class SoundEngine
    {
    public:
        static constexpr uint32_t RamHalfwords = 1u << 20u; // 2 MB sample RAM
        static constexpr uint32_t VoicesPerCore = 24u;
        static constexpr uint32_t CyclesPerSample = 768u;  // IOP 36.864 MHz / 48 kHz

        SoundEngine();
        void reset();

        // ---- LIBSD API (entry codes as libsd: (id << 8) | (voice << 1) | core; master volumes have bit 7 set) ----
        void setParam(uint32_t entry, uint16_t value);   // sceSdSetParam
        [[nodiscard]] uint16_t getParam(uint32_t entry) const; // sceSdGetParam
        void setSwitch(uint32_t entry, uint32_t mask);   // sceSdSetSwitch (KON, KOFF, VMIX*, NON, PMON)
        [[nodiscard]] uint32_t getSwitch(uint32_t entry) const; // sceSdGetSwitch
        void setAddr(uint32_t entry, uint32_t byteAddress); // sceSdSetAddr (SSA, LSA, NAX, ESA, EEA, ...)
        [[nodiscard]] uint32_t getAddr(uint32_t entry) const; // sceSdGetAddr (byte address)
        void setCoreAttr(uint32_t entry, uint32_t value);  // sceSdSetCoreAttr
        [[nodiscard]] uint32_t getCoreAttr(uint32_t entry) const;
        // sceSdSetEffectAttr. mode is SD_EFFECT_MODE_* (bit 0x100 clears the work area). depth is EVOL.
        void setEffectAttr(uint32_t core, int32_t mode, int16_t depthL, int16_t depthR, int32_t delay, int32_t feedback);
        void initCore(uint32_t core);                      // sceSdInit state of one core

        // sample RAM transfers (sceSdVoiceTrans): byte address in sample RAM
        void writeSampleRam(uint32_t byteAddress, const uint8_t *source, size_t bytes);
        void readSampleRam(uint32_t byteAddress, uint8_t *destination, size_t bytes) const;
        void clearSampleRam(uint32_t byteAddress, size_t bytes);

        // core input stream (sceSdBlockTrans with SD_BLOCK_LOOP): the engine reads 512-byte L / R blocks from the ring in
        // IOP RAM at 48 kHz; onHalf is called (from advanceTo) each time a half of the ring has been consumed.
        struct StreamRing
        {
            const uint8_t *base = nullptr; // IOP RAM of the ring
            uint32_t size = 0;             // bytes
            uint32_t position = 0;         // next byte to read
            bool active = false;
            bool loop = false;             // false: the buffer plays once
        };
        void startStream(uint32_t core, const uint8_t *ring, uint32_t size, bool loop);
        void stopStream(uint32_t core);
        [[nodiscard]] uint32_t streamPosition(uint32_t core) const { m_lastPollCycle = m_lastCycle; return m_stream[core & 1u].position; }
        [[nodiscard]] bool takeStreamHalf(uint32_t core) noexcept; // a half was consumed since the last call

        // SPU2 IRQ: bit n = core n's IRQA was reached by a voice (with that core's IRQ enable, attr bit 6, on) since
        // the last call. Like the hardware, the core's IRQ enable stays set; libsd's interrupt clears it.
        [[nodiscard]] uint32_t takeIrq() noexcept { const uint32_t bits = m_irqPending; m_irqPending = 0u; return bits; }

        // time: one stereo sample every CyclesPerSample IOP cycles
        void advanceTo(uint64_t iopCycle);
        uint64_t mixedUpTo() const { return m_lastCycle; } // IOP cycle of the last sample mixed
        // Tekken 4 port (t4iop): the IOP's idle fast path. True when advanceTo(iopCycle) would mix no sample; then it
        // does what advanceTo does in that case (the GOW_SND_STALE largest-step note) and nothing else.
        [[nodiscard]] bool advanceIdle(uint64_t iopCycle) noexcept
        {
            if (iopCycle <= m_lastCycle)
                return true;
            if (iopCycle - m_lastCycle >= CyclesPerSample)
                return false;
            if (iopCycle - m_lastCycle > m_maxAdvance)
                m_maxAdvance = iopCycle - m_lastCycle;
            return true;
        }
        [[nodiscard]] uint32_t irqPendingBits() const noexcept { return m_irqPending; }

        // stereo samples produced so far (producer: IOP thread, consumer: host audio thread)
        [[nodiscard]] size_t pendingFrames() const;
        size_t drainOutput(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames = SIZE_MAX);

    private:
        struct Voice
        {
            // parameters (as the driver sets them)
            uint16_t volL = 0, volR = 0, pitch = 0, adsr1 = 0, adsr2 = 0;
            uint32_t ssa = 0;                // start, halfwords
            // playback state
            bool active = false;
            uint32_t nax = 0;                // next ADPCM block to decode (halfwords)
            uint32_t playBlock = 0;          // block being played (NAX reads from it, like PCSX2's NextA)
            bool playStarted = false;
            uint32_t lsa = 0;                // loop start
            bool lsaFromDriver = false;      // the driver set LSA: block loop-start flags do not override it
            uint32_t pitchCounter = 0;       // 4.12
            int16_t decoded[28]{};
            uint32_t decodedIndex = 28;      // 28 = decode the next block first
            int32_t hist1 = 0, hist2 = 0;
            int16_t prev[4]{};               // newest first (gaussian interpolation)
            uint8_t blockFlags = 0;
            enum class Phase : uint8_t { Off, Attack, Decay, Sustain, Release } phase = Phase::Off;
            int32_t envelope = 0;
            uint32_t envelopeCounter = 0;
        };
        struct Core
        {
            std::array<Voice, VoicesPerCore> voices{};
            uint32_t vmixL = 0, vmixR = 0, vmixEL = 0, vmixER = 0, non = 0, pmon = 0, endx = 0;
            uint16_t mmix = 0, attr = 0;
            uint16_t mvolL = 0, mvolR = 0, evolL = 0, evolR = 0, avolL = 0, avolR = 0, bvolL = 0, bvolR = 0;
            uint32_t esa = 0, eea = 0, irqa = 0, tsa = 0; // halfwords
        };

        void keyOn(Core &core, uint32_t mask);
        void keyOff(Core &core, uint32_t mask);
        void decodeBlock(Core &core, uint32_t index);
        int32_t stepVoice(Core &core, uint32_t index);
        static void stepEnvelope(Voice &v);
        static uint32_t naxOf(const Voice &v) noexcept;
        void streamSample(uint32_t core, int32_t &left, int32_t &right);
        void processReverb(uint32_t core, int32_t inL, int32_t inR, int32_t &outL, int32_t &outR);
        void step();

        // SPU reverb work area, one per core. Addresses are sample offsets from the moving pointer (psx-spx).
        struct Reverb
        {
            bool enabled = false;
            std::array<uint16_t, 32> coef{};
            std::vector<int16_t> buf;
            uint32_t pos = 0;
            bool half = false;
            int32_t accL = 0, accR = 0;
            int32_t holdL = 0, holdR = 0;
        };

        std::vector<uint16_t> m_ram;
        std::array<Core, 2> m_cores{};
        std::array<Reverb, 2> m_reverb{};
        std::array<StreamRing, 2> m_stream{};
        std::array<bool, 2> m_streamHalf{};
        uint32_t m_irqPending = 0u;
        std::vector<int16_t> m_output;
        mutable std::mutex m_outputMutex;
        double m_resamplePhase = 0.0; // drainOutput: fractional read position when playing back faster
        uint64_t m_lastCycle = 0;
        // GOW_SND_STALE (streamSample): when the stream position was last polled, the largest advanceTo step, and a hash of
        // each 1 KB block of the ring as last played
        mutable uint64_t m_lastPollCycle = 0;
        uint64_t m_maxAdvance = 0;
        std::array<uint32_t, 64> m_blockHash{};

    public:
        // Rollback: the SPU2 (sample RAM, voices, reverb, input streams, IRQ, clock). Not the host output queue.
        struct SavedState
        {
            std::vector<uint16_t> ram;
            std::array<Core, 2> cores{};
            std::array<Reverb, 2> reverb{};
            std::array<StreamRing, 2> stream{};
            std::array<bool, 2> streamHalf{};
            uint32_t irqPending = 0u;
            uint64_t lastCycle = 0;
        };
        void saveState(SavedState &out) const;
        void loadState(const SavedState &in);
        [[nodiscard]] const std::vector<uint16_t> &sampleRam() const noexcept { return m_ram; }
        // Re-simulated frames (rollback): the mix runs, nothing reaches the host output.
        static void setOutputMuted(bool muted) noexcept;
    };
}
