// Tekken 4 port (t4vurecomp): inline helpers of the statically recompiled VU microprograms (see ps2_vu_lift.h).
// Every helper computes exactly what the exact engine (ps2_vu1_upper.cpp / ps2_vu1_lower.cpp / ps2_vu1_core.cpp)
// computes for the same instruction -- same float operations in the same order, same operand normalisation, same
// flag rules -- vectorised where the engine loops over components. The rare cases the engine decides from the
// unrounded ("exact", double) result go to out-of-line helpers in ps2_vu_lift.cpp that run the engine's formulas.
#ifndef PS2_VU_LIFT_RT_H
#define PS2_VU_LIFT_RT_H

#include "runtime/ps2_vu_lift.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

// Same floating-point mode as the engine: no contraction, no reassociation (the runtime builds with /fp:fast).
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

#if defined(_MSC_VER)
#define VL_INLINE __forceinline
#define VL_NOINLINE __declspec(noinline)
#else
#define VL_INLINE inline __attribute__((always_inline))
#define VL_NOINLINE __attribute__((noinline))
#endif

namespace vl
{
    // DEST field (x = 8) -> SSE lane mask (x = 1)
    constexpr int kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    enum Kind : int
    {
        kAdd = 0,
        kSub,
        kMul,
        kMadd,
        kMsub,
        kOpmsub,
        kOpmula
    };

    // --- out of line (ps2_vu_lift.cpp) ------------------------------------------------------------------------------
    // The engine's exact path for the dest lanes in mask m (SSE order) whose fast check failed: replaces r's lanes as
    // normalizeFmacExactResult does and returns the per-lane Z/S/U/O flags (4 bits per lane, lane k at bits 4k..4k+3).
    uint32_t fmacExact(int kind, float r[4], const float a[4], const float b[4], const float acc[4], int m);
    // productStickyFlags for the lanes in m; returns the OR of their 4-bit flags.
    uint32_t productStickyExact(const float a[4], const float b[4], const float p[4], int m);
    float eatan(float v);
    float esin(float v);
    float eexp(float v);
    void clipReport(uint32_t pc); // diagnostics

    // --- operands -----------------------------------------------------------------------------------------------------
    // normalizeOperand per lane: exponent 0 -> signed zero, exponent 255 -> signed FLT_MAX
    VL_INLINE __m128 nrm(__m128 v)
    {
        const __m128i b = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i e = _mm_and_si128(b, expMask);
        const __m128i sign = _mm_and_si128(b, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i zero = _mm_cmpeq_epi32(e, _mm_setzero_si128());
        const __m128i big = _mm_cmpeq_epi32(e, expMask);
        __m128i r = _mm_blendv_epi8(b, sign, zero);
        r = _mm_blendv_epi8(r, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF)), big);
        return _mm_castsi128_ps(r);
    }
    VL_INLINE float nrmf(float x)
    {
        uint32_t b;
        std::memcpy(&b, &x, 4);
        const uint32_t e = (b >> 23) & 0xFFu;
        if (e == 0u)
            b &= 0x80000000u;
        else if (e == 0xFFu)
            b = (b & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&x, &b, 4);
        return x;
    }
    VL_INLINE float bitsf(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    VL_INLINE uint32_t fbits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    VL_INLINE __m128 ld(const float *p) { return _mm_loadu_ps(p); }
    template <int M>
    VL_INLINE void st(float *p, __m128 v)
    {
        if constexpr (M == 15)
            _mm_storeu_ps(p, v);
        else if constexpr (M != 0)
            _mm_storeu_ps(p, _mm_blend_ps(_mm_loadu_ps(p), v, M));
    }
    template <int C>
    VL_INLINE __m128 bc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(C, C, C, C)); }
    // OPMSUB / OPMULA operands: lanes (y, z, x, w) of fs and (z, x, y, w) of ft
    VL_INLINE __m128 crossL(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 0, 2, 1)); }
    VL_INLINE __m128 crossR(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(3, 1, 0, 2)); }

    // biased exponent of each lane
    VL_INLINE __m128i expo(__m128 v) { return _mm_and_si128(_mm_srli_epi32(_mm_castps_si128(v), 23), _mm_set1_epi32(0xFF)); }
    // lanes with lo <= e <= hi
    VL_INLINE int inRange(__m128i e, int lo, int hi)
    {
        const __m128i ok = _mm_and_si128(_mm_cmpgt_epi32(e, _mm_set1_epi32(lo - 1)), _mm_cmpgt_epi32(_mm_set1_epi32(hi + 1), e));
        return _mm_movemask_ps(_mm_castsi128_ps(ok));
    }

    // MAC register from per-lane 4-bit flags (lane k at bits 4k): Z -> bit (3-k), S -> 4+(3-k), U -> 8+.., O -> 12+..
    VL_INLINE uint32_t macOf(uint32_t lf)
    {
        uint32_t mac = 0;
        for (int k = 0; k < 4; ++k)
        {
            const uint32_t f = (lf >> (4 * k)) & 0xFu;
            const uint32_t l = 8u >> k;
            mac |= ((f & 1u) ? l : 0u) | ((f & 2u) ? l << 4 : 0u) | ((f & 4u) ? l << 8 : 0u) | ((f & 8u) ? l << 12 : 0u);
        }
        return mac;
    }
    VL_INLINE uint32_t curOf(uint32_t lf) { return (lf | (lf >> 4) | (lf >> 8) | (lf >> 12)) & 0xFu; }

    // Slow halves of fin() (out of line, ps2_vu_lift.cpp): lanes whose fast check failed. finSlow returns the per-lane
    // flags of every dest lane (fast lanes: sign only) and fixes r's lanes; stickySlow returns the product sticky bits.
    uint32_t finSlow(int kind, __m128 *r, const __m128 *a, const __m128 *b, const __m128 *acc, int m, int fast);
    uint32_t stickySlow(const __m128 *a, const __m128 *b, const __m128 *p, int m, int fast);

    // normalizeFmacResult + updateFmacFlags + calculateFmacProductSticky for one FMAC with DEST lanes M (SSE order).
    // r = the float result as the engine computes it (all lanes), a/b = the per-lane operands (normalised; for OPMSUB
    // and OPMULA already in cross order), acc = normalised ACC, p = a*b (product kinds). Returns r with the dest lanes
    // normalised; mac / cur (status bits 0-3) / extra (product sticky) of the flag writer.
    template <int K, int M>
    VL_INLINE __m128 fin(__m128 r, __m128 a, __m128 b, __m128 acc, __m128 p, uint32_t &mac, uint32_t &cur, uint32_t &extra)
    {
        static_assert(M != 0, "an FMAC without DEST lanes pushes no flags");
        int fast;
        const __m128i er = expo(r);
        if constexpr (K == kAdd || K == kSub || K == kMul)
            fast = inRange(er, 1, 0xFD);
        else if constexpr (K == kMadd || K == kMsub || K == kOpmsub)
        {
            // productSumFlagsFast: er in [2, 0xFD] and er >= ep - 20
            const __m128i ep = expo(p);
            const __m128i ok = _mm_cmpgt_epi32(er, _mm_sub_epi32(ep, _mm_set1_epi32(21)));
            fast = inRange(er, 2, 0xFD) & _mm_movemask_ps(_mm_castsi128_ps(ok));
            if constexpr (K == kOpmsub)
                fast &= 7; // w: exact (0)
        }
        else
            fast = 0; // OPMULA: always the exact path
        if ((fast & M) == M)
        {
            const int s = _mm_movemask_ps(r) & M;
            mac = static_cast<uint32_t>(kRev4[s]) << 4;
            cur = s ? 0x2u : 0u;
        }
        else
        {
            const uint32_t lf = finSlow(K, &r, &a, &b, &acc, M, fast);
            mac = macOf(lf);
            cur = curOf(lf);
        }
        if constexpr (K == kMadd || K == kMsub || K == kOpmsub)
        {
            // productStickyFlags(left, right, left * right) of every dest lane
            const int pf = inRange(expo(p), 2, 0xFD);
            if ((pf & M) == M)
                extra = (_mm_movemask_ps(p) & M) ? 0x2u : 0u;
            else
                extra = stickySlow(&a, &b, &p, M, pf);
        }
        else
            extra = 0u;
        return r;
    }

    // status after a flag writer becomes visible (commitReadyFlags)
    VL_INLINE uint32_t statusFmac(uint32_t status, uint32_t cur, uint32_t extra)
    {
        return (status & 0xFF0u) | cur | ((cur | extra) << 6);
    }
    VL_INLINE uint32_t statusDiv(uint32_t status, uint32_t di) { return (status & 0xFCFu) | di | (di << 6); }

    // --- special FMAC-unit ops (no flags) -------------------------------------------------------------------------------
    template <int Shift>
    VL_INLINE __m128 itof(__m128 raw)
    {
        const __m128 v = _mm_cvtepi32_ps(_mm_castps_si128(raw)); // static_cast<float>(int32) under the VU rounding
        if constexpr (Shift == 4)
            return _mm_div_ps(v, _mm_set1_ps(16.0f));
        else if constexpr (Shift == 12)
            return _mm_div_ps(v, _mm_set1_ps(4096.0f));
        else if constexpr (Shift == 15)
            return _mm_div_ps(v, _mm_set1_ps(32768.0f));
        else
            return v;
    }
    // vuFloatToInt(v, scale) on normalised operands: the float product is exact or saturates to FLT_MAX (rounding
    // toward zero), so ">= 2^31 in float" is the engine's ">= INT_MAX in double"; cvtt gives INT_MIN below -2^31.
    template <int Shift>
    VL_INLINE __m128 ftoi(__m128 n)
    {
        constexpr float scale = Shift == 0 ? 1.0f : Shift == 4 ? 16.0f : Shift == 12 ? 4096.0f : 32768.0f;
        const __m128 v = Shift == 0 ? n : _mm_mul_ps(n, _mm_set1_ps(scale));
        const __m128 big = _mm_cmpge_ps(v, _mm_set1_ps(2147483648.0f));
        const __m128i i = _mm_cvttps_epi32(v);
        return _mm_blendv_ps(_mm_castsi128_ps(i), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)), big);
    }
    // CLIP on raw bits (the engine compares the bit patterns against |w|'s)
    VL_INLINE uint32_t clipFlags(const float *vs, const float *vt)
    {
        const uint32_t wBits = fbits(vt[3]);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        uint32_t f = 0;
        for (int c = 0; c < 3; ++c)
        {
            const uint32_t b = fbits(vs[c]);
            if (static_cast<int32_t>(b) > limit)
                f |= 1u << (2 * c);
            if (static_cast<int32_t>(b ^ 0x80000000u) > limit)
                f |= 2u << (2 * c);
        }
        return f;
    }

    // --- FDIV ---------------------------------------------------------------------------------------------------------
    // normalizeResult on a Q/P value (exponent 0 -> signed zero, 255 -> signed FLT_MAX)
    VL_INLINE float nres(float x) { return nrmf(x); }
    VL_INLINE float div(float numRaw, float denRaw, uint32_t &di)
    {
        const float num = nrmf(numRaw), den = nrmf(denRaw);
        float r;
        if (den == 0.0f)
        {
            di = num == 0.0f ? 0x10u : 0x20u;
            r = std::signbit(num) != std::signbit(den) ? -FLT_MAX : FLT_MAX;
        }
        else
        {
            di = 0u;
            r = num / den;
        }
        return nrmf(nrmf(r)); // DIV normalises, then queueQ normalises again
    }
    VL_INLINE float sqrt(float tRaw, uint32_t &di)
    {
        const float v = nrmf(tRaw);
        di = v < 0.0f ? 0x10u : 0u;
        return nrmf(std::sqrt(std::fabs(v)));
    }
    VL_INLINE float rsqrt(float sRaw, float tRaw, uint32_t &di)
    {
        const float num = nrmf(sRaw);
        const float radicand = nrmf(tRaw);
        const float den = std::sqrt(std::fabs(radicand));
        di = radicand < 0.0f ? 0x10u : 0u;
        float r;
        if (den != 0.0f)
            r = num / den;
        else
        {
            di = num == 0.0f ? 0x10u : 0x20u;
            r = std::signbit(num) ? -FLT_MAX : FLT_MAX;
        }
        return nrmf(nrmf(r));
    }

    // --- EFU (values as the engine's lower.cpp; queueP normalises) ------------------------------------------------------
    VL_INLINE float esadd(const float *v)
    {
        const float x = nrmf(v[0]), y = nrmf(v[1]), z = nrmf(v[2]);
        return nrmf(x * x + y * y + z * z);
    }
    VL_INLINE float ersadd(const float *v)
    {
        const float x = nrmf(v[0]), y = nrmf(v[1]), z = nrmf(v[2]);
        const float sum = x * x + y * y + z * z;
        return nrmf(sum != 0.0f ? 1.0f / sum : sum);
    }
    VL_INLINE float eleng(const float *v)
    {
        const float x = nrmf(v[0]), y = nrmf(v[1]), z = nrmf(v[2]);
        return nrmf(std::sqrt(x * x + y * y + z * z));
    }
    VL_INLINE float erleng(const float *v)
    {
        const float x = nrmf(v[0]), y = nrmf(v[1]), z = nrmf(v[2]);
        const float len = std::sqrt(x * x + y * y + z * z);
        return nrmf(len != 0.0f ? 1.0f / len : len);
    }
    VL_INLINE float eatanxy(const float *v)
    {
        const float x = nrmf(v[0]), y = nrmf(v[1]);
        return nrmf(x != 0.0f ? eatan(y / x) : 0.0f);
    }
    VL_INLINE float eatanxz(const float *v)
    {
        const float x = nrmf(v[0]), z = nrmf(v[2]);
        return nrmf(x != 0.0f ? eatan(z / x) : 0.0f);
    }
    VL_INLINE float esum(const float *v)
    {
        float sum = 0.0f;
        for (int c = 0; c < 4; ++c)
            sum += nrmf(v[c]);
        return nrmf(sum);
    }
    VL_INLINE float ersqrt(float raw)
    {
        float r = nrmf(raw);
        if (r >= 0.0f)
        {
            r = std::sqrt(r);
            if (r != 0.0f)
                r = 1.0f / r;
        }
        return nrmf(r);
    }
    VL_INLINE float esqrt(float raw)
    {
        const float v = nrmf(raw);
        return nrmf(v >= 0.0f ? std::sqrt(v) : v);
    }
    VL_INLINE float ercpr(float raw)
    {
        const float v = nrmf(raw);
        return nrmf(v != 0.0f ? 1.0f / v : v);
    }
    VL_INLINE float esinv(float raw) { return nrmf(esin(nrmf(raw))); }
    VL_INLINE float eatanv(float raw) { return nrmf(eatan(nrmf(raw))); }
    VL_INLINE float eexpv(float raw) { return nrmf(eexp(nrmf(raw))); }

    // --- memory -------------------------------------------------------------------------------------------------------
    VL_INLINE uint32_t addr(const VuLiftCtx &X, int32_t qw) { return (static_cast<uint32_t>(qw) * 16u) & X.dmask; }
    VL_INLINE __m128 ldq(const VuLiftCtx &X, uint32_t a) { return _mm_loadu_ps(reinterpret_cast<const float *>(X.data + a)); }
    template <int M>
    VL_INLINE void stq(VuLiftCtx &X, uint32_t a, __m128 v) { st<M>(reinterpret_cast<float *>(X.data + a), v); }
    VL_INLINE int32_t ldw(const VuLiftCtx &X, uint32_t a, int comp)
    {
        uint32_t v;
        std::memcpy(&v, X.data + a + comp * 4, 4);
        return static_cast<int16_t>(v & 0xFFFFu);
    }
    template <int D> // DEST bits (x = 8)
    VL_INLINE void stw(VuLiftCtx &X, uint32_t a, int32_t value)
    {
        const uint32_t w = static_cast<uint16_t>(value & 0xFFFF);
        if constexpr ((D & 8) != 0) std::memcpy(X.data + a, &w, 4);
        if constexpr ((D & 4) != 0) std::memcpy(X.data + a + 4, &w, 4);
        if constexpr ((D & 2) != 0) std::memcpy(X.data + a + 8, &w, 4);
        if constexpr ((D & 1) != 0) std::memcpy(X.data + a + 12, &w, 4);
    }
    VL_INLINE __m128 splatBits(int32_t v) { return _mm_castsi128_ps(_mm_set1_epi32(v)); }
    template <int C>
    VL_INLINE int32_t laneBits(const float *v)
    {
        uint32_t b;
        std::memcpy(&b, v + C, 4);
        return static_cast<int32_t>(b);
    }
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

#endif
