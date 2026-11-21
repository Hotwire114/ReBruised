// GOW-Port: render-target GS renderer, see include/runtime/gs/gs_hw_backend.h.
#include "runtime/gs/gs_hw_backend.h"
#include "gs_texture_store.h"
#include "runtime/gs/gs_swizzle.h"
#include "gs_gl.h"
#include "gs_gl_state_cache.h"
#include "../gs_gpu_helpers.h"
#include "runtime/gs/gs_shared_present.h"
#include "runtime/ps2_perf_stats.h"
#include "runtime/ps2_button_prompts.h"

#include <map>
namespace ps2_stubs { uint32_t mpegPicturesServed() noexcept; } // Kernel/Stubs/MPEG.cpp
#include <atomic>
// Black & Bruised: the HUD filter can be switched while playing (F8, ps2_runtime.cpp): 0 soft, 1 sharp, 2 hd
static std::atomic<int> g_hudModeLive{-1};
void ps2CycleHudMode()
{
    int m = g_hudModeLive.load();
    m = m == 2 ? 0 : m == 0 ? 1 : 2;
    g_hudModeLive.store(m);
    std::fprintf(stderr, "[hud] %s (F8)\n", m == 2 ? "hd" : m == 1 ? "sharp" : "soft");
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using namespace GSGL;

    constexpr GLenum kArrayBuffer = 0x8892;
    constexpr GLenum kFloat = 0x1406;
    constexpr GLenum kColorAttachment0 = 0x8CE0;
    constexpr GLenum kDepthAttachment = 0x8D00;
    constexpr GLenum kFramebufferComplete = 0x8CD5;
    constexpr GLenum kDepthComponent32F = 0x8CAC;
    constexpr GLenum kDepthComponent = 0x1902;
    constexpr GLenum kBlend = 0x0BE2;
    constexpr GLenum kDepthTest = 0x0B71;
    constexpr GLenum kScissorTest = 0x0C11;
    constexpr GLenum kCullFace = 0x0B44;
    constexpr GLenum kOne = 1, kZero = 0;
    constexpr GLenum kSrc1Alpha = 0x8589, kOneMinusSrc1Alpha = 0x88FB;
    constexpr GLenum kConstantColor = 0x8001, kOneMinusConstantColor = 0x8002;
    constexpr GLenum kFuncAdd = 0x8006, kFuncReverseSubtract = 0x800B;
    constexpr GLbitfield kColorBufferBit = 0x4000, kDepthBufferBit = 0x100;
    constexpr GLenum kNever = 0x200, kLess = 0x201, kLEqual = 0x203, kGreater = 0x204, kGEqual = 0x206, kAlways = 0x207;
    constexpr GLenum kTexture0 = 0x84C0;
    constexpr GLint kRepeat = 0x2901;
    constexpr GLenum kUnpackAlignment = 0x0CF5, kPackAlignment = 0x0D05;
    constexpr GLenum kFalse = 0, kTrue = 1;

    const char *kVsSource = R"(#version 430 core
layout(location = 0) in vec2 aPos;     // PS2 pixels, centre-adjusted
layout(location = 1) in float aZ;      // 0..1
layout(location = 2) in vec4 aCol;     // 0..255
layout(location = 3) in vec3 aSTQ;
layout(location = 4) in vec2 aUV;      // texels
layout(location = 5) in vec4 aUVClamp; // texels: the range the GS samples (sprites), else unbounded
uniform vec2 uInvSize;
noperspective out vec4 vCol;
flat out vec4 vColFlat;
noperspective out vec3 vSTQ;
noperspective out vec2 vUV;
flat out vec4 vUVClamp;
void main()
{
    gl_Position = vec4(aPos * uInvSize * 2.0 - 1.0, aZ * 2.0 - 1.0, 1.0);
    vCol = aCol;
    vColFlat = aCol;
    vSTQ = aSTQ;
    vUV = aUV;
    vUVClamp = aUVClamp;
}
)";

    const char *kFsSource = R"(#version 430 core
noperspective in vec4 vCol;
flat in vec4 vColFlat;
noperspective in vec3 vSTQ;
noperspective in vec2 vUV;
flat in vec4 vUVClamp;
layout(location = 0, index = 0) out vec4 oColor;
layout(location = 0, index = 1) out vec4 oBlend;
uniform sampler2D uTex;
uniform vec2 uTexSize;   // size the sampled image is normalised by (render targets: their own size)
uniform vec2 uTexDim;    // TEX0 TW/TH: size STQ coordinates are scaled by

// Reinterpreting fetch: the texture lives in the memory a render target was drawn into, but with another format / width /
// layout (16 bit view of a 32 bit target, T8H = alpha plane, other pitch...). The texel's GS memory address is computed
// from the texture format, mapped back to the render-target pixel that owns it and the bits are re-extracted: pure address
// math on the GPU, no memory image.
uniform int uReinterp;
uniform int uDate, uDatm;   // destination alpha test (reads the target itself)
// GS blend done in the shader (reads the target): destination-alpha factor (Ad, 0x80 = 1.0) and COLCLAMP=0 wrap-around,
// which fixed-function blending cannot express. uAbcd = ALPHA A, B, C, D selectors.
uniform int uSwBlend, uColClamp;
uniform ivec4 uAbcd;
uniform float uFix;
// Z buffer in the colour memory of the target itself (ZBP == FBP): the test reads the Z word from the target's colour,
// a passing write stores the Z word there. 0 = off, else 1 + ZTST.
uniform int uZAlias, uZAliasWrite;
uniform float uZAliasMax;
uniform sampler2D uDest;
uniform int uZTex;     // 16/24/32: sampling a depth buffer (Z16/Z24/Z32 texture)
uniform float uZMax;
)";

    // Shared by the draw shader and the CLUT fetch: a texel of a texture format read from a render target's memory.
    const char *kReinterpretSource = R"(
uniform usampler2D uTables;
uniform sampler2D uRtColor;
uniform sampler2D uPalette;
uniform ivec4 uTexFmt;   // pageShiftX, pageShiftY, pixelsPerPageShift, unpackedShift
uniform ivec4 uTexFmt2;  // bitOffset, texelBits, tableOffset, kind (0 CT32, 1 CT24, 2 CT16, 3 indexed)
uniform ivec4 uRtFmt;    // pageShiftX, pageShiftY, pixelsPerPageShift, bits per pixel
uniform ivec4 uRtFmt2;   // inverseTableOffset, fbp (pages), fbw (64 px), 0
uniform ivec4 uTex0;     // tbp, tbw, width, height
uniform vec2 uRtPx;      // render target size in PS2 pixels

uint tab(uint i) { return texelFetch(uTables, ivec2(int(i & 2047u), int(i >> 11)), 0).r; }

vec4 reinterpretTexel(ivec2 p)
{
    uint x = uint(p.x), y = uint(p.y);
    uint psx = uint(uTexFmt.x), psy = uint(uTexFmt.y);
    uint pagesPerRow = (uint(uTex0.y) * 64u) >> psx;
    uint page = (uint(uTex0.x) >> 5) + (y >> psy) * pagesPerRow + (x >> psx);
    uint entry = ((uint(uTex0.x) & 31u) << (psx + psy)) + ((y & ((1u << psy) - 1u)) << psx) + (x & ((1u << psx) - 1u));
    uint pixelIndex = (page << uint(uTexFmt.z)) + tab(uint(uTexFmt2.z) + entry);
    uint bitpos = (pixelIndex << uint(uTexFmt.w)) + uint(uTexFmt2.x);
    uint B = uint(uRtFmt.w);
    uint rtIdx = bitpos / B;
    uint within = bitpos % B;
    uint rtBase = uint(uRtFmt2.y) << uint(uRtFmt.z);
    if (rtIdx < rtBase) return vec4(1.0, 0.0, 1.0, 1.0);
    uint rel = rtIdx - rtBase;
    uint rpage = rel >> uint(uRtFmt.z);
    uint inpage = rel & ((1u << uint(uRtFmt.z)) - 1u);
    uint e = tab(uint(uRtFmt2.x) + inpage);
    uint rpsx = uint(uRtFmt.x), rpsy = uint(uRtFmt.y);
    uint fbw = uint(uRtFmt2.z);
    uint px = (rpage % fbw) * (1u << rpsx) + (e & ((1u << rpsx) - 1u));
    uint py = (rpage / fbw) * (1u << rpsy) + (e >> rpsx);
    vec4 c = texture(uRtColor, (vec2(float(px), float(py)) + 0.5) / uRtPx) * 255.0;
    c = floor(c + 0.5);
    uint word;
    if (B == 32u)
        word = uint(c.r) | (uint(c.g) << 8) | (uint(c.b) << 16) | (uint(c.a) << 24);
    else
        word = (c.a >= 128.0 ? 0x8000u : 0u) | ((uint(c.b) >> 3) << 10) | ((uint(c.g) >> 3) << 5) | (uint(c.r) >> 3);
    uint bits = uint(uTexFmt2.y);
    uint value = (bits >= 32u) ? (word >> within) : ((word >> within) & ((1u << bits) - 1u));
    int kind = uTexFmt2.w;
    if (kind == 0) return vec4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float((value >> 24) & 255u)) / 255.0;
    if (kind == 1) return vec4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), 128.0) / 255.0; // alpha via uRtTex24 path
    if (kind == 2)
    {
        vec3 rgb = vec3(float(value & 31u), float((value >> 5) & 31u), float((value >> 10) & 31u)) * 8.0;
        return vec4(rgb, ((value & 0x8000u) != 0u) ? 128.0 : 0.0) / 255.0;
    }
    return texelFetch(uPalette, ivec2(int(value & 255u), 0), 0);
}
)";

    const char *kFsTail = R"(
// Paletted textures (T8/T4 and their H variants): uTex holds the indices (R8, cached per VRAM upload), the colour comes
// from uIdxPalette (256x1, cached per CLUT); filtering is done on the looked-up colours like the GS.
uniform int uIndexed, uLinear;
// Tekken 4 port: per axis (x, y) the GS CLAMP mode (WMS, WMT): 0 repeat, 1 clamp, 2 region clamp, 3 region repeat, with
// uRegion = (MINU, MAXU, MINV, MAXV) (region clamp bounds; UMSK/UFIX for region repeat). Tekken's stage textures are
// sub-rectangles of 1024x1024 T4/T8 pages picked this way; treating 2/3 as clamp drew them as flat colours.
uniform ivec4 uWrap;
uniform ivec4 uRegion;
uniform sampler2D uIdxPalette;
int wrapAxis(int c, int size, int mode, int lo, int hi)
{
    if (mode == 0) return ((c % size) + size) % size;
    if (mode == 2) c = clamp(c, lo, hi);
    else if (mode == 3) c = (c & lo) | hi;
    return clamp(c, 0, size - 1);
}
// the region modes on the axes that use them (the others are left to the sampler's wrap)
ivec2 regionTexel(ivec2 p, ivec2 size)
{
    if (uWrap.x >= 2) p.x = wrapAxis(p.x, size.x, uWrap.x, uRegion.x, uRegion.y);
    if (uWrap.y >= 2) p.y = wrapAxis(p.y, size.y, uWrap.y, uRegion.z, uRegion.w);
    return p;
}
vec4 indexedTexel(ivec2 p)
{
    ivec2 size = max(ivec2(uTexDim), ivec2(1));
    p.x = wrapAxis(p.x, size.x, uWrap.x, uRegion.x, uRegion.y);
    p.y = wrapAxis(p.y, size.y, uWrap.y, uRegion.z, uRegion.w);
    int idx = int(texelFetch(uTex, p, 0).r * 255.0 + 0.5);
    return texelFetch(uIdxPalette, ivec2(idx, 0), 0);
}
uniform int uTme, uFst, uTfx, uTcc, uIip;
uniform int uRtTex24; // sampling a render target as PSMCT24 (1) or PSMCT16 (2): alpha comes from TEXA
uniform float uTa0;
uniform float uTa1;
uniform int uAem;
uniform int uAte, uAtst, uAref;
uniform int uFailTint; // debug: GOW_GS_HW_FAILTINT paints alpha-test failing fragments magenta
uniform int uPass; // 0 = no alpha test, 1 = keep passing fragments, 2 = keep failing fragments
uniform float uBlendScale; // 1/128 style: alpha factor = min(a * scale, 1)
// Tekken 4 port (PC graphics options, docs/GRAPHICS.md): uAnisoN = anisotropic taps (1 = off) for filtered 3D draws,
// uSharp = sharp bilinear for 2D sprites of game textures magnified at the internal resolution, uRegionLinear = bilinear
// filtering for the CLAMP region modes (the GS filters them; they were point sampled).
uniform int uAnisoN, uSharp, uRegionLinear;

float clamp255(float v) { return clamp(floor(v), 0.0, 255.0); }

// the host texel holding the centre of texel c (render targets are scaled): never on a texel edge
vec4 hostTexel(vec2 c)
{
    vec2 host = vec2(textureSize(uTex, 0));
    return textureLod(uTex, (floor(c * host / uTexSize + 0.001) + 0.5) / host, 0.0);
}

// one texture lookup at texel coordinate uv (0..255 per channel, not rounded); no implicit derivatives, so it can be
// called in loops (anisotropic taps)
vec4 fetchTexel(vec2 uv)
{
    vec4 t;
    if (uReinterp != 0)
    {
        ivec2 ip = ivec2(floor(uv));
        ip = ivec2(clamp(ip.x, 0, uTex0.z - 1), clamp(ip.y, 0, uTex0.w - 1));
        t = reinterpretTexel(ip) * 255.0;
    }
    else if (uIndexed != 0)
    {
        if (uLinear != 0)
        {
            vec2 f = uv - 0.5;
            ivec2 i0 = ivec2(floor(f));
            vec2 w = f - floor(f);
            t = mix(mix(indexedTexel(i0), indexedTexel(i0 + ivec2(1, 0)), w.x),
                    mix(indexedTexel(i0 + ivec2(0, 1)), indexedTexel(i0 + ivec2(1, 1)), w.x), w.y) * 255.0;
        }
        else
            t = indexedTexel(ivec2(floor(uv))) * 255.0;
    }
    else if (uZTex != 0)
    {
        vec2 zhost = vec2(textureSize(uTex, 0)); // depth is always point sampled: the host texel's centre
        float zf = textureLod(uTex, (floor(uv * zhost / uTexSize + 0.001) + 0.5) / zhost, 0.0).r;
        uint zi = uint(clamp(zf, 0.0, 1.0) * uZMax + 0.5);
        if (uZTex == 16)
        {
            vec3 rgb = vec3(float(zi & 31u), float((zi >> 5) & 31u), float((zi >> 10) & 31u)) * 8.0;
            t = vec4(rgb, ((zi & 0x8000u) != 0u) ? 128.0 : 0.0);
        }
        else
            t = vec4(float(zi & 255u), float((zi >> 8) & 255u), float((zi >> 16) & 255u), 128.0);
    }
    else if (uLinear == 0 || uWrap.x >= 2 || uWrap.y >= 2)
    {
        bool region = uWrap.x >= 2 || uWrap.y >= 2;
        ivec2 size = max(ivec2(uTexDim), ivec2(1));
        if (region && uLinear != 0 && uRegionLinear != 0)
        {
            // bilinear like the GS: the region wrap applies to each of the four texels
            vec2 f = uv - 0.5;
            ivec2 i0 = ivec2(floor(f));
            vec2 w = f - floor(f);
            t = mix(mix(hostTexel(vec2(regionTexel(i0, size)) + 0.5), hostTexel(vec2(regionTexel(i0 + ivec2(1, 0), size)) + 0.5), w.x),
                    mix(hostTexel(vec2(regionTexel(i0 + ivec2(0, 1), size)) + 0.5), hostTexel(vec2(regionTexel(i0 + ivec2(1, 1), size)) + 0.5), w.x), w.y) * 255.0;
        }
        else
        {
            // Tekken 4 port: region clamp/repeat picks the texel in the shader (point sampled)
            if (region)
                uv = vec2(regionTexel(ivec2(floor(uv)), size)) + 0.5;
            t = hostTexel(uv) * 255.0;
        }
    }
    else
        t = textureLod(uTex, uv / uTexSize, 0.0) * 255.0;
    return t;
}

// Black & Bruised (HUD = hd, 2026-11-17): magnified 2D sprites (health bars, pop-up icons, stars, timer) sampled with
// a Catmull-Rom bicubic instead of bilinear, clamped to the range of the four nearest texels (no halos or ringing at the
// art's hard outlines). At 1080p the PS2's HUD textures are magnified ~2.4x: bilinear smears them, this keeps edges and
// gradients crisp. The 4x4 taps stay inside the sprite's texel range (vUVClamp).
vec4 cubicWeights(float x)
{
    float x2 = x * x, x3 = x2 * x;
    return vec4(-0.5 * x3 + x2 - 0.5 * x,
                1.5 * x3 - 2.5 * x2 + 1.0,
                -1.5 * x3 + 2.0 * x2 + 0.5 * x,
                0.5 * x3 - 0.5 * x2);
}

vec4 hdTexel(vec2 uv)
{
    vec2 f = uv - 0.5;
    vec2 fl = floor(f);
    vec2 fr = f - fl;
    vec4 wx = cubicWeights(fr.x), wy = cubicWeights(fr.y);
    vec4 sum = vec4(0.0);
    vec4 lo = vec4(1e9), hi = vec4(-1e9);
    for (int j = 0; j < 4; ++j)
    {
        vec4 row = vec4(0.0);
        for (int i = 0; i < 4; ++i)
        {
            vec2 c = clamp(fl + vec2(float(i) - 0.5, float(j) - 0.5), vUVClamp.xy, vUVClamp.zw);
            vec4 s = fetchTexel(c);
            row += s * wx[i];
            if ((i == 1 || i == 2) && (j == 1 || j == 2))
            {
                lo = min(lo, s);
                hi = max(hi, s);
            }
        }
        sum += row * wy[j];
    }
    return clamp(sum, lo, hi);
}

bool alphaPasses(float a)
{
    int ai = int(a);
    switch (uAtst)
    {
    case 0: return false;
    case 1: return true;
    case 2: return ai < uAref;
    case 3: return ai <= uAref;
    case 4: return ai == uAref;
    case 5: return ai >= uAref;
    case 6: return ai > uAref;
    default: return ai != uAref;
    }
}

vec3 blendSel(int sel, vec3 cs, vec3 cd) { return sel == 0 ? cs : sel == 1 ? cd : vec3(0.0); }

void main()
{
    uint zFrag = uint(clamp(gl_FragCoord.z, 0.0, 1.0) * uZAliasMax + 0.5);
    if (uZAlias != 0)
    {
        vec4 d = floor(texelFetch(uDest, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
        uint zd = uint(d.r) | (uint(d.g) << 8) | (uint(d.b) << 16);
        int ztst = uZAlias - 1;
        bool pass = ztst == 1 || (ztst == 2 && zFrag >= zd) || (ztst == 3 && zFrag > zd);
        if (!pass) discard;
    }
    if (uDate != 0)
    {
        bool bit = texelFetch(uDest, ivec2(gl_FragCoord.xy), 0).a >= 0.5;
        if (bit != (uDatm != 0)) discard;
    }
    vec4 v = (uIip != 0) ? vCol : vColFlat;
    vec3 rgb = v.rgb;
    float a = v.a;
    if (uTme != 0)
    {
        // U = S/Q with the sign of Q: a negative Q flips both coordinates (the menu Kratos has one)
        float q = (abs(vSTQ.z) < 1e-30) ? 1e-30 : vSTQ.z;
        vec2 uv = (uFst != 0) ? vUV : (vSTQ.xy / q) * uTexDim;
        // texel footprint of this host pixel (uniform control flow: uTme is a uniform)
        vec2 duvx = dFdx(uv), duvy = dFdy(uv);
        if (uSharp == 1)
        {
            // sharp bilinear: inside a magnified texel its own colour, a one host pixel wide blend at its edges
            vec2 ppt = 1.0 / max(abs(duvx) + abs(duvy), vec2(1e-5)); // host pixels per texel
            vec2 f = uv - 0.5;
            vec2 fl = floor(f);
            uv = fl + clamp((f - fl - 0.5) * max(ppt, vec2(1.0)) + 0.5, 0.0, 1.0) + 0.5;
        }
        // at the internal scale a pixel samples between the GS's sample points: keep it inside the range the GS samples
        // (sprites), so filtering never reaches the texel beyond the primitive's last one
        uv = clamp(uv, vUVClamp.xy, vUVClamp.zw);
        // point sampling picks texel floor(u) of the GS's 1/16 texel coordinate: truncate like the GS (the 1/64 step
        // absorbs float interpolation error, a 1:1 copy lands exactly on texel edges)
        if (uLinear == 0)
            uv = floor(uv * 16.0 + 0.015625) / 16.0;
        vec4 t;
        float lx = length(duvx), ly = length(duvy);
        if (uAnisoN > 1 && max(lx, ly) > 1.0)
        {
            // anisotropic filtering without mipmaps: up to uAnisoN filtered taps along the footprint's long axis
            vec2 major = lx >= ly ? duvx : duvy;
            int n = int(clamp(ceil(max(lx, ly) / max(min(lx, ly), 1e-4)), 1.0, float(uAnisoN)));
            n = min(n, int(ceil(max(lx, ly))) + 1);
            t = vec4(0.0);
            for (int i = 0; i < n; ++i)
                t += fetchTexel(clamp(uv + major * ((float(i) + 0.5) / float(n) - 0.5), vUVClamp.xy, vUVClamp.zw));
            t /= float(n);
        }
        else if (uSharp == 2)
            t = hdTexel(uv);
        else
            t = fetchTexel(uv);
        t = floor(t + 0.5);
        if (uRtTex24 == 1)
            t.a = (uAem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : uTa0;
        else if (uRtTex24 == 2)
            t.a = (uAem != 0 && t.r + t.g + t.b == 0.0) ? 0.0 : ((t.a >= 128.0) ? uTa1 : uTa0);
        float ta = (uTcc != 0) ? t.a : v.a;
        if (uTfx == 0)
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0), clamp255(t.g * v.g / 128.0), clamp255(t.b * v.b / 128.0));
            a = (uTcc != 0) ? clamp255(t.a * v.a / 128.0) : v.a;
        }
        else if (uTfx == 1)
        {
            rgb = t.rgb;
            a = ta;
        }
        else if (uTfx == 2)
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = (uTcc != 0) ? clamp255(t.a + v.a) : v.a;
        }
        else
        {
            rgb = vec3(clamp255(t.r * v.r / 128.0 + v.a), clamp255(t.g * v.g / 128.0 + v.a), clamp255(t.b * v.b / 128.0 + v.a));
            a = ta;
        }
    }
    if (uPass != 0)
    {
        bool pass = (uAte == 0) || alphaPasses(a);
        if (uPass == 1 && !pass) discard;
        if (uPass == 2 && pass) discard;
        if (uPass == 2 && uFailTint != 0) { rgb = vec3(255.0, 0.0, 255.0); a = 128.0; }
    }
    if (uSwBlend != 0)
    {
        vec4 d = floor(texelFetch(uDest, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
        vec3 cs = floor(rgb);
        float c = uAbcd.z == 0 ? a : uAbcd.z == 1 ? d.a : uFix;
        vec3 r = floor((blendSel(uAbcd.x, cs, d.rgb) - blendSel(uAbcd.y, cs, d.rgb)) * c / 128.0) + blendSel(uAbcd.w, cs, d.rgb);
        rgb = (uColClamp != 0) ? clamp(r, 0.0, 255.0) : r - 256.0 * floor(r / 256.0);
    }
    if (uZAlias != 0 && uZAliasWrite != 0)
        rgb = vec3(float(zFrag & 255u), float((zFrag >> 8) & 255u), float((zFrag >> 16) & 255u));
    oColor = vec4(rgb / 255.0, a / 255.0);
    float f = min(a * uBlendScale, 1.0);
    oBlend = vec4(f);
}
)";

    // Channel shuffle: the game moves bytes between the colour channels of a 32 bit target (or out of the Z buffer) by drawing
    // 8 px sprites through a 16 bit view of that memory. In a 16 bit view, the two 8 px halves of each 16 px column group are
    // the low and high halfwords of the same 8 owner pixels, so every such sprite is a halfword move between owner pixels:
    // one quad per 8x8 owner block, source = owner pixel of the texel (+ offset), halfword selected by uHs / uHd. The written
    // bytes are chosen by the colour mask (FBMSK). The alpha bit goes through TEXA (uTexa), the colour bits are copied.
    const char *kShuffleVs = R"(#version 430 core
layout(location = 0) in vec2 aPos;   // destination owner pixel (PS2 px)
layout(location = 1) in vec2 aOff;   // source owner pixel - destination owner pixel
uniform vec2 uInvSize;
flat out vec2 vOff;
void main()
{
    gl_Position = vec4(aPos * uInvSize * 2.0 - 1.0, 0.0, 1.0);
    vOff = aOff;
}
)";
    const char *kShuffleFs = R"(#version 430 core
flat in vec2 vOff;
out vec4 oColor;
uniform sampler2D uSrc;
uniform int uSrcZ;     // source is a depth buffer (Z words)
uniform float uZMax;
uniform int uHs, uHd;  // source / destination halfword (0 low = R,G; 1 high = B,A)
uniform float uScale;
uniform ivec4 uTexa;   // TA0 >> 7, TA1 >> 7, AEM: what the 16 bit texel's alpha bit becomes after expansion and re-packing
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) + ivec2(round(vOff * uScale));
    vec4 c;
    if (uSrcZ != 0)
    {
        uint z = uint(clamp(texelFetch(uSrc, p, 0).r, 0.0, 1.0) * uZMax + 0.5);
        c = vec4(float(z & 255u), float((z >> 8) & 255u), float((z >> 16) & 255u), float(z >> 24)) / 255.0;
    }
    else
        c = texelFetch(uSrc, p, 0);
    vec2 h = (uHs == 0) ? c.rg : c.ba;
    uint v = uint(h.x * 255.0 + 0.5) | (uint(h.y * 255.0 + 0.5) << 8);
    uint rgb = v & 0x7FFFu;
    uint a = (uTexa.z != 0 && rgb == 0u) ? 0u : uint((v & 0x8000u) != 0u ? uTexa.y : uTexa.x);
    v = rgb | (a << 15);
    h = vec2(float(v & 255u), float(v >> 8)) / 255.0;
    oColor = (uHd == 0) ? vec4(h, 0.0, 0.0) : vec4(0.0, 0.0, h);
}
)";

    struct ShuffleVertex
    {
        float x, y, ox, oy;
    };

    // Display output: copies one read circuit (offset DBX/DBY) into the presentation framebuffer; circuit 1 is blended over
    // circuit 2 by fixed-function blending (ALP or its own alpha x2).
    const char *kPresentVs = R"(#version 430 core
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    const char *kPresentFs = R"(#version 430 core
out vec4 oColor;
uniform sampler2D uSrc;
uniform ivec4 uSrcOffset; // x, y: source pixel of output pixel 0; z: 1 = CT24 (alpha 0x80); w: 1 = alpha x2 as blend factor
uniform int uSrcEdge; // first source row/column shown; the ones before it repeat it
// Tekken 4 port (CPU backend's decodeDisplayLineRepeat / displayField): x = output lines per buffer line ((MAGV+1), x2 in
// interlaced FRAME mode), y = output lines the picture starts lower (odd field of an interlaced FRAME-mode picture).
uniform ivec4 uLineRep;
void main()
{
    ivec2 q = ivec2(gl_FragCoord.xy);
    q.y = max(q.y - uLineRep.y, 0) / max(uLineRep.x, 1);
    ivec2 p = max(q + uSrcOffset.xy, ivec2(uSrcEdge));
    ivec2 size = textureSize(uSrc, 0);
    vec4 c = (p.x < size.x && p.y < size.y) ? texelFetch(uSrc, p, 0) : vec4(0.0);
    float a = (uSrcOffset.z != 0) ? 128.0 / 255.0 : c.a;
    oColor = vec4(c.rgb, (uSrcOffset.w != 0) ? min(1.0, a * 2.0) : a);
}
)";

    // Tekken 4 port (PC graphics options): post passes on the presented picture. Area resample (SSAA resolve, the
    // internal picture rendered larger and averaged down) and FXAA 3.11 (quality preset, after Timothy Lottes).
    const char *kAreaFs = R"(#version 430 core
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec2 uRatio; // source pixels per output pixel
void main()
{
    vec2 size = vec2(textureSize(uSrc, 0));
    vec2 lo = floor(gl_FragCoord.xy) * uRatio;
    ivec2 n = ivec2(clamp(ceil(uRatio) + 1.0, 1.0, 8.0));
    vec4 sum = vec4(0.0);
    for (int y = 0; y < n.y; ++y)
        for (int x = 0; x < n.x; ++x)
            sum += textureLod(uSrc, (lo + (vec2(x, y) + 0.5) * uRatio / vec2(n)) / size, 0.0);
    oColor = vec4(sum.rgb / float(n.x * n.y), 1.0);
}
)";
    // Tekken 4 port: smooth upscale of a picture uploaded into a render target at an internal scale above 1 (the intro
    // movie). Catmull-Rom bicubic from the native-size staging copy, clamped to the 2x2 nearest texels (no halos): bilinear
    // at 3x and more shows the source pixel grid as blocks. Taps outside uBounds repeat the border texel.
    const char *kMovieScaleFs = R"(#version 430 core
out vec4 oColor;
uniform sampler2D uSrc;
uniform vec2 uScale;    // output pixels per source pixel
uniform ivec4 uBounds;  // first / last source texel (x0, y0, x1, y1) that may be read
vec4 tap(ivec2 p) { return texelFetch(uSrc, clamp(p, uBounds.xy, uBounds.zw), 0); }
vec4 weights(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
void main()
{
    vec2 p = gl_FragCoord.xy / uScale - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 wx = weights(f.x), wy = weights(f.y);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; ++y)
    {
        vec4 row = vec4(0.0);
        for (int x = 0; x < 4; ++x)
            row += wx[x] * tap(i + ivec2(x - 1, y - 1));
        sum += wy[y] * row;
    }
    vec4 a = tap(i), b = tap(i + ivec2(1, 0)), c = tap(i + ivec2(0, 1)), d = tap(i + ivec2(1, 1));
    oColor = clamp(sum, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}
)";
    const char *kFxaaFs = R"(#version 430 core
out vec4 oColor;
uniform sampler2D uSrc;
uniform float uSubpix;
float luma(vec3 c) { return sqrt(dot(c, vec3(0.299, 0.587, 0.114))); }
float lumaAt(vec2 uv) { return luma(textureLod(uSrc, uv, 0.0).rgb); }
const float kQuality[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
void main()
{
    vec2 inv = 1.0 / vec2(textureSize(uSrc, 0));
    vec2 uv = gl_FragCoord.xy * inv;
    vec3 cC = textureLod(uSrc, uv, 0.0).rgb;
    float lC = luma(cC);
    float lD = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(0, -1)).rgb), lU = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(0, 1)).rgb);
    float lL = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(-1, 0)).rgb), lR = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(1, 0)).rgb);
    float lMin = min(lC, min(min(lD, lU), min(lL, lR))), lMax = max(lC, max(max(lD, lU), max(lL, lR)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) { oColor = vec4(cC, 1.0); return; }
    float lDL = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(-1, -1)).rgb), lUR = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(1, 1)).rgb);
    float lUL = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(-1, 1)).rgb), lDR = luma(textureLodOffset(uSrc, uv, 0.0, ivec2(1, -1)).rgb);
    float lDU = lD + lU, lLR = lL + lR, lLc = lDL + lUL, lDc = lDL + lDR, lRc = lDR + lUR, lUc = lUR + lUL;
    float edgeH = abs(-2.0 * lL + lLc) + abs(-2.0 * lC + lDU) * 2.0 + abs(-2.0 * lR + lRc);
    float edgeV = abs(-2.0 * lU + lUc) + abs(-2.0 * lC + lLR) * 2.0 + abs(-2.0 * lD + lDc);
    bool horz = edgeH >= edgeV;
    float l1 = horz ? lD : lL, l2 = horz ? lU : lR;
    float g1 = l1 - lC, g2 = l2 - lC;
    bool steep1 = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float stepLen = horz ? inv.y : inv.x;
    float lLocal;
    if (steep1) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lC); } else lLocal = 0.5 * (l2 + lC);
    vec2 cuv = uv;
    if (horz) cuv.y += stepLen * 0.5; else cuv.x += stepLen * 0.5;
    vec2 off = horz ? vec2(inv.x, 0.0) : vec2(0.0, inv.y);
    vec2 uv1 = cuv - off, uv2 = cuv + off;
    float e1 = lumaAt(uv1) - lLocal, e2 = lumaAt(uv2) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    if (!r1) uv1 -= off;
    if (!r2) uv2 += off;
    for (int i = 2; i < 12 && !(r1 && r2); ++i)
    {
        if (!r1) e1 = lumaAt(uv1) - lLocal;
        if (!r2) e2 = lumaAt(uv2) - lLocal;
        r1 = abs(e1) >= gScaled;
        r2 = abs(e2) >= gScaled;
        if (!r1) uv1 -= off * kQuality[i];
        if (!r2) uv2 += off * kQuality[i];
    }
    float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y, d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool dir1 = d1 < d2;
    float edgeLen = d1 + d2;
    float pixOff = -min(d1, d2) / edgeLen + 0.5;
    bool correct = ((dir1 ? e1 : e2) < 0.0) != (lC < lLocal);
    float finalOff = correct ? pixOff : 0.0;
    float lAvg = (1.0 / 12.0) * (2.0 * (lDU + lLR) + lLc + lRc);
    float s1 = clamp(abs(lAvg - lC) / range, 0.0, 1.0);
    float s2 = (-2.0 * s1 + 3.0) * s1 * s1;
    finalOff = max(finalOff, s2 * s2 * uSubpix);
    vec2 fuv = uv;
    if (horz) fuv.y += finalOff * stepLen; else fuv.x += finalOff * stepLen;
    oColor = vec4(textureLod(uSrc, fuv, 0.0).rgb, 1.0);
}
)";

    // CLUT fetch: output pixel i is the raw word of CLUT source pixel i (its x, y in uCoords) as the game's GS memory holds
    // it, read from the render target that owns that memory through the reinterpreting fetch (kind 0: the word's bytes).
    const char *kClutFsHead = R"(#version 430 core
out vec4 oColor;
)";
    const char *kClutFsMain = R"(
uniform usampler2D uCoords;
void main()
{
    uvec2 c = texelFetch(uCoords, ivec2(int(gl_FragCoord.x), 0), 0).rg;
    oColor = reinterpretTexel(ivec2(c));
}
)";

    // GS vertex as sent to the GPU.
    struct HwVertex
    {
        float x, y, z;
        float r, g, b, a;
        float s, t, q;
        float u, v;
        float cu0 = -1e30f, cv0 = -1e30f, cu1 = 1e30f, cv1 = 1e30f; // texel range the GS samples (sprites)
    };

    struct Texture
    {
        GLuint id = 0;
        uint32_t width = 0, height = 0;
        uint32_t blockLo = 0, blockHi = 0;
        bool valid = false;
        uint64_t lastUse = 0;
        // Tekken 4 port: an index texture keeps its layout so an upload in that same layout (font glyphs into a font
        // page) only marks rows [dirtyY0, dirtyY1) for re-decoding instead of invalidating the whole page.
        bool indexed = false;
        uint32_t tbp = 0, tbw = 0;
        uint8_t psm = 0;
        uint32_t dirtyY0 = 0, dirtyY1 = 0;
        uint64_t contentHash = 0; // hash of the raw blocks [blockLo, blockHi) it was decoded from (0 = none)
        bool listed = false;      // in liveTextures
        // Tekken 4 port (perf): earlier decodes of this index texture slot by content hash. The game streams several
        // textures through one VRAM area per frame (the Arena: ~48 index decodes a frame, GSMem::ReadIndexRun ~47% of
        // the GL thread); a slot whose raw blocks hash like an earlier version takes that version's GL texture back.
        struct Version
        {
            uint64_t hash = 0;
            GLuint id = 0;
            uint64_t lastUse = 0;
        };
        std::vector<Version> versions;
        // Tekken 4 port (perf): a partial index texture holds only texels [colLo, colHi) x [rowLo, rowHi) (the rest is not
        // decoded yet)
        bool partial = false;
        uint32_t rowLo = 0, rowHi = 0, colLo = 0, colHi = 0;
    };

    struct RenderTarget
    {
        GLuint color = 0, depth = 0, fbo = 0;
        uint32_t fbp = 0, fbw = 1; // PS2 page base and width in 64 px units
        uint8_t psm = 0;
        uint32_t zbp = ~0u;
        uint32_t pxW = 0, pxH = 0; // size in PS2 pixels
        uint32_t glW = 0, glH = 0;
        bool depthCleared = false;
        uint64_t version = 1, lastDrawSeq = 0;
        uint64_t lastDepthSeq = 0; // writeSeq of the last draw that wrote this target's depth (memory at zbp)
        uint32_t usedW = 0, usedH = 0; // PS2 pixels touched by draws (scissor extents)
        int lastDrawPresent = -1; // Tekken 4 port: presentCount at its last draw
        // Tekken 4 port: writeSeq of the last upload into each block of its memory (index = block - fbp * 32). The upload
        // log forgets: Tekken's stage textures are uploaded once into a 640x448 target of the menus (fbp 290) and the
        // per-frame character uploads pushed them out of the log, so the fight sampled the stage from that old picture.
        std::vector<uint64_t> blockUploadSeq;
        // Tekken 4 port: at an internal scale other than 1, uploads into the target go through a native-size staging
        // texture and are blitted (filtered) into the scaled target (the intro movie uploads its pictures this way)
        GLuint stageTex = 0, stageFbo = 0;
    };

    uint32_t pageRowsFor(uint32_t psm)
    {
        switch (psm & 0x3F)
        {
        case 0x13: case 0x1B: case 0x24: case 0x2C: return 64u;  // T8 / T8H
        case 0x14: case 0x2C + 1: return 128u;                    // T4
        case 0x02: case 0x0A: case 0x32: case 0x3A: return 64u;  // 16 bit
        default: return 32u;
        }
    }

    // conservative block (256 B) span of `rows` rows of a buffer `bw` (64 px units) wide
    uint32_t spanBlocks(uint32_t psm, uint32_t bw, uint32_t rows)
    {
        const uint32_t pagesDown = (rows + pageRowsFor(psm) - 1u) / pageRowsFor(psm);
        return std::max<uint32_t>(1u, pagesDown) * std::max<uint32_t>(1u, bw) * 32u;
    }
}

struct GSHwBackend::Impl
{
    bool xferWatchHit = false; // GOW_GS_HW_XFERWATCH: log the next upload's first bytes
    GSTextureStore store; // uploaded texture bytes + CLUT (no rasteriser, no framebuffer)
    GSGL::Context context;
    bool initialized = false, failed = false;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;

    // Tekken 4 port: 1 by default (RAM frames have a 640-pixel stride; Tekken's frame copy repeats at 2).
    float scale = 1.0f;
    GLuint program = 0, vao = 0, vbo = 0;
    GLint locInvSize = -1, locTex = -1, locTexSize = -1, locTexDim = -1, locTme = -1, locFst = -1, locTfx = -1, locTcc = -1, locIip = -1,
          locAte = -1, locFailTint = -1, locAtst = -1, locAref = -1, locPass = -1, locBlendScale = -1, locRtTex24 = -1, locTa0 = -1, locTa1 = -1, locReinterp = -1, locDate = -1, locDatm = -1, locDest = -1, locZTex = -1, locZMax = -1, locTables = -1, locRtColor = -1, locPalette = -1, locTexFmt = -1, locTexFmt2 = -1,
          locRtFmt = -1, locRtFmt2 = -1, locTex0 = -1, locRtPx = -1, locAem = -1;
    GLint locIndexed = -1, locLinear = -1, locWrap = -1, locRegion = -1, locIdxPalette = -1;
    GLint locSwBlend = -1, locColClamp = -1, locAbcd = -1, locFix = -1, locZAlias = -1, locZAliasWrite = -1, locZAliasMax = -1;

    GLuint tableTex = 0, paletteTex = 0;
    GLuint presentProgram = 0, presentVao = 0, presentFbo = 0, presentTex = 0;
    // Tekken 4 port (PC graphics options, docs/GRAPHICS.md)
    float outScale = 1.0f;      // the presented picture's scale (internal resolution step)
    float ssaa = 1.0f;          // supersampling: targets drawn at outScale * ssaa, averaged down when presented
    bool fxaa = false;          // FXAA on the presented picture
    float fxaaSubpix = 0.5f;
    int aniso = 1;              // anisotropic taps for filtered 3D draws
    int hudMode = 0;            // magnified 2D sprites: 0 bilinear (soft), 1 sharp bilinear, 2 hd (bicubic, B&B)
    bool regionLinear = true;   // bilinear for the CLAMP region modes
    GLuint areaProgram = 0, fxaaProgram = 0, movieScaleProgram = 0;
    GLint locMsSrc = -1, locMsScale = -1, locMsBounds = -1;
    GLint locAreaSrc = -1, locAreaRatio = -1, locFxaaSrc = -1, locFxaaSubpix = -1;
    GLint locAnisoN = -1, locSharp = -1, locRegionLinear = -1;
    GLuint postFbo[2] = {}, postTex[2] = {};
    uint32_t postW[2] = {}, postH[2] = {};
    GLint locPresentSrc = -1, locPresentOffset = -1, locPresentEdge = -1, locPresentLineRep = -1;
    uint32_t presentW = 0, presentH = 0;
    GLuint shuffleProgram = 0, shuffleVao = 0, shuffleVbo = 0;
    GLint locShTexa = -1;
    GLint locShInvSize = -1, locShSrc = -1, locShSrcZ = -1, locShZMax = -1, locShHs = -1, locShHd = -1, locShScale = -1;
    // queued shuffle sprites of one state, as owner-space quads per (source half, destination half)
    std::vector<ShuffleVertex> shuffleQuads[4];
    GSDrawState shuffleState{};
    bool haveShuffle = false;
    int shuffleWhy = 0; // debug: which tryShuffle check rejected the last sprite
    RenderTarget *shuffleDst = nullptr, *shuffleSrc = nullptr;
    bool shuffleSrcZ = false;
    std::unordered_map<uint32_t, std::vector<uint16_t>> ownerInverse; // psm -> in-page word index -> y * 64 + x
    std::unordered_map<const uint16_t *, uint32_t> tableOffset;
    std::unordered_map<uint32_t, uint32_t> inverseOffset; // psm -> offset of the inverse page table
    std::vector<HwVertex> pending;
    // the previous sprite of the pending batch (strip seams: see the sprite path in draw)
    struct SpriteEdges
    {
        float xs[2], ys[2], us[2], vs[2]; // corners: texel coordinates us/vs at screen edges xs/ys
    };
    struct LastSprite
    {
        SpriteEdges edges{};
        size_t end = 0;           // pending.size() right after it
        uint64_t batch = ~0ull;   // batchSerial when it was added
    } lastSprite;
    uint64_t batchSerial = 0; // counts flushes of the pending batch
    // Tekken 4 port: the last sprites of the pending batch (shared strip sides, see the sprite path in draw)
    static constexpr size_t kRecentSprites = 48;
    std::vector<LastSprite> recentSprites;
    uint64_t recentBatch = ~0ull;
    // PS2X_GL_SPRITE_SNAP=0: sprites keep their fractional edges and only the previous sprite's side opens (old path)
    static bool spriteSnap()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_GL_SPRITE_SNAP");
            return !(value && *value == '0');
        }();
        return on;
    }

    // Opens the clamp on the side two consecutive sprites share when the texture continues across it: same extent and
    // texel range along the seam, touching screen edges, equal texel coordinate at the seam.
    static void openSharedSide(const SpriteEdges &prev, const SpriteEdges &cur, HwVertex *prevVertices, float &cu0, float &cu1,
                               float &cv0, float &cv1)
    {
        const auto same = [](const float a[2], const float b[2], float tol)
        {
            return std::fabs(std::min(a[0], a[1]) - std::min(b[0], b[1])) <= tol &&
                   std::fabs(std::max(a[0], a[1]) - std::max(b[0], b[1])) <= tol;
        };
        constexpr float kXyTol = 1.0f / 64.0f, kUvTol = 1.0f / 32.0f;
        // along one axis: p/c = screen edges, pt/ct = texel coordinates at them; returns the edge index of each sprite
        const auto seam = [&](const float p[2], const float pt[2], const float c[2], const float ct[2], int &ip, int &ic)
        {
            for (ip = 0; ip < 2; ++ip)
                for (ic = 0; ic < 2; ++ic)
                    if (std::fabs(p[ip] - c[ic]) <= kXyTol && std::fabs(pt[ip] - ct[ic]) <= kUvTol &&
                        // the two sprites lie on opposite sides of the seam
                        (p[1 - ip] - p[ip]) * (c[1 - ic] - c[ic]) < 0.0f)
                        return true;
            return false;
        };
        int ip = 0, ic = 0;
        if (same(prev.ys, cur.ys, kXyTol) && same(prev.vs, cur.vs, kUvTol) && seam(prev.xs, prev.us, cur.xs, cur.us, ip, ic))
        {
            const bool prevHigh = prev.us[ip] > prev.us[1 - ip], curHigh = cur.us[ic] > cur.us[1 - ic];
            for (int i = 0; i < 6; ++i)
                (prevHigh ? prevVertices[i].cu1 : prevVertices[i].cu0) = prevHigh ? 1e30f : -1e30f;
            (curHigh ? cu1 : cu0) = curHigh ? 1e30f : -1e30f;
        }
        else if (same(prev.xs, cur.xs, kXyTol) && same(prev.us, cur.us, kUvTol) && seam(prev.ys, prev.vs, cur.ys, cur.vs, ip, ic))
        {
            const bool prevHigh = prev.vs[ip] > prev.vs[1 - ip], curHigh = cur.vs[ic] > cur.vs[1 - ic];
            for (int i = 0; i < 6; ++i)
                (prevHigh ? prevVertices[i].cv1 : prevVertices[i].cv0) = prevHigh ? 1e30f : -1e30f;
            (curHigh ? cv1 : cv0) = curHigh ? 1e30f : -1e30f;
        }
    }
    GSDrawState pendingState{};
    // Black & Bruised (perf): the boxers are drawn per 1-5 triangles in two passes on the same vertices: A = the body
    // texture as a plain write (ALPHA 0x2a: (0-0)*FIX + Cs), B = the cel-shading ramp blended over it (ALPHA 0x09:
    // Cd*As + Cs), both ZTST GEQUAL with Z write, no alpha/destination test. A, B, A, B... ended a batch every few
    // triangles (~11k GL batches a frame in fights, 35-38 fps). While A is pending, a B triangle with the vertices of the
    // next A triangle of the run goes into a second, deferred batch drawn right after A: every A of the run, then every B.
    // Same pixels: an opaque Z-writing draw only depends on depth, and a B triangle passes GEQUAL only where its own A
    // triangle is the nearest (ties of exactly equal depth aside), on that A's colour, as in the original order. Anything
    // else (other state, upload, CLUT change, present...) flushes A then B. PS2X_GL_PAIR_MERGE=0: off.
    struct DeferredPass
    {
        bool active = false;
        GSDrawState state{}, key{};
        std::vector<HwVertex> verts;
        uint64_t clutHash = 0;
        GLuint palette = 0;
    } deferB;
    // Other opaque Z-writing passes into the same target between them (the outline pass: untextured, ABE 0) wait here
    // too, drawn after A and before the B passes: opaque Z-writing draws give the same pixels in any order (equal-depth
    // ties aside), and a B triangle still lands only where its own A triangle is the nearest.
    static constexpr size_t kExtraOpaque = 3u;
    std::vector<DeferredPass> extraOpaque;
    struct TriPos
    {
        float v[9];
        bool operator==(const TriPos &o) const { return std::memcmp(v, o.v, sizeof(v)) == 0; }
    };
    std::vector<TriPos> runA;  // A triangles of the current run, in order
    size_t runAIdx = 0;        // the next A triangle a B triangle must repeat
    bool runHasB = false;      // a B triangle followed this run (the next A starts a new run)
    bool pendingOpaque = false; // the pending batch is an A-type pass (isOpaqueZ)
    static bool pairMergeOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_GL_PAIR_MERGE"); return !v || *v != '0'; }();
        return on;
    }
    static TriPos triPos(const GSPrimitiveBatch &b)
    {
        TriPos p;
        for (int i = 0; i < 3; ++i)
        {
            p.v[i * 3] = b.vertices[i].x;
            p.v[i * 3 + 1] = b.vertices[i].y;
            p.v[i * 3 + 2] = static_cast<float>(b.vertices[i].z);
        }
        return p;
    }
    static bool depthOrdered(const GSDrawState &s)
    {
        const GSContext &c = s.context;
        return ((c.test >> 16) & 1u) != 0u && ((c.test >> 17) & 3u) >= 2u && (c.test & 1u) == 0u &&
               ((c.test >> 14) & 1u) == 0u && c.frame.fbmsk == 0u && c.zbuf.zbp != c.frame.fbp;
    }
    // A pass: Z written, the colour does not depend on the destination
    static bool isOpaqueZ(const GSDrawState &s)
    {
        if (!depthOrdered(s) || s.context.zbuf.zmask)
            return false;
        if (!s.prim.abe)
            return true;
        const uint64_t al = s.context.alpha;
        return (al & 3u) == ((al >> 2) & 3u) && ((al >> 6) & 3u) == 0u;
    }
    static bool sameTarget(const GSDrawState &a, const GSDrawState &s)
    {
        const auto same = [](const auto &x, const auto &y) { return std::memcmp(&x, &y, sizeof(x)) == 0; };
        const GSContext &ca = a.context, &cb = s.context;
        return same(ca.frame, cb.frame) && same(ca.zbuf.zbp, cb.zbuf.zbp) && same(ca.zbuf.psm, cb.zbuf.psm) &&
               same(ca.scissor, cb.scissor) && same(ca.xyoffset, cb.xyoffset);
    }
    // B pass over the pending A: same target, depth buffer, scissor and offset, blended with fixed-function blending
    bool isPassB(const GSDrawState &a, const GSDrawState &s)
    {
        if (!s.prim.abe || !depthOrdered(s) || !sameTarget(a, s) || (a.colclamp & 1u) != (s.colclamp & 1u))
            return false;
        bool enabled = false, soft = false;
        GLenum sf, df, eq;
        float fixScale, constant;
        blendState(s, enabled, sf, df, fixScale, constant, eq, soft);
        return !soft;
    }
    // Tekken 4 port (perf): batch vertices go into a persistently mapped ring buffer (vbo) instead of a glBufferData
    // per batch (a driver-side allocation for every one of ~1000 batches a frame). Four fenced quarters: a quarter is
    // written again only after the GPU finished the draws that read it. PS2X_GL_VERTEX_RING=0: glBufferData as before.
    static constexpr size_t kRingBytes = 64u << 20, kRingQuarter = kRingBytes / 4u;
    uint8_t *ringPtr = nullptr;
    size_t ringHead = 0;
    GLsync ringFence[4] = {};
    void setupRing(const Api &g)
    {
        const char *value = std::getenv("PS2X_GL_VERTEX_RING");
        if ((value && *value == '0') || !g.BufferStorage || !g.MapBufferRange || !g.FenceSync || !g.ClientWaitSync || !g.DeleteSync)
            return;
        const GLbitfield flags = kMapWriteBit | kMapPersistentBit | kMapCoherentBit;
        g.BufferStorage(kArrayBuffer, static_cast<GLsizeiptr>(kRingBytes), nullptr, flags);
        ringPtr = static_cast<uint8_t *>(g.MapBufferRange(kArrayBuffer, 0, static_cast<GLsizeiptr>(kRingBytes), flags));
    }
    // Copies the vertices into the ring (vbo bound) and returns the index of the first one.
    size_t ringQuarter = 0; // the quarter ringHead is in
    void ringWaitQuarter(const Api &g, size_t q)
    {
        if (!ringFence[q])
            return;
        while (g.ClientWaitSync(ringFence[q], kSyncFlushCommandsBit, 1000000000ull) == kTimeoutExpired)
        {
        }
        g.DeleteSync(ringFence[q]);
        ringFence[q] = nullptr;
    }
    GLint ringWrite(const Api &g, const void *data, size_t bytes)
    {
        if (bytes > kRingQuarter)
        {
            // never seen (a quarter holds 262144 vertices): wait for every draw and start over at 0
            if (!ringFence[ringQuarter])
                ringFence[ringQuarter] = g.FenceSync(kSyncGpuCommandsComplete, 0u);
            for (size_t q = 0; q < 4u; ++q)
                ringWaitQuarter(g, q);
            ringHead = 0;
            ringQuarter = 0;
        }
        const size_t at = ringHead + bytes > kRingBytes ? 0u : ringHead;
        const size_t last = (at + bytes - 1u) / kRingQuarter;
        // crossing into the next quarter(s): fence the one left behind, wait until the GPU finished the next one
        while (ringQuarter != last)
        {
            ringFence[ringQuarter] = g.FenceSync(kSyncGpuCommandsComplete, 0u);
            ringQuarter = (ringQuarter + 1u) % 4u;
            ringWaitQuarter(g, ringQuarter);
        }
        std::memcpy(ringPtr + at, data, bytes);
        ringHead = at + bytes;
        return static_cast<GLint>(at / sizeof(HwVertex));
    }
    GSDrawState pendingKey{}; // batchKey(pendingState)

    // Tekken 4 port (perf): the part of a primitive's state the batch draw uses. Registers the renderer does not
    // implement or applies per vertex (XYOFFSET, fog colour, PRIM type/FGE/AA1/FIX/CTXT, PABE, SCANMSK, DIMX, DTHE, FBA,
    // TEXCLUT, MIPTBP) and, for untextured primitives, the texture registers are zeroed: primitives that differ only
    // there join one batch (the cage of the Arena / airport fences: XYOFFSET or TEX0 changes between flat-coloured
    // primitives). PS2X_GL_BATCH_KEY=0: the whole state must match, as before.
    static GSDrawState batchKey(const GSDrawState &s)
    {
        static const bool loose = []
        {
            const char *value = std::getenv("PS2X_GL_BATCH_KEY");
            return !value || *value != '0';
        }();
        if (!loose)
            return s;
        // built field by field into zeroed memory: the key is compared with memcmp, so no padding byte may differ
        // (a GSDrawState{} leaves padding indeterminate; whole-struct copies carry the source's padding)
        GSDrawState k;
        std::memset(&k, 0, sizeof(k));
        const GSContext &c = s.context;
        k.context.frame.fbp = c.frame.fbp, k.context.frame.fbw = c.frame.fbw, k.context.frame.psm = c.frame.psm;
        k.context.frame.fbmsk = c.frame.fbmsk;
        k.context.scissor.x0 = c.scissor.x0, k.context.scissor.x1 = c.scissor.x1;
        k.context.scissor.y0 = c.scissor.y0, k.context.scissor.y1 = c.scissor.y1;
        k.context.zbuf.zbp = c.zbuf.zbp, k.context.zbuf.psm = c.zbuf.psm, k.context.zbuf.zmask = c.zbuf.zmask;
        k.context.alpha = c.alpha;
        k.context.test = c.test;
        k.prim.iip = s.prim.iip;
        k.prim.tme = s.prim.tme;
        k.prim.abe = s.prim.abe;
        k.prim.fst = s.prim.fst;
        k.colclamp = s.colclamp;
        k.textureWidth = k.textureHeight = 1;
        if (s.prim.tme)
        {
            const GSTex0Reg &t = c.tex0;
            GSTex0Reg &kt = k.context.tex0;
            kt.tbp0 = t.tbp0, kt.tbw = t.tbw, kt.psm = t.psm, kt.tw = t.tw, kt.th = t.th, kt.tcc = t.tcc, kt.tfx = t.tfx;
            kt.cbp = t.cbp, kt.cpsm = t.cpsm, kt.csm = t.csm, kt.csa = t.csa, kt.cld = t.cld;
            k.context.tex1 = c.tex1;
            k.context.clamp = c.clamp;
            k.texa.ta0 = s.texa.ta0, k.texa.aem = s.texa.aem, k.texa.ta1 = s.texa.ta1;
            k.textureWidth = s.textureWidth;
            k.textureHeight = s.textureHeight;
            k.linearFilter = s.linearFilter;
        }
        return k;
    }
    bool havePending = false;
    double firstRawZ = 0.0;

    std::unordered_map<uint64_t, Texture> textures;
    std::vector<uint8_t> decodeScratch;
    // PS2X_GL_DECODE_LOG=1: every 5 s, the texture slots decoded most often (tbp, format, size, decodes, distinct contents)
    struct DecodeLogEntry
    {
        uint64_t count = 0;
        std::vector<uint64_t> hashes;
    };
    std::unordered_map<uint64_t, DecodeLogEntry> decodeLog;
    void noteDecode(uint32_t tbp, uint32_t psm, uint32_t w, uint32_t h, bool indexed, uint64_t contentHash)
    {
        static const bool on = std::getenv("PS2X_GL_DECODE_LOG") != nullptr;
        if (!on)
            return;
        const uint64_t key = tbp | (static_cast<uint64_t>(psm) << 16) | (static_cast<uint64_t>(w) << 24) |
                             (static_cast<uint64_t>(h) << 40) | (indexed ? 1ull << 60 : 0ull);
        DecodeLogEntry &e = decodeLog[key];
        ++e.count;
        if (e.hashes.size() < 64u && std::find(e.hashes.begin(), e.hashes.end(), contentHash) == e.hashes.end())
            e.hashes.push_back(contentHash);
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(5))
            return;
        last = now;
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (const auto &kv : decodeLog)
            top.emplace_back(kv.second.count, kv.first);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 12u; ++i)
        {
            const uint64_t k = top[i].second;
            std::fprintf(stderr, "[gs-decode] tbp=%llx psm=%llx %llux%llu %s decodes=%llu contents=%zu\n",
                         static_cast<unsigned long long>(k & 0xFFFFu), static_cast<unsigned long long>((k >> 16) & 0xFFu),
                         static_cast<unsigned long long>((k >> 24) & 0xFFFFu), static_cast<unsigned long long>((k >> 40) & 0xFFFFu),
                         (k >> 60) ? "index" : "direct", static_cast<unsigned long long>(top[i].first), decodeLog[k].hashes.size());
        }
        decodeLog.clear();
    }
    static bool indexVersions()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_GL_TEX_VERSIONS");
            return !value || *value != '0';
        }();
        return on;
    }
    // Tekken 4 port (perf): the valid textures, so an upload's invalidation walks those instead of the whole cache (up to
    // 6000 entries, most of them invalid, per upload: several ms per frame in stages with many uploads). The map is
    // node based: element pointers stay valid until the entry is erased (evictOld rebuilds the list).
    std::vector<Texture *> liveTextures;
    void markValid(Texture &t)
    {
        t.valid = true;
        if (!t.listed)
        {
            t.listed = true;
            liveTextures.push_back(&t);
        }
    }
    void rebuildLiveTextures()
    {
        liveTextures.clear();
        for (auto &kv : textures)
        {
            kv.second.listed = kv.second.valid;
            if (kv.second.valid)
                liveTextures.push_back(&kv.second);
        }
    }
    std::vector<RenderTarget> targets;
    uint64_t useCounter = 0, writeSeq = 0;
    struct UploadRecord { uint32_t lo, hi; uint64_t seq; };
    std::vector<UploadRecord> uploadLog;
    uint64_t clutHash = 0;
    // Tekken 4 port: the CLUT a pending batch was started with and its palette, resolved when the batch starts. A CLUT
    // load then only ends the batch when the next indexed draw sees a different CLUT, instead of on every TEX0 write
    // (Tekken writes the same TEX0 with CLD=1 before each strip: 92% of the batches in a fight ended there).
    uint64_t pendingClutHash = 0;
    GLuint pendingPalette = 0;

    struct Stats
    {
        uint64_t shuffles = 0;
        uint64_t draws = 0, batches = 0, skippedPsm = 0, unsupportedBlend = 0, texDecodes = 0, texRowDecodes = 0, texRevalidated = 0, texHits = 0, skippedPrim = 0, clutReadbacks = 0, clutRtHits = 0;
        // Tekken 4 port: what ended each batch (FlushWhy) and, for state changes, which part of the state differed
        uint64_t flushWhy[10] = {};
        uint64_t rtUploads = 0; // Tekken 4 port: uploads also written into a render target
        uint64_t uploadsSame = 0; // uploads that left the raw blocks as they were (textures kept)
        std::map<std::string, uint64_t> transfers; // Tekken 4 port: "dir dbp dbw dpsm wxh" -> count per stats window
        uint64_t stateDiff[24] = {};
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    } stats;
    bool statsEnabled = std::getenv("GOW_GS_HW_STATS") != nullptr;
    enum FlushWhy { WhyOther, WhyState, WhyClut, WhyTransfer, WhyTexFlush, WhyFlush, WhySync, WhyClear, WhyPresent, WhyShuffle };
    static constexpr const char *kFlushWhyNames[10] = {"other", "state", "clut", "xfer", "texflush", "flush", "sync", "clear", "present", "shuffle"};
    static constexpr const char *kStateDiffNames[24] = {"frame", "scissor", "tex0", "xyoffset", "zbuf", "tex1", "clamp", "alpha", "test",
                                                        "fba/mip", "type", "iip", "tme", "fge", "abe", "fst", "ctxt", "aa1/fix", "texa",
                                                        "texclut", "misc", "fog", "texsize", "padding"};
    void noteStateDiff(const GSDrawState &a, const GSDrawState &b)
    {
        const auto differs = [](const auto &x, const auto &y) { return std::memcmp(&x, &y, sizeof(x)) != 0; };
        const GSContext &ca = a.context, &cb = b.context;
        const bool d[23] = {differs(ca.frame, cb.frame), differs(ca.scissor, cb.scissor), differs(ca.tex0, cb.tex0),
                            differs(ca.xyoffset, cb.xyoffset), differs(ca.zbuf, cb.zbuf), ca.tex1 != cb.tex1, ca.clamp != cb.clamp,
                            ca.alpha != cb.alpha, ca.test != cb.test,
                            ca.fba != cb.fba || ca.miptbp1 != cb.miptbp1 || ca.miptbp2 != cb.miptbp2, a.prim.type != b.prim.type,
                            a.prim.iip != b.prim.iip, a.prim.tme != b.prim.tme, a.prim.fge != b.prim.fge, a.prim.abe != b.prim.abe,
                            a.prim.fst != b.prim.fst, a.prim.ctxt != b.prim.ctxt, a.prim.aa1 != b.prim.aa1 || a.prim.fix != b.prim.fix,
                            differs(a.texa, b.texa), differs(a.texclut, b.texclut),
                            a.pabe != b.pabe || a.scanmsk != b.scanmsk || a.dimx != b.dimx || a.dthe != b.dthe || a.colclamp != b.colclamp,
                            a.fogR != b.fogR || a.fogG != b.fogG || a.fogB != b.fogB,
                            a.textureWidth != b.textureWidth || a.textureHeight != b.textureHeight || a.linearFilter != b.linearFilter};
        bool any = false;
        for (int i = 0; i < 23; ++i)
            if (d[i])
                ++stats.stateDiff[i], any = true;
        if (!any)
            ++stats.stateDiff[23];
    }

    // GL entry points: the context's, or the redundant-state-call filter over them (gs_gl_state_cache.h)
    Api cachedApi;
    bool cachedApiReady = false;
    const Api &gl() const { return cachedApiReady ? cachedApi : context.gl(); }

    GLuint compile(GLenum type, const char *src) { return compile(type, {src}); }
    GLuint compile(GLenum type, std::initializer_list<const char *> parts)
    {
        const Api &g = gl();
        const GLuint sh = g.CreateShader(type);
        g.ShaderSource(sh, static_cast<GLsizei>(parts.size()), parts.begin(), nullptr);
        g.CompileShader(sh);
        GLint ok = 0;
        g.GetShaderiv(sh, kCompileStatus, &ok);
        if (!ok)
        {
            char log[2048];
            GLsizei n = 0;
            g.GetShaderInfoLog(sh, sizeof(log), &n, log);
            std::fprintf(stderr, "[gs-hw] shader error: %s\n", log);
            return 0;
        }
        return sh;
    }

    bool ensure()
    {
        if (initialized)
            return true;
        if (failed)
            return false;
        std::string error;
        if (!context.Create(error) || !gl().hwRaster)
        {
            std::fprintf(stderr, "[gs-hw] GL unavailable: %s\n", error.c_str());
            failed = true;
            return false;
        }
        if (GSGL::GLStateCache::Enabled())
        {
            cachedApi = GSGL::GLStateCache::Install(context.gl());
            cachedApiReady = true;
        }
        const Api &g = gl();
        const GLuint v = compile(GSGL::kVertexShader, kVsSource);
        const GLuint f = compile(GSGL::kFragmentShader, {kFsSource, kReinterpretSource, kFsTail});
        if (!v || !f)
        {
            failed = true;
            return false;
        }
        program = g.CreateProgram();
        g.AttachShader(program, v);
        g.AttachShader(program, f);
        g.LinkProgram(program);
        GLint ok = 0;
        g.GetProgramiv(program, kLinkStatus, &ok);
        if (!ok)
        {
            char log[2048];
            GLsizei n = 0;
            g.GetProgramInfoLog(program, sizeof(log), &n, log);
            std::fprintf(stderr, "[gs-hw] link error: %s\n", log);
            failed = true;
            return false;
        }
        const auto loc = [&](const char *name) { return g.GetUniformLocation(program, name); };
        locInvSize = loc("uInvSize"); locTex = loc("uTex"); locTexSize = loc("uTexSize"); locTexDim = loc("uTexDim"); locTme = loc("uTme");
        locFst = loc("uFst"); locTfx = loc("uTfx"); locTcc = loc("uTcc"); locIip = loc("uIip"); locAte = loc("uAte");
        locAtst = loc("uAtst"); locFailTint = loc("uFailTint"); locAref = loc("uAref"); locPass = loc("uPass"); locBlendScale = loc("uBlendScale");
        locReinterp = loc("uReinterp"); locDate = loc("uDate"); locDatm = loc("uDatm"); locDest = loc("uDest"); locZTex = loc("uZTex"); locZMax = loc("uZMax"); locTables = loc("uTables"); locRtColor = loc("uRtColor"); locPalette = loc("uPalette");
        locTexFmt = loc("uTexFmt"); locTexFmt2 = loc("uTexFmt2"); locRtFmt = loc("uRtFmt"); locRtFmt2 = loc("uRtFmt2");
        locTex0 = loc("uTex0"); locRtPx = loc("uRtPx");
        locAnisoN = loc("uAnisoN"); locSharp = loc("uSharp"); locRegionLinear = loc("uRegionLinear");
        locIndexed = loc("uIndexed"); locLinear = loc("uLinear"); locWrap = loc("uWrap"); locRegion = loc("uRegion"); locIdxPalette = loc("uIdxPalette");
        locSwBlend = loc("uSwBlend"); locColClamp = loc("uColClamp"); locAbcd = loc("uAbcd"); locFix = loc("uFix");
        locZAlias = loc("uZAlias"); locZAliasWrite = loc("uZAliasWrite"); locZAliasMax = loc("uZAliasMax");
        locRtTex24 = loc("uRtTex24"); locTa0 = loc("uTa0"); locTa1 = loc("uTa1"); locAem = loc("uAem");

        g.GenVertexArrays(1, &vao);
        g.BindVertexArray(vao);
        g.GenBuffers(1, &vbo);
        g.BindBuffer(kArrayBuffer, vbo);
        setupRing(g);
        const GLsizei stride = sizeof(HwVertex);
        g.VertexAttribPointer(0, 2, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, x)));
        g.VertexAttribPointer(1, 1, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, z)));
        g.VertexAttribPointer(2, 4, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, r)));
        g.VertexAttribPointer(3, 3, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, s)));
        g.VertexAttribPointer(4, 2, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, u)));
        g.VertexAttribPointer(5, 4, kFloat, kFalse, stride, reinterpret_cast<const void *>(offsetof(HwVertex, cu0)));
        for (GLuint i = 0; i < 6; ++i)
            g.EnableVertexAttribArray(i);
        buildTables();
        {
            const GLuint sv = compile(GSGL::kVertexShader, kShuffleVs), sfs = compile(GSGL::kFragmentShader, kShuffleFs);
            if (sv && sfs)
            {
                shuffleProgram = g.CreateProgram();
                g.AttachShader(shuffleProgram, sv);
                g.AttachShader(shuffleProgram, sfs);
                g.LinkProgram(shuffleProgram);
                const auto sloc = [&](const char *name) { return g.GetUniformLocation(shuffleProgram, name); };
                locShInvSize = sloc("uInvSize"); locShSrc = sloc("uSrc"); locShSrcZ = sloc("uSrcZ"); locShZMax = sloc("uZMax");
                locShHs = sloc("uHs"); locShHd = sloc("uHd"); locShScale = sloc("uScale"); locShTexa = sloc("uTexa");
                g.GenVertexArrays(1, &shuffleVao);
                g.BindVertexArray(shuffleVao);
                g.GenBuffers(1, &shuffleVbo);
                g.BindBuffer(kArrayBuffer, shuffleVbo);
                g.VertexAttribPointer(0, 2, kFloat, kFalse, sizeof(ShuffleVertex), reinterpret_cast<const void *>(offsetof(ShuffleVertex, x)));
                g.VertexAttribPointer(1, 2, kFloat, kFalse, sizeof(ShuffleVertex), reinterpret_cast<const void *>(offsetof(ShuffleVertex, ox)));
                g.EnableVertexAttribArray(0);
                g.EnableVertexAttribArray(1);
                g.BindVertexArray(vao);
                g.BindBuffer(kArrayBuffer, vbo);
            }
        }
        {
            const GLuint pv = compile(GSGL::kVertexShader, kPresentVs), pf = compile(GSGL::kFragmentShader, kPresentFs);
            if (pv && pf)
            {
                presentProgram = g.CreateProgram();
                g.AttachShader(presentProgram, pv);
                g.AttachShader(presentProgram, pf);
                g.LinkProgram(presentProgram);
                locPresentSrc = g.GetUniformLocation(presentProgram, "uSrc");
                locPresentOffset = g.GetUniformLocation(presentProgram, "uSrcOffset");
                locPresentEdge = g.GetUniformLocation(presentProgram, "uSrcEdge");
                locPresentLineRep = g.GetUniformLocation(presentProgram, "uLineRep");
                g.GenVertexArrays(1, &presentVao);
                g.BindVertexArray(vao);
            }
        }
        {
            const GLuint pv = compile(GSGL::kVertexShader, kPresentVs);
            const auto link = [&](const char *fs, GLuint &program)
            {
                const GLuint f = compile(GSGL::kFragmentShader, fs);
                if (!pv || !f)
                    return;
                program = g.CreateProgram();
                g.AttachShader(program, pv);
                g.AttachShader(program, f);
                g.LinkProgram(program);
            };
            link(kAreaFs, areaProgram);
            link(kFxaaFs, fxaaProgram);
            link(kMovieScaleFs, movieScaleProgram);
            if (movieScaleProgram)
                locMsSrc = g.GetUniformLocation(movieScaleProgram, "uSrc"), locMsScale = g.GetUniformLocation(movieScaleProgram, "uScale"),
                locMsBounds = g.GetUniformLocation(movieScaleProgram, "uBounds");
            if (areaProgram)
                locAreaSrc = g.GetUniformLocation(areaProgram, "uSrc"), locAreaRatio = g.GetUniformLocation(areaProgram, "uRatio");
            if (fxaaProgram)
                locFxaaSrc = g.GetUniformLocation(fxaaProgram, "uSrc"), locFxaaSubpix = g.GetUniformLocation(fxaaProgram, "uSubpix");
        }
        const char *renderer = reinterpret_cast<const char *>(g.GetString(kRenderer));
        readGraphicsSettings(renderer ? renderer : "");
        std::fprintf(stderr, "[gs-hw] render-target renderer ready, scale %.3f (%s)\n", scale, renderer ? renderer : "?");
        initialized = true;
        return true;
    }

    // Tekken 4 port: integrated graphics (they share the processor's memory and power): Auto stays at 1080p there
    static bool integratedGpu(const std::string &renderer)
    {
        std::string r = renderer;
        for (char &c : r)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (r.find("intel") != std::string::npos)
            return r.find("arc(tm) a") == std::string::npos && r.find("arc a") == std::string::npos &&
                   r.find("arc(tm) b") == std::string::npos && r.find("arc b") == std::string::npos;
        if (r.find("radeon") != std::string::npos)
            return r.find(" rx ") == std::string::npos && r.find("pro ") == std::string::npos &&
                   (r.find("radeon(tm) graphics") != std::string::npos || r.find("radeon graphics") != std::string::npos ||
                    r.find("vega") != std::string::npos || r.find("m graphics") != std::string::npos);
        return r.find("llvmpipe") != std::string::npos || r.find("gdi generic") != std::string::npos ||
               r.find("microsoft basic") != std::string::npos;
    }

    // Tekken 4 port: the PC graphics options (docs/GRAPHICS.md). Internal resolution (T5DR's steps): the render
    // targets are drawn at the scale that gives the step's picture height for the game's 448-line picture
    // (TEKKEN4_INTERNAL_RES=lowest|native|1080p|1440p|4k|auto; PS2X_GS_HW_SCALE=<number or step> overrides it).
    // Fractional scales are exact: sprites are snapped to the PS2 pixels they cover, so strips stay seamless.
    void readGraphicsSettings(const std::string &renderer)
    {
        const auto env = [](const char *name) -> std::string
        {
            const char *v = std::getenv(name);
            std::string s = v ? v : "";
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const char *linesEnv = std::getenv("PS2X_GS_NATIVE_LINES");
        const float lines = linesEnv ? std::max(1.0f, static_cast<float>(std::atof(linesEnv))) : 448.0f;
        std::string spec = env("PS2X_GS_HW_SCALE");
        if (spec.empty())
            spec = env("TEKKEN4_INTERNAL_RES");
        if (spec.empty())
            spec = "native";
        const bool integrated = integratedGpu(renderer);
        std::string step = spec;
        float value = 1.0f;
        char *end = nullptr;
        const double number = std::strtod(spec.c_str(), &end);
        if (end && end != spec.c_str() && (*end == '\0' || *end == 'x'))
            value = static_cast<float>(number), step = "custom";
        else if (spec == "lowest")
            value = 0.5f;
        else if (spec == "lower")
            value = 0.75f;
        else if (spec == "native")
            value = 1.0f;
        else if (spec == "720p")
            value = 720.0f / lines;
        else if (spec == "1080p")
            value = 1080.0f / lines;
        else if (spec == "1440p")
            value = 1440.0f / lines;
        else if (spec == "4k" || spec == "2160p")
            value = 2160.0f / lines;
        else if (spec == "auto")
        {
            // the step that fits the screen of the game window: the largest of 1080p/1440p/4K not taller than the
            // monitor's desktop, else Native; at most 1080p on integrated graphics
            uint32_t mw = 0, mh = 0;
            if (!GSSharedPresent::HostMonitorSize(mw, mh))
                mh = 1080;
            uint32_t pick = mh >= 2160u ? 2160u : mh >= 1440u ? 1440u : mh >= 1080u ? 1080u : 0u;
            if (integrated && pick > 1080u)
                pick = 1080u;
            value = pick ? static_cast<float>(pick) / lines : 1.0f;
            step = pick == 2160u ? "auto (4k)" : pick ? "auto (" + std::to_string(pick) + "p)" : "auto (native)";
            std::fprintf(stderr, "[gs-hw] internal resolution auto: monitor %ux%u, %s graphics\n", mw, mh,
                         integrated ? "integrated" : "discrete");
        }
        else
            std::fprintf(stderr, "[gs-hw] unknown internal resolution '%s', native\n", spec.c_str());
        outScale = std::min(8.0f, std::max(0.5f, value));
        // anti-aliasing: off | fxaa | ssaa<f> | msaa<n> (n samples per pixel = SSAA at sqrt(n) per axis: true MSAA
        // cannot be used with the GS emulation, see docs/GRAPHICS.md) | smaa (FXAA here); '+' combines, e.g. ssaa2+fxaa
        std::string aa = env("TEKKEN4_AA");
        if (aa.empty())
            aa = "fxaa";
        fxaa = false;
        ssaa = 1.0f;
        size_t pos = 0;
        while (pos <= aa.size())
        {
            const size_t plus = aa.find('+', pos);
            const std::string tok = aa.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
            if (tok == "fxaa" || tok == "smaa" || tok == "1" || tok == "on")
                fxaa = true;
            else if (tok.rfind("ssaa", 0) == 0)
                ssaa = static_cast<float>(std::atof(tok.c_str() + 4));
            else if (tok.rfind("msaa", 0) == 0)
                ssaa = std::sqrt(static_cast<float>(std::atof(tok.c_str() + 4)));
            else if (tok != "off" && tok != "none" && tok != "0" && !tok.empty())
                std::fprintf(stderr, "[gs-hw] unknown anti-aliasing '%s'\n", tok.c_str());
            if (plus == std::string::npos)
                break;
            pos = plus + 1;
        }
        ssaa = std::min(4.0f, std::max(1.0f, ssaa));
        if (const char *sub = std::getenv("TEKKEN4_FXAA_SUBPIX"))
            fxaaSubpix = std::min(1.0f, std::max(0.0f, static_cast<float>(std::atof(sub))));
        if (outScale * ssaa > 8.0f)
            ssaa = 8.0f / outScale;
        const std::string an = env("TEKKEN4_ANISO");
        aniso = an.empty() ? 4 : (an == "off" ? 1 : std::atoi(an.c_str()));
        aniso = std::min(16, std::max(1, aniso));
        hudMode = env("TEKKEN4_HUD") == "sharp" ? 1 : env("TEKKEN4_HUD") == "soft" ? 0 : 2; // B&B: hd unless asked otherwise
        g_hudModeLive.store(hudMode);
        regionLinear = env("PS2X_GL_REGION_LINEAR") != "0";
        scale = outScale * ssaa;
        std::fprintf(stderr, "[gs-hw] graphics: internal resolution %s (scale %.3f), anti-aliasing %s%s (ssaa %.2f), "
                             "anisotropic %dx, HUD %s, region filtering %s\n",
                     step.c_str(), outScale, fxaa ? "fxaa" : "", ssaa > 1.0f ? (fxaa ? "+ssaa" : "ssaa") : (fxaa ? "" : "off"),
                     ssaa, aniso, hudMode == 1 ? "sharp" : hudMode == 2 ? "hd" : "soft", regionLinear ? "bilinear" : "point");
    }

    // forward page tables of every texture format + inverse tables (in-page pixel index -> in-page x/y) of the render-target
    // formats, in one R16UI texture (2048 entries per row)
    void buildTables()
    {
        const Api &g = gl();
        std::vector<uint16_t> data;
        const uint32_t psms[] = {0x00, 0x01, 0x02, 0x0A, 0x13, 0x14, 0x1B, 0x24, 0x2C};
        for (const uint32_t psm : psms)
        {
            const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
            if (!f.table || tableOffset.count(f.table))
                continue;
            const uint32_t size = 32u << (f.pageShiftX + f.pageShiftY);
            tableOffset[f.table] = static_cast<uint32_t>(data.size());
            data.insert(data.end(), f.table, f.table + size);
        }
        for (const uint32_t psm : {0x00u, 0x01u, 0x02u, 0x0Au})
        {
            const GSSwizzle::Format &f = GSSwizzle::GetFormat(psm);
            const uint32_t pageSize = 1u << (f.pageShiftX + f.pageShiftY);
            inverseOffset[psm] = static_cast<uint32_t>(data.size());
            data.resize(data.size() + pageSize, 0);
            uint16_t *inv = &data[inverseOffset[psm]];
            for (uint32_t e = 0; e < pageSize; ++e)
                inv[f.table[e]] = static_cast<uint16_t>(e);
        }
        const uint32_t rows = static_cast<uint32_t>((data.size() + 2047u) / 2048u);
        data.resize(static_cast<size_t>(rows) * 2048u, 0);
        g.GenTextures(1, &tableTex);
        g.BindTexture(kTexture2D, tableTex);
        g.PixelStorei(kUnpackAlignment, 1);
        g.TexImage2D(kTexture2D, 0, 0x8234 /*R16UI*/, 2048, rows, 0, 0x8D94 /*RED_INTEGER*/, 0x1403 /*UNSIGNED_SHORT*/, data.data());
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.GenTextures(1, &paletteTex);
        g.BindTexture(kTexture2D, paletteTex);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), 256, 1, 0, kRgba, kUnsignedByte, nullptr);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
    }

    // ---- render targets -------------------------------------------------------------------------------------------
    RenderTarget &target(uint32_t fbp, uint32_t fbw)
    {
        for (RenderTarget &t : targets)
            if (t.fbp == fbp && t.fbw >= fbw)
                return t;
        static const bool rtLog = std::getenv("GOW_GS_HW_RTLOG") != nullptr;
        if (rtLog)
            std::fprintf(stderr, "[gs-hw-rt] new/grow fbp=%u fbw=%u\n", fbp, fbw);
        for (RenderTarget &t : targets) // grow an existing one
            if (t.fbp == fbp)
            {
                destroy(t);
                createTarget(t, fbp, fbw);
                return t;
            }
        targets.emplace_back();
        createTarget(targets.back(), fbp, fbw);
        return targets.back();
    }

    void destroy(RenderTarget &t)
    {
        const Api &g = gl();
        if (t.fbo) g.DeleteFramebuffers(1, &t.fbo);
        if (t.stageFbo) g.DeleteFramebuffers(1, &t.stageFbo);
        if (t.stageTex) g.DeleteTextures(1, &t.stageTex);
        if (t.color) g.DeleteTextures(1, &t.color);
        if (t.depth) g.DeleteTextures(1, &t.depth);
        t = {};
    }

    void createTarget(RenderTarget &t, uint32_t fbp, uint32_t fbw)
    {
        const Api &g = gl();
        t.fbp = fbp;
        t.psm = 0;
        t.fbw = std::max<uint32_t>(fbw, 1u);
        t.pxW = t.fbw * 64u;
        t.pxH = 512u;
        t.glW = static_cast<uint32_t>(std::ceil(t.pxW * scale));
        t.glH = static_cast<uint32_t>(std::ceil(t.pxH * scale));
        g.GenTextures(1, &t.color);
        g.BindTexture(kTexture2D, t.color);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), t.glW, t.glH, 0, kRgba, kUnsignedByte, nullptr);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
        g.GenFramebuffers(1, &t.fbo);
        g.BindFramebuffer(kFramebuffer, t.fbo);
        g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, t.color, 0);
        g.ClearColor(0.f, 0.f, 0.f, 0.f);
        g.Clear(kColorBufferBit);
        std::fprintf(stderr, "[gs-hw] render target fbp=%u fbw=%u -> %ux%u\n", fbp, t.fbw, t.glW, t.glH);
    }

    void attachDepth(RenderTarget &t, uint32_t zbp)
    {
        const Api &g = gl();
        if (!t.depth)
        {
            g.GenTextures(1, &t.depth);
            g.BindTexture(kTexture2D, t.depth);
            g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kDepthComponent32F), t.glW, t.glH, 0, kDepthComponent, kFloat, nullptr);
            g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
            g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            g.BindFramebuffer(kFramebuffer, t.fbo);
            g.FramebufferTexture2D(kFramebuffer, kDepthAttachment, kTexture2D, t.depth, 0);
            g.ClearDepth(0.0);
            g.Clear(kDepthBufferBit);
        }
        t.zbp = zbp;
    }

    // ---- textures -------------------------------------------------------------------------------------------------
    static bool isIndexed(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    Texture &textureFor(const GSDrawState &s)
    {
        const GSTex0Reg &t = s.context.tex0;
        uint64_t key = t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                       (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
        key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
        if (isIndexed(t.psm))
        {
            key = key * 1099511628211ull ^ clutHash;
            key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16));
        }
        Texture &tex = textures[key];
        tex.lastUse = ++useCounter;
        if (tex.valid)
        {
            ++stats.texHits;
            return tex;
        }
        ++stats.texDecodes, PS2_PERF_COUNT(texDecodes);
        noteDecode(t.tbp0, t.psm, s.textureWidth, s.textureHeight, false, key);
        classifyTexture(s);
        const Api &g = gl();
        std::vector<uint32_t> pixels;
        decodeDirect(s, pixels);
        static const int texDumpLimit = std::getenv("GOW_GS_HW_TEXDUMP") ? std::atoi(std::getenv("GOW_GS_HW_TEXDUMP")) : 0;
        static int texDumped = 0;
        if (texDumped < texDumpLimit && s.textureWidth > 16)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "gs_hw_tex_%02d_tbp%x_psm%u_%ux%u.ppm", texDumped++, t.tbp0, t.psm, s.textureWidth, s.textureHeight);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P6 %u %u 255 ", s.textureWidth, s.textureHeight);
                for (uint32_t px : pixels)
                {
                    const uint8_t rgb[3] = {static_cast<uint8_t>(px), static_cast<uint8_t>(px >> 8), static_cast<uint8_t>(px >> 16)};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
        tex.width = std::max<uint32_t>(1u, s.textureWidth);
        tex.height = std::max<uint32_t>(1u, s.textureHeight);
        tex.blockLo = t.tbp0;
        tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
        if (!tex.id)
            g.GenTextures(1, &tex.id);
        g.BindTexture(kTexture2D, tex.id);
        g.PixelStorei(kUnpackAlignment, 4);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), tex.width, tex.height, 0, kRgba, kUnsignedByte, pixels.data());
        markValid(tex);
        if (textures.size() > 6000u)
            evictOld();
        return tex;
    }

    // Texels of a direct-colour texture read from GS memory with the swizzle tables, alpha expanded by TEXA like the GS
    // texture unit (CT24: TA0 / AEM, CT16: TA0/TA1 by the alpha bit).
    void decodeDirect(const GSDrawState &s, std::vector<uint32_t> &out)
    {
        const GSTex0Reg &t = s.context.tex0;
        const GSSwizzle::Format &f = GSSwizzle::GetFormat(t.psm);
        const uint32_t w = std::max<uint32_t>(1u, s.textureWidth), h = std::max<uint32_t>(1u, s.textureHeight);
        const bool sixteen = f.kind == GSSwizzle::Kind::Bits16;
        if (!vram || !f.table || !(sixteen || f.kind == GSSwizzle::Kind::Bits32 || f.kind == GSSwizzle::Kind::Bits24))
        {
            store.DecodeTextureRgba(s, out); // formats without a direct layout (none seen in this game)
            return;
        }
        out.resize(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
        {
            uint32_t *line = out.data() + static_cast<size_t>(y) * w;
            // One row at a time with the format resolved once (GSMem::ReadColorRun, same page tables as GSSwizzle):
            // big direct-colour textures (CT24 views of CT32 uploads in the Aegean Sea) were decoded per texel.
            if (!GSMem::ReadColorRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, w, line))
            {
                const GSSwizzle::AxisPart row = GSSwizzle::RowPart(f, t.tbp0, t.tbw, y);
                for (uint32_t x = 0; x < w; ++x)
                {
                    const GSSwizzle::Location at = GSSwizzle::Combine(f, row, GSSwizzle::ColumnPart(f, x));
                    line[x] = GSSwizzle::LoadAt(f, vram + at.byte, at.shift);
                }
            }
            for (uint32_t x = 0; x < w; ++x)
                line[x] = GSCpuInternal::applyTexa(s.texa, t.psm, sixteen ? GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(line[x])) : line[x]);
        }
        static const bool check = std::getenv("GOW_GS_TEXCHECK") != nullptr;
        if (check)
        {
            static int mismatches = 0, checked = 0;
            for (uint32_t y = 0; y < h; ++y)
            {
                const GSSwizzle::AxisPart row = GSSwizzle::RowPart(f, t.tbp0, t.tbw, y);
                for (uint32_t x = 0; x < w; ++x)
                {
                    const GSSwizzle::Location at = GSSwizzle::Combine(f, row, GSSwizzle::ColumnPart(f, x));
                    const uint32_t raw = GSSwizzle::LoadAt(f, vram + at.byte, at.shift);
                    const uint32_t ref = GSCpuInternal::applyTexa(s.texa, t.psm, sixteen ? GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(raw)) : raw);
                    if (ref != out[static_cast<size_t>(y) * w + x] && mismatches++ < 20)
                        std::fprintf(stderr, "[gs-texcheck] psm=%u tbp=%x tbw=%u %ux%u at %u,%u ref=%08x run=%08x\n", t.psm, t.tbp0, t.tbw, w, h, x, y, ref, out[static_cast<size_t>(y) * w + x]);
                }
            }
            if (++checked % 200 == 0)
                std::fprintf(stderr, "[gs-texcheck] %d textures checked, %d mismatching texels\n", checked, mismatches);
        }
    }

    // Index texture of a paletted format: the raw indices, read from GS memory with the swizzle tables. Independent of
    // the CLUT, so palette changes (animated CLUTs, the same image with several palettes) cost nothing.
    // Tekken 4 port (perf): rows of an index texture decoded on demand. The game declares big texture sizes (T8 1024x1024,
    // 128x1024) and samples a small part of them; every new upload into such an area re-decoded the whole declared
    // size (Arena: ~41 decodes of up to 1M texels per frame, GSMem::ReadIndexRun ~49% of the GL thread). draw() passes
    // the rows its vertices can sample (needLo/needHi, all rows when unknown or when the addressing wraps); a tall
    // texture is then decoded only there and grown when a later draw needs more. PS2X_GL_PARTIAL_ROWS=0: off.
    static bool partialRows()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_GL_PARTIAL_ROWS");
            return !value || *value != '0';
        }();
        return on;
    }

    // The texels the batch's vertices can sample along one axis: [lo, hi). All (lo 0, hi ~0) when the addressing may wrap
    // or a coordinate is not finite. Perspective-correct interpolation stays inside the vertices' range.
    static void sampledRange(float c0, float c1, float size, uint32_t wrap, float rmin, float rmax, uint32_t &lo, uint32_t &hi)
    {
        lo = 0u, hi = ~0u;
        float a = std::floor(c0) - 2.0f, b = std::floor(c1) + 3.0f; // filtering and rounding margin
        if (wrap == 0u) // REPEAT: only when nothing wraps
        {
            if (c0 < 0.0f || c1 > size) // a primitive's far edge may lie exactly on the texture's end
                return;
            a = std::max(a, 0.0f), b = std::min(b, size);
        }
        else if (wrap == 1u) // CLAMP
            a = std::max(a, 0.0f), b = std::min(b, size);
        else if (wrap == 2u) // REGION_CLAMP: inside [MIN, MAX]
        {
            a = std::max(std::min(a, rmax), rmin), b = std::min(std::max(b, rmin + 1.0f), rmax + 1.0f);
            a = std::max(a, 0.0f), b = std::min(b, size);
        }
        else
        {
            // REGION_REPEAT: texel (u & UMSK) | UFIX (MINU holds UMSK, MAXU holds UFIX), whatever the coordinate
            const uint32_t msk = static_cast<uint32_t>(rmin), fix = static_cast<uint32_t>(rmax);
            a = static_cast<float>(fix), b = static_cast<float>((msk | fix) + 1u);
            b = std::min(b, size);
        }
        if (b <= a)
            return;
        lo = static_cast<uint32_t>(a), hi = static_cast<uint32_t>(b);
    }
    static void sampledRect(const GSDrawState &s, const std::vector<HwVertex> &verts, uint32_t &x0, uint32_t &x1, uint32_t &y0,
                            uint32_t &y1)
    {
        x0 = y0 = 0u, x1 = y1 = ~0u;
        if (verts.empty())
            return;
        const float texW = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1));
        const float texH = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (const HwVertex &v : verts)
        {
            float tu, tv;
            if (s.prim.fst || v.cu0 > -1e29f) // sprites carry texel coordinates
                tu = v.u, tv = v.v;
            else
            {
                const float q = std::fabs(v.q) < 1e-30f ? 1e-30f : v.q;
                tu = v.s / q * texW, tv = v.t / q * texH;
            }
            if (!(tu == tu) || !(tv == tv) || std::fabs(tu) > 1e7f || std::fabs(tv) > 1e7f)
                return;
            u0 = std::min(u0, tu), u1 = std::max(u1, tu), v0 = std::min(v0, tv), v1 = std::max(v1, tv);
        }
        const uint64_t cl = s.context.clamp;
        sampledRange(u0, u1, texW, static_cast<uint32_t>(cl & 3u), static_cast<float>((cl >> 4) & 0x3FFu),
                     static_cast<float>((cl >> 14) & 0x3FFu), x0, x1);
        sampledRange(v0, v1, texH, static_cast<uint32_t>((cl >> 2) & 3u), static_cast<float>((cl >> 24) & 0x3FFu),
                     static_cast<float>((cl >> 34) & 0x3FFu), y0, y1);
    }

    void decodeIndexRect(Texture &tex, const GSTex0Reg &t, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
    {
        if (y1 <= y0 || x1 <= x0)
            return;
        const uint32_t w = x1 - x0;
        std::vector<uint8_t> &rows = decodeScratch;
        rows.resize(static_cast<size_t>(w) * (y1 - y0));
        for (uint32_t y = y0; y < y1; ++y)
            GSMem::ReadIndexRun(vram, t.psm, t.tbp0, t.tbw, x0, y, w, rows.data() + static_cast<size_t>(y - y0) * w);
        const Api &g = gl();
        g.BindTexture(kTexture2D, tex.id);
        g.PixelStorei(kUnpackAlignment, 1);
        g.TexSubImage2D(kTexture2D, 0, static_cast<GLint>(x0), static_cast<GLint>(y0), w, y1 - y0, 0x1903 /*RED*/, kUnsignedByte, rows.data());
    }

    Texture &indexTextureFor(const GSDrawState &s, uint32_t needX0 = 0u, uint32_t needX1 = ~0u, uint32_t needLo = 0u,
                             uint32_t needHi = ~0u)
    {
        const GSTex0Reg &t = s.context.tex0;
        const uint64_t key = (1ull << 63) | t.tbp0 | (static_cast<uint64_t>(t.tbw) << 14) | (static_cast<uint64_t>(t.psm & 63) << 20) |
                             (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 38);
        Texture &tex = textures[key];
        tex.lastUse = ++useCounter;
        const uint32_t fullH = std::max<uint32_t>(1u, s.textureHeight), fullW = std::max<uint32_t>(1u, s.textureWidth);
        needHi = std::min(needHi, fullH);
        needLo = std::min(needLo, needHi);
        needX1 = std::min(needX1, fullW);
        needX0 = std::min(needX0, needX1);
        const uint64_t needArea = static_cast<uint64_t>(needHi - needLo) * (needX1 - needX0);
        const bool wantPartial = partialRows() && static_cast<uint64_t>(fullW) * fullH >= 16384u && needArea != 0u &&
                                 needArea * 2u <= static_cast<uint64_t>(fullW) * fullH;
        if (tex.valid && tex.partial)
        {
            // rows uploaded again since (same layout): re-decode those that are decoded
            if (tex.dirtyY1 > tex.dirtyY0)
            {
                ++stats.texRowDecodes, PS2_PERF_COUNT(texRowDecodes);
                decodeIndexRect(tex, t, tex.colLo, tex.colHi, std::max(tex.dirtyY0, tex.rowLo), std::min(tex.dirtyY1, tex.rowHi));
                tex.dirtyY0 = tex.dirtyY1 = 0;
            }
            // grow the decoded rectangle to the bounding box of it and the texels this draw samples: decode the new bands
            const uint32_t lo = std::min(needLo, tex.rowLo), hi = std::max(needHi, tex.rowHi);
            const uint32_t cl = std::min(needX0, tex.colLo), ch = std::max(needX1, tex.colHi);
            if (lo < tex.rowLo || hi > tex.rowHi || cl < tex.colLo || ch > tex.colHi)
            {
                PS2_PERF_COUNT(texRowDecodes);
                decodeIndexRect(tex, t, cl, ch, lo, tex.rowLo);                 // above
                decodeIndexRect(tex, t, cl, ch, tex.rowHi, hi);                 // below
                decodeIndexRect(tex, t, cl, tex.colLo, tex.rowLo, tex.rowHi);   // left
                decodeIndexRect(tex, t, tex.colHi, ch, tex.rowLo, tex.rowHi);   // right
                tex.rowLo = lo, tex.rowHi = hi, tex.colLo = cl, tex.colHi = ch;
            }
            ++stats.texHits;
            return tex;
        }
        if (!tex.valid && wantPartial)
        {
            ++stats.texDecodes, PS2_PERF_COUNT(texDecodes);
            noteDecode(t.tbp0, t.psm, s.textureWidth, s.textureHeight, true, 0u);
            const Api &g = gl();
            const bool sameLayout = tex.id && tex.indexed && tex.width == fullW && tex.height == fullH;
            for (const Texture::Version &v : tex.versions)
                g.DeleteTextures(1, &v.id);
            tex.versions.clear();
            tex.width = fullW;
            tex.height = fullH;
            tex.contentHash = 0u; // partial content is never revalidated by hash
            tex.indexed = true;
            tex.tbp = t.tbp0;
            tex.tbw = t.tbw;
            tex.psm = static_cast<uint8_t>(t.psm & 63u);
            tex.dirtyY0 = tex.dirtyY1 = 0;
            tex.blockLo = t.tbp0;
            tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
            if (!tex.id)
                g.GenTextures(1, &tex.id);
            if (!sameLayout)
            {
                g.BindTexture(kTexture2D, tex.id);
                g.TexImage2D(kTexture2D, 0, 0x8229 /*R8*/, tex.width, tex.height, 0, 0x1903 /*RED*/, kUnsignedByte, nullptr);
                g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
                g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            }
            decodeIndexRect(tex, t, needX0, needX1, needLo, needHi);
            tex.partial = true;
            tex.rowLo = needLo;
            tex.rowHi = needHi;
            tex.colLo = needX0;
            tex.colHi = needX1;
            markValid(tex);
            if (textures.size() > 6000u)
                evictOld();
            return tex;
        }
        if (tex.valid && tex.dirtyY1 > tex.dirtyY0)
        {
            ++stats.texRowDecodes, PS2_PERF_COUNT(texRowDecodes);
            const uint32_t y0 = tex.dirtyY0, y1 = std::min(tex.dirtyY1, tex.height);
            tex.dirtyY0 = tex.dirtyY1 = 0;
            if (y1 > y0)
            {
                std::vector<uint8_t> rows(static_cast<size_t>(tex.width) * (y1 - y0));
                for (uint32_t y = y0; y < y1; ++y)
                    GSMem::ReadIndexRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, tex.width,
                                        rows.data() + static_cast<size_t>(y - y0) * tex.width);
                const Api &g = gl();
                g.BindTexture(kTexture2D, tex.id);
                g.PixelStorei(kUnpackAlignment, 1);
                g.TexSubImage2D(kTexture2D, 0, 0, static_cast<GLint>(y0), tex.width, y1 - y0, 0x1903 /*RED*/, kUnsignedByte, rows.data());
                // Tekken 4 port: the texture now holds the current memory, so its hash must too. With the hash of the last
                // full decode the fight's name plates (both drawn from one area at 0x3b60, re-uploaded per name) were
                // revalidated with the other name's rows: player 1 showed "LAW" instead of "KAZUYA".
                tex.contentHash = hashBlocks(t.tbp0, t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height));
            }
            return tex;
        }
        if (tex.valid)
        {
            ++stats.texHits;
            return tex;
        }
        // Tekken 4 port: invalidated by an upload of the same bytes (the game re-uploads its character textures every
        // frame): the raw blocks hash as before, so the decoded indices are still right.
        const uint64_t contentHash = hashBlocks(t.tbp0, t.tbp0 + spanBlocks(t.psm, t.tbw, std::max<uint32_t>(1u, s.textureHeight)));
        // (a partial texture left behind is decoded whole below: never revalidated, never kept as a version)
        if (tex.partial)
            tex.contentHash = 0u;
        tex.partial = false;
        tex.rowLo = 0u;
        tex.rowHi = fullH;
        tex.colLo = 0u;
        tex.colHi = fullW;
        if (contentHash != 0u && tex.id && tex.contentHash == contentHash && tex.indexed &&
            tex.width == std::max<uint32_t>(1u, s.textureWidth) && tex.height == std::max<uint32_t>(1u, s.textureHeight))
        {
            ++stats.texRevalidated;
            markValid(tex);
            tex.dirtyY0 = tex.dirtyY1 = 0;
            return tex;
        }
        const bool sameLayout = tex.id && tex.indexed && tex.width == std::max<uint32_t>(1u, s.textureWidth) &&
                                tex.height == std::max<uint32_t>(1u, s.textureHeight);
        GLuint reuse = 0; // a GL texture of an evicted version to decode into
        if (indexVersions() && sameLayout && contentHash != 0u)
        {
            for (Texture::Version &v : tex.versions)
                if (v.hash == contentHash)
                {
                    // an earlier decode of the same bytes: swap it in, keep the current one as a version
                    std::swap(v.hash, tex.contentHash);
                    std::swap(v.id, tex.id);
                    v.lastUse = useCounter;
                    ++stats.texRevalidated;
                    PS2_PERF_COUNT(texVersionHits);
                    tex.blockLo = t.tbp0;
                    tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
                    markValid(tex);
                    tex.dirtyY0 = tex.dirtyY1 = 0;
                    return tex;
                }
            if (tex.contentHash != 0u)
            {
                // keep the current decode as a version (at most kIndexVersions; the least recently used one is reused)
                constexpr size_t kIndexVersions = 8u;
                if (tex.versions.size() < kIndexVersions)
                    tex.versions.push_back({tex.contentHash, tex.id, useCounter});
                else
                {
                    auto oldest = std::min_element(tex.versions.begin(), tex.versions.end(),
                                                   [](const Texture::Version &a, const Texture::Version &b) { return a.lastUse < b.lastUse; });
                    reuse = oldest->id;
                    *oldest = {tex.contentHash, tex.id, useCounter};
                }
                tex.id = reuse;
            }
        }
        else if (!sameLayout && !tex.versions.empty())
        {
            // another layout in this slot: the versions no longer fit it
            const Api &g = gl();
            for (const Texture::Version &v : tex.versions)
                g.DeleteTextures(1, &v.id);
            tex.versions.clear();
        }
        ++stats.texDecodes, PS2_PERF_COUNT(texDecodes);
        noteDecode(t.tbp0, t.psm, s.textureWidth, s.textureHeight, true, contentHash);
        classifyTexture(s);
        tex.width = std::max<uint32_t>(1u, s.textureWidth);
        tex.height = std::max<uint32_t>(1u, s.textureHeight);
        tex.contentHash = contentHash;
        tex.indexed = true;
        tex.tbp = t.tbp0;
        tex.tbw = t.tbw;
        tex.psm = static_cast<uint8_t>(t.psm & 63u);
        tex.dirtyY0 = tex.dirtyY1 = 0;
        std::vector<uint8_t> &indices = decodeScratch; // reused: no allocation per decode
        indices.resize(static_cast<size_t>(tex.width) * tex.height);
        for (uint32_t y = 0; y < tex.height; ++y)
            GSMem::ReadIndexRun(vram, t.psm, t.tbp0, t.tbw, 0u, y, tex.width,
                                indices.data() + static_cast<size_t>(y) * tex.width);
        static const long idxDumpTbp = std::getenv("GOW_GS_HW_PALDUMP") ? std::strtol(std::getenv("GOW_GS_HW_PALDUMP"), nullptr, 16) : -1;
        static int idxDumped = 0;
        // GOW_GS_HW_PALDUMPMAX=<n>: dump the first n decodes of that block's index texture (default 4)
        static const int idxDumpMax = std::getenv("GOW_GS_HW_PALDUMPMAX") ? std::atoi(std::getenv("GOW_GS_HW_PALDUMPMAX")) : 4;
        if (idxDumpTbp >= 0 && static_cast<long>(t.tbp0) == idxDumpTbp && idxDumped < idxDumpMax)
        {
            ++idxDumped;
            uint32_t hist[256] = {};
            for (uint8_t i : indices)
                ++hist[i];
            uint32_t pageHash = 2166136261u;
            for (uint32_t i = 0; i < 8192u; ++i)
                pageHash = (pageHash ^ vram[static_cast<size_t>(t.tbp0) * 256u + i]) * 16777619u;
            std::fprintf(stderr, "[gs-hw-idx] tbp=%x page hash %08x %ux%u psm=%u tbw=%u (index:count)", t.tbp0, pageHash, tex.width, tex.height, t.psm, t.tbw);
            for (int i = 0; i < 256; ++i)
                if (hist[i])
                    std::fprintf(stderr, " %d:%u", i, hist[i]);
            std::fprintf(stderr, "\n");
            char name[64];
            std::snprintf(name, sizeof(name), "gs_hw_idx_%d_tbp%x_%ux%u.pgm", idxDumped, t.tbp0, tex.width, tex.height);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P5 %u %u 255 ", tex.width, tex.height);
                std::fwrite(indices.data(), 1, indices.size(), f);
                std::fclose(f);
            }
        }
        tex.blockLo = t.tbp0;
        tex.blockHi = t.tbp0 + spanBlocks(t.psm, t.tbw, tex.height);
        const Api &g = gl();
        if (!tex.id)
            g.GenTextures(1, &tex.id);
        g.BindTexture(kTexture2D, tex.id);
        g.PixelStorei(kUnpackAlignment, 1);
        g.TexImage2D(kTexture2D, 0, 0x8229 /*R8*/, tex.width, tex.height, 0, 0x1903 /*RED*/, kUnsignedByte, indices.data());
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        markValid(tex);
        if (textures.size() > 6000u)
            evictOld();
        return tex;
    }

    // 256x1 RGBA palette of the current CLUT as the draw reads it (format, offset, TEXA alpha)
    std::unordered_map<uint64_t, GLuint> palettes;
    GLuint paletteFor(const GSDrawState &s)
    {
        const GSTex0Reg &t = s.context.tex0;
        uint64_t key = clutHash;
        key = key * 1099511628211ull ^ (t.cpsm | (t.csm << 8) | (t.csa << 16) | (static_cast<uint64_t>(t.psm & 63) << 24));
        key = key * 1099511628211ull ^ (static_cast<uint64_t>(s.texa.ta0) | (static_cast<uint64_t>(s.texa.ta1) << 8) | (static_cast<uint64_t>(s.texa.aem) << 16));
        // GOW_GS_HW_PALDUMP=<tbp hex>: debug, print the palette of each CLUT (cbp) the draws textured from that block use (rgba per entry)
        static const long palDumpTbp = std::getenv("GOW_GS_HW_PALDUMP") ? std::strtol(std::getenv("GOW_GS_HW_PALDUMP"), nullptr, 16) : -1;
        static std::vector<uint32_t> palDumpedCbp; // one dump per CLUT address
        if (palDumpTbp >= 0 && static_cast<long>(t.tbp0) == palDumpTbp && palDumpedCbp.size() < 8 &&
            std::find(palDumpedCbp.begin(), palDumpedCbp.end(), t.cbp) == palDumpedCbp.end())
        {
            palDumpedCbp.push_back(t.cbp);
            uint32_t colors[256];
            store.BuildPalette(s, colors);
            std::fprintf(stderr, "[gs-hw-pal] tbp=%x cbp=%x cpsm=%u csm=%u csa=%u:", t.tbp0, t.cbp, t.cpsm, t.csm, t.csa);
            for (int i = 0; i < 256; ++i)
                std::fprintf(stderr, "%s%08x", (i % 16) ? " " : "\n  ", colors[i]);
            std::fprintf(stderr, "\n");
        }
        const auto it = palettes.find(key);
        if (it != palettes.end())
            return it->second;
        const Api &g = gl();
        if (palettes.size() > 4096u)
        {
            for (auto &kv : palettes)
                g.DeleteTextures(1, &kv.second);
            palettes.clear();
        }
        uint32_t colors[256];
        store.BuildPalette(s, colors);
        GLuint id = 0;
        g.GenTextures(1, &id);
        g.BindTexture(kTexture2D, id);
        g.PixelStorei(kUnpackAlignment, 4);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), 256, 1, 0, kRgba, kUnsignedByte, colors);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        palettes[key] = id;
        return id;
    }

    void evictOld()
    {
        const Api &g = gl();
        for (auto it = textures.begin(); it != textures.end();)
        {
            if (it->second.lastUse + 3000u < useCounter)
            {
                if (it->second.id)
                    g.DeleteTextures(1, &it->second.id);
                for (const Texture::Version &v : it->second.versions)
                    g.DeleteTextures(1, &v.id);
                it = textures.erase(it);
            }
            else
                ++it;
        }
        rebuildLiveTextures();
    }

    // Raw GS memory of blocks [lo, hi) (256 bytes each); 0 when the range wraps past the end (never revalidated).
    uint64_t hashBlocks(uint32_t lo, uint32_t hi) const
    {
        if (!vram || hi <= lo || static_cast<uint64_t>(hi) * 256u > vramSize)
            return 0u;
        uint64_t hash = 1469598103934665603ull;
        const uint8_t *p = vram + static_cast<size_t>(lo) * 256u, *end = vram + static_cast<size_t>(hi) * 256u;
        for (; p < end; p += 8)
        {
            uint64_t word;
            std::memcpy(&word, p, sizeof(word));
            hash = (hash ^ word) * 1099511628211ull;
            hash ^= hash >> 29u;
        }
        return hash | 1u;
    }

    void invalidateBlocks(uint32_t lo, uint32_t hi)
    {
        for (size_t i = 0; i < liveTextures.size();)
        {
            Texture &t = *liveTextures[i];
            if (t.valid && !(t.blockLo < hi && lo < t.blockHi))
            {
                ++i;
                continue;
            }
            t.valid = false;
            t.listed = false;
            liveTextures[i] = liveTextures.back();
            liveTextures.pop_back();
        }
    }

    // Tekken 4 port: like invalidateBlocks, but an index texture with exactly the upload's layout (same base, width and
    // format, rectangle inside it) keeps its decoded rows and only re-decodes the uploaded ones at its next use.
    void invalidateForUpload(const GSTransferCommand &c, uint32_t lo, uint32_t hi)
    {
        const uint32_t x0 = c.trxpos.dsax, y0 = c.trxpos.dsay, x1 = x0 + c.trxreg.rrw, y1 = y0 + c.trxreg.rrh;
        for (size_t i = 0; i < liveTextures.size();)
        {
            Texture &t = *liveTextures[i];
            if (t.valid && !(t.blockLo < hi && lo < t.blockHi))
            {
                ++i;
                continue;
            }
            if (t.valid && t.indexed && t.tbp == c.bitbltbuf.dbp && t.tbw == c.bitbltbuf.dbw &&
                t.psm == (c.bitbltbuf.dpsm & 63u) && x1 <= t.width && y1 <= t.height)
            {
                if (t.dirtyY1 <= t.dirtyY0)
                    t.dirtyY0 = y0, t.dirtyY1 = y1;
                else
                    t.dirtyY0 = std::min(t.dirtyY0, y0), t.dirtyY1 = std::max(t.dirtyY1, y1);
                ++i;
                continue;
            }
            t.valid = false;
            t.listed = false;
            liveTextures[i] = liveTextures.back();
            liveTextures.pop_back();
        }
    }

    // The render target whose memory range holds the texture's first block (and was not overwritten by an upload since).
    // Formats that live only in bits 24-31 (T8H, T4HL, T4HH) are never written by a 24 bit target (CT24 colour, Z24): the
    // game keeps uploaded textures there, so such a target is not their source.
    RenderTarget *findReinterpretSource(uint32_t tbp, uint8_t tpsm)
    {
        const bool highByte = tpsm == GS_PSM_T8H || tpsm == GS_PSM_T4HL || tpsm == GS_PSM_T4HH;
        for (RenderTarget &t : targets)
        {
            if (t.usedW == 0u || (highByte && t.psm == GS_PSM_CT24))
                continue;
            const uint32_t tlo = t.fbp * 32u, thi = tlo + spanBlocks(t.psm, t.fbw, t.usedH);
            if (tbp < tlo || tbp >= thi)
                continue;
            // Tekken 4 port: only the page area the target actually drew in (usedW x usedH) holds its picture. Tekken's
            // back buffer is 1024 wide (fbw 16) but draws 640; its CLUTs and textures live in the pages to the right,
            // which came from uploads (reading them from the target cost a synchronous GPU readback per CLUT load).
            {
                const uint32_t page = (tbp - tlo) / 32u, pageH = family16(t.psm) ? 64u : 32u;
                if ((page % std::max<uint32_t>(t.fbw, 1u)) * 64u >= t.usedW || (page / std::max<uint32_t>(t.fbw, 1u)) * pageH >= t.usedH)
                    continue;
            }
            // Tekken 4 port: the O(targets) depth check after the range test, not before it for every target.
            if (overwrittenByDepth(t))
                continue;
            if (!uploadedSinceDraw(t, tbp))
                return &t;
        }
        return nullptr;
    }

    // True when the game uploaded new pixels at `tbp` after the target's last draw: the memory then holds the upload,
    // not the target's picture (e.g. FMV frames uploaded where a temporary target lived), so the texture comes from
    // the store.
    // True when another target's depth buffer lives in this target's memory (ZBP == its FBP) and was written after this
    // target's last colour draw: the memory then holds Z, not the colour picture. Example: the movie player draws its
    // pictures into fbp 104, which is the gameplay Z buffer; after the movie the depth writes replace them, and the
    // game's passes that read the Z memory as PSMCT24 (fog/DOF strips into fbp 320) must not see the old movie frame.
    bool overwrittenByDepth(const RenderTarget &t) const
    {
        for (const RenderTarget &o : targets)
            if (&o != &t && o.depth && o.zbp == t.fbp && o.lastDepthSeq > t.lastDrawSeq)
                return true;
        return false;
    }

    // Tekken 4 port: records an upload (or local copy) into blocks [lo, hi) on every target whose memory it overlaps.
    void noteUploadOnTargets(uint32_t lo, uint32_t hi, uint64_t seq)
    {
        for (RenderTarget &t : targets)
        {
            const uint32_t tlo = t.fbp * 32u, thi = tlo + spanBlocks(t.psm, t.fbw, t.pxH);
            const uint32_t a = std::max(lo, tlo), b = std::min(hi, thi);
            if (a >= b)
                continue;
            if (t.blockUploadSeq.size() < thi - tlo)
                t.blockUploadSeq.resize(thi - tlo, 0u);
            std::fill(t.blockUploadSeq.begin() + (a - tlo), t.blockUploadSeq.begin() + (b - tlo), seq);
        }
    }

    bool uploadedSinceDraw(const RenderTarget &t, uint32_t tbp) const
    {
        const uint32_t tlo = t.fbp * 32u;
        // Tekken 4 port: the per-block record replaces the scan of the (trimmed) upload log; it is cleared where an upload
        // was also written into the target (applyRtUpload), so those blocks come from the target again.
        return tbp >= tlo && tbp - tlo < t.blockUploadSeq.size() && t.blockUploadSeq[tbp - tlo] > t.lastDrawSeq;
    }

    // Tekken 4 port: a host->local transfer to a render target's base in its own format and width (the intro movie's
    // pictures go into the back buffer as 16x448 columns) is also written into the target's GL texture, once its data has
    // arrived (the next flush). Without it the target kept its old picture and the movie stayed black.
    GSTransferCommand rtUpload{};
    bool rtUploadPending = false;

    RenderTarget *rtUploadTarget(const GSTransferCommand &c)
    {
        const uint32_t dpsm = c.bitbltbuf.dpsm & 0x3Fu;
        if (dpsm != GS_PSM_CT32 && dpsm != GS_PSM_CT24 && dpsm != GS_PSM_CT16 && dpsm != GS_PSM_CT16S)
            return nullptr;
        if ((c.bitbltbuf.dbp % 32u) != 0u)
            return nullptr;
        RenderTarget *t = findTarget(c.bitbltbuf.dbp / 32u);
        // pixel (x, y) maps to target pixel (x, y) for any width up to the target's, like draws (target())
        if (!t || t->psm != dpsm || t->fbw < c.bitbltbuf.dbw)
            return nullptr;
        if (c.trxpos.dsax + c.trxreg.rrw > t->pxW || c.trxpos.dsay + c.trxreg.rrh > t->pxH)
            return nullptr;
        return t;
    }

    // Tekken 4 port: true while a movie hands out pictures (one in the last 0.5 s). Only its uploads get the smooth
    // upscale; the game's own uploads into targets keep the filtered blit.
    uint32_t movieSeenPictures = 0u;
    std::chrono::steady_clock::time_point movieSeenAt{};
    bool movieActive()
    {
        const uint32_t n = ps2_stubs::mpegPicturesServed();
        const auto now = std::chrono::steady_clock::now();
        if (n != movieSeenPictures)
        {
            movieSeenPictures = n;
            movieSeenAt = now;
        }
        return n != 0u && now - movieSeenAt < std::chrono::milliseconds(500);
    }

    // ---- Black & Bruised: native image tiles (2026-11-20) ----
    // The game shows movies and pictures as a mosaic: each 16x32 tile is uploaded into one texture slot (tbp 0x3220)
    // and drawn as a sprite about 1:1 with PS2 pixels, then the next tile overwrites the slot. At an internal scale above
    // 1 every tile was filtered on its own and clamped at its edges: a faint grid over the whole picture (user: "a
    // transparent checkerboard", "cuts"; none at native resolution). Such sprites are now drawn at native resolution on
    // the CPU (GS sampling: point at the integer pixel, bilinear about texel centres, CLAMP/REPEAT, decal/modulate) into
    // the target's native staging copy, and the region plus the previous tiles' last texels is upscaled into the target
    // from there, like an upload into the target (applyRtUpload): neighbouring tiles are filtered against each other.
    // Depth is not written (the tiles are full-screen pictures). PS2X_GL_TILE_CANVAS=0: off.
    static bool tileCanvasOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_GL_TILE_CANVAS"); return !v || *v != '0'; }();
        return on;
    }
    bool ensureStage(RenderTarget &t)
    {
        if (t.stageTex)
            return true;
        const Api &g = gl();
        g.GenTextures(1, &t.stageTex);
        g.BindTexture(kTexture2D, t.stageTex);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), t.pxW, t.pxH, 0, kRgba, kUnsignedByte, nullptr);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
        g.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        g.GenFramebuffers(1, &t.stageFbo);
        g.BindFramebuffer(kFramebuffer, t.stageFbo);
        g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, t.stageTex, 0);
        return t.stageTex != 0u;
    }
    // Upscale [x0, x0 + w) x [y0, y0 + h) of the staging copy into the target. With `left` / `up` (the texels there
    // were drawn by this path in this picture: a neighbouring tile), the last texels of the neighbour are re-filtered
    // against the new tile too; otherwise the filter clamps at the tile's own edge (a lone sprite: the staging copy
    // around it is stale). Right / below always clamp: the next tile re-filters them.
    void blitStage(RenderTarget &t, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, bool left, bool up)
    {
        const Api &g = gl();
        const auto host = [&](uint32_t v) { return static_cast<GLint>(std::lround(static_cast<double>(v) * scale)); };
        if (movieScaleProgram)
        {
            const uint32_t sx0 = left && x0 >= 2u ? x0 - 2u : x0, sy0 = up && y0 >= 2u ? y0 - 2u : y0;
            const uint32_t bx0 = left && x0 >= 4u ? x0 - 4u : x0, by0 = up && y0 >= 4u ? y0 - 4u : y0;
            g.BindFramebuffer(kFramebuffer, t.fbo);
            g.Viewport(0, 0, t.glW, t.glH);
            g.Enable(kScissorTest);
            g.Scissor(host(sx0), host(sy0), host(x0 + w) - host(sx0), host(y0 + h) - host(sy0));
            g.Disable(kBlend);
            g.Disable(kDepthTest);
            g.Disable(kCullFace);
            g.DepthMask(kFalse);
            g.ColorMask(kTrue, kTrue, kTrue, kTrue);
            g.UseProgram(movieScaleProgram);
            g.ActiveTexture(kTexture0);
            g.BindTexture(kTexture2D, t.stageTex);
            g.Uniform1i(locMsSrc, 0);
            g.Uniform2f(locMsScale, scale, scale);
            g.Uniform4i(locMsBounds, static_cast<GLint>(bx0), static_cast<GLint>(by0), static_cast<GLint>(x0 + w) - 1,
                        static_cast<GLint>(y0 + h) - 1);
            g.BindVertexArray(presentVao);
            g.DrawArrays(kTriangles, 0, 3);
            g.BindVertexArray(vao);
            g.UseProgram(program);
            g.BindBuffer(kArrayBuffer, vbo);
            g.Disable(kScissorTest);
        }
        else
        {
            const uint32_t sx0 = left && x0 ? x0 - 1u : x0, sy0 = up && y0 ? y0 - 1u : y0;
            g.BindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, t.stageFbo);
            g.BindFramebuffer(0x8CA9 /*DRAW_FRAMEBUFFER*/, t.fbo);
            g.Disable(kScissorTest);
            g.BlitFramebuffer(static_cast<GLint>(sx0), static_cast<GLint>(sy0), static_cast<GLint>(x0 + w), static_cast<GLint>(y0 + h),
                              host(sx0), host(sy0), host(x0 + w), host(y0 + h), kColorBufferBit, kLinear);
            g.BindFramebuffer(kFramebuffer, t.fbo);
        }
    }
    std::vector<uint32_t> tileScratch;
    // per staging pixel: presentCount + 1 of the picture whose tile last wrote it (0 = never); one map per target fbp
    std::unordered_map<uint32_t, std::vector<int>> tileStamp;
    // xs/ys: sprite edges in vertex coordinates (with XYOFFSET), us/vs: texel coordinates at those edges; cv: colour
    bool tryCanvasSprite(const GSDrawState &s, const GSVertex &cv, const float xs[2], const float ys[2], const float us[2],
                         const float vs[2])
    {
        const GSContext &c = s.context;
        if (!tileCanvasOn() || scale <= 1.0f || !vram || !s.prim.tme || s.prim.fge)
            return false;
        const uint32_t psm = c.tex0.psm & 63u, fpsm = c.frame.psm;
        // (16-bit targets are held as RGBA8 like every target here; the GL draws do not reduce their colours either)
        if ((psm != GS_PSM_CT32 && psm != GS_PSM_CT24) ||
            (fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24 && fpsm != GS_PSM_CT16 && fpsm != GS_PSM_CT16S))
            return false;
        const uint32_t tw = std::max<uint32_t>(1u, s.textureWidth), th = std::max<uint32_t>(1u, s.textureHeight);
        if (tw > 64u || th > 64u || c.frame.fbmsk != 0u || (c.test & 1u) != 0u || ((c.test >> 14) & 1u) != 0u)
            return false;
        if (s.prim.abe)
        {
            const uint64_t al = c.alpha;
            if ((al & 3u) != ((al >> 2) & 3u) || ((al >> 6) & 3u) != 0u)
                return false; // only a plain write of the texture colour
        }
        const uint32_t tfx = c.tex0.tfx;
        const uint32_t wms = static_cast<uint32_t>(c.clamp & 3u), wmt = static_cast<uint32_t>((c.clamp >> 2) & 3u);
        if (tfx > 1u || wms > 1u || wmt > 1u || xs[1] == xs[0] || ys[1] == ys[0])
            return false;
        // about 1:1 with PS2 pixels (0.5 .. 2 texels per pixel): the native image mosaic, not scaled sprites
        const float du = (us[1] - us[0]) / (xs[1] - xs[0]), dv = (vs[1] - vs[0]) / (ys[1] - ys[0]);
        if (std::fabs(du) < 0.5f || std::fabs(du) > 2.0f || std::fabs(dv) < 0.5f || std::fabs(dv) > 2.0f)
            return false;
        const float ofx = c.xyoffset.ofx / 16.0f, ofy = c.xyoffset.ofy / 16.0f;
        // covered pixels: n with ceil(lo) <= n < ceil(hi), clipped to the scissor and the target
        RenderTarget &rt = target(c.frame.fbp, c.frame.fbw);
        int px0 = static_cast<int>(std::ceil(std::min(xs[0], xs[1]) - ofx)), px1 = static_cast<int>(std::ceil(std::max(xs[0], xs[1]) - ofx));
        int py0 = static_cast<int>(std::ceil(std::min(ys[0], ys[1]) - ofy)), py1 = static_cast<int>(std::ceil(std::max(ys[0], ys[1]) - ofy));
        px0 = std::max(px0, static_cast<int>(c.scissor.x0)), py0 = std::max(py0, static_cast<int>(c.scissor.y0));
        px1 = std::min({px1, static_cast<int>(c.scissor.x1) + 1, static_cast<int>(rt.pxW)});
        py1 = std::min({py1, static_cast<int>(c.scissor.y1) + 1, static_cast<int>(rt.pxH)});
        if (px1 <= px0 || py1 <= py0)
            return true; // nothing covered
        flushPending(WhyOther); // earlier draws first
        if (!ensure() || !ensureStage(rt))
            return false;
        const uint32_t w = static_cast<uint32_t>(px1 - px0), h = static_cast<uint32_t>(py1 - py0);
        tileScratch.resize(static_cast<size_t>(w) * h);
        const uint32_t tbw = std::max<uint32_t>(1u, c.tex0.tbw);
        const bool linear = s.linearFilter;
        const auto texel = [&](int x, int y) -> uint32_t
        {
            x = wms == 0u ? ((x % static_cast<int>(tw)) + static_cast<int>(tw)) % static_cast<int>(tw) : std::clamp(x, 0, static_cast<int>(tw) - 1);
            y = wmt == 0u ? ((y % static_cast<int>(th)) + static_cast<int>(th)) % static_cast<int>(th) : std::clamp(y, 0, static_cast<int>(th) - 1);
            uint32_t v = store.Read(psm, c.tex0.tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            if (psm == GS_PSM_CT24) // alpha from TEXA
                v = (v & 0xFFFFFFu) | (static_cast<uint32_t>((s.texa.aem && (v & 0xFFFFFFu) == 0u) ? 0u : s.texa.ta0) << 24);
            return v;
        };
        const float vr = cv.r, vg = cv.g, vb = cv.b, va = cv.a;
        for (uint32_t j = 0; j < h; ++j)
        {
            const float v = vs[0] + (static_cast<float>(py0 + static_cast<int>(j)) + ofy - ys[0]) * dv;
            for (uint32_t i = 0; i < w; ++i)
            {
                const float u = us[0] + (static_cast<float>(px0 + static_cast<int>(i)) + ofx - xs[0]) * du;
                float ch[4];
                if (linear)
                {
                    const float fu = u - 0.5f, fv = v - 0.5f;
                    const int iu = static_cast<int>(std::floor(fu)), iv = static_cast<int>(std::floor(fv));
                    const float au = fu - static_cast<float>(iu), av = fv - static_cast<float>(iv);
                    const uint32_t t00 = texel(iu, iv), t10 = texel(iu + 1, iv), t01 = texel(iu, iv + 1), t11 = texel(iu + 1, iv + 1);
                    for (int k = 0; k < 4; ++k)
                    {
                        const float a0 = static_cast<float>((t00 >> (8 * k)) & 0xFFu) * (1.0f - au) + static_cast<float>((t10 >> (8 * k)) & 0xFFu) * au;
                        const float a1 = static_cast<float>((t01 >> (8 * k)) & 0xFFu) * (1.0f - au) + static_cast<float>((t11 >> (8 * k)) & 0xFFu) * au;
                        ch[k] = a0 * (1.0f - av) + a1 * av;
                    }
                }
                else
                {
                    const uint32_t t = texel(static_cast<int>(std::floor(u)), static_cast<int>(std::floor(v)));
                    for (int k = 0; k < 4; ++k)
                        ch[k] = static_cast<float>((t >> (8 * k)) & 0xFFu);
                }
                if (tfx == 0u) // MODULATE: colour * vertex / 128
                {
                    ch[0] *= vr / 128.0f, ch[1] *= vg / 128.0f, ch[2] *= vb / 128.0f;
                    ch[3] = c.tex0.tcc ? ch[3] * va / 128.0f : va;
                }
                else if (!c.tex0.tcc) // DECAL: the texture colour, alpha from the vertex without TCC
                    ch[3] = va;
                uint32_t out = 0;
                for (int k = 0; k < 4; ++k)
                    out |= static_cast<uint32_t>(std::clamp(std::lround(ch[k]), 0L, 255L)) << (8 * k);
                tileScratch[static_cast<size_t>(j) * w + i] = out;
            }
        }
        const Api &g = gl();
        g.BindTexture(kTexture2D, rt.stageTex);
        g.PixelStorei(kUnpackAlignment, 4);
        g.TexSubImage2D(kTexture2D, 0, px0, py0, w, h, kRgba, kUnsignedByte, tileScratch.data());
        // neighbours drawn by this path in this picture (4 texels: the bicubic's reach)
        std::vector<int> &stamp = tileStamp[rt.fbp];
        if (stamp.size() != static_cast<size_t>(rt.pxW) * rt.pxH)
            stamp.assign(static_cast<size_t>(rt.pxW) * rt.pxH, 0);
        const int now = presentCount + 1;
        const auto drawnNow = [&](int x, int y) { return stamp[static_cast<size_t>(y) * rt.pxW + static_cast<size_t>(x)] == now; };
        bool left = px0 >= 4, up = py0 >= 4;
        for (int y = py0; left && y < py1; ++y)
            for (int x = px0 - 4; left && x < px0; ++x)
                left = drawnNow(x, y);
        for (int y = py0 - 4; up && y < py0; ++y)
            for (int x = px0; up && x < px1; ++x)
                up = drawnNow(x, y);
        for (int y = py0; y < py1; ++y)
            std::fill_n(stamp.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * rt.pxW + static_cast<size_t>(px0)), w, now);
        blitStage(rt, static_cast<uint32_t>(px0), static_cast<uint32_t>(py0), w, h, left, up);
        rt.psm = static_cast<uint8_t>(fpsm);
        rt.lastDrawPresent = presentCount;
        ++rt.version;
        rt.lastDrawSeq = ++writeSeq;
        rt.usedW = std::max(rt.usedW, static_cast<uint32_t>(px1));
        rt.usedH = std::max(rt.usedH, static_cast<uint32_t>(py1));
        return true;
    }

    void applyRtUpload()
    {
        if (!rtUploadPending)
            return;
        rtUploadPending = false;
        const GSTransferCommand &c = rtUpload;
        RenderTarget *t = initialized && vram ? rtUploadTarget(c) : nullptr;
        if (!t)
            return;
        const uint32_t x0 = c.trxpos.dsax, y0 = c.trxpos.dsay, w = c.trxreg.rrw, h = c.trxreg.rrh;
        if (w == 0u || h == 0u)
            return;
        const bool sixteen = family16(t->psm);
        std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
        for (uint32_t y = 0; y < h; ++y)
        {
            uint32_t *line = pixels.data() + static_cast<size_t>(y) * w;
            GSMem::ReadColorRun(vram, t->psm, c.bitbltbuf.dbp, c.bitbltbuf.dbw, x0, y0 + y, w, line);
            if (sixteen)
                for (uint32_t x = 0; x < w; ++x)
                    line[x] = GSCpuInternal::Rgba5551ToRgba8888(static_cast<uint16_t>(line[x]));
        }
        const Api &g = gl();
        if (scale == 1.0f)
        {
            g.BindTexture(kTexture2D, t->color);
            g.PixelStorei(kUnpackAlignment, 4);
            g.TexSubImage2D(kTexture2D, 0, static_cast<GLint>(x0), static_cast<GLint>(y0), w, h, kRgba, kUnsignedByte, pixels.data());
        }
        else
        {
            // Tekken 4 port (internal resolution): into the native-size staging copy, then a filtered blit of the region
            // into the scaled target. The region is widened by one texel up and left so the previous strip's last
            // texels are filtered against the new neighbours (the movie arrives as 16 px wide columns).
            if (!t->stageTex)
            {
                g.GenTextures(1, &t->stageTex);
                g.BindTexture(kTexture2D, t->stageTex);
                g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), t->pxW, t->pxH, 0, kRgba, kUnsignedByte, nullptr);
                g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
                g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
                g.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
                g.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
                g.GenFramebuffers(1, &t->stageFbo);
                g.BindFramebuffer(kFramebuffer, t->stageFbo);
                g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, t->stageTex, 0);
            }
            g.BindTexture(kTexture2D, t->stageTex);
            g.PixelStorei(kUnpackAlignment, 4);
            g.TexSubImage2D(kTexture2D, 0, static_cast<GLint>(x0), static_cast<GLint>(y0), w, h, kRgba, kUnsignedByte, pixels.data());
            const auto host = [&](uint32_t v) { return static_cast<GLint>(std::lround(static_cast<double>(v) * scale)); };
            if (scale > 1.0f && movieScaleProgram && movieActive())
            {
                // Bicubic pass over the strip and 2 texels left / up of it: the bicubic reaches 2 texels to the right and below,
                // so the previous strip's last columns (drawn with this strip's border repeated) are redrawn with their
                // true neighbours; the last strip of the picture needs no neighbours to its right.
                const uint32_t sx0 = x0 >= 2u ? x0 - 2u : 0u, sy0 = y0 >= 2u ? y0 - 2u : 0u;
                g.BindFramebuffer(kFramebuffer, t->fbo);
                g.Viewport(0, 0, t->glW, t->glH);
                g.Enable(kScissorTest);
                g.Scissor(host(sx0), host(sy0), host(x0 + w) - host(sx0), host(y0 + h) - host(sy0));
                g.Disable(kBlend);
                g.Disable(kDepthTest);
                g.Disable(kCullFace);
                g.DepthMask(kFalse);
                g.ColorMask(kTrue, kTrue, kTrue, kTrue);
                g.UseProgram(movieScaleProgram);
                g.ActiveTexture(kTexture0);
                g.BindTexture(kTexture2D, t->stageTex);
                g.Uniform1i(locMsSrc, 0);
                g.Uniform2f(locMsScale, scale, scale);
                g.Uniform4i(locMsBounds, 0, 0, static_cast<GLint>(x0 + w) - 1, static_cast<GLint>(y0 + h) - 1);
                g.BindVertexArray(presentVao);
                g.DrawArrays(kTriangles, 0, 3);
                g.BindVertexArray(vao);
                g.UseProgram(program);
                g.BindBuffer(kArrayBuffer, vbo);
                g.Disable(kScissorTest);
            }
            else
            {
                const uint32_t sx0 = x0 ? x0 - 1u : 0u, sy0 = y0 ? y0 - 1u : 0u;
                g.BindFramebuffer(0x8CA8 /*READ_FRAMEBUFFER*/, t->stageFbo);
                g.BindFramebuffer(0x8CA9 /*DRAW_FRAMEBUFFER*/, t->fbo);
                g.Disable(kScissorTest);
                g.BlitFramebuffer(static_cast<GLint>(sx0), static_cast<GLint>(sy0), static_cast<GLint>(x0 + w), static_cast<GLint>(y0 + h),
                                  host(sx0), host(sy0), host(x0 + w), host(y0 + h), kColorBufferBit, scale > 1.0f ? kLinear : kNearest);
                g.BindFramebuffer(kFramebuffer, t->fbo);
            }
        }
        t->usedW = std::max(t->usedW, x0 + w);
        t->usedH = std::max(t->usedH, y0 + h);
        ++t->version;
        // the target holds these blocks now
        const uint32_t tlo = t->fbp * 32u, lo = c.bitbltbuf.dbp, hi = lo + spanBlocks(c.bitbltbuf.dpsm, c.bitbltbuf.dbw, y0 + h);
        for (uint32_t b = std::max(lo, tlo); b < hi && b - tlo < t->blockUploadSeq.size(); ++b)
            t->blockUploadSeq[b - tlo] = 0u;
        ++stats.rtUploads, PS2_PERF_COUNT(rtUploads);
    }

    void setupReinterpret(const GSDrawState &s, RenderTarget &rt)
    {
        const Api &g = gl();
        const GSContext &c = s.context;
        const GSSwizzle::Format &tf = GSSwizzle::GetFormat(c.tex0.psm);
        const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rt.psm);
        const auto offsetOf = [&](const uint16_t *table) { const auto it = tableOffset.find(table); return it == tableOffset.end() ? 0u : it->second; };
        int kind = 3, texelBits = 8;
        switch (tf.kind)
        {
        case GSSwizzle::Kind::Bits32: kind = 0; texelBits = 32; break;
        case GSSwizzle::Kind::Bits24: kind = 1; texelBits = 24; break;
        case GSSwizzle::Kind::Bits16: kind = 2; texelBits = 16; break;
        case GSSwizzle::Kind::Bits8: kind = 3; texelBits = 8; break;
        default: kind = 3; texelBits = 4; break;
        }
        if (kind == 3 && pendingPalette)
        {
            // Tekken 4 port: the palette the batch started with (same BuildPalette colours); the CLUT may have changed since
            g.ActiveTexture(kTexture0 + 3);
            g.BindTexture(kTexture2D, pendingPalette);
        }
        else if (kind == 3)
        {
            uint32_t palette[256];
            store.BuildPalette(s, palette);
            g.ActiveTexture(kTexture0 + 3);
            g.BindTexture(kTexture2D, paletteTex);
            g.PixelStorei(kUnpackAlignment, 4);
            g.TexSubImage2D(kTexture2D, 0, 0, 0, 256, 1, kRgba, kUnsignedByte, palette);
        }
        g.ActiveTexture(kTexture0 + 1);
        g.BindTexture(kTexture2D, rt.color);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.ActiveTexture(kTexture0 + 2);
        g.BindTexture(kTexture2D, tableTex);
        g.ActiveTexture(kTexture0);
        g.Uniform1i(locRtColor, 1);
        g.Uniform1i(locTables, 2);
        g.Uniform1i(locPalette, 3);
        g.Uniform4i(locTexFmt, tf.pageShiftX, tf.pageShiftY, tf.pixelsPerPageShift, tf.unpackedShift);
        g.Uniform4i(locTexFmt2, tf.bitOffset, texelBits, static_cast<GLint>(offsetOf(tf.table)), kind);
        const auto inv = inverseOffset.find(rt.psm);
        g.Uniform4i(locRtFmt, rf.pageShiftX, rf.pageShiftY, rf.pixelsPerPageShift, 1 << rf.unpackedShift);
        g.Uniform4i(locRtFmt2, inv == inverseOffset.end() ? 0 : static_cast<GLint>(inv->second), static_cast<GLint>(rt.fbp), static_cast<GLint>(rt.fbw), 0);
        g.Uniform4i(locTex0, static_cast<GLint>(c.tex0.tbp0), c.tex0.tbw, s.textureWidth, s.textureHeight);
        g.Uniform2f(locRtPx, static_cast<float>(rt.pxW), static_cast<float>(rt.pxH));
    }

    // ---- CLUT from a render target --------------------------------------------------------------------------------
    // A CLUT whose source memory a render target was drawn into is read from that target on the GPU: one pass writes the
    // raw source words (reinterpreting fetch, CPSM view of the target) into a 256x1 target, the 1 KB result goes to the
    // CLUT store. Results are cached per target version, so a target read many times per frame costs one pass.
    GLuint clutProgram = 0, clutFbo = 0, clutTex = 0, clutCoordTex = 0;
    GLint locClTables = -1, locClRtColor = -1, locClCoords = -1, locClTexFmt = -1, locClTexFmt2 = -1, locClRtFmt = -1, locClRtFmt2 = -1,
          locClTex0 = -1, locClRtPx = -1;
    std::unordered_map<uint64_t, std::vector<uint32_t>> clutCache;

    bool ensureClutProgram()
    {
        if (clutProgram)
            return true;
        const Api &g = gl();
        const GLuint v = compile(GSGL::kVertexShader, kPresentVs), f = compile(GSGL::kFragmentShader, {kClutFsHead, kReinterpretSource, kClutFsMain});
        if (!v || !f)
            return false;
        clutProgram = g.CreateProgram();
        g.AttachShader(clutProgram, v);
        g.AttachShader(clutProgram, f);
        g.LinkProgram(clutProgram);
        const auto loc = [&](const char *name) { return g.GetUniformLocation(clutProgram, name); };
        locClTables = loc("uTables"); locClRtColor = loc("uRtColor"); locClCoords = loc("uCoords"); locClTexFmt = loc("uTexFmt");
        locClTexFmt2 = loc("uTexFmt2"); locClRtFmt = loc("uRtFmt"); locClRtFmt2 = loc("uRtFmt2"); locClTex0 = loc("uTex0"); locClRtPx = loc("uRtPx");
        g.GenTextures(1, &clutTex);
        g.BindTexture(kTexture2D, clutTex);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), 256, 1, 0, kRgba, kUnsignedByte, nullptr);
        g.GenTextures(1, &clutCoordTex);
        g.BindTexture(kTexture2D, clutCoordTex);
        g.TexImage2D(kTexture2D, 0, 0x823A /*RG16UI*/, 256, 1, 0, 0x8228 /*RG_INTEGER*/, 0x1403 /*UNSIGNED_SHORT*/, nullptr);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.GenFramebuffers(1, &clutFbo);
        g.BindFramebuffer(kFramebuffer, clutFbo);
        g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, clutTex, 0);
        return true;
    }

    bool clutFromTarget(const GSTex0Reg &tex0, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
    {
        RenderTarget *rt = findReinterpretSource(tex0.cbp, tex0.cpsm);
        if (!rt || count == 0u || count > 256u || !ensureClutProgram())
            return false;
        uint64_t key = 1469598103934665603ull;
        const auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
        mix(rt->fbp); mix(rt->version); mix(tex0.cbp); mix(tex0.cpsm); mix(width); mix(count);
        for (uint32_t i = 0; i < count * 2u; ++i)
            mix(xy[i]);
        if (texSrcDiag)
            ++texSources["clut-from-rt psm=" + std::to_string(rt->psm)];
        if (const auto it = clutCache.find(key); it != clutCache.end())
        {
            ++stats.clutRtHits;
            std::memcpy(words, it->second.data(), count * sizeof(uint32_t));
            return true;
        }
        ++stats.clutReadbacks;
        // TEKKEN4_GL_CLUTLOG=1: the first CLUT loads read back from a render target (which target, where)
        static const bool clutLog = std::getenv("TEKKEN4_GL_CLUTLOG") != nullptr;
        static int clutLogged = 0;
        if (clutLog && clutLogged < 40)
        {
            ++clutLogged;
            std::fprintf(stderr, "[gs-hw-clut] cbp=%x cpsm=%u csm=%u count=%u width=%u xy0=%u,%u from fbp=%u fbw=%u psm=%u used=%ux%u version=%llu\n",
                         tex0.cbp, tex0.cpsm, tex0.csm, count, width, xy[0], xy[1], rt->fbp, rt->fbw, rt->psm, rt->usedW, rt->usedH,
                         (unsigned long long)rt->version);
        }
        const Api &g = gl();
        const GSSwizzle::Format &tf = GSSwizzle::GetFormat(tex0.cpsm);
        const GSSwizzle::Format &rf = GSSwizzle::GetFormat(rt->psm);
        const auto table = tableOffset.find(tf.table);
        const auto inv = inverseOffset.find(rt->psm);
        if (table == tableOffset.end() || inv == inverseOffset.end())
            return false;
        const int texelBits = tf.kind == GSSwizzle::Kind::Bits32 ? 32 : tf.kind == GSSwizzle::Kind::Bits24 ? 24 : 16;
        g.UseProgram(clutProgram);
        g.ActiveTexture(kTexture0 + 1);
        g.BindTexture(kTexture2D, rt->color);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.ActiveTexture(kTexture0 + 2);
        g.BindTexture(kTexture2D, tableTex);
        g.ActiveTexture(kTexture0 + 3);
        g.BindTexture(kTexture2D, clutCoordTex);
        g.PixelStorei(kUnpackAlignment, 2);
        g.TexSubImage2D(kTexture2D, 0, 0, 0, static_cast<GLsizei>(count), 1, 0x8228 /*RG_INTEGER*/, 0x1403 /*UNSIGNED_SHORT*/, xy);
        g.ActiveTexture(kTexture0);
        g.Uniform1i(locClRtColor, 1);
        g.Uniform1i(locClTables, 2);
        g.Uniform1i(locClCoords, 3);
        g.Uniform4i(locClTexFmt, tf.pageShiftX, tf.pageShiftY, tf.pixelsPerPageShift, tf.unpackedShift);
        g.Uniform4i(locClTexFmt2, tf.bitOffset, texelBits, static_cast<GLint>(table->second), 0);
        g.Uniform4i(locClRtFmt, rf.pageShiftX, rf.pageShiftY, rf.pixelsPerPageShift, 1 << rf.unpackedShift);
        g.Uniform4i(locClRtFmt2, static_cast<GLint>(inv->second), static_cast<GLint>(rt->fbp), static_cast<GLint>(rt->fbw), 0);
        g.Uniform4i(locClTex0, static_cast<GLint>(tex0.cbp), static_cast<GLint>(width), 256, 256);
        g.Uniform2f(locClRtPx, static_cast<float>(rt->pxW), static_cast<float>(rt->pxH));
        g.BindFramebuffer(kFramebuffer, clutFbo);
        g.Disable(kBlend);
        g.Disable(kDepthTest);
        g.Disable(kScissorTest);
        g.ColorMask(kTrue, kTrue, kTrue, kTrue);
        g.Viewport(0, 0, static_cast<GLsizei>(count), 1);
        g.BindVertexArray(presentVao);
        g.DrawArrays(kTriangles, 0, 3);
        g.PixelStorei(kPackAlignment, 4);
        g.ReadPixels(0, 0, static_cast<GLsizei>(count), 1, kRgba, kUnsignedByte, words);
        g.BindVertexArray(vao);
        g.UseProgram(program);
        g.BindBuffer(kArrayBuffer, vbo);
        if (clutCache.size() > 4096u)
            clutCache.clear();
        clutCache.emplace(key, std::vector<uint32_t>(words, words + count));
        return true;
    }

    // ---- drawing --------------------------------------------------------------------------------------------------
    static bool sameState(const GSDrawState &a, const GSDrawState &b)
    {
        return std::memcmp(&a, &b, sizeof(GSDrawState)) == 0;
    }

    // ---- Black & Bruised: controller button prompts (runtime/ps2_button_prompts.h) ---------------------------------
    uint64_t transferCount = 0;                 // GS uploads (cell fingerprints stay valid until the next one)
    uint64_t sigCacheTransfers = ~0ull;
    std::unordered_map<uint64_t, int> sigCache; // texture -> atlas index or -1
    int promptPublishFrame = -1;
    // the cell's shape: texels whose index differs from the cell's corner (background). Copies of the same font atlas
    // use different palette indices, and the menus fade by animating palette alpha, so neither indices nor the
    // palette identify a symbol; the shape does.
    uint32_t cellHash(const GSDrawState &s, int u0, int v0, int u1, int v1)
    {
        const GSTex0Reg &tx = s.context.tex0;
        const uint32_t bw = std::max<uint32_t>(tx.tbw, 1u);
        const uint32_t bg = store.Read(tx.psm, tx.tbp0, bw, static_cast<uint32_t>(u0), static_cast<uint32_t>(v0)) & 0xFFu;
        uint32_t h = 2166136261u;
        for (int y = v0; y < v1; ++y)
            for (int x = u0; x < u1; ++x)
            {
                const uint32_t idx = store.Read(tx.psm, tx.tbp0, bw, static_cast<uint32_t>(x), static_cast<uint32_t>(y)) & 0xFFu;
                h = (h ^ (idx != bg ? 1u : 0u)) * 16777619u;
            }
        return h;
    }
    void notePromptSprite(const GSDrawState &s, const GSVertex &a, const GSVertex &c, float u0, float u1, float v0, float v1)
    {
        namespace bp = ps2_button_prompts;
        if (!bp::enabled() || !s.prim.tme || !vram)
            return;
        const GSTex0Reg &tx = s.context.tex0;
        const float ox = s.context.xyoffset.ofx / 16.0f, oy = s.context.xyoffset.ofy / 16.0f;
        // a sprite sampling a whole render target = the display copy of that frame: publish its marks (once a frame)
        if ((tx.tbp0 & 31u) == 0u && s.textureWidth >= 512 && presentCount != promptPublishFrame)
            if (findTarget(tx.tbp0 / 32u) && tx.tbp0 / 32u != s.context.frame.fbp)
            {
                promptPublishFrame = presentCount;
                const bool any = bp::hasMarksFor(tx.tbp0 / 32u);
                if (any) bp::publish(tx.tbp0 / 32u);
                else bp::publishEmpty();
                static const bool plog = std::getenv("BNB_PROMPT_LOG") != nullptr;
                static int pshown = 0;
                if (plog && any && pshown++ < 20)
                    std::fprintf(stderr, "[bnb-prompt] publish fbp=%u (copy into fbp %u) marks=%zu\n", tx.tbp0 / 32u,
                                 s.context.frame.fbp, bp::latest().size());
                return;
            }
        if (!isIndexed(tx.psm))
            return;
        const float uw = std::fabs(u1 - u0), vh = std::fabs(v1 - v0);
        if (uw < 12 || uw > 70 || vh < 12 || vh > 70)
            return;
        if (sigCacheTransfers != transferCount) { sigCache.clear(); sigCacheTransfers = transferCount; }
        const uint64_t key = (static_cast<uint64_t>(tx.tbp0) | (static_cast<uint64_t>(tx.tbw) << 14) | (static_cast<uint64_t>(tx.psm) << 20) |
                              (static_cast<uint64_t>(s.textureWidth) << 26) | (static_cast<uint64_t>(s.textureHeight) << 40)) ^
                             (static_cast<uint64_t>(tx.cbp) * 0x9E3779B97F4A7C15ull); // the palette decides the shape
        auto it = sigCache.find(key);
        bp::State &st = bp::state();
        if (it == sigCache.end())
        {
            int found = -1;
            for (size_t i = 0; i < st.atlases.size() && found < 0; ++i)
            {
                const bp::Atlas &at = st.atlases[i];
                if (at.sigU1 <= static_cast<int>(s.textureWidth) && at.sigV1 <= static_cast<int>(s.textureHeight) &&
                    cellHash(s, at.sigU0, at.sigV0, at.sigU1, at.sigV1) == at.signature)
                    found = static_cast<int>(i);
            }
            it = sigCache.emplace(key, found).first;
        }
        static const bool log = std::getenv("BNB_PROMPT_LOG") != nullptr;
        const float bu0 = std::min(u0, u1), bu1 = std::max(u0, u1), bv0 = std::min(v0, v1), bv1 = std::max(v0, v1);
        if (log && bu0 < 4.0f)
        {
            static std::vector<uint64_t> seen; // each texture/cell once
            const uint64_t k = key ^ (static_cast<uint64_t>(bv0) << 52) ^ (static_cast<uint64_t>(it->second + 1) << 62);
            if (std::find(seen.begin(), seen.end(), k) == seen.end() && seen.size() < 400)
            {
                seen.push_back(k);
                std::fprintf(stderr, "[bnb-prompt] tbp=%x tbw=%u psm=%u tex=%ux%u uv=%.0f-%.0f,%.0f-%.0f hash=%08x atlas=%d\n",
                             tx.tbp0, tx.tbw, tx.psm, s.textureWidth, s.textureHeight, bu0, bu1, bv0, bv1,
                             cellHash(s, static_cast<int>(bu0), static_cast<int>(bv0), static_cast<int>(bu1), static_cast<int>(bv1)), it->second);
            }
        }
        if (it->second < 0)
            return;
        for (const bp::Cell &cell : st.atlases[it->second].cells)
            if (std::fabs(bu0 - cell.u0) < 2.5f && std::fabs(bu1 - cell.u1) < 2.5f && std::fabs(bv0 - cell.v0) < 2.5f && std::fabs(bv1 - cell.v1) < 2.5f)
            {
                bp::add(s.context.frame.fbp, bp::Mark{std::min(a.x, c.x) - ox, std::min(a.y, c.y) - oy, std::max(a.x, c.x) - ox,
                                                      std::max(a.y, c.y) - oy, cell.button, c.a / 128.0f});
                return;
            }
    }

    void addVertex(const GSDrawState &s, const GSVertex &v, float u, float tv, float zmax)
    {
        const GSContext &c = s.context;
        if (pending.empty())
            firstRawZ = v.z;
        // The GS samples pixel (x, y) at the integer point (x, y); GL samples host pixel h at its centre h + 0.5. Shifting
        // by half a HOST pixel maps the PS2 sample of pixel x onto the centre of host pixel scale * x, so a primitive
        // covering PS2 pixels [x0, x1) covers host pixels [scale * x0, scale * x1) and its texture coordinates at host
        // pixel scale * x equal the PS2's at x. (Half a PS2 pixel moved every primitive one host pixel down and right at
        // 2x: host row/column 0 of every target stayed unpainted and each copy pass shifted the picture by a host pixel;
        // the filtered passes smeared that stale edge into the picture, the bright bar along the top and left.)
        const float half = 0.5f / scale;
        HwVertex h;
        h.x = v.x - static_cast<float>(c.xyoffset.ofx) / 16.0f + half;
        h.y = v.y - static_cast<float>(c.xyoffset.ofy) / 16.0f + half;
        // the Z buffer keeps only its low bits (Z24: 24, Z16: 16): a vertex Z of 1<<24 is stored as 0
        double zq = std::max(v.z, 0.0);
        // Black & Bruised: PS2X_GS_Z_CLAMP=1 clamps a vertex Z above the format's maximum to that maximum instead
        // (its menus draw sprites at Z 0x80000000 into a Z16 buffer, meant to be in front of everything).
        static const bool zClamp = [] { const char *e = std::getenv("PS2X_GS_Z_CLAMP"); return e && *e == '1'; }();
        if (zmax < 4294967295.0f && zClamp)
            zq = std::min(zq, static_cast<double>(zmax));
        else if (zmax < 4294967295.0f)
            zq = static_cast<double>(static_cast<uint64_t>(zq) & (zmax < 70000.0f ? 0xFFFFull : 0xFFFFFFull));
        h.z = static_cast<float>(std::min(zq / zmax, 1.0));
        h.r = v.r; h.g = v.g; h.b = v.b; h.a = v.a;
        h.s = v.s; h.t = v.t; h.q = v.q;
        h.u = u; h.v = tv;
        pending.push_back(h);
    }

    static bool family16(uint8_t psm) { return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S; }

    static float zMaxFor(uint8_t zpsm)
    {
        switch (zpsm)
        {
        case 0x30: return 4294967295.0f;
        case 0x32: case 0x3A: return 65535.0f;
        default: return 16777215.0f;
        }
    }

    // ---- channel shuffles (see kShuffleFs) -------------------------------------------------------------------------
    const std::vector<uint16_t> &inverseFor(uint32_t ownerPsm)
    {
        std::vector<uint16_t> &inv = ownerInverse[ownerPsm];
        if (inv.empty())
        {
            const GSSwizzle::Format &f = GSSwizzle::GetFormat(ownerPsm);
            inv.assign(2048u, 0);
            for (uint32_t e = 0; e < 2048u; ++e)
                inv[f.table[e]] = static_cast<uint16_t>(e);
        }
        return inv;
    }

    // pixel (x, y) of a 16 bit view (psm/base/bw) -> pixel and halfword of the 32 bit owner laid out as ownerPsm at fbp/fbw
    bool viewToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                     uint32_t &ox, uint32_t &oy, uint32_t &half)
    {
        const uint32_t byte = GSSwizzle::Locate(GSSwizzle::GetFormat(viewPsm), base, bw, x, y).byte;
        const uint32_t word = byte >> 2, first = fbp * 2048u;
        if (word < first)
            return false;
        const uint32_t rel = word - first, page = rel >> 11u, e = inverseFor(ownerPsm)[rel & 2047u];
        half = (byte >> 1) & 1u;
        ox = (page % fbw) * 64u + (e & 63u);
        oy = (page / fbw) * 32u + (e >> 6u);
        return true;
    }

    // an 8x8 unit of the view at (x, y) -> an 8x8 owner block at (ox, oy), one halfword (checked at the far corner)
    bool unitToOwner(uint32_t viewPsm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t ownerPsm, uint32_t fbp, uint32_t fbw,
                     uint32_t &ox, uint32_t &oy, uint32_t &half)
    {
        uint32_t ex, ey, eh;
        return viewToOwner(viewPsm, base, bw, x, y, ownerPsm, fbp, fbw, ox, oy, half) &&
               viewToOwner(viewPsm, base, bw, x + 7u, y + 7u, ownerPsm, fbp, fbw, ex, ey, eh) && eh == half && ex == ox + 7u && ey == oy + 7u;
    }

    static uint32_t fbmsk16(uint32_t m)
    {
        return ((m >> 3) & 0x1Fu) | ((m >> 6) & 0x3E0u) | ((m >> 9) & 0x7C00u) | ((m >> 16) & 0x8000u);
    }

    // a sprite drawn through a 16 bit view of a 32 bit target, textured from a 16 bit view of a 32 bit target or a Z buffer,
    // 8 px aligned, copying texels 1:1: queued as a channel shuffle. False = not that shape (the caller decides).
    bool tryShuffle(const GSDrawState &s, const GSPrimitiveBatch &b)
    {
        const GSContext &c = s.context;
        const uint8_t tpsm = c.tex0.psm;
        const bool zTex = tpsm == 0x32 || tpsm == 0x3A;
        if (s.prim.type != GS_PRIM_SPRITE || !s.prim.tme || !s.prim.fst || s.prim.abe || !(family16(tpsm) || zTex))
            { shuffleWhy = 1; return false; }
        if ((c.test & 1u) != 0u && ((c.test >> 1) & 7u) != 1u) // alpha test other than ALWAYS
            { shuffleWhy = 2; return false; }
        if (((c.test >> 14) & 1u) != 0u) // DATE
            { shuffleWhy = 3; return false; }
        {
            // the texture function must pass the texel through: decal, or modulate by 128
            const GSVertex &v = b.vertices[1];
            const bool identity = c.tex0.tfx == 1 || (c.tex0.tfx == 0 && v.r == 128 && v.g == 128 && v.b == 128 && (!c.tex0.tcc || v.a == 128));
            if (!identity)
                { shuffleWhy = 4; return false; }
        }
        // each byte of the halfword must be fully written or fully kept
        const uint32_t m16 = fbmsk16(c.frame.fbmsk);
        const uint32_t lowByte = m16 & 0xFFu, highByte = m16 & 0xFF00u;
        if ((lowByte != 0u && lowByte != 0xFFu) || (highByte != 0u && highByte != 0xFF00u) || m16 == 0xFFFFu)
            { shuffleWhy = 5; return false; }
        // destination owner: an existing 32 bit target at FBP, or a new one
        RenderTarget *dst = findTarget(c.frame.fbp);
        if (dst && family16(dst->psm) && dst->usedW != 0u)
            { shuffleWhy = 6; return false; }
        // source owner
        if ((c.tex0.tbp0 % 32u) != 0u)
            { shuffleWhy = 7; return false; }
        // a Z16 view reads a Z buffer, or a colour target laid out as Z (the address mapping covers both)
        RenderTarget *src = nullptr;
        bool srcDepth = false;
        if (zTex)
            for (RenderTarget &t : targets)
                if (t.depth && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
                {
                    src = &t;
                    srcDepth = true;
                }
        if (!src)
            if (RenderTarget *t = findTarget(c.tex0.tbp0 / 32u))
                if (!family16(t->psm) && t->usedW != 0u)
                    src = t;
        if (!src)
            { shuffleWhy = 8; return false; }
        // geometry: integer, 8 px aligned rectangle, texels 1:1 at an 8 px aligned offset
        const GSVertex &a = b.vertices[0], &e = b.vertices[1];
        const float ofx = c.xyoffset.ofx / 16.0f, ofy = c.xyoffset.ofy / 16.0f;
        const float fx0 = std::min(a.x, e.x) - ofx, fx1 = std::max(a.x, e.x) - ofx, fy0 = std::min(a.y, e.y) - ofy, fy1 = std::max(a.y, e.y) - ofy;
        const auto whole = [](float v) { return std::fabs(v - std::round(v)) < 1e-3f && v >= 0.0f; };
        if (!whole(fx0) || !whole(fx1) || !whole(fy0) || !whole(fy1))
            { shuffleWhy = 9; return false; }
        const int x0 = static_cast<int>(std::lround(fx0)), x1 = static_cast<int>(std::lround(fx1));
        const int y0 = static_cast<int>(std::lround(fy0)), y1 = static_cast<int>(std::lround(fy1));
        const GSVertex &left = (a.x <= e.x) ? a : e;
        const GSVertex &top = (a.y <= e.y) ? a : e;
        const int du = static_cast<int>(std::floor(left.u / 16.0f)) - x0, dv = static_cast<int>(std::floor(top.v / 16.0f)) - y0;
        const int uSpan = static_cast<int>(std::lround(std::fabs(static_cast<float>(a.u) - static_cast<float>(e.u)) / 16.0f));
        const int vSpan = static_cast<int>(std::lround(std::fabs(static_cast<float>(a.v) - static_cast<float>(e.v)) / 16.0f));
        if (((x0 | x1 | y0 | y1 | du | dv) & 7) != 0 || x1 <= x0 || y1 <= y0 || uSpan != x1 - x0 || vSpan != y1 - y0)
            { shuffleWhy = 10; return false; }
        if (x0 + du < 0 || y0 + dv < 0 || x0 < static_cast<int>(c.scissor.x0) || y0 < static_cast<int>(c.scissor.y0) ||
            x1 - 1 > static_cast<int>(c.scissor.x1) || y1 - 1 > static_cast<int>(c.scissor.y1))
            { shuffleWhy = 11; return false; }
        if (!dst)
        {
            // creating a target may move the others: nothing queued may point at them, src is found again
            flushShuffle();
            const size_t srcIndex = static_cast<size_t>(src - targets.data());
            dst = &target(c.frame.fbp, c.frame.fbw);
            src = &targets[srcIndex];
        }
        // map every 8x8 unit; the whole sprite is rejected if any unit does not land on one owner block
        const uint32_t srcOwnerPsm = srcDepth ? 0x30u : 0x00u, srcFbp = srcDepth ? src->zbp : src->fbp;
        std::vector<ShuffleVertex> quads[4];
        uint32_t maxX = 0, maxY = 0;
        for (int y = y0; y < y1; y += 8)
            for (int x = x0; x < x1; x += 8)
            {
                uint32_t dx, dy, hd, sx, sy, hs;
                if (!unitToOwner(c.frame.psm, c.frame.fbp * 32u, c.frame.fbw, x, y, 0x00u, dst->fbp, dst->fbw, dx, dy, hd) ||
                    !unitToOwner(tpsm, c.tex0.tbp0, c.tex0.tbw, x + du, y + dv, srcOwnerPsm, srcFbp, src->fbw, sx, sy, hs))
                    { shuffleWhy = 12; return false; }
                if (dx + 8u > dst->pxW || dy + 8u > dst->pxH || sx + 8u > src->pxW || sy + 8u > src->pxH)
                    { shuffleWhy = 13; return false; }
                if (src == dst && !srcDepth && (sx != dx || sy != dy))
                    { shuffleWhy = 14; return false; } // would read texels that other quads of the pass write
                const float ox = static_cast<float>(sx) - static_cast<float>(dx), oy = static_cast<float>(sy) - static_cast<float>(dy);
                const float X0 = static_cast<float>(dx), Y0 = static_cast<float>(dy), X1 = X0 + 8.0f, Y1 = Y0 + 8.0f;
                std::vector<ShuffleVertex> &q = quads[hs * 2u + hd];
                q.insert(q.end(), {{X0, Y0, ox, oy}, {X1, Y0, ox, oy}, {X0, Y1, ox, oy}, {X1, Y0, ox, oy}, {X1, Y1, ox, oy}, {X0, Y1, ox, oy}});
                maxX = std::max(maxX, dx + 8u);
                maxY = std::max(maxY, dy + 8u);
            }
        // queue: one GL pass per state run
        if (havePending)
            flushPending(WhyShuffle);
        if (haveShuffle && (!sameState(shuffleState, s) || shuffleDst != dst || shuffleSrc != src || shuffleSrcZ != srcDepth))
            flushShuffle();
        if (!haveShuffle)
        {
            haveShuffle = true;
            shuffleState = s;
            shuffleDst = dst;
            shuffleSrc = src;
            shuffleSrcZ = srcDepth;
        }
        for (int i = 0; i < 4; ++i)
            shuffleQuads[i].insert(shuffleQuads[i].end(), quads[i].begin(), quads[i].end());
        dst->usedW = std::max(dst->usedW, std::min(maxX, dst->pxW));
        dst->usedH = std::max(dst->usedH, std::min(maxY, dst->pxH));
        ++stats.shuffles;
        return true;
    }

    void flushShuffle()
    {
        if (!haveShuffle)
            return;
        haveShuffle = false;
        if (!shuffleProgram)
        {
            for (auto &q : shuffleQuads)
                q.clear();
            return;
        }
        RenderTarget &dst = *shuffleDst, &src = *shuffleSrc;
        const GSContext &c = shuffleState.context;
        const Api &g = gl();
        ++dst.version;
        dst.lastDrawSeq = ++writeSeq;
        const uint32_t m16 = fbmsk16(c.frame.fbmsk);
        const GLboolean lo = (m16 & 0xFFu) == 0u ? kTrue : kFalse, hi = (m16 & 0xFF00u) == 0u ? kTrue : kFalse;
        g.BindFramebuffer(kFramebuffer, dst.fbo);
        g.Viewport(0, 0, dst.glW, dst.glH);
        g.Disable(kScissorTest);
        g.Disable(kBlend);
        g.Disable(kDepthTest);
        g.DepthMask(kFalse);
        g.UseProgram(shuffleProgram);
        g.ActiveTexture(kTexture0);
        g.BindTexture(kTexture2D, shuffleSrcZ ? src.depth : src.color);
        g.Uniform1i(locShSrc, 0);
        g.Uniform1i(locShSrcZ, shuffleSrcZ ? 1 : 0);
        g.Uniform1f(locShZMax, zMaxFor(c.zbuf.psm));
        g.Uniform1f(locShScale, scale);
        g.Uniform4i(locShTexa, shuffleState.texa.ta0 >> 7, shuffleState.texa.ta1 >> 7, shuffleState.texa.aem ? 1 : 0, 0);
        g.Uniform2f(locShInvSize, 1.0f / dst.pxW, 1.0f / dst.pxH);
        if (&src == &dst)
            g.TextureBarrier();
        g.BindVertexArray(shuffleVao);
        g.BindBuffer(kArrayBuffer, shuffleVbo);
        for (uint32_t i = 0; i < 4; ++i)
        {
            std::vector<ShuffleVertex> &q = shuffleQuads[i];
            if (q.empty())
                continue;
            const uint32_t hs = i >> 1, hd = i & 1u;
            g.Uniform1i(locShHs, static_cast<GLint>(hs));
            g.Uniform1i(locShHd, static_cast<GLint>(hd));
            g.ColorMask(hd == 0u ? lo : kFalse, hd == 0u ? hi : kFalse, hd == 1u ? lo : kFalse, hd == 1u ? hi : kFalse);
            g.BufferData(kArrayBuffer, static_cast<GLsizeiptr>(q.size() * sizeof(ShuffleVertex)), q.data(), kStreamDraw);
            g.DrawArrays(kTriangles, 0, static_cast<GLsizei>(q.size()));
            q.clear();
        }
        g.ColorMask(kTrue, kTrue, kTrue, kTrue);
        g.BindVertexArray(vao);
        g.BindBuffer(kArrayBuffer, vbo);
        ++stats.batches, PS2_PERF_COUNT(glBatches);
    }

    // GOW_GS_HW_DUMP window: skipped batches are listed in order with the drawn ones
    void dumpSkipped(const GSDrawState &s, const GSPrimitiveBatch &b, const char *why)
    {
        if (dumpPresents < 0 || presentCount < dumpPresents || presentCount >= dumpPresents + 3 || dumped >= dumpMax)
            return;
        flushPending();
        ++dumped;
        dumpAfterSkip = true;
        const GSContext &c = s.context;
        const GSVertex &v0 = b.vertices[0], &v1 = b.vertices[1];
        std::fprintf(stderr,
                     "[gs-hw-skip] #%d %s why=%d prim=%d fbp=%u fbw=%u fpsm=%u fbmsk=%x zbp=%x zmsk=%d tme=%d tbp=%x tbw=%u tpsm=%u %ux%u cbp=%x test=%llx alpha=%llx abe=%d v0=(%.1f,%.1f uv=%u,%u col=%u,%u,%u,%u) v1=(%.1f,%.1f uv=%u,%u) scis=%u-%u,%u-%u\n",
                     dumped, why, shuffleWhy, static_cast<int>(s.prim.type), c.frame.fbp, c.frame.fbw, c.frame.psm, c.frame.fbmsk, c.zbuf.zbp, c.zbuf.zmask ? 1 : 0,
                     s.prim.tme ? 1 : 0, c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, s.textureWidth, s.textureHeight, c.tex0.cbp,
                     (unsigned long long)c.test, (unsigned long long)c.alpha, s.prim.abe ? 1 : 0,
                     v0.x, v0.y, v0.u, v0.v, (unsigned)v0.r, (unsigned)v0.g, (unsigned)v0.b, (unsigned)v0.a, v1.x, v1.y, v1.u, v1.v,
                     c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1);
    }

    void submit(const GSPrimitiveBatch &b)
    {
        const GSDrawState &s = b.state;
        const GSPrimType type = s.prim.type;
        const bool tri = type == GS_PRIM_TRIANGLE || type == GS_PRIM_TRISTRIP || type == GS_PRIM_TRIFAN;
        const bool sprite = type == GS_PRIM_SPRITE;
        const bool line = type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP;
        if (!tri && !sprite && !line)
        {
            ++stats.skippedPrim;
            dumpSkipped(s, b, "prim");
            return;
        }
        const uint8_t fpsm = s.context.frame.psm;
        if (family16(fpsm) && tryShuffle(s, b))
            return;
        flushShuffle();
        if (fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24 && fpsm != GS_PSM_CT16 && fpsm != GS_PSM_CT16S)
        {
            ++stats.skippedPsm;
            return;
        }
        // a draw through a different-format view (CT16) of memory that a CT32/CT24 target already owns writes bit fields of its
        // pixels; it cannot be rasterised into the RGBA target at the same coordinates
        for (RenderTarget &t : targets)
            if (t.fbp == s.context.frame.fbp && t.usedW != 0u && family16(t.psm) != family16(fpsm))
            {
                // Tekken 4 port: a buffer re-purposed for the other family (the 16 bit PAL50 display buffer is 32 bit in
                // PAL60) takes the new format when the old one has not drawn there since the last present; within a frame
                // (GoW's bit-field views) the draw is still skipped.
                if (t.lastDrawPresent != presentCount)
                {
                    t.psm = fpsm;
                    t.usedW = t.usedH = 0;
                    break;
                }
                ++stats.skippedPsm;
                dumpSkipped(s, b, "view");
                return;
            }
        const bool usesClut = s.prim.tme && isIndexed(s.context.tex0.psm);
        const GSDrawState key = batchKey(s);
        // Black & Bruised: deferred B passes (see DeferredPass)
        if (tri && havePending && pendingOpaque && pairMergeOn() && !sameState(pendingKey, key) && runAIdx < runA.size() &&
            runA[runAIdx] == triPos(b))
        {
            bool join = false;
            if (deferB.active)
                join = sameState(deferB.key, key) && !(usesClut && deferB.clutHash != clutHash);
            else if (isPassB(pendingState, s))
            {
                deferB.active = true;
                deferB.state = s;
                deferB.key = key;
                deferB.clutHash = clutHash;
                deferB.palette = (usesClut && vram) ? paletteFor(s) : 0u;
                deferB.verts.clear();
                join = true;
            }
            if (join)
            {
                ++runAIdx;
                runHasB = true;
                const float zmaxB = zMaxFor(s.context.zbuf.psm);
                std::swap(pending, deferB.verts);
                for (int i = 0; i < 3; ++i) // texel UVs as the triangle path below (texelUV)
                    addVertex(s, b.vertices[i], s.prim.fst ? static_cast<float>(b.vertices[i].u) / 16.0f : 0.0f,
                              s.prim.fst ? static_cast<float>(b.vertices[i].v) / 16.0f : 0.0f, zmaxB);
                std::swap(pending, deferB.verts);
                ++stats.draws, PS2_PERF_COUNT(glDraws);
                return;
            }
        }
        // Black & Bruised: other opaque Z-writing passes into the same target wait beside A (see extraOpaque). Only
        // untextured ones (the boxers' outline pass): the pending "A" batch can be any opaque draw, and hoisting textured
        // arena/crowd draws past each other changed which one wins equal-depth ties (GEQUAL: the later draw), so
        // coplanar decals and crowd layers could pop now and then (2026-11-17). PS2X_GL_EXTRA_TEXTURED=1: the old rule.
        static const bool extraTextured = [] { const char *v = std::getenv("PS2X_GL_EXTRA_TEXTURED"); return v && *v == '1'; }();
        if (tri && havePending && pendingOpaque && pairMergeOn() && !sameState(pendingKey, key) && isOpaqueZ(s) &&
            (extraTextured || !s.prim.tme) && sameTarget(pendingState, s))
        {
            DeferredPass *slot = nullptr;
            for (DeferredPass &p : extraOpaque)
                if (sameState(p.key, key) && !(usesClut && p.clutHash != clutHash))
                    slot = &p;
            if (!slot && extraOpaque.size() < kExtraOpaque)
            {
                for (const DeferredPass &p : extraOpaque)
                    if (sameState(p.key, key)) // same state with another CLUT: keep the order, flush
                        goto noExtra;
                extraOpaque.emplace_back();
                slot = &extraOpaque.back();
                slot->active = true;
                slot->state = s;
                slot->key = key;
                slot->clutHash = clutHash;
                slot->palette = (usesClut && vram) ? paletteFor(s) : 0u;
            }
            if (slot)
            {
                const float zmaxC = zMaxFor(s.context.zbuf.psm);
                std::swap(pending, slot->verts);
                for (int i = 0; i < 3; ++i)
                    addVertex(s, b.vertices[i], s.prim.fst ? static_cast<float>(b.vertices[i].u) / 16.0f : 0.0f,
                              s.prim.fst ? static_cast<float>(b.vertices[i].v) / 16.0f : 0.0f, zmaxC);
                std::swap(pending, slot->verts);
                ++stats.draws, PS2_PERF_COUNT(glDraws);
                return;
            }
        }
    noExtra:
        if (havePending && !sameState(pendingKey, key))
        {
            if (statsEnabled)
                noteStateDiff(pendingState, s);
            flushPending(WhyState);
        }
        else if (havePending && usesClut && pendingClutHash != clutHash)
            flushPending(WhyClut);
        if (!havePending)
        {
            pendingState = s;
            pendingKey = key;
            havePending = true;
            pendingClutHash = clutHash;
            pendingPalette = (usesClut && vram) ? paletteFor(s) : 0u;
            pendingOpaque = tri && pairMergeOn() && isOpaqueZ(s);
            runA.clear();
            runAIdx = 0;
            runHasB = false;
        }
        if (tri && pendingOpaque)
        {
            if (runHasB) // an A after B passes: a new run
            {
                runA.clear();
                runAIdx = 0;
                runHasB = false;
            }
            if (runA.size() < 4096u)
                runA.push_back(triPos(b));
        }
        const float zmax = zMaxFor(s.context.zbuf.psm);
        const float texW = static_cast<float>(std::max<uint16_t>(s.textureWidth, 1)), texH = static_cast<float>(std::max<uint16_t>(s.textureHeight, 1));
        const auto texelUV = [&](const GSVertex &v, float &u, float &t)
        {
            if (s.prim.fst)
            {
                u = static_cast<float>(v.u) / 16.0f;
                t = static_cast<float>(v.v) / 16.0f;
            }
            else
            {
                u = 0.0f;
                t = 0.0f;
            }
        };
        if (line)
        {
            // a GS line covers about one pixel across its length: drawn as a quad one PS2 pixel wide around the segment
            // (at least one pixel long), colour/Z/texture coordinates taken from the nearer end point. The game uses
            // them for the sand streaks and for the per-frame black line that clears row 0 of the frame buffer.
            const GSVertex &a = b.vertices[0], &c = b.vertices[1];
            float dx = c.x - a.x, dy = c.y - a.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-4f)
                dx = 1.0f, dy = 0.0f;
            else
                dx /= len, dy /= len;
            const float ext = len < 1.0f ? (1.0f - len) * 0.5f : 0.0f; // short lines still cover one pixel
            const float nx = -dy * 0.5f, ny = dx * 0.5f;
            GSVertex e[2] = {a, c};
            e[0].x -= dx * ext; e[0].y -= dy * ext;
            e[1].x += dx * ext; e[1].y += dy * ext;
            const int order[6][2] = {{0, 1}, {1, 1}, {0, -1}, {1, 1}, {1, -1}, {0, -1}};
            // across the line the quad is centred on the PS2 pixel centre (sample + 0.5), so it covers every host pixel of
            // the pixels it crosses; along the line it covers [start, end) like a sprite edge
            const float centre = 0.5f - 0.5f / scale;
            for (const auto &o : order)
            {
                GSVertex v = e[o[0]];
                v.x += nx * static_cast<float>(o[1]) + centre * std::fabs(dy);
                v.y += ny * static_cast<float>(o[1]) + centre * std::fabs(dx);
                float u, t;
                texelUV(v, u, t);
                addVertex(s, v, u, t, zmax);
            }
        }
        else if (tri)
        {
            for (int i = 0; i < 3; ++i)
            {
                float u, t;
                texelUV(b.vertices[i], u, t);
                addVertex(s, b.vertices[i], u, t, zmax);
            }
        }
        else
        {
            const GSVertex &a = b.vertices[0];
            const GSVertex &c = b.vertices[1];
            float u0, v0, u1, v1;
            if (s.prim.fst)
            {
                u0 = a.u / 16.0f; v0 = a.v / 16.0f; u1 = c.u / 16.0f; v1 = c.v / 16.0f;
            }
            else
            {
                const float q0 = std::fabs(a.q) < 1e-20f ? 1e-20f : a.q, q1 = std::fabs(c.q) < 1e-20f ? 1e-20f : c.q;
                u0 = a.s / q0 * texW; v0 = a.t / q0 * texH; u1 = c.s / q1 * texW; v1 = c.t / q1 * texH;
            }
            GSDrawState sp = s; // sprites carry texel UVs, never STQ
            (void)sp;
            // Black & Bruised: BNB_GS_SPRITETRACE=1: one line per textured sprite (screen box, texture, texel box),
            // optional BNB_GS_SPRITEBOX=x0,y0,x1,y1 (screen area), BNB_GS_TRACEAFTER=<s>; at most 200 per frame
            {
                static const bool on = std::getenv("BNB_GS_SPRITETRACE") != nullptr;
                static float bx0 = -1e9f, by0 = -1e9f, bx1 = 1e9f, by1 = 1e9f;
                static const bool boxRead = [] {
                    if (const char *e = std::getenv("BNB_GS_SPRITEBOX")) std::sscanf(e, "%f,%f,%f,%f", &bx0, &by0, &bx1, &by1);
                    return true; }();
                (void)boxRead;
                static const double after = std::getenv("BNB_GS_TRACEAFTER") ? std::atof(std::getenv("BNB_GS_TRACEAFTER")) : 0.0;
                static const auto t0 = std::chrono::steady_clock::now();
                static int frame = -1, lines = 0;
                // BNB_GS_TEXDUMP=<hex tbp>[,...]: the decoded texture (palette applied; alpha over magenta) as
                // bnb_tex_<tbp>.ppm the first time a sprite draws from it
                {
                    static std::vector<long> want = [] {
                        std::vector<long> v;
                        if (const char *e = std::getenv("BNB_GS_TEXDUMP"))
                            for (const char *p = e; *p;) { char *end = nullptr; v.push_back(std::strtol(p, &end, 16)); if (end == p) break; p = *end == ',' ? end + 1 : end; }
                        return v; }();
                    auto it = std::find(want.begin(), want.end(), static_cast<long>(s.context.tex0.tbp0));
                    if (s.prim.tme && it != want.end() && vram)
                    {
                        want.erase(it);
                        const GSTex0Reg &tx = s.context.tex0;
                        const uint32_t tw = s.textureWidth, th = s.textureHeight;
                        uint32_t pal[256] = {};
                        const bool indexed = isIndexed(tx.psm);
                        if (indexed) store.BuildPalette(s, pal);
                        char name[64];
                        std::snprintf(name, sizeof(name), "bnb_tex_%x.ppm", tx.tbp0);
                        if (FILE *f = std::fopen(name, "wb"))
                        {
                            std::fprintf(f, "P6 %u %u 255 ", tw, th);
                            for (uint32_t y = 0; y < th; ++y)
                                for (uint32_t x = 0; x < tw; ++x)
                                {
                                    uint32_t col = store.Read(tx.psm, tx.tbp0, std::max<uint32_t>(tx.tbw, 1u), x, y);
                                    if (indexed) col = pal[col & 0xFFu];
                                    const float al = std::min(1.0f, ((col >> 24) & 0xFFu) / 128.0f);
                                    const unsigned char rgb[3] = {
                                        static_cast<unsigned char>((col & 0xFF) * al + 255 * (1 - al)),
                                        static_cast<unsigned char>(((col >> 8) & 0xFF) * al),
                                        static_cast<unsigned char>(((col >> 16) & 0xFF) * al + 255 * (1 - al))};
                                    std::fwrite(rgb, 1, 3, f);
                                }
                            std::fclose(f);
                            std::fprintf(stderr, "[bnb-texdump] %s %ux%u psm=%u cbp=%x\n", name, tw, th, tx.psm, tx.cbp);
                        }
                    }
                }
                notePromptSprite(s, a, c, u0, u1, v0, v1);
                if (on && s.prim.tme)
                {
                    if (frame != presentCount) frame = presentCount, lines = 0;
                    const float sx0 = std::min(a.x, c.x) - s.context.xyoffset.ofx / 16.0f, sx1 = std::max(a.x, c.x) - s.context.xyoffset.ofx / 16.0f;
                    const float sy0 = std::min(a.y, c.y) - s.context.xyoffset.ofy / 16.0f, sy1 = std::max(a.y, c.y) - s.context.xyoffset.ofy / 16.0f;
                    if (lines < 200 && sx1 >= bx0 && sx0 <= bx1 && sy1 >= by0 && sy0 <= by1 &&
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= after)
                    {
                        ++lines;
                        const GSTex0Reg &tx = s.context.tex0;
                        std::fprintf(stderr, "[bnb-sprite] p=%d tbp=%x tbw=%u psm=%u tex=%ux%u cbp=%x csa=%u fbp=%u scr=%.1f-%.1f,%.1f-%.1f uv=%.1f-%.1f,%.1f-%.1f rgba=%.0f,%.0f,%.0f,%.0f\n",
                                     presentCount, tx.tbp0, tx.tbw, tx.psm, s.textureWidth, s.textureHeight, tx.cbp, tx.csa,
                                     s.context.frame.fbp, sx0, sx1, sy0, sy1, u0, u1, v0, v1, c.r, c.g, c.b, c.a);
                    }
                }
            }
            GSVertex corner[4] = {a, a, a, a};
            float xs[2] = {a.x, c.x}, ys[2] = {a.y, c.y};
            float us[2] = {u0, u1}, vs[2] = {v0, v1};
            // the texel coordinates of the first and last pixel the GS covers (pixel n is covered when lo <= n < hi);
            // a sprite covering no pixel on the GS keeps its whole range
            const auto sampled = [](float p0, float p1, float t0, float t1, float &lo, float &hi)
            {
                lo = std::min(t0, t1);
                hi = std::max(t0, t1);
                const float first = std::ceil(std::min(p0, p1)), last = std::ceil(std::max(p0, p1)) - 1.0f;
                if (last < first || p1 == p0)
                    return;
                const float ta = t0 + (first - p0) * (t1 - t0) / (p1 - p0), tb = t0 + (last - p0) * (t1 - t0) / (p1 - p0);
                lo = std::min(ta, tb);
                hi = std::max(ta, tb);
            };
            float cu0, cu1, cv0, cv1;
            sampled(a.x - s.context.xyoffset.ofx / 16.0f, c.x - s.context.xyoffset.ofx / 16.0f, u0, u1, cu0, cu1);
            sampled(a.y - s.context.xyoffset.ofy / 16.0f, c.y - s.context.xyoffset.ofy / 16.0f, v0, v1, cv0, cv1);
            // A picture drawn as strips (full-screen passes, logos, fonts) continues across each strip seam: there the clamp
            // would repeat the last sampled texel for the host pixels between the two strips' GS sample points (a step at
            // every seam). The side a sprite shares with the previous sprite of the batch, with the texture continuing
            // across it, is left open on both sprites; outer edges stay clamped.
            // Tekken 4 port (upscaling): the quad covers exactly the PS2 pixels the GS covers, [ceil(x0), ceil(x1)), with
            // the texture coordinates continued linearly to those edges (the same sampling function, so 1x is unchanged).
            // The game's full-screen copy draws 64 px columns from X 64k to 64k + 63.5: the GS covers 64k .. 64k + 63 and
            // the next column starts at 64k + 64, but at scale n the host pixels of [63.5, 64) of every column stayed
            // unpainted (the black line every 64 PS2 pixels at 3x..5x). Like PCSX2's "align sprite" at native pixel edges.
            if (spriteSnap())
            {
                const auto snap = [](float p[2], float t[2], float offset)
                {
                    const float q0 = std::ceil(p[0] - offset) + offset, q1 = std::ceil(p[1] - offset) + offset;
                    if (p[1] != p[0])
                    {
                        const float k = (t[1] - t[0]) / (p[1] - p[0]);
                        const float t0 = t[0] + (q0 - p[0]) * k, t1 = t[0] + (q1 - p[0]) * k;
                        t[0] = t0;
                        t[1] = t1;
                    }
                    p[0] = q0;
                    p[1] = q1;
                };
                snap(xs, us, s.context.xyoffset.ofx / 16.0f);
                snap(ys, vs, s.context.xyoffset.ofy / 16.0f);
            }
            // Black & Bruised: native image tiles (movies, pictures) at native resolution, see tryCanvasSprite
            if (tryCanvasSprite(s, c, xs, ys, us, vs))
            {
                ++stats.draws, PS2_PERF_COUNT(glDraws);
                return;
            }
            SpriteEdges cur{{xs[0], xs[1]}, {ys[0], ys[1]}, {us[0], us[1]}, {vs[0], vs[1]}};
            // The sides shared with an earlier sprite of the batch (not only the previous one: a copy drawn as columns of
            // bands has its left neighbour several sprites back) are opened on both.
            if (recentBatch != batchSerial)
                recentSprites.clear(), recentBatch = batchSerial;
            if (spriteSnap())
            {
                for (size_t i = recentSprites.size(); i-- > 0;)
                    if (recentSprites[i].end >= 6 && recentSprites[i].end <= pending.size())
                        openSharedSide(recentSprites[i].edges, cur, &pending[recentSprites[i].end - 6], cu0, cu1, cv0, cv1);
            }
            else if (lastSprite.batch == batchSerial && lastSprite.end == pending.size() && lastSprite.end >= 6)
                openSharedSide(lastSprite.edges, cur, &pending[lastSprite.end - 6], cu0, cu1, cv0, cv1);
            const int order[6][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 0}, {1, 1}, {0, 1}};
            for (const auto &o : order)
            {
                GSVertex v = c; // colour, z, fog come from the second vertex
                v.x = xs[o[0]];
                v.y = ys[o[1]];
                addVertex(s, v, us[o[0]], vs[o[1]], zmax);
                pending.back().s = us[o[0]] / texW;
                pending.back().t = vs[o[1]] / texH;
                pending.back().q = 1.0f;
                pending.back().cu0 = cu0;
                pending.back().cv0 = cv0;
                pending.back().cu1 = cu1;
                pending.back().cv1 = cv1;
            }
            lastSprite = {cur, pending.size(), batchSerial};
            if (recentSprites.size() >= kRecentSprites)
                recentSprites.erase(recentSprites.begin());
            recentSprites.push_back(lastSprite);
            (void)corner;
        }
        ++stats.draws, PS2_PERF_COUNT(glDraws);
    }

    // Tekken 4 port (perf): the game re-uploads many textures every frame with the same bytes. A host->local upload's
    // texture invalidation waits until the transfer's data is in (the next draw, transfer or present): when the raw
    // blocks hash as before, nothing changed and the textures stay valid (no re-decode, no revalidation hash).
    // Uploads spanning more than kUploadHashMaxBlocks are invalidated right away as before. Off by default since the
    // partial index-texture decodes: hashing every upload twice then cost more GL time than the decodes it saved
    // (arena: GL 9.5-10.2 ms per frame with it, 8.5-8.7 without). PS2X_GL_UPLOAD_SAME=1: on.
    static constexpr uint32_t kUploadHashMaxBlocks = 2048u; // 512 KB
    struct DeferredUpload
    {
        bool active = false;
        GSTransferCommand command{};
        uint32_t lo = 0, hi = 0;
        uint64_t before = 0;
    } deferredUpload;
    static bool uploadSameCheck()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_GL_UPLOAD_SAME");
            return value && *value == '1';
        }();
        return on;
    }
    void finishDeferredUpload()
    {
        if (!deferredUpload.active)
            return;
        deferredUpload.active = false;
        // still receiving data (a draw in the middle of the transfer): invalidate as before
        const bool complete = store.TransferState().direction != 0u;
        const uint64_t after = complete ? hashBlocks(deferredUpload.lo, deferredUpload.hi) : 0u;
        if (after != 0u && after == deferredUpload.before)
        {
            ++stats.uploadsSame;
            return;
        }
        invalidateForUpload(deferredUpload.command, deferredUpload.lo, deferredUpload.hi);
    }

    void flushPending(FlushWhy why = WhyOther)
    {
        finishDeferredUpload();
        applyRtUpload();
        ++batchSerial; // the next sprite starts a new batch: no seam with the previous one
        flushShuffle();
        if (havePending && !pending.empty())
            ++stats.flushWhy[why];
        runA.clear();
        runAIdx = 0;
        runHasB = false;
        pendingOpaque = false;
        if (!havePending || pending.empty())
        {
            havePending = false;
            pending.clear();
            deferB.active = false;
            deferB.verts.clear();
            extraOpaque.clear();
            return;
        }
        havePending = false;
        if (!ensure())
        {
            pending.clear();
            deferB.active = false;
            deferB.verts.clear();
            extraOpaque.clear();
            return;
        }
        draw(pendingState, pending);
        pending.clear();
        pendingPalette = 0u;
        for (DeferredPass &p : extraOpaque) // other opaque passes of the run, before the B passes
        {
            if (p.verts.empty())
                continue;
            ++stats.flushWhy[why];
            pendingPalette = p.palette;
            std::swap(pending, p.verts);
            draw(p.state, pending);
            std::swap(pending, p.verts);
            pendingPalette = 0u;
        }
        extraOpaque.clear();
        if (deferB.active) // the deferred B passes of the run, right after their A passes
        {
            deferB.active = false;
            if (!deferB.verts.empty())
            {
                ++stats.flushWhy[why];
                pendingPalette = deferB.palette;
                std::swap(pending, deferB.verts);
                draw(deferB.state, pending);
                std::swap(pending, deferB.verts);
                pendingPalette = 0u;
            }
            deferB.verts.clear();
        }
    }

    void blendState(const GSDrawState &s, bool &enabled, GLenum &sf, GLenum &df, float &fixScale, float &constant, GLenum &equation, bool &soft)
    {
        enabled = false;
        soft = false;
        equation = kFuncAdd;
        sf = kOne; df = kZero;
        fixScale = 1.0f / 128.0f;
        constant = 0.0f;
        if (!s.prim.abe)
            return;
        const uint64_t alpha = s.context.alpha;
        const uint32_t a = alpha & 3, b = (alpha >> 2) & 3, c = (alpha >> 4) & 3, d = (alpha >> 6) & 3;
        const uint32_t fix = static_cast<uint32_t>((alpha >> 32) & 0xFF);
        GLenum cs, cd; // factor C and 1-C
        if (c == 0) { cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; }
        else if (c == 2) { cs = kConstantColor; cd = kOneMinusConstantColor; constant = std::min(fix / 128.0f, 1.0f); }
        else { soft = true; cs = kSrc1Alpha; cd = kOneMinusSrc1Alpha; } // C = Ad
        // COLCLAMP=0: results wrap around instead of saturating, below 0 (B != 0) and above 255 (Cs*C + Cd). The character
        // shadow-volume pass counts in the G channel this way (Cs*FIX + Cd, G cleared to 127): it draws its triangles once
        // adding 255 (= -1) and once adding 1. Clamping saturated the first half at 255, the second could not undo it, and
        // everything the volume covered was darkened (black wedges on walls, rectangles under Kratos).
        if ((s.colclamp & 1u) == 0u && a != b)
            soft = true;
        if (a == b)
        {
            // (A-A)*C + D = D
            if (d == 0) return;            // Cs: plain write
            enabled = true; sf = kZero; df = kOne; return; // Cd: destination unchanged
        }
        enabled = true;
        if (a == 0 && b == 1 && d == 1) { sf = cs; df = cd; }            // Cs*C + Cd*(1-C)
        else if (a == 0 && b == 2 && d == 1) { sf = cs; df = kOne; }     // Cs*C + Cd
        else if (a == 2 && b == 1 && d == 1) { sf = kZero; df = cd; }    // Cd*(1-C)
        else if (a == 0 && b == 2 && d == 2) { sf = cs; df = kZero; }    // Cs*C
        else if (a == 2 && b == 0 && d == 2) { sf = kZero; df = kZero; } // 0
        else if (a == 2 && b == 0 && d == 1) { sf = cs; df = kOne; equation = kFuncReverseSubtract; } // Cd - Cs*C
        // Black & Bruised: (Cd - 0)*C + Cs = Cs + Cd*C, the cel shading's second pass on every boxer (ALPHA 0x09). It
        // went through the shader blend, a texture barrier per batch: ~40 % of all batches, the GL thread saturated on
        // the fighter select (~23 fps). Fixed-function blending does it exactly (COLCLAMP=1 saturates like the unorm target).
        else if (a == 1 && b == 2 && d == 0 && (s.colclamp & 1u) != 0u) { sf = kOne; df = cs; }
        else { soft = true; sf = cs; df = cd; }
        if (soft)
        {
            ++stats.unsupportedBlend;
            if (statsEnabled) // which equations take the shader blend: (alpha low byte, fix, COLCLAMP, ATE/AFAIL bits)
                ++softWhy[(static_cast<uint32_t>(alpha & 0xFFu) << 16) | (fix << 8) | ((s.colclamp & 1u) << 1) | (s.prim.tme ? 1u : 0u)];
        }
    }
    std::map<uint32_t, uint64_t> softWhy;

    int dumpPresents = std::getenv("GOW_GS_HW_DUMP") ? std::atoi(std::getenv("GOW_GS_HW_DUMP")) : -1;
    int presentCount = 0, dumped = 0, dumpedAt = 0;
    const int dumpMax = std::getenv("GOW_GS_HW_DUMPMAX") ? std::atoi(std::getenv("GOW_GS_HW_DUMPMAX")) : 4000;
    bool dumpAfterSkip = false;
    const bool dumpAll = std::getenv("GOW_GS_HW_DUMPALL") != nullptr;

    int onlyLo = std::getenv("GOW_GS_HW_ONLY") ? std::atoi(std::getenv("GOW_GS_HW_ONLY")) : -1;
    int onlyHi = std::getenv("GOW_GS_HW_ONLY") && std::strchr(std::getenv("GOW_GS_HW_ONLY"), '-') ? std::atoi(std::strchr(std::getenv("GOW_GS_HW_ONLY"), '-') + 1) : 1 << 30;
    int frameBatch = 0;

    // Z buffers whose memory the game has inverted (Z' = 0xFFFFFF - Z) with a full-screen colour draw into the Z memory
    // (CT24 at FBP == ZBP, Cs - Cd with Cs = 255): the shadow passes invert the Z buffer, draw the shadow mask with ZTST GEQUAL
    // (= LEQUAL against the real depth) and invert it back. The GL depth buffer keeps the real depth; while the flag is set,
    // depth tests use 1 - z and the reversed comparison, which is the same test on the inverted memory.
    std::vector<uint32_t> invertedZ;
    bool zInverted(uint32_t zbp) const { return std::find(invertedZ.begin(), invertedZ.end(), zbp) != invertedZ.end(); }
    static bool isZInversion(const GSDrawState &s, const std::vector<HwVertex> &verts)
    {
        const GSContext &c = s.context;
        const uint64_t al = c.alpha;
        if (c.frame.fbp != c.zbuf.zbp || c.frame.psm != GS_PSM_CT24 || s.prim.tme || !s.prim.abe || c.frame.fbmsk != 0u ||
            (al & 0xFFu) != 0xA4u || ((al >> 32) & 0xFFu) != 0x80u || verts.empty())
            return false;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (const HwVertex &v : verts)
        {
            if (v.r < 254.5f || v.g < 254.5f || v.b < 254.5f)
                return false;
            x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
        }
        return x0 <= c.scissor.x0 + 1.0f && y0 <= c.scissor.y0 + 1.0f && x1 >= c.scissor.x1 && y1 >= c.scissor.y1;
    }

    void draw(const GSDrawState &s, const std::vector<HwVertex> &verts)
    {
        const Api &g = gl();
        ++frameBatch;
        if (onlyLo >= 0 && (frameBatch < onlyLo || frameBatch > onlyHi) && s.context.frame.fbp == 0 && s.context.frame.fbw == 8 && !(s.prim.tme && s.context.tex0.tbp0 == 0))
            return;
        const GSContext &c = s.context;
        // GOW_GS_HW_SKIPTBP=<hex>: debug, skip draws textured from that block (bisecting post passes)
        static const long skipTbp = std::getenv("GOW_GS_HW_SKIPTBP") ? std::strtol(std::getenv("GOW_GS_HW_SKIPTBP"), nullptr, 16) : -1;
        if (skipTbp >= 0 && s.prim.tme && static_cast<long>(c.tex0.tbp0) == skipTbp)
            return;
        // Black & Bruised: BNB_GS_TRACETBP=<hex>[,<hex>...]: one line per draw textured from those blocks (frame, box,
        // alpha range), to follow a sprite's animation frame by frame. BNB_GS_TRACEALL=1: every textured sprite draw.
        {
            static const std::vector<long> traceTbp = []
            {
                std::vector<long> v;
                if (const char *e = std::getenv("BNB_GS_TRACETBP"))
                    for (const char *p = e; *p;)
                    {
                        char *end = nullptr;
                        v.push_back(std::strtol(p, &end, 16));
                        p = (*end == ',') ? end + 1 : end;
                        if (end == p && *p) break;
                    }
                return v;
            }();
            static const bool traceAll = std::getenv("BNB_GS_TRACEALL") != nullptr;
            // BNB_GS_TRACEAFTER=<s>: start tracing that many seconds after the first draw (skip the intro movies)
            static const double traceAfter = std::getenv("BNB_GS_TRACEAFTER") ? std::atof(std::getenv("BNB_GS_TRACEAFTER")) : 0.0;
            static const auto traceT0 = std::chrono::steady_clock::now();
            static int traceFrame = -1, traceLines = 0; // at most 64 lines per frame (movies draw thousands of tiles)
            if (traceFrame != presentCount)
                traceFrame = presentCount, traceLines = 0;
            const bool traceOn = (traceAfter <= 0.0 ||
                std::chrono::duration<double>(std::chrono::steady_clock::now() - traceT0).count() >= traceAfter) &&
                traceLines < 64;
            if (traceOn && ((traceAll && s.prim.tme && s.prim.type == 6u) ||
                (s.prim.tme && std::find(traceTbp.begin(), traceTbp.end(), static_cast<long>(c.tex0.tbp0)) != traceTbp.end())))
            {
                ++traceLines;
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, a0 = 1e9f, a1 = -1e9f;
                for (const HwVertex &v : verts)
                    x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y),
                    a0 = std::min(a0, v.a), a1 = std::max(a1, v.a);
                float z0 = 2.0f, z1 = -1.0f;
                for (const HwVertex &v : verts)
                    z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
                std::fprintf(stderr, "[bnb-trace] p=%d b=%d tbp=%x fbp=%u prim=%u v=%zu box=%.0f-%.0f,%.0f-%.0f a=%.0f-%.0f "
                             "z=%.5f-%.5f rawz0=%.0f test=%llx zmsk=%d zpsm=%x\n",
                             presentCount, frameBatch, c.tex0.tbp0, c.frame.fbp, static_cast<unsigned>(s.prim.type),
                             verts.size(), x0, x1, y0, y1, a0, a1, z0, z1, firstRawZ, (unsigned long long)c.test,
                             c.zbuf.zmask ? 1 : 0, c.zbuf.psm);
            }
        }
        static const double dumpAtSec = std::getenv("GOW_GS_HW_DUMPAT") ? std::atof(std::getenv("GOW_GS_HW_DUMPAT")) : -1.0;
        static const auto t0 = std::chrono::steady_clock::now();
        const double nowSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (dumpAtSec >= 0.0 && nowSec >= dumpAtSec && dumpedAt == 0 && frameBatch == 1)
            dumpedAt = 1;
        if (dumpedAt == 1 && dumped < dumpMax && dumpPresents < 0)
            dumpPresents = 0, presentCount = 0;
        if (dumpPresents >= 0 && presentCount >= dumpPresents && presentCount < dumpPresents + 3 && dumped < dumpMax && (dumpAll || c.frame.fbp == 208 || c.frame.fbp == 320 || dumped < 40 || dumpAfterSkip))
        {
            ++dumped;
            const HwVertex &v0 = verts[0];
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f, cmax = 0.0f, amax = 0.0f, zlo = 2.0f, zhi = -1.0f;
            for (const HwVertex &v : verts)
            {
                bx0 = std::min(bx0, v.x), by0 = std::min(by0, v.y), bx1 = std::max(bx1, v.x), by1 = std::max(by1, v.y);
                cmax = std::max({cmax, v.r, v.g, v.b}), amax = std::max(amax, v.a), zlo = std::min(zlo, v.z), zhi = std::max(zhi, v.z);
            }
            const float zscale = zMaxFor(c.zbuf.psm) < 4294967295.0f ? zMaxFor(c.zbuf.psm) : 16777215.0f;
            std::fprintf(stderr, "[gs-hw-dump-bb] #%d %.0f-%.0f,%.0f-%.0f ta0=%x ta1=%x aem=%d fge=%d tfx=%u tcc=%u iip=%d cmax=%.0f amax=%.0f prim=%u z=%x-%x\n",
                         dumped, bx0, bx1, by0, by1, s.texa.ta0, s.texa.ta1, s.texa.aem ? 1 : 0, s.prim.fge ? 1 : 0,
                         c.tex0.tfx, c.tex0.tcc, s.prim.iip ? 1 : 0, cmax, amax, static_cast<unsigned>(s.prim.type),
                         static_cast<unsigned>(zlo * zscale + 0.5f), static_cast<unsigned>(zhi * zscale + 0.5f));
            std::fprintf(stderr,
                         "[gs-hw-dump] #%d fb=%d verts=%zu fbp=%u fbw=%u fpsm=%u zbp=%x zpsm=%x zmsk=%d tme=%d tbp=%x tbw=%u tpsm=%u %ux%u cbp=%x test=%llx alpha=%llx abe=%d fbmsk=%x rawz=%.0f v0=(%.1f,%.1f z=%.3f col=%.0f,%.0f,%.0f,%.0f uv=%.1f,%.1f) scis=%u-%u,%u-%u\n",
                         dumped, frameBatch, verts.size(), c.frame.fbp, c.frame.fbw, c.frame.psm, c.zbuf.zbp, c.zbuf.psm, c.zbuf.zmask ? 1 : 0, s.prim.tme ? 1 : 0,
                         c.tex0.tbp0, c.tex0.tbw, c.tex0.psm, s.textureWidth, s.textureHeight, c.tex0.cbp, (unsigned long long)c.test,
                         (unsigned long long)c.alpha, s.prim.abe ? 1 : 0, c.frame.fbmsk, firstRawZ, v0.x, v0.y, v0.z, v0.r, v0.g, v0.b, v0.a, v0.u, v0.v,
                         c.scissor.x0, c.scissor.x1, c.scissor.y0, c.scissor.y1);
            if (s.prim.tme)
            {
                // Tekken 4 port: texture addressing of the batch (CLAMP, TEX1, texel coordinate range)
                float u0 = 1e30f, u1 = -1e30f, w0 = 1e30f, w1 = -1e30f;
                for (const HwVertex &v : verts)
                {
                    const float q = std::fabs(v.q) < 1e-30f ? 1e-30f : v.q;
                    const float u = s.prim.fst ? v.u : v.s / q * s.textureWidth, w = s.prim.fst ? v.v : v.t / q * s.textureHeight;
                    u0 = std::min(u0, u), u1 = std::max(u1, u), w0 = std::min(w0, w), w1 = std::max(w1, w);
                }
                std::fprintf(stderr, "[gs-hw-dump-tex] #%d clamp=%llx tex1=%llx fst=%d u=%.1f..%.1f v=%.1f..%.1f\n", dumped,
                             (unsigned long long)c.clamp, (unsigned long long)c.tex1, s.prim.fst ? 1 : 0, u0, u1, w0, w1);
            }
        }
        RenderTarget &rt = target(c.frame.fbp, c.frame.fbw);
        attachDepth(rt, c.zbuf.zbp);
        rt.psm = c.frame.psm;
        rt.lastDrawPresent = presentCount;
        ++rt.version;
        rt.lastDrawSeq = ++writeSeq;
        {
            // real extent of what was drawn (vertices clipped to the scissor), not the scissor itself
            float maxX = 0.0f, maxY = 0.0f;
            for (const HwVertex &v : verts)
            {
                maxX = std::max(maxX, v.x);
                maxY = std::max(maxY, v.y);
            }
            const uint32_t bx = static_cast<uint32_t>(std::min<float>(std::max(maxX, 0.0f), static_cast<float>(c.scissor.x1 + 1u)));
            const uint32_t by = static_cast<uint32_t>(std::min<float>(std::max(maxY, 0.0f), static_cast<float>(c.scissor.y1 + 1u)));
            rt.usedW = std::max<uint32_t>(rt.usedW, std::min<uint32_t>(bx, rt.pxW));
            rt.usedH = std::max<uint32_t>(rt.usedH, std::min<uint32_t>(by, rt.pxH));
        }
        g.BindFramebuffer(kFramebuffer, rt.fbo);
        g.Viewport(0, 0, rt.glW, rt.glH);
        g.Enable(kScissorTest);
        const int sx0 = c.scissor.x0, sy0 = c.scissor.y0;
        const int sw = std::max(0, static_cast<int>(c.scissor.x1) - sx0 + 1), sh = std::max(0, static_cast<int>(c.scissor.y1) - sy0 + 1);
        g.Scissor(static_cast<GLint>(std::floor(sx0 * scale)), static_cast<GLint>(std::floor(sy0 * scale)),
                  static_cast<GLsizei>(std::ceil(sw * scale)), static_cast<GLsizei>(std::ceil(sh * scale)));
        g.Disable(kCullFace);

        // depth
        // ZTE = 0 means no depth test on the GS (the movie player draws its picture sprite with TEST = 0): ALWAYS
        const uint32_t ztst = ((c.test >> 16) & 1u) ? static_cast<uint32_t>((c.test >> 17) & 3u) : 1u;
        static const bool noDepth = std::getenv("GOW_GS_HW_NODEPTH") != nullptr;
        // Z buffer in this target's own colour memory: tested and written in the shader against the colour words
        const bool zAlias = c.zbuf.zbp == c.frame.fbp && ((c.test >> 16) & 1u) != 0u &&
                            (c.frame.psm == GS_PSM_CT32 || c.frame.psm == GS_PSM_CT24);
        if (noDepth || zAlias) g.Disable(kDepthTest); else g.Enable(kDepthTest);
        const bool zInv = !zAlias && ((c.test >> 16) & 1u) != 0u && zInverted(c.zbuf.zbp);
        if (zInv)
            g.DepthFunc(ztst == 0 ? kNever : ztst == 1 ? kAlways : ztst == 2 ? kLEqual : kLess);
        else
            g.DepthFunc(ztst == 0 ? kNever : ztst == 1 ? kAlways : ztst == 2 ? kGEqual : kGreater);
        std::vector<HwVertex> flippedZ;
        if (zInv)
        {
            flippedZ = verts;
            for (HwVertex &v : flippedZ)
                v.z = 1.0f - v.z;
        }
        const std::vector<HwVertex> &dverts = zInv ? flippedZ : verts;
        if (isZInversion(s, verts))
        {
            if (zInverted(c.zbuf.zbp))
                invertedZ.erase(std::find(invertedZ.begin(), invertedZ.end(), c.zbuf.zbp));
            else
                invertedZ.push_back(c.zbuf.zbp);
        }
        const bool zwrite = !c.zbuf.zmask && !zAlias;
        if (zwrite && ((c.test >> 16) & 1u) != 0u)
            rt.lastDepthSeq = rt.lastDrawSeq;
        const bool zAliasWrite = zAlias && !c.zbuf.zmask;

        // colour mask
        const uint32_t fbmsk = c.frame.fbmsk;
        const GLboolean mr = (fbmsk & 0xFF) == 0xFF ? kFalse : kTrue, mg = (fbmsk & 0xFF00) == 0xFF00 ? kFalse : kTrue,
                        mb = (fbmsk & 0xFF0000) == 0xFF0000 ? kFalse : kTrue, ma = (fbmsk & 0xFF000000u) == 0xFF000000u ? kFalse : kTrue;

        bool blend = false, softBlend = false;
        GLenum sf, df, equation;
        float fixScale, constant;
        blendState(s, blend, sf, df, fixScale, constant, equation, softBlend);
        // COLCLAMP=0 with Cs*FIX + Cd and one flat untextured colour (the shadow-volume counting passes: G += 255, then
        // G += 1, modulo 256): every fragment adds the same constant modulo 256, and additions commute, so the fixed-function
        // blend does it in one draw: adding v < 128 is an add, adding v >= 128 is subtracting 256 - v. Exact while the
        // count stays inside 0..255 (it starts at 127). Drawing such passes per triangle through the shader blend stalled
        // the renderer for seconds in fights with many shadow casters.
        float wrapAmount[3] = {0.0f, 0.0f, 0.0f};
        bool wrapCount = false;
        if (softBlend && !zAliasWrite && (s.colclamp & 1u) == 0u && !s.prim.tme && !verts.empty())
        {
            const uint64_t al = c.alpha;
            const uint32_t a = al & 3u, b = (al >> 2) & 3u, cc = (al >> 4) & 3u, d = (al >> 6) & 3u, fix = static_cast<uint32_t>((al >> 32) & 0xFFu);
            if (a == 0u && b == 2u && cc == 2u && d == 1u)
            {
                const float col[3] = {verts[0].r, verts[0].g, verts[0].b};
                const GLboolean written[3] = {mr, mg, mb};
                bool flat = true;
                for (const HwVertex &v : verts)
                    flat = flat && v.r == col[0] && v.g == col[1] && v.b == col[2];
                int direction = 0; // +1 add, -1 subtract, 2 mixed
                for (int ch = 0; ch < 3 && flat; ++ch)
                {
                    if (!written[ch])
                        continue;
                    const uint32_t v = std::min<uint32_t>(255u, (static_cast<uint32_t>(col[ch]) * fix) >> 7);
                    const int dir = v >= 128u ? -1 : 1;
                    direction = direction == 0 || direction == dir ? dir : 2;
                    // the blend multiplies the constant by the source colour (Cs / 255): factor = amount / source
                    wrapAmount[ch] = col[ch] > 0.0f ? static_cast<float>(dir < 0 ? 256u - v : v) / col[ch] : 0.0f;
                }
                if (flat && (direction == 1 || direction == -1))
                {
                    wrapCount = true;
                    softBlend = false;
                    blend = true;
                    sf = kConstantColor;
                    df = kOne;
                    equation = direction < 0 ? kFuncReverseSubtract : kFuncAdd;
                }
            }
        }
        if (softBlend || zAliasWrite) // the shader produces the final colour (a Z write replaces the colour word)
            blend = false;
        if (blend)
        {
            g.Enable(kBlend);
            g.BlendEquationSeparate(equation, kFuncAdd);
            g.BlendFuncSeparate(sf, df, kOne, kZero);
            if (wrapCount)
                g.BlendColor(wrapAmount[0], wrapAmount[1], wrapAmount[2], 0.0f);
            else
                g.BlendColor(constant, constant, constant, constant);
        }
        else
            g.Disable(kBlend);

        g.UseProgram(program);
        {
            const bool date = ((c.test >> 14) & 1u) != 0u && c.frame.psm != GS_PSM_CT24;
            g.Uniform1i(locDate, date ? 1 : 0);
            g.Uniform1i(locDatm, ((c.test >> 15) & 1u) != 0u ? 1 : 0);
            const uint64_t al = c.alpha;
            g.Uniform1i(locSwBlend, softBlend && !zAliasWrite ? 1 : 0);
            g.Uniform1i(locColClamp, (s.colclamp & 1u) != 0u ? 1 : 0);
            g.Uniform4i(locAbcd, static_cast<GLint>(al & 3u), static_cast<GLint>((al >> 2) & 3u), static_cast<GLint>((al >> 4) & 3u), static_cast<GLint>((al >> 6) & 3u));
            g.Uniform1f(locFix, static_cast<float>((al >> 32) & 0xFFu));
            g.Uniform1i(locZAlias, zAlias ? 1 + static_cast<GLint>(ztst) : 0);
            g.Uniform1i(locZAliasWrite, zAliasWrite ? 1 : 0);
            g.Uniform1f(locZAliasMax, zMaxFor(c.zbuf.psm));
            if (date || softBlend || zAlias)
            {
                g.ActiveTexture(kTexture0 + 4);
                g.BindTexture(kTexture2D, rt.color);
                g.Uniform1i(locDest, 4);
                g.ActiveTexture(kTexture0);
                g.TextureBarrier();
            }
        }
        g.Uniform2f(locInvSize, 1.0f / rt.pxW, 1.0f / rt.pxH);
        g.Uniform1i(locTme, s.prim.tme ? 1 : 0);
        g.Uniform1i(locFst, s.prim.fst ? 1 : 0);
        g.Uniform1i(locIip, s.prim.iip ? 1 : 0);
        g.Uniform1i(locTfx, c.tex0.tfx);
        g.Uniform1i(locTcc, c.tex0.tcc);
        g.Uniform1f(locBlendScale, 1.0f / 128.0f);
        if (s.prim.tme)
        {
            // a texture that lives in a render target of the same format is sampled straight from it
            RenderTarget *src = nullptr;
            const auto family = [](uint8_t psm) { return (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S) ? 16 : (psm == GS_PSM_CT32 || psm == GS_PSM_CT24) ? 32 : 0; };
            if (family(c.tex0.psm) != 0 && (c.tex0.tbp0 % 32u) == 0u)
                if (RenderTarget *candidate = findTarget(c.tex0.tbp0 / 32u))
                {
                    if (family(candidate->psm) == family(c.tex0.psm) && !uploadedSinceDraw(*candidate, c.tex0.tbp0) &&
                        !overwrittenByDepth(*candidate))
                        src = candidate;
                    if (statsEnabled)
                    {
                        // Tekken 4 port: textures at a target's base, sampled from it or from the store (first sighting)
                        static std::vector<uint64_t> seenRt;
                        const uint64_t key = (static_cast<uint64_t>(c.tex0.tbp0) << 8) | (src ? 1u : 0u);
                        if (seenRt.size() < 64u && std::find(seenRt.begin(), seenRt.end(), key) == seenRt.end())
                        {
                            seenRt.push_back(key);
                            std::fprintf(stderr, "[gs-hw] rt texture tbp=%x tpsm=%u target fbp=%u psm=%u -> %s\n", c.tex0.tbp0, c.tex0.psm,
                                         candidate->fbp, candidate->psm, src ? "target" : "store");
                        }
                    }
                }
            if (src == &rt)
                g.TextureBarrier();
            Texture dummy;
            RenderTarget *zsrc = nullptr;
            const bool zFormat = c.tex0.psm == 0x30 || c.tex0.psm == 0x31 || c.tex0.psm == 0x32 || c.tex0.psm == 0x3A;
            if (zFormat && (c.tex0.tbp0 % 32u) == 0u)
                for (RenderTarget &t : targets)
                    if (t.depth && t.zbp == c.tex0.tbp0 / 32u && t.fbp != t.zbp)
                        zsrc = &t;
            if (zsrc)
                src = nullptr;
            RenderTarget *rsrc = nullptr;
            if (!src && !zsrc)
                rsrc = findReinterpretSource(c.tex0.tbp0, c.tex0.psm);
            if (rsrc && statsEnabled)
            {
                // Tekken 4 port: which textures are read from which target's memory (first sighting of each pair)
                static std::vector<uint64_t> seen;
                const uint64_t pair = (static_cast<uint64_t>(c.tex0.tbp0) << 32) | (static_cast<uint64_t>(c.tex0.psm) << 16) | rsrc->fbp;
                if (seen.size() < 64u && std::find(seen.begin(), seen.end(), pair) == seen.end())
                {
                    seen.push_back(pair);
                    std::fprintf(stderr, "[gs-hw] reinterpret tbp=%x tpsm=%u from fbp=%u fbw=%u psm=%u used %ux%u\n", c.tex0.tbp0,
                                 c.tex0.psm, rsrc->fbp, rsrc->fbw, rsrc->psm, rsrc->usedW, rsrc->usedH);
                }
            }
            if (rsrc == &rt)
                g.TextureBarrier();
            const bool indexed = !(src || rsrc || zsrc) && isIndexed(c.tex0.psm) && vram != nullptr;
            uint32_t needX0 = 0u, needX1 = ~0u, needLo = 0u, needHi = ~0u;
            if (indexed)
                sampledRect(s, verts, needX0, needX1, needLo, needHi);
            {
                static const bool decodeLogOn = std::getenv("PS2X_GL_DECODE_LOG") != nullptr;
                static std::vector<uint32_t> fullShown;
                if (decodeLogOn && indexed && fullShown.size() < 40 && s.textureWidth * s.textureHeight >= 65536 &&
                    (needX1 == ~0u || needHi == ~0u) &&
                    std::find(fullShown.begin(), fullShown.end(), c.tex0.tbp0) == fullShown.end())
                {
                    fullShown.push_back(c.tex0.tbp0);
                    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
                    for (const HwVertex &v : verts)
                    {
                        const float q = std::fabs(v.q) < 1e-30f ? 1e-30f : v.q;
                        const float tu = (s.prim.fst || v.cu0 > -1e29f) ? v.u : v.s / q * s.textureWidth;
                        const float tv = (s.prim.fst || v.cu0 > -1e29f) ? v.v : v.t / q * s.textureHeight;
                        u0 = std::min(u0, tu), u1 = std::max(u1, tu), v0 = std::min(v0, tv), v1 = std::max(v1, tv);
                    }
                    std::fprintf(stderr, "[gs-full] tbp=%x psm=%x %ux%u clamp=%llx fst=%d verts=%zu u=%.1f..%.1f v=%.1f..%.1f\n",
                                 c.tex0.tbp0, c.tex0.psm, s.textureWidth, s.textureHeight, (unsigned long long)c.clamp,
                                 s.prim.fst ? 1 : 0, verts.size(), u0, u1, v0, v1);
                }
            }
            Texture &tex = (src || rsrc || zsrc) ? dummy : indexed ? indexTextureFor(s, needX0, needX1, needLo, needHi) : textureFor(s);
            g.Uniform1i(locIndexed, indexed ? 1 : 0);
            g.Uniform1i(locLinear, s.linearFilter ? 1 : 0);
            {
                // Tekken 4 port (PC options): anisotropic taps for filtered 3D draws; sharp 2D for sprites of game
                // textures (not render targets: the full-screen copies stay 1:1)
                const bool tri3d = s.prim.type == GS_PRIM_TRIANGLE || s.prim.type == GS_PRIM_TRISTRIP || s.prim.type == GS_PRIM_TRIFAN;
                const bool gameTexture = !src && !rsrc && !zsrc;
                g.Uniform1i(locAnisoN, (tri3d && s.linearFilter && gameTexture) ? aniso : 1);
                const int hudNow = g_hudModeLive.load(std::memory_order_relaxed);
                g.Uniform1i(locSharp, (hudNow > 0 && s.prim.type == GS_PRIM_SPRITE && s.linearFilter && gameTexture && scale > 1.0f) ? hudNow : 0);
                g.Uniform1i(locRegionLinear, regionLinear ? 1 : 0);
            }
            // Tekken 4 port: the CLAMP modes as they are (region modes for every texture source but depth/reinterpret)
            const bool regionOk = !zsrc && !rsrc;
            const GLint wms = static_cast<GLint>(c.clamp & 3u), wmt = static_cast<GLint>((c.clamp >> 2) & 3u);
            g.Uniform4i(locWrap, regionOk || wms < 2 ? wms : 1, regionOk || wmt < 2 ? wmt : 1, 0, 0);
            g.Uniform4i(locRegion, static_cast<GLint>((c.clamp >> 4) & 0x3FFu), static_cast<GLint>((c.clamp >> 14) & 0x3FFu),
                        static_cast<GLint>((c.clamp >> 24) & 0x3FFu), static_cast<GLint>((c.clamp >> 34) & 0x3FFu));
            if (indexed)
            {
                g.ActiveTexture(kTexture0 + 5);
                g.BindTexture(kTexture2D, pendingPalette ? pendingPalette : paletteFor(s));
                g.Uniform1i(locIdxPalette, 5);
            }
            g.ActiveTexture(kTexture0);
            g.BindTexture(kTexture2D, zsrc ? zsrc->depth : src ? src->color : rsrc ? rsrc->color : tex.id);
            g.Uniform1i(locZTex, !zsrc ? 0 : c.tex0.psm == 0x32 || c.tex0.psm == 0x3A ? 16 : c.tex0.psm == 0x31 ? 24 : 32);
            g.Uniform1f(locZMax, zsrc ? zMaxFor(static_cast<uint8_t>(c.tex0.psm)) : 1.0f);
            g.Uniform1i(locRtTex24, !(src || rsrc) ? 0 : c.tex0.psm == GS_PSM_CT24 ? 1 : family16(c.tex0.psm) ? 2 : 0);
            g.Uniform1i(locReinterp, rsrc ? 1 : 0);
            if (rsrc)
                setupReinterpret(s, *rsrc);
            g.Uniform1f(locTa0, static_cast<float>(s.texa.ta0));
            g.Uniform1f(locTa1, static_cast<float>(s.texa.ta1));
            g.Uniform1i(locAem, s.texa.aem ? 1 : 0);
            const GLint filter = s.linearFilter ? kLinear : kNearest;
            g.TexParameteri(kTexture2D, kTextureMinFilter, filter);
            g.TexParameteri(kTexture2D, kTextureMagFilter, filter);
            const uint64_t clamp = c.clamp;
            const GLint wu = (clamp & 3u) == 0u ? kRepeat : kClampToEdge, wv = ((clamp >> 2) & 3u) == 0u ? kRepeat : kClampToEdge;
            g.TexParameteri(kTexture2D, kTextureWrapS, wu);
            g.TexParameteri(kTexture2D, kTextureWrapT, wv);
            if (indexed)
            {
                g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
                g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            }
            g.Uniform1i(locTex, 0);
            g.Uniform2f(locTexDim, static_cast<float>(std::max<uint16_t>(s.textureWidth, 1)), static_cast<float>(std::max<uint16_t>(s.textureHeight, 1)));
            if (zsrc)
            {
                g.Uniform2f(locTexSize, static_cast<float>(zsrc->pxW), static_cast<float>(zsrc->pxH));
                g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
                g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
            }
            else if (src)
                g.Uniform2f(locTexSize, static_cast<float>(src->pxW), static_cast<float>(src->pxH));
            else
                g.Uniform2f(locTexSize, static_cast<float>(tex.width), static_cast<float>(tex.height));
        }
        g.BindVertexArray(vao);
        g.BindBuffer(kArrayBuffer, vbo);
        GLint first = 0;
        if (ringPtr)
            first = ringWrite(g, dverts.data(), dverts.size() * sizeof(HwVertex));
        else
            g.BufferData(kArrayBuffer, static_cast<GLsizeiptr>(dverts.size() * sizeof(HwVertex)), dverts.data(), kStreamDraw);

        const bool ate = (c.test & 1u) != 0u;
        const uint32_t atst = static_cast<uint32_t>((c.test >> 1) & 7u), aref = static_cast<uint32_t>((c.test >> 4) & 0xFFu);
        const uint32_t afail = static_cast<uint32_t>((c.test >> 12) & 3u);
        g.Uniform1i(locAte, ate ? 1 : 0);
        g.Uniform1i(locAtst, static_cast<GLint>(atst));
        g.Uniform1i(locAref, static_cast<GLint>(aref));
        static const bool forceNoTest = std::getenv("GOW_GS_HW_NOATEST") != nullptr;
        g.Uniform1i(locPass, (ate && !forceNoTest) ? 1 : 0);
        g.Uniform1i(locFailTint, 0);
        static const int traceAt = std::getenv("GOW_GS_HW_DEPTHTRACE") ? std::atoi(std::getenv("GOW_GS_HW_DEPTHTRACE")) : -1;
        const bool tracing = traceAt >= 0 && presentCount >= traceAt && presentCount < traceAt + 1 && rt.depth && rt.fbp == 0;
        const auto depthStats = [&](const char *when)
        {
            std::vector<float> dz(static_cast<size_t>(rt.glW) * rt.glH);
            g.BindFramebuffer(kFramebuffer, rt.fbo);
            g.ReadPixels(0, 0, rt.glW, rt.glH, kDepthComponent, kFloat, dz.data());
            double mx = 0.0;
            size_t nz = 0;
            for (float v : dz) { mx = std::max<double>(mx, v); nz += v > 0.0f; }
            std::fprintf(stderr, "[gs-hw-depth] batch %d %s: max=%.3f nonzero=%zu/%zu (this batch z=%.3f zmsk=%d ztst=%u verts=%zu tbp=%x)\n", frameBatch, when, mx, nz, dz.size(),
                         verts[0].z, c.zbuf.zmask ? 1 : 0, ztst, verts.size(), c.tex0.tbp0);
        };
        if (tracing) depthStats("before");
        // GOW_GS_HW_VOLTRACE=<n>: the first n COLCLAMP=0 draws (shadow volumes): vertex depth sent (after the Z-inversion
        // flip), the depth buffer under the volume's bounding box, the depth test and the blend path
        static const int volTrace = std::getenv("GOW_GS_HW_VOLTRACE") ? std::atoi(std::getenv("GOW_GS_HW_VOLTRACE")) : 0;
        static int volTraced = 0;
        int volBox[4] = {0, -1, 0, -1};
        if ((s.colclamp & 1u) == 0u && zInv && volTraced < volTrace && rt.depth)
        {
            ++volTraced;
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 2.0f, z1 = -1.0f;
            for (const HwVertex &v : dverts)
                x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y), z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
            const int bx0 = std::max(0, static_cast<int>(x0 * scale)), bx1 = std::min(static_cast<int>(rt.glW) - 1, static_cast<int>(x1 * scale));
            const int by0 = std::max(0, static_cast<int>(y0 * scale)), by1 = std::min(static_cast<int>(rt.glH) - 1, static_cast<int>(y1 * scale));
            volBox[0] = bx0, volBox[1] = bx1, volBox[2] = by0, volBox[3] = by1;
            size_t trisDown = 0, trisUp = 0;
            for (size_t i = 2; i < dverts.size(); i += 3)
                (dverts[i].g >= 128.0f ? trisDown : trisUp) += 1;
            std::fprintf(stderr, "[gs-hw-vol]   triangles G>=128 %zu, G<128 %zu\n", trisDown, trisUp);
            float d0 = 2.0f, d1 = -1.0f;
            if (bx1 >= bx0 && by1 >= by0)
            {
                std::vector<float> dz(static_cast<size_t>(bx1 - bx0 + 1) * (by1 - by0 + 1));
                g.BindFramebuffer(kFramebuffer, rt.fbo);
                g.ReadPixels(bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, kDepthComponent, kFloat, dz.data());
                for (float v : dz)
                    d0 = std::min(d0, v), d1 = std::max(d1, v);
            }
            std::fprintf(stderr, "[gs-hw-vol] batch %d verts=%zu zInv=%d ztst=%u vertex z %.6f-%.6f (x %.0f-%.0f y %.0f-%.0f) depth under it %.6f-%.6f softBlend=%d wrapCount=%d zbp=%x attached=%x\n",
                         frameBatch, dverts.size(), zInv ? 1 : 0, ztst, z0, z1, x0, x1, y0, y1, d0, d1, softBlend ? 1 : 0, wrapCount ? 1 : 0,
                         c.zbuf.zbp, rt.zbp);
        }
        g.ColorMask(mr, mg, mb, ma);
        g.DepthMask(zwrite ? kTrue : kFalse);
        if (softBlend && !zAliasWrite && (s.colclamp & 1u) == 0u)
        {
            // wrapping blend (COLCLAMP=0, only the shadow-volume passes): the shader reads the destination, so draw one
            // triangle at a time with a barrier in between; overlapping triangles of one draw then see each other's results
            // like on the GS (the volumes count overlaps this way)
            for (size_t i = 0; i + 3 <= verts.size(); i += 3)
            {
                if (i != 0)
                    g.TextureBarrier();
                g.DrawArrays(kTriangles, first + static_cast<GLint>(i), 3);
            }
        }
        else
            g.DrawArrays(kTriangles, first, static_cast<GLsizei>(verts.size()));
        if (volBox[1] >= volBox[0] && volBox[3] >= volBox[2])
        {
            const int w = volBox[1] - volBox[0] + 1, h = volBox[3] - volBox[2] + 1;
            std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4u);
            g.PixelStorei(kPackAlignment, 1);
            g.ReadPixels(volBox[0], volBox[2], w, h, kRgba, kUnsignedByte, px.data());
            size_t below = 0, above = 0, equal = 0;
            for (size_t i = 1; i < px.size(); i += 4)
                (px[i] < 127 ? below : px[i] > 127 ? above : equal) += 1;
            std::fprintf(stderr, "[gs-hw-vol]   after: G<127 %zu, G>127 %zu, G=127 %zu\n", below, above, equal);
        }
        static const bool noFailPass = std::getenv("GOW_GS_HW_NOFAIL") != nullptr;
        if (ate && afail != 0u && !noFailPass)
        {
            // failing fragments: 1 FB_ONLY (colour+alpha, no Z), 2 ZB_ONLY (Z only), 3 RGB_ONLY (colour, no alpha, no Z)
            g.Uniform1i(locPass, 2);
            static const bool failTint = std::getenv("GOW_GS_HW_FAILTINT") != nullptr;
            g.Uniform1i(locFailTint, (failTint && afail == 3u && s.prim.tme && c.frame.fbp == 0u) ? 1 : 0);
            if (afail == 1u) { g.DepthMask(kFalse); }
            else if (afail == 2u) { g.ColorMask(kFalse, kFalse, kFalse, kFalse); }
            else { g.DepthMask(kFalse); g.ColorMask(mr, mg, mb, kFalse); }
            g.DrawArrays(kTriangles, first, static_cast<GLsizei>(verts.size()));
        }
        ++stats.batches, PS2_PERF_COUNT(glBatches);
        g.Disable(kScissorTest);
        static const int rtDumpAt = std::getenv("GOW_GS_HW_RTDUMP") ? std::atoi(std::getenv("GOW_GS_HW_RTDUMP")) : -1;
        static const double rtDumpSec = std::getenv("GOW_GS_HW_RTDUMPAT") ? std::atof(std::getenv("GOW_GS_HW_RTDUMPAT")) : -1.0;
        static const auto rtT0 = std::chrono::steady_clock::now();
        static int rtDumpPresent = -1;
        // GOW_GS_HW_RTDUMPRANGE=a-b: batches of that frame to dump (default 26-46)
        static const char *rtRange = std::getenv("GOW_GS_HW_RTDUMPRANGE");
        static const int rtDumpLo = rtRange ? std::atoi(rtRange) : 26;
        static const int rtDumpHi = rtRange && std::strchr(rtRange, '-') ? std::atoi(std::strchr(rtRange, '-') + 1) : 46;
        // Tekken 4 port: GOW_GS_HW_RTDUMPSTEP=<n>: only every n-th batch of that range (watch a whole frame build up)
        static const int rtDumpStep = std::getenv("GOW_GS_HW_RTDUMPSTEP") ? std::max(1, std::atoi(std::getenv("GOW_GS_HW_RTDUMPSTEP"))) : 1;
        // GOW_GS_HW_RTDUMPFBP=<n>: dump that render target instead of fbp 0 / 208
        static const long rtDumpFbp = std::getenv("GOW_GS_HW_RTDUMPFBP") ? std::strtol(std::getenv("GOW_GS_HW_RTDUMPFBP"), nullptr, 0) : -1;
        static std::vector<bool> rtDumped(1024, false); // with the GOW_GS_HW_DUMP window: the first frame that reaches a batch wins
        if (rtDumpSec >= 0.0 && rtDumpPresent < 0 && frameBatch == 1 && std::chrono::duration<double>(std::chrono::steady_clock::now() - rtT0).count() >= rtDumpSec)
            rtDumpPresent = presentCount;
        if (((rtDumpAt >= 0 && presentCount == rtDumpAt) || (rtDumpPresent >= 0 && presentCount >= rtDumpPresent && presentCount <= rtDumpPresent + 1) || (rtRange && dumpPresents >= 0 && presentCount >= dumpPresents && presentCount < dumpPresents + 3 && !rtDumped[frameBatch & 1023])) && (rtDumpFbp >= 0 ? static_cast<long>(rt.fbp) == rtDumpFbp : (rt.fbp == 0 || rt.fbp == 208)) && frameBatch >= rtDumpLo && frameBatch <= rtDumpHi && (frameBatch - rtDumpLo) % rtDumpStep == 0)
        {
            std::vector<uint8_t> px(static_cast<size_t>(rt.glW) * rt.glH * 4u);
            g.BindFramebuffer(kFramebuffer, rt.fbo);
            g.PixelStorei(kPackAlignment, 1);
            g.ReadPixels(0, 0, rt.glW, rt.glH, kRgba, kUnsignedByte, px.data());
            char name[64];
            std::snprintf(name, sizeof(name), "gs_hw_rt%u_p%d_b%03d.ppm", rt.fbp, presentCount, frameBatch);
            rtDumped[frameBatch & 1023] = true;
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P6 %u %u 255 ", rt.glW, rt.glH);
                for (size_t i = 0; i + 3 < px.size(); i += 4)
                    std::fwrite(&px[i], 1, 3, f);
                std::fclose(f);
            }
            std::snprintf(name, sizeof(name), "gs_hw_rt%u_p%d_b%03d_a.pgm", rt.fbp, presentCount, frameBatch);
            if (FILE *f = std::fopen(name, "wb")) // alpha (the shadow mask reaches the darken pass through it)
            {
                std::fprintf(f, "P5 %u %u 255 ", rt.glW, rt.glH);
                for (size_t i = 3; i < px.size(); i += 4)
                    std::fwrite(&px[i], 1, 1, f);
                std::fclose(f);
            }
        }

        static int errorsShown = 0;
        if (errorsShown < 8)
            if (const GLenum err = g.GetError())
                std::fprintf(stderr, "[gs-hw] GL error 0x%x after batch %llu\n", err, (unsigned long long)stats.batches), ++errorsShown;
    }

    // GOW-Port: GOW_GS_TEXSRC=1 classifies every texture decode (and target readback) by where its data came from, every
    // 5 s: one upload of the same format and buffer, parts / other formats of uploads, local copies, render targets.
    // Measures what a texture store without the GS memory image must handle (docs/native-port.md, step 5).
    struct UploadRect { uint32_t dbp, dbw, dpsm, x, y, w, h, dir; };
    std::vector<UploadRect> uploadRects;
    std::map<std::string, uint64_t> texSources;
    const bool texSrcDiag = std::getenv("GOW_GS_TEXSRC") != nullptr;
    void noteTransfer(const GSTransferCommand &c)
    {
        if (!texSrcDiag)
            return;
        if (uploadRects.size() > 8192u)
            uploadRects.erase(uploadRects.begin(), uploadRects.begin() + 4096);
        uploadRects.push_back({c.bitbltbuf.dbp, c.bitbltbuf.dbw, c.bitbltbuf.dpsm, c.trxpos.dsax, c.trxpos.dsay,
                               c.trxreg.rrw, c.trxreg.rrh, c.direction});
    }
    void classifyTexture(const GSDrawState &s)
    {
        if (!texSrcDiag)
            return;
        const GSTex0Reg &t = s.context.tex0;
        const uint32_t lo = t.tbp0, hi = lo + spanBlocks(t.psm, t.tbw, std::max<uint32_t>(1u, s.textureHeight));
        const char *what = "none";
        uint32_t otherPsm = 0;
        for (auto it = uploadRects.rbegin(); it != uploadRects.rend(); ++it)
        {
            const UploadRect &u = *it;
            const uint32_t ulo = u.dbp, uhi = ulo + spanBlocks(u.dpsm, u.dbw, u.y + u.h);
            if (!(ulo < hi && lo < uhi))
                continue;
            if (u.dir == 2u)
                what = "local-copy";
            else if (u.dpsm != t.psm)
                what = "upload-other-psm", otherPsm = u.dpsm;
            else if (u.dbp == t.tbp0 && u.dbw == t.tbw && u.x == 0u && u.y == 0u && u.w >= s.textureWidth && u.h >= s.textureHeight)
                what = "upload-exact";
            else
                what = "upload-same-psm-part";
            break;
        }
        char key[96];
        std::snprintf(key, sizeof(key), "%s psm=%02x%s", what, t.psm, otherPsm ? (std::string(" from ") + std::to_string(otherPsm)).c_str() : "");
        ++texSources[key];
    }

    void reportStats()
    {
        if (texSrcDiag)
        {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(5))
            {
                last = now;
                for (const auto &kv : texSources)
                    std::fprintf(stderr, "[gs-texsrc] %s x%llu\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
                texSources.clear();
            }
        }
        if (!statsEnabled)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - stats.last < std::chrono::seconds(5))
            return;
        stats.last = now;
        std::fprintf(stderr, "[gs-hw] shuffles=%llu draws=%llu batches=%llu texDecodes=%llu texRowDecodes=%llu texRevalidated=%llu clutReadbacks=%llu clutRtHits=%llu texHits=%llu skippedPsm=%llu skippedPrim=%llu unsupportedBlend=%llu textures=%zu\n",
                     (unsigned long long)stats.shuffles, (unsigned long long)stats.draws, (unsigned long long)stats.batches, (unsigned long long)stats.texDecodes, (unsigned long long)stats.texRowDecodes, (unsigned long long)stats.texRevalidated, (unsigned long long)stats.clutReadbacks, (unsigned long long)stats.clutRtHits,
                     (unsigned long long)stats.texHits, (unsigned long long)stats.skippedPsm, (unsigned long long)stats.skippedPrim,
                     (unsigned long long)stats.unsupportedBlend, textures.size());
        {
            std::string why = "[gs-hw] rtUploads=" + std::to_string(stats.rtUploads) + " uploadsSame=" +
                              std::to_string(stats.uploadsSame) + " batch ends:";
            stats.rtUploads = 0;
            stats.uploadsSame = 0;
            char item[48];
            for (int i = 0; i < 10; ++i)
                if (stats.flushWhy[i])
                    std::snprintf(item, sizeof(item), " %s=%llu", kFlushWhyNames[i], (unsigned long long)stats.flushWhy[i]), why += item;
            why += " | state diffs:";
            for (int i = 0; i < 24; ++i)
                if (stats.stateDiff[i])
                    std::snprintf(item, sizeof(item), " %s=%llu", kStateDiffNames[i], (unsigned long long)stats.stateDiff[i]), why += item;
            std::fprintf(stderr, "%s\n", why.c_str());
            std::memset(stats.flushWhy, 0, sizeof(stats.flushWhy));
            std::vector<std::pair<uint64_t, std::string>> top;
            for (const auto &kv : stats.transfers)
                top.emplace_back(kv.second, kv.first);
            std::sort(top.rbegin(), top.rend());
            for (size_t i = 0; i < top.size() && i < 12u; ++i)
                std::fprintf(stderr, "[gs-hw] transfers x%llu %s\n", (unsigned long long)top[i].first, top[i].second.c_str());
            stats.transfers.clear();
            std::memset(stats.stateDiff, 0, sizeof(stats.stateDiff));
            for (const auto &kv : softWhy)
                std::fprintf(stderr, "[gs-hw] soft blend alpha=%02x fix=%02x colclamp=%u tme=%u x%llu\n", kv.first >> 16,
                             (kv.first >> 8) & 0xFFu, (kv.first >> 1) & 1u, kv.first & 1u, (unsigned long long)kv.second);
            softWhy.clear();
        }
        stats.shuffles = stats.draws = stats.batches = stats.texDecodes = stats.texRowDecodes = stats.texRevalidated = stats.clutReadbacks = stats.clutRtHits = stats.texHits = stats.skippedPsm = stats.skippedPrim = stats.unsupportedBlend = 0;
    }

    RenderTarget *findTarget(uint32_t fbp)
    {
        for (RenderTarget &t : targets)
            if (t.fbp == fbp)
                return &t;
        return nullptr;
    }
};

GSHwBackend::GSHwBackend() : m_impl(std::make_unique<Impl>()) {}
GSHwBackend::~GSHwBackend() = default;

void GSHwBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    m_impl->vram = vram;
    m_impl->vramSize = vramSize;
    m_impl->store.Initialize(vram, vramSize);
    // GOW-Port: create the GL context now (on the GS worker, while the host context is released) so that it can share
    // objects with the host window and UsesSharedPresentation() is known right after Initialize.
    m_impl->ensure();
}

bool GSHwBackend::UsesSharedPresentation() const
{
    return m_impl->context.Shared() && GSSharedPresent::Active();
}

void GSHwBackend::Reset()
{
    Impl &d = *m_impl;
    d.flushPending();
    d.store.Reset();
    d.clutHash = d.store.ClutHash();
    d.invertedZ.clear();
    for (auto &kv : d.textures)
        kv.second.valid = false, kv.second.listed = false;
    d.liveTextures.clear();
}

void GSHwBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount == 0u)
        return;
    Impl &d = *m_impl;
    if (!d.ensure())
        return;
    d.submit(batch);
}

void GSHwBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Impl &d = *m_impl;
    // Tekken 4 port: CLUTs come from the uploaded bytes. GoW read a CLUT whose blocks a render target drew over back from
    // the GPU (synchronous ReadPixels); Tekken's T4 stage textures name CLUTs at cbp 0 (the back buffer) and those
    // readbacks gave wrong palettes (the stage was invisible) and ~3 pipeline stalls per frame (34.9 -> 38.8 fps).
    // TEKKEN4_GL_CLUT_RT=1 restores GoW's behaviour.
    // No flush here: a pending batch keeps the palette it started with, and submit() ends it when the CLUT changed.
    static const bool clutFromRt = std::getenv("TEKKEN4_GL_CLUT_RT") && std::getenv("TEKKEN4_GL_CLUT_RT")[0] == '1';
    if (d.initialized && clutFromRt)
    {
        d.flushPending(Impl::WhyClut); // the readback must see every draw before it
        d.store.LoadClut(tex0, texclut, [&d](const GSTex0Reg &t, uint32_t width, const uint16_t *xy, uint32_t count, uint32_t *words)
                         { return d.clutFromTarget(t, width, xy, count, words); });
        d.clutHash = d.store.ClutHash();
        return;
    }
    if (!d.vram)
        d.flushPending(Impl::WhyClut); // textureFor() decodes indexed textures through the current CLUT
    // Black & Bruised: through the CLUT cache (gs_texture_store.h LoadClutCached)
    uint64_t newHash = 0;
    if (d.store.LoadClutCached(tex0, texclut, d.clutHash, newHash))
        d.clutHash = newHash;
}

void GSHwBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl &d = *m_impl;
    d.flushPending(Impl::WhyTransfer);
    ++d.transferCount; // button-prompt cell fingerprints are re-checked after an upload
    d.store.BeginTransfer(command);
    if (d.statsEnabled && d.stats.transfers.size() < 4096u)
    {
        char key[96];
        std::snprintf(key, sizeof(key), "dir=%u dbp=%x dbw=%u dpsm=%u %ux%u", command.direction, command.bitbltbuf.dbp,
                      command.bitbltbuf.dbw, command.bitbltbuf.dpsm, command.trxreg.rrw, command.trxreg.rrh);
        ++d.stats.transfers[key];
    }
    d.noteTransfer(command);
    if (command.direction == 0u || command.direction == 2u)
    {
        const uint32_t rows = static_cast<uint32_t>(command.trxpos.dsay) + command.trxreg.rrh;
        const uint32_t lo = command.bitbltbuf.dbp, hi = lo + spanBlocks(command.bitbltbuf.dpsm, command.bitbltbuf.dbw, rows);
        if (command.direction == 0u && d.uploadSameCheck() && hi > lo && hi - lo <= Impl::kUploadHashMaxBlocks &&
            (d.deferredUpload.before = d.hashBlocks(lo, hi)) != 0u)
        {
            d.deferredUpload.active = true;
            d.deferredUpload.command = command;
            d.deferredUpload.lo = lo;
            d.deferredUpload.hi = hi;
        }
        else if (command.direction == 0u)
            d.invalidateForUpload(command, lo, hi);
        else
            d.invalidateBlocks(lo, hi);
        // GOW_GS_HW_XFERWATCH=<hex block>: debug, log the transfers whose block range holds that block
        static const long xferWatch = std::getenv("GOW_GS_HW_XFERWATCH") ? std::strtol(std::getenv("GOW_GS_HW_XFERWATCH"), nullptr, 16) : -1;
        // GOW_GS_HW_XFERRANGE=<lo hex>-<hi hex>: also log every transfer whose destination block lies in [lo, hi)
        static const char *xferRange = std::getenv("GOW_GS_HW_XFERRANGE");
        static const long rangeLo = xferRange ? std::strtol(xferRange, nullptr, 16) : -1;
        static const long rangeHi = xferRange && std::strchr(xferRange, '-') ? std::strtol(std::strchr(xferRange, '-') + 1, nullptr, 16) : -1;
        if (rangeLo >= 0 && static_cast<long>(lo) >= rangeLo && static_cast<long>(lo) < rangeHi && command.bitbltbuf.dbp != 0x2800u)
            std::fprintf(stderr, "[gs-hw-xfer-range] dir=%u dbp=%x dbw=%u dpsm=%u rr=%ux%u\n", command.direction, command.bitbltbuf.dbp,
                         command.bitbltbuf.dbw, command.bitbltbuf.dpsm, command.trxreg.rrw, command.trxreg.rrh);
        if (xferWatch >= 0 && lo <= static_cast<uint32_t>(xferWatch) && static_cast<uint32_t>(xferWatch) < hi)
            d.xferWatchHit = true, std::fprintf(stderr, "[gs-hw-xfer] dir=%u dbp=%x dbw=%u dpsm=%u dsa=%u,%u rr=%ux%u\n", command.direction,
                         command.bitbltbuf.dbp, command.bitbltbuf.dbw, command.bitbltbuf.dpsm, command.trxpos.dsax,
                         command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh);
        if (d.uploadLog.size() > 4096u)
            d.uploadLog.erase(d.uploadLog.begin(), d.uploadLog.begin() + 2048);
        d.uploadLog.push_back({lo, hi, ++d.writeSeq});
        d.noteUploadOnTargets(lo, hi, d.writeSeq);
        if (command.direction == 0u && d.rtUploadTarget(command))
        {
            d.rtUpload = command;
            d.rtUploadPending = true;
        }
    }
}

void GSHwBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl &d = *m_impl;
    if (d.xferWatchHit && sizeBytes >= 32u)
    {
        std::fprintf(stderr, "[gs-hw-xfer] data %u bytes:", sizeBytes);
        for (uint32_t i = 0; i < 32u; ++i)
            std::fprintf(stderr, " %02x", data[i]);
        std::fprintf(stderr, "\n");
    }
    const bool watched = d.xferWatchHit;
    d.xferWatchHit = false;
    d.store.Upload(data, sizeBytes);
    if (watched && d.vram)
    {
        static const long block = std::strtol(std::getenv("GOW_GS_HW_XFERWATCH"), nullptr, 16);
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < 8192u; ++i)
            h = (h ^ d.vram[static_cast<size_t>(block) * 256u + i]) * 16777619u;
        std::fprintf(stderr, "[gs-hw-xfer] page hash after upload %08x\n", h);
    }
}

void GSHwBackend::Flush()
{
    m_impl->flushPending(Impl::WhyFlush);
}

void GSHwBackend::TextureFlush()
{
    m_impl->flushPending(Impl::WhyTexFlush); // textures are re-read from the store when an upload overlaps them, not on TEXFLUSH
}

void GSHwBackend::Sync(GSSyncReason)
{
    m_impl->flushPending(Impl::WhySync);
}

bool GSHwBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Impl &d = *m_impl;
    if (!d.ensure() || context.frame.fbw == 0u)
        return false;
    d.flushPending(Impl::WhyClear);
    const Api &g = d.gl();
    RenderTarget &rt = d.target(context.frame.fbp, context.frame.fbw);
    g.BindFramebuffer(kFramebuffer, rt.fbo);
    g.Enable(kScissorTest);
    const int sx0 = context.scissor.x0, sy0 = context.scissor.y0;
    g.Scissor(static_cast<GLint>(std::floor(sx0 * d.scale)), static_cast<GLint>(std::floor(sy0 * d.scale)),
              static_cast<GLsizei>(std::ceil((context.scissor.x1 - sx0 + 1) * d.scale)),
              static_cast<GLsizei>(std::ceil((context.scissor.y1 - sy0 + 1) * d.scale)));
    g.ColorMask(kTrue, kTrue, kTrue, kTrue);
    g.ClearColor((rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f, ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f);
    g.Clear(kColorBufferBit);
    g.Disable(kScissorTest);
    return true;
}

// GOW-Port: the game never reads GS memory back (LOCAL->HOST), so there is nothing to hand out.
uint32_t GSHwBackend::ConsumeLocalToHostBytes(uint8_t *, uint32_t)
{
    return 0u;
}

uint32_t GSHwBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_impl->store.Read(psm, base, bw, x, y);
}

void GSHwBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_impl->store.Write(psm, base, bw, x, y, value);
}

void GSHwBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    m_impl->store.Snapshot(out);
}

GSTransferSnapshot GSHwBackend::GetTransferSnapshot() const
{
    return m_impl->store.TransferState();
}

PresentationFrame GSHwBackend::Present(const GSPresentationRequest &request)
{
    Impl &d = *m_impl;
    PresentationFrame frame;
    if (!d.ensure())
        return frame;
    d.flushPending(Impl::WhyPresent);
    ++d.presentCount;
    d.dumpAfterSkip = false;
    d.frameBatch = 0;
    d.reportStats();
    const bool enable1 = (request.pmode & 1ull) != 0ull, enable2 = (request.pmode & 2ull) != 0ull;
    const auto decode = [&](bool enabled, uint64_t dispfb, uint64_t display, uint32_t &fbp, uint32_t &fbw, uint32_t &dbx, uint32_t &dby,
                            uint32_t &w, uint32_t &h)
    {
        fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        fbw = std::max<uint32_t>(1u, static_cast<uint32_t>((dispfb >> 9) & 0x3Fu));
        dbx = static_cast<uint32_t>((dispfb >> 32) & 0x7FFu);
        dby = static_cast<uint32_t>((dispfb >> 43) & 0x7FFu);
        const uint32_t hdiv = static_cast<uint32_t>((display >> 23) & 0xFu) + 1u;
        w = (static_cast<uint32_t>((display >> 32) & 0xFFFu) + 1u) / hdiv;
        // Tekken 4 port: DH+1 output lines like the CPU backend; MAGV and interlaced FRAME mode repeat buffer lines
        // (uLineRep) instead of shortening the picture.
        h = static_cast<uint32_t>((display >> 44) & 0x7FFu) + 1u;
        return enabled && display != 0u && w != 0u && h != 0u;
    };
    // GOW_GS_HW_DISPLOG=1: the display registers each time they change (picture size and placement per mode)
    static const bool dispLog = std::getenv("GOW_GS_HW_DISPLOG") != nullptr;
    static uint64_t lastDisp[6] = {};
    if (dispLog && (request.pmode != lastDisp[0] || request.dispfb1 != lastDisp[1] || request.display1 != lastDisp[2] ||
                    request.dispfb2 != lastDisp[3] || request.display2 != lastDisp[4] || request.smode2 != lastDisp[5]))
    {
        lastDisp[0] = request.pmode; lastDisp[1] = request.dispfb1; lastDisp[2] = request.display1;
        lastDisp[3] = request.dispfb2; lastDisp[4] = request.display2; lastDisp[5] = request.smode2;
        std::fprintf(stderr, "[gs-hw-disp] present=%d pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                     d.presentCount, (unsigned long long)request.pmode, (unsigned long long)request.smode2, (unsigned long long)request.dispfb1,
                     (unsigned long long)request.display1, (unsigned long long)request.dispfb2, (unsigned long long)request.display2);
    }
    if (d.statsEnabled)
    {
        static int shown = 0;
        if (shown++ < 6)
            std::fprintf(stderr, "[gs-hw] present pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx\n",
                         (unsigned long long)request.pmode, (unsigned long long)request.smode2, (unsigned long long)request.dispfb1,
                         (unsigned long long)request.display1, (unsigned long long)request.dispfb2, (unsigned long long)request.display2);
    }
    // the two read circuits; circuit 1 is shown over circuit 2 (or BGCOLOR), blended by ALP (MMOD) or its own alpha x2
    uint32_t fbp1, fbw1, dbx1, dby1, w1, h1, fbp2, fbw2, dbx2, dby2, w2, h2;
    const bool ok1 = enable1 && decode(true, request.dispfb1, request.display1, fbp1, fbw1, dbx1, dby1, w1, h1);
    const bool ok2 = enable2 && decode(true, request.dispfb2, request.display2, fbp2, fbw2, dbx2, dby2, w2, h2);
    const bool shared = !request.forceReadback && d.context.Shared() && GSSharedPresent::Active();
    if (!ok1 && !ok2)
    {
        if (shared)
            GSGL::PublishSharedTexture(d.gl(), 0u, 0u, 0u, d.presentCount, 0u, 0u, request.vsyncTick);
        return frame;
    }
    // The host window shows the TV area of the game's own picture: 448 lines from DY 50 (gameplay, menus). The movies set
    // a 480-line display at the same DY; their streams are 640x480 with the picture in the top 448 lines and 32 black
    // lines below, which a TV shows in its bottom border. Taller displays are cut to 448 lines, so a movie fills the
    // window like the game does instead of leaving a black band under it.
    // Tekken 4 port: no 448-line TV cut (GoW's movies); the host frame holds 512 lines like the CPU backend's.
    constexpr uint32_t kTvLines = 512u;
    const uint32_t fbp = ok1 ? fbp1 : fbp2, w = std::min(ok1 ? w1 : w2, 640u), h = std::min(ok1 ? h1 : h2, kTvLines);
    frame.displayFbp = fbp;
    frame.sourceFbp = fbp;
    // GOW_GS_HW_DISPDUMP=<n>: the n-th present after the displayed buffer's size changed (e.g. a movie starts) writes that
    // whole render target to gs_hw_disp_<fbp>_<w>x<h>.ppm (what the read circuit can reach, beyond the displayed area)
    static const int dispDumpAt = std::getenv("GOW_GS_HW_DISPDUMP") ? std::atoi(std::getenv("GOW_GS_HW_DISPDUMP")) : -1;
    static uint32_t dispDumpSize = 0;
    static int dispDumpCount = 0;
    if (dispDumpAt >= 0)
    {
        if ((w << 16 | h) != dispDumpSize)
            dispDumpSize = w << 16 | h, dispDumpCount = 0;
        if (dispDumpCount++ == dispDumpAt)
            if (RenderTarget *rt = d.findTarget(fbp))
            {
                std::vector<uint8_t> px(static_cast<size_t>(rt->glW) * rt->glH * 4u);
                d.gl().BindFramebuffer(kFramebuffer, rt->fbo);
                d.gl().PixelStorei(kPackAlignment, 1);
                d.gl().ReadPixels(0, 0, rt->glW, rt->glH, kRgba, kUnsignedByte, px.data());
                char name[64];
                std::snprintf(name, sizeof(name), "gs_hw_disp_%u_%ux%u.ppm", fbp, w, h);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fprintf(f, "P6 %u %u 255 ", rt->glW, rt->glH);
                    for (size_t i = 0; i + 3 < px.size(); i += 4)
                        std::fwrite(&px[i], 1, 3, f);
                    std::fclose(f);
                }
            }
    }
    const uint32_t outW = static_cast<uint32_t>(std::ceil(w * d.scale)), outH = static_cast<uint32_t>(std::ceil(h * d.scale));
    // Tekken 4 port: the picture handed to the window is at the internal resolution step (outScale); with SSAA the
    // targets are drawn larger (scale = outScale * ssaa) and averaged down here
    const uint32_t finalW = static_cast<uint32_t>(std::ceil(w * d.outScale)), finalH = static_cast<uint32_t>(std::ceil(h * d.outScale));
    const Api &g = d.gl();
    g.PixelStorei(kPackAlignment, 1);
    static const int depthDumpAt = std::getenv("GOW_GS_HW_DEPTHDUMP") ? std::atoi(std::getenv("GOW_GS_HW_DEPTHDUMP")) : -1;
    if (depthDumpAt >= 0 && d.presentCount == depthDumpAt)
    {
        RenderTarget *main = d.findTarget(0);
        if (main && main->depth)
        {
            std::vector<float> depth(static_cast<size_t>(main->glW) * main->glH);
            g.BindFramebuffer(kFramebuffer, main->fbo);
            g.ReadPixels(0, 0, main->glW, main->glH, kDepthComponent, kFloat, depth.data());
            if (FILE *f = std::fopen("gs_hw_depth.pgm", "wb"))
            {
                std::fprintf(f, "P5 %u %u 255 ", main->glW, main->glH);
                for (float v : depth)
                {
                    const uint8_t b = static_cast<uint8_t>(std::min(255.0f, std::max(0.0f, v * 255.0f)));
                    std::fwrite(&b, 1, 1, f);
                }
                std::fclose(f);
            }
        }
    }

    // GOW_GS_HW_EDGELOG=1: every 300 presents, the displayed buffer's first host rows (at x = width/2) and columns
    // (at y = height/2) before composition (edge artefacts: what the frame buffer holds there)
    static const bool edgeLog = std::getenv("GOW_GS_HW_EDGELOG") != nullptr;
    if (edgeLog && d.presentCount % 300 == 0)
        if (RenderTarget *rt = d.findTarget(fbp))
        {
            uint8_t px[4 * 8];
            g.BindFramebuffer(kFramebuffer, rt->fbo);
            std::fprintf(stderr, "[gs-hw-edge] fbp=%x gl=%ux%u rows:", fbp, rt->glW, rt->glH);
            for (uint32_t y = 0; y < 8; ++y)
            {
                g.ReadPixels(static_cast<GLint>(rt->glW / 2u), static_cast<GLint>(y), 1, 1, kRgba, kUnsignedByte, px);
                std::fprintf(stderr, " %02x%02x%02x", px[0], px[1], px[2]);
            }
            std::fprintf(stderr, " cols:");
            for (uint32_t x = 0; x < 8; ++x)
            {
                g.ReadPixels(static_cast<GLint>(x), static_cast<GLint>(rt->glH / 4u), 1, 1, kRgba, kUnsignedByte, px);
                std::fprintf(stderr, " %02x%02x%02x", px[0], px[1], px[2]);
            }
            std::fprintf(stderr, " usedW=%u pxW=%u\n", rt->usedW, rt->pxW);
        }

    // compose the read circuits on the GPU, then read the result back once
    if (!d.presentProgram)
        return frame;
    if (!d.presentFbo || d.presentW != outW || d.presentH != outH)
    {
        if (d.presentTex)
            g.DeleteTextures(1, &d.presentTex);
        if (!d.presentFbo)
            g.GenFramebuffers(1, &d.presentFbo);
        g.GenTextures(1, &d.presentTex);
        g.BindTexture(kTexture2D, d.presentTex);
        g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), outW, outH, 0, kRgba, kUnsignedByte, nullptr);
        // complete without mipmaps (glCopyImageSubData refuses incomplete textures)
        g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
        g.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
        g.BindFramebuffer(kFramebuffer, d.presentFbo);
        g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, d.presentTex, 0);
        d.presentW = outW;
        d.presentH = outH;
    }
    g.BindFramebuffer(kFramebuffer, d.presentFbo);
    g.Viewport(0, 0, outW, outH);
    g.Disable(kScissorTest);
    g.Disable(kDepthTest);
    g.ColorMask(kTrue, kTrue, kTrue, kTrue);
    g.ClearColor(0.f, 0.f, 0.f, 1.f);
    g.Clear(kColorBufferBit);
    g.UseProgram(d.presentProgram);
    g.BindVertexArray(d.presentVao);
    g.ActiveTexture(kTexture0);
    g.Uniform1i(d.locPresentSrc, 0);
    // Row 0 of the displayed buffer is the game's black line (the 416 -> 448 stretch starts at y 0.5 and a line
    // primitive clears row 0), which a TV's overscan hides on the console: the first two PS2 rows and columns repeat
    // the next one so no dark line shows along the top and left edge.
    // Tekken 4 port: GoW repeated the first two rows/columns (its black line under the overscan); show them as they are.
    g.Uniform1i(d.locPresentEdge, 0);
    const bool interlacedFrame = (request.smode2 & 1ull) != 0ull && ((request.smode2 >> 1) & 1ull) != 0ull;
    const auto lineRepeat = [&](uint64_t display) {
        return static_cast<GLint>((((display >> 27) & 0x3u) + 1u) * (interlacedFrame ? 2u : 1u));
    };
    const GLint fieldOffset = (interlacedFrame && request.displayField == 1) ? static_cast<GLint>(std::lround(d.scale)) : 0;
    const bool mmod = ((request.pmode >> 5) & 1u) != 0u, slbg = ((request.pmode >> 7) & 1u) != 0u;
    const uint32_t alp = static_cast<uint32_t>((request.pmode >> 8) & 0xFFu);
    const auto drawCircuit = [&](uint32_t cfbp, uint64_t dispfb, uint32_t cdbx, uint32_t cdby, bool alphaFactor)
    {
        RenderTarget *rt = d.findTarget(cfbp);
        if (!rt)
            return;
        const GLint repeat = lineRepeat(dispfb == request.dispfb1 ? request.display1 : request.display2);
        g.Uniform4i(d.locPresentLineRep, repeat, repeat > 1 ? fieldOffset : 0, 0, 0);
        g.BindTexture(kTexture2D, rt->color);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kNearest);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kNearest);
        g.Uniform4i(d.locPresentOffset, static_cast<GLint>(cdbx * d.scale), static_cast<GLint>(cdby * d.scale),
                    ((dispfb >> 15) & 0x1Fu) == GS_PSM_CT24 ? 1 : 0, alphaFactor ? 1 : 0);
        g.DrawArrays(kTriangles, 0, 3);
        g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
        g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
    };
    g.Disable(kBlend);
    if (ok1 && ok2)
    {
        if (slbg)
        {
            g.ClearColor(static_cast<float>(request.bgcolor & 0xFFu) / 255.f, static_cast<float>((request.bgcolor >> 8) & 0xFFu) / 255.f,
                         static_cast<float>((request.bgcolor >> 16) & 0xFFu) / 255.f, 1.f);
            g.Clear(kColorBufferBit);
        }
        else
            drawCircuit(fbp2, request.dispfb2, dbx2, dby2, false);
        g.Enable(kBlend);
        g.BlendEquationSeparate(kFuncAdd, kFuncAdd);
        if (mmod)
        {
            const float f = static_cast<float>(alp) / 255.f;
            g.BlendColor(f, f, f, f);
            g.BlendFuncSeparate(kConstantColor, kOneMinusConstantColor, kZero, kOne);
        }
        else
            g.BlendFuncSeparate(0x0302 /*SRC_ALPHA*/, 0x0303 /*ONE_MINUS_SRC_ALPHA*/, kZero, kOne);
        drawCircuit(fbp1, request.dispfb1, dbx1, dby1, true);
        g.Disable(kBlend);
    }
    else
        drawCircuit(fbp, ok1 ? request.dispfb1 : request.dispfb2, ok1 ? dbx1 : dbx2, ok1 ? dby1 : dby2, false);
    g.BindVertexArray(d.vao);
    // the displayed image is opaque: force alpha to 1 on the GPU (the host draws it with blending)
    g.ColorMask(kFalse, kFalse, kFalse, kTrue);
    g.ClearColor(0.f, 0.f, 0.f, 1.f);
    g.Clear(kColorBufferBit);
    g.ColorMask(kTrue, kTrue, kTrue, kTrue);
    // Tekken 4 port (PC graphics options): FXAA on the composed picture, then the SSAA resolve to the output size
    GLuint finalTex = d.presentTex, finalFbo = d.presentFbo;
    uint32_t curW = outW, curH = outH;
    const auto postPass = [&](int slot, uint32_t pw, uint32_t ph, GLuint program, const std::function<void()> &uniforms)
    {
        if (!d.postFbo[slot])
            g.GenFramebuffers(1, &d.postFbo[slot]);
        if (!d.postTex[slot] || d.postW[slot] != pw || d.postH[slot] != ph)
        {
            if (d.postTex[slot])
                g.DeleteTextures(1, &d.postTex[slot]);
            g.GenTextures(1, &d.postTex[slot]);
            g.BindTexture(kTexture2D, d.postTex[slot]);
            g.TexImage2D(kTexture2D, 0, static_cast<GLint>(kRgba8), pw, ph, 0, kRgba, kUnsignedByte, nullptr);
            g.TexParameteri(kTexture2D, kTextureMinFilter, kLinear);
            g.TexParameteri(kTexture2D, kTextureMagFilter, kLinear);
            g.TexParameteri(kTexture2D, kTextureWrapS, kClampToEdge);
            g.TexParameteri(kTexture2D, kTextureWrapT, kClampToEdge);
            g.BindFramebuffer(kFramebuffer, d.postFbo[slot]);
            g.FramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, d.postTex[slot], 0);
            d.postW[slot] = pw;
            d.postH[slot] = ph;
        }
        g.BindFramebuffer(kFramebuffer, d.postFbo[slot]);
        g.Viewport(0, 0, pw, ph);
        g.Disable(kBlend);
        g.UseProgram(program);
        g.BindVertexArray(d.presentVao);
        g.ActiveTexture(kTexture0);
        g.BindTexture(kTexture2D, finalTex);
        uniforms();
        g.DrawArrays(kTriangles, 0, 3);
        finalTex = d.postTex[slot];
        finalFbo = d.postFbo[slot];
        curW = pw;
        curH = ph;
    };
    if (d.fxaa && d.fxaaProgram)
        postPass(0, curW, curH, d.fxaaProgram, [&] { g.Uniform1i(d.locFxaaSrc, 0); g.Uniform1f(d.locFxaaSubpix, d.fxaaSubpix); });
    if ((finalW != curW || finalH != curH) && d.areaProgram)
    {
        const float rx = static_cast<float>(curW) / static_cast<float>(finalW), ry = static_cast<float>(curH) / static_cast<float>(finalH);
        postPass(1, finalW, finalH, d.areaProgram, [&] { g.Uniform1i(d.locAreaSrc, 0); g.Uniform2f(d.locAreaRatio, rx, ry); });
    }
    g.BindVertexArray(d.vao);
    g.UseProgram(d.program);
    frame.width = curW;
    frame.height = curH;
    if (shared)
    {
        // GOW-Port: zero-copy presentation: GPU copy into a texture shared with the host window, no pixels returned.
        GSGL::PublishSharedTexture(g, finalTex, curW, curH, d.presentCount, fbp, fbp, request.vsyncTick);
        return PresentationFrame{};
    }
    frame.pixels.assign(static_cast<size_t>(curW) * curH * 4u, 0u);
    g.BindFramebuffer(kFramebuffer, finalFbo);
    g.ReadPixels(0, 0, curW, curH, kRgba, kUnsignedByte, frame.pixels.data());
    // the frontend packs presentation rows with a fixed 640 pixel stride
    if (frame.width < 640u)
    {
        std::vector<uint8_t> rows(static_cast<size_t>(640u) * frame.height * 4u, 0u);
        for (uint32_t y = 0; y < frame.height; ++y)
            std::memcpy(rows.data() + static_cast<size_t>(y) * 640u * 4u, frame.pixels.data() + static_cast<size_t>(y) * frame.width * 4u,
                        static_cast<size_t>(frame.width) * 4u);
        frame.pixels = std::move(rows);
    }
    return frame;
}
