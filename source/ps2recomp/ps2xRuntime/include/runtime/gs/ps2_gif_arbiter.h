#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

// The GIF path of the packet the GS frontend is processing on this thread (0 = not inside an arbiter call).
// Host-only: the widescreen presentation tells VU1 (PATH1, the 3D) from EE (PATH2/3: HUD, menus, copies) draws.
uint8_t gifCurrentPath() noexcept;

// Restore the path on the thread actually processing a packet, including queued worker dispatch.
class GifPathScope
{
public:
    explicit GifPathScope(GifPathId id) noexcept;
    ~GifPathScope();
    GifPathScope(const GifPathScope &) = delete;
    GifPathScope &operator=(const GifPathScope &) = delete;
private:
    uint8_t saved;
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    // drainImmediately: the caller drains before another path can queue. An empty queue has nothing to arbitrate,
    // so the GS sees the caller's bytes with no heap copy. A non-empty queue still copies so drain() can sort.
    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false,
                bool drainImmediately = false);

    void drain();
    bool empty() const { return m_queue.empty(); }
    // The GS's packet function itself (the VU1 worker calls it for queued packets).
    void processPacket(const uint8_t *data, uint32_t sizeBytes);

private:
    void dispatch(const uint8_t *data, uint32_t sizeBytes);
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
