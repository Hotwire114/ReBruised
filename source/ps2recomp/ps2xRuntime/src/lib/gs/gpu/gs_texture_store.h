// GOW-Port: native texture / CLUT store of the GS renderer (GSHwBackend).
// Keeps the bytes the game uploads (HOST->LOCAL image transfers, LOCAL->LOCAL copies) at their GS block addresses in
// the 4 MiB GS memory image, because the game uploads data in one format and reads it back in another (PSMT8 textures
// from CT32 uploads, CT24 from CT32): the swizzle/address math is how the game's data is laid out, not a hardware model.
// It also holds the CLUT the game loads (CLD rules of TEX0) and builds palettes from it. There is no rasteriser, no
// framebuffer and no LOCAL->HOST readback here: all drawing is done by the renderer on the GPU.
// Single-threaded: it is only used from the thread that drives GSHwBackend (the GS worker).
#pragma once

#include "runtime/gs/gs_types.h"
#include "runtime/gs/gs_upload24.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

class GSTextureStore
{
public:
    void Initialize(uint8_t *vram, uint32_t vramSize);
    void Reset();

    // Where a CLUT load reads its source pixels when they are not the uploaded bytes: the renderer, when TEX0.CBP lies in a
    // render target. Gets the `count` source pixels as (x, y) pairs of a `width` (64 px units) wide TEX0.CPSM buffer at
    // TEX0.CBP and writes their raw words; returns false when that memory holds no render target.
    using ClutSource = std::function<bool(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)>;
    // TEX0 with an indexed PSM: load the CLUT when TEX0.CLD says so, from `source` or else from the uploaded bytes.
    // Tekken 4 port: true when CLUT memory was written (CLD loads it), false when the TEX0 write left it as it was.
    bool LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut, const ClutSource &source = {});
    // Black & Bruised: LoadClut from the uploaded bytes through a small cache. A load's result depends only on the CLUT
    // registers, the CLUT before it (priorHash = ClutHash() of it) and GS memory, which only this store writes
    // (m_vramGen counts the writes): a repeat gets the saved CLUT instead of 256 swizzled reads (the game reloads one
    // of a few palettes with nearly every draw). newHash = ClutHash() after the load. PS2X_GL_CLUT_CACHE=0: off.
    bool LoadClutCached(const GSTex0Reg &tex0, const GSTexClutReg &texclut, uint64_t priorHash, uint64_t &newHash);
    uint64_t ClutHash() const;
    // 256-entry RGBA palette for the indexed texture described by `state` (CSA/CSM/CPSM + TEXA applied).
    void BuildPalette(const GSDrawState &state, uint32_t out[256]) const;
    // Point-sampled RGBA texels of the texture selected by `state` (generic path for formats without a direct layout).
    void DecodeTextureRgba(const GSDrawState &state, std::vector<uint32_t> &out) const;

    // TRXDIR: 0 = host->local (bytes follow through Upload), 2 = local->local (done at once), 1 = local->host (not
    // served: the game never reads GS memory back), 3 = none.
    void BeginTransfer(const GSTransferCommand &command);
    void Upload(const uint8_t *data, uint32_t sizeBytes);
    const GSTransferSnapshot &TransferState() const { return m_state; }

    // One pixel of GS memory in the given layout (debug reads/writes through the backend interface).
    uint32_t Read(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void Write(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    void Snapshot(std::vector<uint8_t> &out) const;

private:
    uint32_t ClutColor(const GSDrawState &state, uint32_t index) const;
    void UploadAligned(const uint8_t *data, uint32_t sizeBytes);
    void CopyLocalToLocal();
    bool ApplyClutLoadControl(const GSTex0Reg &tex0); // CLD: false when this TEX0 write loads nothing
    void LoadClutEntries(const GSTex0Reg &tex0, const GSTexClutReg &texclut, const ClutSource &source);

    struct ClutCacheEntry
    {
        uint64_t key = 0, prior = 0, gen = ~0ull, hash = 0;
        std::array<uint16_t, 512> clut{};
    };
    std::array<ClutCacheEntry, 16> m_clutCache{};
    uint32_t m_clutCacheNext = 0;
    uint64_t m_vramGen = 0;

    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0u;
    std::array<uint16_t, 512> m_clut{}; // CT32: low halves [0,256), high halves [256,512); CT16: 512 entries
    std::array<uint32_t, 2> m_clutCbp{};
    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_state{};
    GSUpload24State m_upload24{};
};
