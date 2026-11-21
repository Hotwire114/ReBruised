#pragma once

#include "ps2_stubs.h"

namespace ps2_stubs
{
    void resetMpegStubState();
    void enqueueMpegDecodedFrameForTesting(uint32_t mpegAddr);
    void notifyMpegCdStreamStart(PS2Runtime *runtime = nullptr);
    void notifyMpegCdStreamDataProduced(uint32_t byteCount, bool endOfStream);
    void notifyMpegCdStreamEof(PS2Runtime *runtime = nullptr);
    // Decodes one IPU intra picture, as games keep compressed images for the IPU's IDEC command: the IPU_CTRL flags
    // byte, the macroblocks, then the end code 00 00 01 B0 (FFmpeg's "ipu" format). Writes the IPU's RGB32 output:
    // macroblocks in raster order, each 16x16 pixels (1024 bytes), colour-converted like the IPU, alpha 0x80.
    // Returns false when the picture does not decode or the runtime has no FFmpeg.
    bool decodeIpuIntraPicture(const uint8_t *data, size_t size, uint32_t mbWidth, uint32_t mbHeight, uint8_t *rgba32Out);
    void sceMpegFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegAddBs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegAddCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegAddStrCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDemuxPss(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDemuxPssRing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDispCenterOffX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDispCenterOffY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDispHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegDispWidth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegGetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // Pictures sceMpegGetPicture has handed out since start (all movies): a game override can time them.
    uint32_t mpegPicturesServed() noexcept;
    // Wall-clock timing of the pictures sceMpegGetPicture handed out (since the last reset): count, intervals
    // between consecutive pictures in buckets <20, 20-28, 28-38, 38-45, 45-60, >=60 ms, the largest, their sum.
    struct MpegServeStats
    {
        uint32_t count = 0;
        uint32_t intervals[6] = {};
        double maxIntervalMs = 0.0;
        double spanMs = 0.0;
    };
    MpegServeStats mpegServeStats(bool reset = false);
    void sceMpegGetPictureRAW8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegGetPictureRAW8xy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegIsRefBuffEmpty(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegResetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegSetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegSetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceMpegSetImageBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
