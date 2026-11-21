#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include "ps2x/iop/iop_host.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}

std::mutex g_movieMutex;
std::vector<int16_t> g_moviePcm;
// Movie PCM accounting since the last ps2ClearMoviePcm (sync / gap measurement): frames mixed from the queue, and
// mixer frames that found the queue short after the first push (a gap in the movie's sound).
bool g_movieStarted = false;
// Host pacing hint only: never serialized or included in the guest/net state.
std::atomic<bool> g_movieActive{false};
uint64_t g_movieMixClipped = 0u;
uint64_t g_moviePlayedFrames = 0u;
uint64_t g_movieMissingFrames = 0u;
// Tekken 4 port: the audio device plays the movie's sound from its own copy of the PCM, pulled by the device callback,
// not through the mixer tick -> DeviceQueue path (a 43 ms buffer that the game thread refills once per frame): any
// game-thread stall over ~40 ms (at 1440p / 4K the frame work is heavier and the host is busier) left the device
// with silence in the middle of the movie's music although seconds of it were queued. g_moviePcm stays the
// producer-side copy (mixer tick: WAV capture, played / missing accounting); both are filled and cleared together.
std::mutex g_movieDevMutex;
std::vector<int16_t> g_movieDevPcm;
size_t g_movieDevRead = 0u;      // samples already played from the front of g_movieDevPcm
bool g_movieDevStarted = false;
std::atomic<uint64_t> g_movieDevMissing{0u}; // device frames asked for after the first push that found the copy empty
std::atomic<uint64_t> g_movieDevPlayed{0u}, g_movieDevTotalMissing{0u};
std::atomic<size_t> g_movieDevQueued{0u};
std::atomic<uint64_t> g_movieDevClipped{0u};

namespace
{
#if !defined(PLATFORM_VITA)
    // Playback queue between the game thread (push) and raylib's audio thread (pull, stream callback).
    // UpdateAudioStream pads every write up to the stream's sub-buffer (device rate / 30 = 1600 frames at 48 kHz)
    // with silence, so the old 1024-frame writes played ~36 % silence in 12 ms gaps (choppy sound, unintelligible
    // movie voices) and fell behind until the queue dropped whole seconds. The device now pulls what it needs.
    // Never destroyed: the audio thread can still call in during static destruction.
    class DeviceQueue
    {
    public:
        // Frames buffered ahead of the device: PS2X_AUDIO_DEFAULT_BUFFER_MS (a port's default, 64 if unset; a game-thread
        // hitch up to that long is inaudible), TEKKEN4_AUDIO_BUFFER_MS overrides it (20..150). Host side only.
        static size_t targetFrames()
        {
            static const size_t frames = []() -> size_t {
                int ms = 64;
                if (const char *env = std::getenv("PS2X_AUDIO_DEFAULT_BUFFER_MS"))
                    ms = std::atoi(env);
                if (const char *env = std::getenv("TEKKEN4_AUDIO_BUFFER_MS"))
                    ms = std::atoi(env);
                ms = ms < 20 ? 20 : (ms > 150 ? 150 : ms);
                return static_cast<size_t>(ms) * 48u;
            }();
            return frames;
        }
        static constexpr size_t kMaxFrames = 9600u;    // 200 ms: after a stall, cut back to the target

        void push(const int16_t *pcm, size_t frames)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_read != 0u && m_read * 2u >= m_samples.size())
            {
                m_samples.erase(m_samples.begin(), m_samples.begin() + static_cast<std::ptrdiff_t>(m_read));
                m_read = 0u;
            }
            m_samples.insert(m_samples.end(), pcm, pcm + frames * 2u);
            const size_t level = levelLocked();
            if (level > kMaxFrames)
            {
                m_read += (level - targetFrames()) * 2u;
                m_dropped += level - targetFrames();
                m_phase = 0.0;
            }
        }

        // Fills exactly `frames` frames. Around the target level the queue plays up to 5 % faster or slower (linear
        // interpolation), so a guest clock running ahead doesn't build latency and one running behind doesn't gap.
        // An empty queue plays silence and waits for the target level again instead of crackling at the edge.
        void pull(int16_t *out, size_t frames)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const size_t level = levelLocked();
            if (!m_primed && level >= targetFrames())
                m_primed = true;
            size_t written = 0u;
            double rate = 1.0;
            if (m_primed)
            {
                // Black & Bruised (2026-11-17): the IOP mix arrives in bursts (in fights the level swung 150..3100
                // frames within a second), and steering by the momentary level moved the playback rate between 0.95
                // and 1.05 several times a second: an audible warble on voices and music. Steer by the level
                // averaged over ~0.5 s, at most 2 % (slow, inaudible); the queue absorbs the bursts.
                // PS2X_AUDIO_FAST_RATE=1: the old momentary +-5 % steering.
                static const bool fastRate = [] { const char *e = std::getenv("PS2X_AUDIO_FAST_RATE"); return e && e[0] == '1'; }();
                double over;
                if (fastRate)
                {
                    over = (static_cast<double>(level) - static_cast<double>(targetFrames())) /
                           static_cast<double>(targetFrames());
                    rate = 1.0 + std::clamp(over * 0.1, -0.05, 0.05);
                }
                else
                {
                    const double alpha = std::min(1.0, static_cast<double>(frames) / 24000.0);
                    m_avgLevel = m_avgLevel < 0.0 ? static_cast<double>(level)
                                                  : m_avgLevel + (static_cast<double>(level) - m_avgLevel) * alpha;
                    over = (m_avgLevel - static_cast<double>(targetFrames())) / static_cast<double>(targetFrames());
                    rate = 1.0 + std::clamp(over * 0.04, -0.02, 0.02);
                }
                const int16_t *in = m_samples.data() + m_read;
                double position = m_phase;
                for (; written < frames; ++written, position += rate)
                {
                    const size_t index = static_cast<size_t>(position);
                    if (index + 1u >= level)
                        break;
                    const double fraction = position - static_cast<double>(index);
                    for (size_t side = 0u; side < 2u; ++side)
                    {
                        const double a = in[index * 2u + side], b = in[(index + 1u) * 2u + side];
                        out[written * 2u + side] = static_cast<int16_t>(std::lround(a + (b - a) * fraction));
                    }
                }
                const size_t consumed = std::min(level, static_cast<size_t>(position));
                m_phase = position - static_cast<double>(consumed);
                m_read += consumed * 2u;
                if (written < frames)
                {
                    m_primed = false;
                    m_avgLevel = -1.0;
                }
            }
            if (written < frames)
            {
                std::memset(out + written * 2u, 0, (frames - written) * 4u);
                m_missing += frames - written;
            }
            logStats(frames, level, rate);
        }

    private:
        size_t levelLocked() const { return (m_samples.size() - m_read) / 2u; }

        // TEKKEN4_AUDIO_STATS=1: every ~5 s of device time, frames the device asked for, frames it got as silence
        // (underrun), frames cut after a stall, the queue level range and the playback rate range.
        void logStats(size_t frames, size_t level, double rate)
        {
            static const bool enabled = std::getenv("TEKKEN4_AUDIO_STATS") != nullptr;
            if (!enabled)
                return;
            m_statPulled += frames;
            m_statMinLevel = std::min(m_statMinLevel, level);
            m_statMaxLevel = std::max(m_statMaxLevel, level);
            m_statMinRate = std::min(m_statMinRate, rate);
            m_statMaxRate = std::max(m_statMaxRate, rate);
            if (m_statPulled < 240000u)
                return;
            std::fprintf(stderr, "[audio-device] pulled=%zu missing=%zu dropped=%zu level=%zu-%zu rate=%.3f-%.3f movie-missing=%llu movie-played=%llu movie-total-missing=%llu movie-queued=%zu\n",
                         m_statPulled, m_missing, m_dropped, m_statMinLevel, m_statMaxLevel, m_statMinRate,
                         m_statMaxRate, static_cast<unsigned long long>(g_movieDevMissing.exchange(0u)),
                         static_cast<unsigned long long>(g_movieDevPlayed.load()),
                         static_cast<unsigned long long>(g_movieDevTotalMissing.load()), g_movieDevQueued.load());
            m_statPulled = m_missing = m_dropped = m_statMaxLevel = 0u;
            m_statMinLevel = SIZE_MAX;
            m_statMinRate = m_statMaxRate = 1.0;
        }

        std::mutex m_mutex;
        std::vector<int16_t> m_samples;
        size_t m_read = 0u; // samples already played from the front of m_samples
        double m_phase = 0.0;
        double m_avgLevel = -1.0; // smoothed queue level (frames); < 0 = not measured yet
        bool m_primed = false;
        size_t m_missing = 0u, m_dropped = 0u;
        size_t m_statPulled = 0u, m_statMinLevel = SIZE_MAX, m_statMaxLevel = 0u;
        double m_statMinRate = 1.0, m_statMaxRate = 1.0;
    };

    DeviceQueue &deviceQueue()
    {
        static DeviceQueue *queue = new DeviceQueue();
        return *queue;
    }

    // Adds the movie's sound straight from the device callback (see g_movieDevPcm).
    void mixDeviceMovie(int16_t *out, size_t frames)
    {
        std::lock_guard<std::mutex> lock(g_movieDevMutex);
        if (!g_movieDevStarted)
            return;
        const size_t have = (g_movieDevPcm.size() - g_movieDevRead) / 2u;
        const size_t n = frames < have ? frames : have;
        const int16_t *in = g_movieDevPcm.data() + g_movieDevRead;
        for (size_t i = 0; i < n * 2u; ++i)
        {
            const int32_t sum = static_cast<int32_t>(out[i]) + static_cast<int32_t>(in[i]);
            g_movieDevClipped += sum > 32767 || sum < -32768;
            out[i] = static_cast<int16_t>(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
        }
        g_movieDevRead += n * 2u;
        g_movieDevPlayed += n;
        g_movieDevQueued = have - n;
        if (frames > n)
        {
            g_movieDevMissing += frames - n;
            g_movieDevTotalMissing += frames - n;
        }
        if (g_movieDevRead >= 48000u * 2u)
        {
            g_movieDevPcm.erase(g_movieDevPcm.begin(), g_movieDevPcm.begin() + static_cast<std::ptrdiff_t>(g_movieDevRead));
            g_movieDevRead = 0u;
        }
    }

    // Debug capture of the callback result, after both queues and before master volume.
    // TEKKEN4_SOUND_WAV remains the producer/guest-clock capture for comparison.
    class DeviceWavDump
    {
    public:
        void push(const int16_t *pcm, size_t frames)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_finished)
                return;
            if (!m_tried)
            {
                m_tried = true;
                const char *path = std::getenv("TEKKEN4_AUDIO_DEVICE_WAV");
                if (!path || !*path)
                    return;
                std::error_code ec;
                const std::filesystem::path file(path);
                if (file.has_parent_path())
                    std::filesystem::create_directories(file.parent_path(), ec);
                m_file.open(file, std::ios::binary | std::ios::trunc);
                if (!m_file)
                {
                    std::fprintf(stderr, "[audio-device] wav open failed %s\n", path);
                    return;
                }
                const char header[44] = {};
                m_file.write(header, sizeof(header));
                patchHeader();
                std::fprintf(stderr, "[audio-device] wav %s (before master volume)\n", path);
            }
            if (!m_file)
                return;
            m_file.write(reinterpret_cast<const char *>(pcm), static_cast<std::streamsize>(frames * 4u));
            m_frames += frames;
            m_sinceFlush += frames;
            if (m_sinceFlush >= 48000u)
            {
                patchHeader();
                m_file.flush();
                m_sinceFlush = 0u;
            }
        }

        ~DeviceWavDump() { finish(); }

        void finish()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_finished && m_file.is_open())
            {
                patchHeader();
                m_file.flush();
                m_file.close();
            }
            m_finished = true;
        }

    private:
        void patchHeader()
        {
            uint8_t h[44] = {};
            const auto le32 = [&h](size_t offset, uint32_t value) {
                for (size_t b = 0; b < 4u; ++b)
                    h[offset + b] = static_cast<uint8_t>(value >> (b * 8u));
            };
            const uint32_t bytes = static_cast<uint32_t>(std::min<uint64_t>(m_frames * 4u, 0xFFFFFFD8u));
            std::memcpy(h, "RIFF", 4u);
            le32(4u, 36u + bytes);
            std::memcpy(h + 8u, "WAVEfmt ", 8u);
            le32(16u, 16u);
            h[20] = 1u; h[22] = 2u;
            le32(24u, 48000u); le32(28u, 192000u);
            h[32] = 4u; h[34] = 16u;
            std::memcpy(h + 36u, "data", 4u);
            le32(40u, bytes);
            const auto end = m_file.tellp();
            m_file.seekp(0);
            m_file.write(reinterpret_cast<const char *>(h), sizeof(h));
            m_file.seekp(end);
        }

        std::mutex m_mutex;
        std::ofstream m_file;
        bool m_tried = false, m_finished = false;
        uint64_t m_frames = 0u, m_sinceFlush = 0u;
    };
    bool deviceWavEnabled()
    {
        static const bool enabled = []() {
            const char *path = std::getenv("TEKKEN4_AUDIO_DEVICE_WAV");
            return path && *path;
        }();
        return enabled;
    }

    DeviceWavDump &deviceWavDump()
    {
        // The audio callback may outlive ordinary static destructors. MixedOutput explicitly closes the file.
        static DeviceWavDump *dump = new DeviceWavDump();
        return *dump;
    }

    void onDeviceRequest(void *buffer, unsigned int frames)
    {
        int16_t *out = static_cast<int16_t *>(buffer);
        deviceQueue().pull(out, frames);
        mixDeviceMovie(out, frames);
        if (deviceWavEnabled())
            deviceWavDump().push(out, frames);
        // TEKKEN4_AUDIO_MUTE=1: the device plays silence; everything before it (IOP, SPU2 mix, queue, movie copy) runs
        // as usual. Host output only (test runs next to a player on the same PC).
        static const bool mute = [] {
            const char *env = std::getenv("TEKKEN4_AUDIO_MUTE");
            return env && std::strcmp(env, "1") == 0;
        }();
        if (mute)
            std::memset(out, 0, static_cast<size_t>(frames) * 4u);
    }
#endif

    // 48 kHz s16 stereo from the IOP mixer (ps2x::iop::setMixedAudioFn). The WAV is the check Claude can read;
    // the raylib stream plays it through DeviceQueue.
    class MixedOutput
    {
    public:
        // wavPcm: the SPU mix plus the movie's sound (WAV capture); devicePcm: the SPU mix alone, the device callback adds the
        // movie's sound itself.
        void push(const int16_t *wavPcm, const int16_t *devicePcm, size_t frames)
        {
            const int16_t *pcm = wavPcm;
            if (!pcm || frames == 0u)
                return;
            std::lock_guard<std::mutex> lock(m_mutex);
            openWav();
            if (m_wav)
            {
                m_wav.write(reinterpret_cast<const char *>(pcm), static_cast<std::streamsize>(frames * 4u));
                m_frames += static_cast<uint64_t>(frames);
                m_sinceFlush += static_cast<uint32_t>(frames);
                if (m_sinceFlush >= 48000u)
                {
                    patchHeader();
                    m_wav.flush();
                    m_sinceFlush = 0u;
                }
            }
#if !defined(PLATFORM_VITA)
            pumpDevice(devicePcm ? devicePcm : pcm, frames);
#endif
        }

        ~MixedOutput()
        {
#if !defined(PLATFORM_VITA)
            m_testClockStop = true;
            if (m_testClock.joinable())
                m_testClock.join();
            if (deviceWavEnabled())
                deviceWavDump().finish();
#endif
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_wav)
            {
                patchHeader();
                m_wav.flush();
            }
        }

    private:
        static void writeLe32(uint8_t *p, uint32_t value)
        {
            p[0] = static_cast<uint8_t>(value);
            p[1] = static_cast<uint8_t>(value >> 8);
            p[2] = static_cast<uint8_t>(value >> 16);
            p[3] = static_cast<uint8_t>(value >> 24);
        }

        void openWav()
        {
            if (m_wavTried)
                return;
            m_wavTried = true;
            const char *path = std::getenv("TEKKEN4_SOUND_WAV");
            if (!path || path[0] == '\0')
                return;
            std::error_code ec;
            const std::filesystem::path file(path);
            if (file.has_parent_path())
                std::filesystem::create_directories(file.parent_path(), ec);
            m_wav.open(file, std::ios::binary | std::ios::trunc);
            if (!m_wav)
            {
                std::fprintf(stderr, "[tekken4] sound wav open failed %s\n", path);
                return;
            }
            uint8_t header[44] = {};
            m_wav.write(reinterpret_cast<const char *>(header), 44);
            patchHeader();
            m_wav.flush();
            std::fprintf(stderr, "[tekken4] sound wav %s\n", path);
        }

        void patchHeader()
        {
            if (!m_wav)
                return;
            const uint64_t dataBytes64 = m_frames * 4u;
            const uint32_t dataBytes = dataBytes64 > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(dataBytes64);
            const uint32_t fileSize = 36u + dataBytes;
            uint8_t h[44] = {};
            h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
            writeLe32(h + 4, fileSize);
            h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
            h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
            writeLe32(h + 16, 16u);
            h[20] = 1; h[22] = 2;
            writeLe32(h + 24, 48000u);
            writeLe32(h + 28, 48000u * 4u);
            h[32] = 4; h[34] = 16;
            h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
            writeLe32(h + 40, dataBytes);
            const auto end = m_wav.tellp();
            m_wav.seekp(0);
            m_wav.write(reinterpret_cast<const char *>(h), 44);
            m_wav.seekp(end);
        }

#if !defined(PLATFORM_VITA)
        void pumpDevice(const int16_t *pcm, size_t frames)
        {
            // Explicit diagnostic clock for machines without an audio endpoint. It exercises the identical
            // callback and host queues at 48 kHz independently of the guest; this is not a hardware capture.
            if (!m_streamOn && !m_testClockOn && !IsAudioDeviceReady())
            {
                const char *testClock = std::getenv("TEKKEN4_AUDIO_TEST_CLOCK");
                if (testClock && testClock[0] == '1' && testClock[1] == '\0')
                {
                    m_testClockOn = true;
                    m_testClock = std::thread([this]() {
                        int16_t block[480u * 2u];
                        auto deadline = std::chrono::steady_clock::now();
                        while (!m_testClockStop.load())
                        {
                            deadline += std::chrono::milliseconds(10);
                            std::this_thread::sleep_until(deadline);
                            if (m_testClockStop.load())
                                break;
                            onDeviceRequest(block, 480u);
                        }
                    });
                    std::fprintf(stderr, "[audio-device] diagnostic host clock 48000 Hz, 480 frames/10 ms; no hardware endpoint\n");
                }
            }
            if (!m_deviceGaveUp && !m_streamOn && !m_testClockOn && IsAudioDeviceReady())
            {
                // PS2X_AUDIO_DEVICE_FRAMES: raylib's stream sub-buffer (default rate / 30 = 1600). The stream is pulled
                // by its callback, so this is not output latency; 480 made the device pull ~1.4x real time (underruns).
                if (const char *env = std::getenv("PS2X_AUDIO_DEVICE_FRAMES"))
                {
                    const int deviceFrames = std::atoi(env);
                    if (deviceFrames >= 128 && deviceFrames <= 4096)
                        SetAudioStreamBufferSizeDefault(deviceFrames);
                }
                m_stream = LoadAudioStream(48000, 16, 2);
                if (m_stream.sampleRate == 48000)
                {
                    // Keep diagnostic callbacks running while background test runs are inaudible.
                    const char *mute = std::getenv("PS3_AUDIO_MUTE");
                    if (mute && mute[0] == '1' && mute[1] == '\0')
                        SetMasterVolume(0.0f);
                    SetAudioStreamCallback(m_stream, &onDeviceRequest);
                    PlayAudioStream(m_stream);
                    m_streamOn = true;
                    std::fprintf(stderr, "[tekken4] sound device 48000 Hz stereo (pulled by the audio thread)\n");
                }
                else
                    m_deviceGaveUp = true;
            }
            if (m_streamOn || m_testClockOn)
                deviceQueue().push(pcm, frames);
        }

        std::thread m_testClock;
        std::atomic<bool> m_testClockStop{false};
        bool m_testClockOn = false;
        AudioStream m_stream{};
        bool m_streamOn = false;
        bool m_deviceGaveUp = false;
#endif
        std::mutex m_mutex;
        std::ofstream m_wav;
        bool m_wavTried = false;
        uint64_t m_frames = 0;
        uint32_t m_sinceFlush = 0;
    };

    MixedOutput &mixedOutput()
    {
        static MixedOutput output;
        return output;
    }

    void onMixedAudio(const int16_t *pcm, size_t frames)
    {
        std::vector<int16_t> mixed;
        const int16_t *played = pcm;
        {
            std::lock_guard<std::mutex> lock(g_movieMutex);
            if (g_movieStarted && pcm && frames != 0u)
            {
                const size_t queued = g_moviePcm.size() / 2u;
                g_moviePlayedFrames += frames < queued ? frames : queued;
                g_movieMissingFrames += frames > queued ? frames - queued : 0u;
            }
            if (!g_moviePcm.empty() && pcm && frames != 0u)
            {
                mixed.assign(pcm, pcm + frames * 2u);
                const size_t have = g_moviePcm.size() / 2u;
                const size_t n = frames < have ? frames : have;
                for (size_t i = 0; i < n * 2u; ++i)
                {
                    const int32_t sum = static_cast<int32_t>(mixed[i]) + static_cast<int32_t>(g_moviePcm[i]);
                    g_movieMixClipped += sum > 32767 || sum < -32768;
                    mixed[i] = static_cast<int16_t>(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
                }
                g_moviePcm.erase(g_moviePcm.begin(), g_moviePcm.begin() + static_cast<std::ptrdiff_t>(n * 2u));
                played = mixed.data();
            }
        }
        mixedOutput().push(played, pcm, frames);
    }

    struct MixedAudioRegistration
    {
        MixedAudioRegistration()
        {
            ps2x::iop::setMixedAudioFn(&onMixedAudio);
        }
        ~MixedAudioRegistration()
        {
            ps2x::iop::setMixedAudioFn(nullptr);
        }
    };

    MixedAudioRegistration g_mixedAudioRegistration;
}

void ps2PushMoviePcm(const int16_t *stereoInterleaved, size_t frames)
{
    if (!stereoInterleaved || frames == 0u)
        return;
    std::lock_guard<std::mutex> lock(g_movieMutex);
    g_movieStarted = true;
    constexpr size_t kMaxFrames = 48000u * 8u;
    g_moviePcm.insert(g_moviePcm.end(), stereoInterleaved, stereoInterleaved + frames * 2u);
    if (g_moviePcm.size() / 2u > kMaxFrames)
        g_moviePcm.erase(g_moviePcm.begin(), g_moviePcm.end() - static_cast<std::ptrdiff_t>(kMaxFrames * 2u));
    std::lock_guard<std::mutex> deviceLock(g_movieDevMutex);
    g_movieDevStarted = true;
    g_movieDevPcm.insert(g_movieDevPcm.end(), stereoInterleaved, stereoInterleaved + frames * 2u);
    if ((g_movieDevPcm.size() - g_movieDevRead) / 2u > kMaxFrames)
        g_movieDevRead = g_movieDevPcm.size() - kMaxFrames * 2u;
    g_movieDevQueued = (g_movieDevPcm.size() - g_movieDevRead) / 2u;
    g_movieActive.store(true, std::memory_order_relaxed);
}

void ps2ClearMoviePcm()
{
    std::lock_guard<std::mutex> lock(g_movieMutex);
    g_moviePcm.clear();
    g_movieStarted = false;
    g_movieActive.store(false, std::memory_order_relaxed);
    g_moviePlayedFrames = 0u;
    g_movieMissingFrames = 0u;
    std::lock_guard<std::mutex> deviceLock(g_movieDevMutex);
    if (g_movieDevStarted && std::getenv("TEKKEN4_AUDIO_STATS"))
        std::fprintf(stderr, "[audio-movie-device] played=%llu missing=%llu queued=%zu clipped-samples=%llu producer-clipped-samples=%llu\n",
                     static_cast<unsigned long long>(g_movieDevPlayed.load()),
                     static_cast<unsigned long long>(g_movieDevTotalMissing.load()),
                     (g_movieDevPcm.size() - g_movieDevRead) / 2u,
                     static_cast<unsigned long long>(g_movieDevClipped.load()),
                     static_cast<unsigned long long>(g_movieMixClipped));
    g_movieDevPcm.clear();
    g_movieDevRead = 0u;
    g_movieDevStarted = false;
    g_movieDevMissing = 0u;
    g_movieDevPlayed = 0u;
    g_movieDevTotalMissing = 0u;
    g_movieDevQueued = 0u;
    g_movieDevClipped = 0u;
    g_movieMixClipped = 0u;
}

bool ps2MoviePcmActive()
{
    return g_movieActive.load(std::memory_order_relaxed);
}

Ps2MoviePcmStats ps2MoviePcmStats()
{
    std::lock_guard<std::mutex> lock(g_movieMutex);
    return Ps2MoviePcmStats{g_moviePlayedFrames, g_movieMissingFrames, g_moviePcm.size() / 2u};
}
