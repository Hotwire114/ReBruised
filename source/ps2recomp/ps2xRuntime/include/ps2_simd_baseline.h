#pragma once
// Tekken 4 port (old PCs, t4-perf): the SSE3/SSSE3/SSE4.1 intrinsics the runtime and the recompiled code use, written
// with SSE2 instructions only, so the game runs on any x86-64 processor (AMD Phenom II / Athlon II, the first Core 2 --
// no SSE4.1 there). Force-included into every source file when the build has PS2X_SIMD_BASELINE=ON (CMake). Every
// replacement gives the same bits as the instruction it replaces (pure selects, integer arithmetic, byte moves), so a
// baseline build computes the same game state as the default build: online peers may mix them.
#if defined(PS2X_SIMD_BASELINE) && (defined(_M_X64) || defined(__x86_64__))
#include <immintrin.h> // declares the originals first; the macros below then only rename the later uses
#include <intrin.h>
#include <cstdint>

static inline __m128 ps2x_bl_blendv_ps(__m128 a, __m128 b, __m128 mask)
{
    const __m128 select = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(mask), 31));
    return _mm_or_ps(_mm_and_ps(select, b), _mm_andnot_ps(select, a));
}

static inline __m128i ps2x_bl_blendv_epi8(__m128i a, __m128i b, __m128i mask)
{
    const __m128i select = _mm_cmplt_epi8(mask, _mm_setzero_si128());
    return _mm_or_si128(_mm_and_si128(select, b), _mm_andnot_si128(select, a));
}

static inline __m128 ps2x_bl_blend_ps(__m128 a, __m128 b, int imm)
{
    const __m128 select = _mm_castsi128_ps(_mm_set_epi32((imm & 8) ? -1 : 0, (imm & 4) ? -1 : 0, (imm & 2) ? -1 : 0,
                                                         (imm & 1) ? -1 : 0));
    return _mm_or_ps(_mm_and_ps(select, b), _mm_andnot_ps(select, a));
}

static inline int32_t ps2x_bl_extract_epi32(__m128i v, int index)
{
    alignas(16) int32_t lanes[4];
    _mm_store_si128(reinterpret_cast<__m128i *>(lanes), v);
    return lanes[index & 3];
}

static inline int64_t ps2x_bl_extract_epi64(__m128i v, int index)
{
    return (index & 1) ? _mm_cvtsi128_si64(_mm_unpackhi_epi64(v, v)) : _mm_cvtsi128_si64(v);
}

static inline __m128i ps2x_bl_insert_epi8(__m128i v, int value, int index)
{
    alignas(16) uint8_t bytes[16];
    _mm_store_si128(reinterpret_cast<__m128i *>(bytes), v);
    bytes[index & 15] = static_cast<uint8_t>(value);
    return _mm_load_si128(reinterpret_cast<const __m128i *>(bytes));
}

static inline __m128i ps2x_bl_mullo_epi32(__m128i a, __m128i b)
{
    // the low 32 bits of a product are the same signed or unsigned
    const __m128i even = _mm_mul_epu32(a, b);
    const __m128i odd = _mm_mul_epu32(_mm_srli_si128(a, 4), _mm_srli_si128(b, 4));
    return _mm_unpacklo_epi32(_mm_shuffle_epi32(even, _MM_SHUFFLE(0, 0, 2, 0)),
                              _mm_shuffle_epi32(odd, _MM_SHUFFLE(0, 0, 2, 0)));
}

static inline __m128i ps2x_bl_min_epi32(__m128i a, __m128i b)
{
    const __m128i aGreater = _mm_cmpgt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(aGreater, b), _mm_andnot_si128(aGreater, a));
}

static inline __m128i ps2x_bl_max_epi32(__m128i a, __m128i b)
{
    const __m128i aGreater = _mm_cmpgt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(aGreater, a), _mm_andnot_si128(aGreater, b));
}

static inline __m128i ps2x_bl_abs_epi32(__m128i v)
{
    const __m128i sign = _mm_srai_epi32(v, 31);
    return _mm_sub_epi32(_mm_xor_si128(v, sign), sign);
}

static inline __m128i ps2x_bl_abs_epi16(__m128i v)
{
    const __m128i sign = _mm_srai_epi16(v, 15);
    return _mm_sub_epi16(_mm_xor_si128(v, sign), sign);
}

static inline __m128i ps2x_bl_abs_epi8(__m128i v)
{
    const __m128i sign = _mm_cmpgt_epi8(_mm_setzero_si128(), v);
    return _mm_sub_epi8(_mm_xor_si128(v, sign), sign);
}

static inline __m128i ps2x_bl_shuffle_epi8(__m128i v, __m128i control)
{
    alignas(16) uint8_t in[16], ctl[16], out[16];
    _mm_store_si128(reinterpret_cast<__m128i *>(in), v);
    _mm_store_si128(reinterpret_cast<__m128i *>(ctl), control);
    for (int i = 0; i < 16; ++i)
        out[i] = (ctl[i] & 0x80u) ? 0u : in[ctl[i] & 15u];
    return _mm_load_si128(reinterpret_cast<const __m128i *>(out));
}

#define _mm_blendv_ps(a, b, m) ps2x_bl_blendv_ps((a), (b), (m))
#define _mm_blendv_epi8(a, b, m) ps2x_bl_blendv_epi8((a), (b), (m))
#define _mm_blend_ps(a, b, imm) ps2x_bl_blend_ps((a), (b), (imm))
#define _mm_extract_epi32(v, i) ps2x_bl_extract_epi32((v), (i))
#define _mm_extract_epi64(v, i) ps2x_bl_extract_epi64((v), (i))
#define _mm_insert_epi8(v, x, i) ps2x_bl_insert_epi8((v), (x), (i))
#define _mm_mullo_epi32(a, b) ps2x_bl_mullo_epi32((a), (b))
#define _mm_min_epi32(a, b) ps2x_bl_min_epi32((a), (b))
#define _mm_max_epi32(a, b) ps2x_bl_max_epi32((a), (b))
#define _mm_abs_epi32(v) ps2x_bl_abs_epi32(v)
#define _mm_abs_epi16(v) ps2x_bl_abs_epi16(v)
#define _mm_abs_epi8(v) ps2x_bl_abs_epi8(v)
#define _mm_shuffle_epi8(v, c) ps2x_bl_shuffle_epi8((v), (c))
#endif
