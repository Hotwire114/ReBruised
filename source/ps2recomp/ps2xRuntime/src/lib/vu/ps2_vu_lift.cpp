// Tekken 4 port (t4vurecomp): registry, run-time dispatch, exact-engine hand-over and bit-exact verification of the
// statically recompiled VU microprograms. See include/runtime/ps2_vu_lift.h.
#include "runtime/ps2_vu_lift.h"
#include "runtime/ps2_vu_lift_rt.h"
#include "runtime/ps2_recomp_audit.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#endif

// ps2_vu1_lower.cpp: the engine's EFU polynomials; ps2_vu1_core.cpp: the verify packet capture
float ps2VuEatan(float v);
float ps2VuEsin(float v);
float ps2VuEexp(float v);
std::vector<uint8_t> *ps2VuVerifyCapture(bool on);

namespace vl
{
    // normalizeFmacExactResult on the exact value for every lane in m; the engine's formulas in double (MSVC's long
    // double is double; the same SSE2 operations under the same rounding mode).
    uint32_t fmacExact(int kind, float r[4], const float a[4], const float b[4], const float acc[4], int m)
    {
        uint32_t lf = 0;
        for (int k = 0; k < 4; ++k)
        {
            if (!(m & (1 << k)))
                continue;
            const long double va = static_cast<long double>(a[k]);
            const long double vb = static_cast<long double>(b[k]);
            const long double vc = static_cast<long double>(acc[k]);
            long double exact = 0.0L;
            switch (kind)
            {
            case kAdd: exact = va + vb; break;
            case kSub: exact = va - vb; break;
            case kMul: exact = va * vb; break;
            case kMadd: exact = vc + va * vb; break;
            case kMsub: exact = vc - va * vb; break;
            case kOpmsub: exact = k == 3 ? 0.0L : vc - va * vb; break;
            case kOpmula: exact = k == 3 ? 0.0L : va * vb; break;
            default: break;
            }
            const bool negative = std::signbit(exact);
            const long double magnitude = std::fabs(exact);
            uint32_t flags = negative ? 0x2u : 0u;
            const uint32_t sign = negative ? 0x80000000u : 0u;
            if (magnitude == 0.0L)
            {
                flags |= 0x1u;
                r[k] = bitsf(sign);
            }
            else if (magnitude > static_cast<long double>(FLT_MAX))
            {
                flags |= 0x8u;
                r[k] = bitsf(sign | 0x7F7FFFFFu);
            }
            else if (magnitude < static_cast<long double>(FLT_MIN))
            {
                flags |= 0x5u;
                r[k] = bitsf(sign);
            }
            lf |= flags << (4 * k);
        }
        return lf;
    }

    uint32_t productStickyExact(const float a[4], const float b[4], const float p[4], int m)
    {
        uint32_t extra = 0;
        for (int k = 0; k < 4; ++k)
        {
            if (!(m & (1 << k)))
                continue;
            // VU1Interpreter::productStickyFlags (the rounded product's exponent was not 2..0xFD)
            const uint32_t aBits = fbits(a[k]), bBits = fbits(b[k]);
            (void)p;
            if ((aBits & 0x7FFFFFFFu) == 0u || (bBits & 0x7FFFFFFFu) == 0u)
            {
                extra |= ((aBits ^ bBits) & 0x80000000u) != 0u ? 0x3u : 0x1u;
                continue;
            }
            const double exact = static_cast<double>(a[k]) * static_cast<double>(b[k]);
            const double magnitude = std::fabs(exact);
            uint32_t flags = std::signbit(exact) ? 0x2u : 0u;
            if (magnitude == 0.0)
                flags |= 0x1u;
            else if (magnitude > static_cast<double>(FLT_MAX))
                flags |= 0x8u;
            else if (magnitude < static_cast<double>(FLT_MIN))
                flags |= 0x5u;
            extra |= flags & 0xFu;
        }
        return extra;
    }

    VL_NOINLINE uint32_t finSlow(int kind, __m128 *r, const __m128 *a, const __m128 *b, const __m128 *acc, int m, int fast)
    {
        alignas(16) float rr[4], aa[4], bb[4], cc[4];
        _mm_store_ps(rr, *r);
        _mm_store_ps(aa, *a);
        _mm_store_ps(bb, *b);
        _mm_store_ps(cc, *acc);
        const int s = _mm_movemask_ps(*r) & fast & m;
        uint32_t lf = ((s & 1) ? 0x2u : 0u) | ((s & 2) ? 0x20u : 0u) | ((s & 4) ? 0x200u : 0u) | ((s & 8) ? 0x2000u : 0u);
        lf |= fmacExact(kind, rr, aa, bb, cc, m & ~fast);
        *r = _mm_load_ps(rr);
        return lf;
    }
    VL_NOINLINE uint32_t stickySlow(const __m128 *a, const __m128 *b, const __m128 *p, int m, int fast)
    {
        alignas(16) float aa[4], bb[4], pp[4];
        _mm_store_ps(aa, *a);
        _mm_store_ps(bb, *b);
        _mm_store_ps(pp, *p);
        return ((_mm_movemask_ps(*p) & fast & m) ? 0x2u : 0u) | productStickyExact(aa, bb, pp, m & ~fast);
    }
    float eatan(float v) { return ps2VuEatan(v); }
    float esin(float v) { return ps2VuEsin(v); }
    float eexp(float v) { return ps2VuEexp(v); }
    void clipReport(uint32_t) {}
}

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Engine access

bool VuLiftAccess::kickActive(const VU1Interpreter &vu) { return vu.m_xgkick.active; }

void VuLiftAccess::kickStart(VU1Interpreter &vu, uint32_t qwordAddress, uint64_t cycle, uint64_t &kickCycle)
{
    vu.startXgkick(qwordAddress); // credit 1 at the issue cycle (or the whole packet with GOW_XGKICK_IMMEDIATE)
    kickCycle = cycle;
}

void VuLiftAccess::kickAdvance(VU1Interpreter &vu, uint64_t cycle, uint64_t &kickCycle)
{
    // The engine adds one credit per cycle and copies a qword per two credits; lazily: n cycles at once, the same
    // copies, made before any VU memory write of a later cycle (the generated code calls this before every store).
    if (!vu.m_xgkick.active || cycle <= kickCycle)
        return;
    const uint64_t n = cycle - kickCycle;
    vu.m_xgkick.cycleCredit += static_cast<uint32_t>(std::min<uint64_t>(n - 1u, 0x3FFFFFFFu));
    vu.progressXgkick();
    kickCycle = cycle;
}

uint64_t VuLiftAccess::kickFinish(VU1Interpreter &vu, uint64_t &kickCycle)
{
    if (!vu.m_xgkick.active)
        return 0u;
    // as flushPipelines' bulk transfer: n copies with start credit c0 end 2n - c0 cycles later
    constexpr uint32_t kCredit = 0x40000000u;
    const uint32_t startCredit = vu.m_xgkick.cycleCredit;
    vu.m_xgkick.cycleCredit = kCredit - 1u; // progressXgkick adds the first one
    vu.progressXgkick();
    const uint64_t iterations = (static_cast<uint64_t>(kCredit) + 1u - vu.m_xgkick.cycleCredit) / 2u;
    kickCycle += 2u * iterations - startCredit;
    vu.m_xgkick.active = false;
    return kickCycle;
}

uint64_t VuLiftAccess::cycle(const VU1Interpreter &vu) { return vu.m_cycle; }
bool VuLiftAccess::directWrites(const VU1Interpreter &vu) { return vu.m_directRegisterWrites; }

namespace
{
    constexpr uint8_t laneBit(uint32_t component) { return static_cast<uint8_t>(1u << (3u - component)); }
}

void VuLiftAccess::exportState(VU1Interpreter &vu, VuLiftCtx &x, uint64_t cycle, uint32_t pc)
{
    // The engine's state at the start of the pair at pc, step prologue not yet run (m_cycle = that pair's "now").
    vu.m_cycle = cycle;
    vu.m_state.cycles = cycle;
    vu.m_state.pc = pc;
    vu.m_state.branchPending = false;
    vu.m_state.branchDelay = 0;
    vu.m_state.ebit = false;
    vu.m_state.haltAfterDelaySlot = false;
    for (uint32_t r = 0; r < 32; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            vu.m_vfReady[r][c] = x.lane[r * 4 + c];
    for (uint32_t r = 0; r < 16; ++r)
        vu.m_viReady[r] = x.lane[128 + r];
    for (uint32_t c = 0; c < 4; ++c)
        vu.m_accReady[c] = x.lane[144 + c];
    vu.m_fdiv = {};
    if (x.qValid)
    {
        vu.m_fdiv.valid = true;
        vu.m_fdiv.readyCycle = x.qReady;
        vu.m_fdiv.value = x.qValue;
        vu.m_fdiv.statusDi = x.qDi;
    }
    for (uint32_t k = 0; k < 2; ++k)
    {
        vu.m_efu[k] = {};
        if (x.pValid[k])
        {
            vu.m_efu[k].valid = true;
            vu.m_efu[k].readyCycle = x.pReady[k];
            vu.m_efu[k].value = x.pValue[k];
        }
    }
    vu.m_efuResourceReady = x.efuRes;
    vu.m_flagPipeline = {};
    vu.m_flagHead = 0u;
    vu.m_flagCount = x.flagCount;
    for (uint32_t k = 0; k < x.flagCount; ++k)
    {
        auto &e = vu.m_flagPipeline[k];
        const VuLiftFlagOut &f = x.flags[k];
        e.valid = true;
        e.readyCycle = f.ready;
        e.issueCycle = f.ready - VU1Interpreter::kFmacLatency;
        e.mac = f.mac;
        e.status = f.cur;
        e.extraSticky = f.extra;
        e.clip = f.clip;
        e.writesMac = f.writesMac != 0;
        e.writesStatus = f.writesStatus != 0;
        e.writesClip = f.writesClip != 0;
        e.writesSticky = false;
    }
    vu.m_workingClip = x.workingClip;
    vu.m_viBranchBackupValid = x.branchBackupReg != 0;
    vu.m_viBranchBackupReg = x.branchBackupReg;
    vu.m_viBranchBackupValue = x.branchBackupValue;
    uint64_t next = ~0ull;
    if (vu.m_fdiv.valid)
        next = std::min(next, vu.m_fdiv.readyCycle);
    for (const auto &e : vu.m_efu)
        if (e.valid)
            next = std::min(next, e.readyCycle);
    vu.m_nextCommitCycle = next;
}

void VuLiftAccess::finishRun(VU1Interpreter &vu, VuLiftCtx &x, uint64_t endCycle, uint32_t pc)
{
    // flushPipelines done by the generated code (everything committed, the transfer finished at endCycle)
    vu.m_cycle = endCycle;
    vu.m_state.cycles = endCycle;
    vu.m_state.pc = pc;
    vu.m_state.branchPending = false;
    vu.m_state.branchDelay = 0;
    vu.m_state.ebit = false;
    vu.m_state.haltAfterDelaySlot = false;
    // lanes still in flight after the end (only a resume() could see them; earlier ones behave as "ready")
    for (uint32_t r = 0; r < 32; ++r)
        for (uint32_t c = 0; c < 4; ++c)
            if (x.lane[r * 4 + c] > endCycle)
                vu.m_vfReady[r][c] = x.lane[r * 4 + c];
    for (uint32_t r = 0; r < 16; ++r)
        if (x.lane[128 + r] > endCycle)
            vu.m_viReady[r] = x.lane[128 + r];
    for (uint32_t c = 0; c < 4; ++c)
        if (x.lane[144 + c] > endCycle)
            vu.m_accReady[c] = x.lane[144 + c];
    if (x.efuRes > endCycle)
        vu.m_efuResourceReady = x.efuRes;
    vu.m_workingClip = x.workingClip;
    vu.m_viBranchBackupValid = x.branchBackupReg != 0;
    vu.m_viBranchBackupReg = x.branchBackupReg;
    vu.m_viBranchBackupValue = x.branchBackupValue;
    vu.m_nextCommitCycle = ~0ull;
}

// ---------------------------------------------------------------------------------------------------------------------
// Registry

namespace
{
    struct UnitTable
    {
        std::vector<const VuLiftProgramDesc *> programs;
        // per run-time code generation: entry pc (pair index) -> program, after checking its pairs
        std::vector<const VuLiftProgramDesc *> byEntry;
        uint64_t generation = ~0ull;
        const uint8_t *code = nullptr;
        uint32_t validPrograms = 0;
    };
    UnitTable g_units[2];
    std::mutex g_registerLock;

    struct Options
    {
        bool enabled = true;
        long verify = 0;   // runs left to verify (-1: all)
        bool stats = false;
        bool log = true;
    };
    Options &options()
    {
        static Options o = []
        {
            Options r;
            if (const char *e = std::getenv("PS2X_VULIFT"))
                r.enabled = std::strcmp(e, "0") != 0;
            if (const char *e = std::getenv("PS2X_VULIFT_VERIFY"))
                r.verify = std::strcmp(e, "all") == 0 ? -1 : std::atol(e);
            if (const char *e = std::getenv("PS2X_VULIFT_STATS"))
                r.stats = std::strcmp(e, "0") != 0;
            return r;
        }();
        return o;
    }

    // One process-wide verification budget, shared safely by EE/VU0 and the VU1 worker. The options stay immutable.
    bool takeVerificationRun()
    {
        static std::atomic<long> remaining{options().verify};
        long value = remaining.load(std::memory_order_relaxed);
        while (value > 0 && !remaining.compare_exchange_weak(value, value - 1, std::memory_order_relaxed))
        {
        }
        return value != 0; // -1 verifies every run; positive values reserve exactly one comparison
    }

    struct PcStats
    {
        uint64_t lifted = 0, bailed = 0, missing = 0, ns = 0, cycles = 0;
    };
    struct Stats
    {
        std::unordered_map<uint32_t, PcStats> pcs; // unit << 16 | pc
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        uint64_t verified = 0, mismatches = 0, rejected = 0;
    };
    Stats &stats()
    {
        static Stats s;
        return s;
    }
    std::mutex g_statsLock;

    void refreshTable(UnitTable &t, uint32_t unit, const uint8_t *code, uint32_t codeSize, uint64_t generation)
    {
        t.generation = generation;
        t.code = code;
        t.byEntry.assign(codeSize / 8u, nullptr);
        t.validPrograms = 0;
        for (const VuLiftProgramDesc *p : t.programs)
        {
            bool fits = true;
            for (uint32_t k = 0; k < p->rangeCount; ++k)
                fits = fits && p->ranges[k].end <= codeSize;
            if (!fits || vuLiftRangeHash(code, p->ranges, p->rangeCount) != p->imageHash)
                continue;
            ++t.validPrograms;
            for (uint32_t k = 0; k < p->entryCount; ++k)
                if (!t.byEntry[p->entries[k] / 8u])
                    t.byEntry[p->entries[k] / 8u] = p;
        }
        static unsigned shown[2] = {};
        if (options().log && shown[unit] < 8u)
        {
            ++shown[unit];
            std::fprintf(stderr, "[vulift] VU%u code generation %llu: %u of %zu lifted program groups match\n", unit,
                         static_cast<unsigned long long>(generation), t.validPrograms, t.programs.size());
        }
    }
}

void vuLiftRegister(const VuLiftProgramDesc *programs, uint32_t count)
{
    std::lock_guard<std::mutex> lock(g_registerLock);
    for (uint32_t i = 0; i < count; ++i)
        g_units[programs[i].unit & 1u].programs.push_back(&programs[i]);
}

uint32_t vuLiftRangeHash(const uint8_t *code, const VuLiftRange *ranges, uint32_t count)
{
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < count; ++k)
        for (uint32_t o = ranges[k].begin; o < ranges[k].end; ++o)
            h = (h ^ code[o]) * 16777619u;
    return h;
}

// ---------------------------------------------------------------------------------------------------------------------
// Run

struct VuLiftRunner
{
    static bool directOk(VU1Interpreter &vu) { return VuLiftAccess::directWrites(vu); }
};

namespace
{
    // tests (ps2xTest vulift_fuzz): a code image without a PS2Memory
    const uint8_t *g_testCode[2] = {nullptr, nullptr};
    uint64_t g_testGeneration[2] = {0, 0};

    const VuLiftProgramDesc *lookup(VU1Interpreter &vu, uint32_t unit, VU1Interpreter::StepContext &context, uint32_t pc)
    {
        if (!context.memory)
        {
            UnitTable &t = g_units[unit];
            if (!g_testCode[unit] || context.vuCode != g_testCode[unit] || t.programs.empty())
                return nullptr;
            if (g_testGeneration[unit] != t.generation || t.code != g_testCode[unit])
                refreshTable(t, unit, g_testCode[unit], context.codeSize, g_testGeneration[unit]);
            const uint32_t index = pc / 8u;
            return index < t.byEntry.size() ? t.byEntry[index] : nullptr;
        }
        UnitTable &t = g_units[unit];
        static const bool capturing = std::getenv("PS2X_VU_CAPTURE") != nullptr;
        if (t.programs.empty() && !capturing)
            return nullptr;
        const uint8_t *code = unit ? context.memory->getVU1Code() : context.memory->getVU0Code();
        if (context.vuCode != code)
            return nullptr;
        const uint64_t generation = unit ? context.memory->getVU1CodeGeneration() : context.memory->getVU0CodeGeneration();
        if (generation != t.generation || code != t.code)
        {
            refreshTable(t, unit, code, context.codeSize, generation);
            // PS2X_VU_CAPTURE=<folder>: each distinct micro memory a program runs from (game code: stays local)
            static const char *captureDir = std::getenv("PS2X_VU_CAPTURE");
            if (captureDir)
            {
                const VuLiftRange all{0, static_cast<uint16_t>(context.codeSize > 0xFFFFu ? 0xFFFFu : context.codeSize)};
                char name[64];
                std::snprintf(name, sizeof(name), "/vu%u_%08x.bin", unit, vuLiftRangeHash(code, &all, 1));
                const std::string path = std::string(captureDir) + name;
                if (std::FILE *probe = std::fopen(path.c_str(), "rb"))
                    std::fclose(probe);
                else if (std::FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    std::fwrite(code, 1, context.codeSize, f);
                    std::fclose(f);
                }
            }
        }
        const uint32_t index = pc / 8u;
        return index < t.byEntry.size() ? t.byEntry[index] : nullptr;
    }

    void noteStats(uint32_t unit, uint32_t pc, int what, uint64_t ns, uint64_t cycles)
    {
        std::lock_guard<std::mutex> lock(g_statsLock);
        Stats &s = stats();
        PcStats &p = s.pcs[unit << 16 | pc];
        if (what == 0) ++p.lifted;
        else if (what == 1) ++p.bailed;
        else ++p.missing;
        p.ns += ns;
        p.cycles += cycles;
        const auto now = std::chrono::steady_clock::now();
        if (now - s.last > std::chrono::seconds(10))
        {
            s.last = now;
            std::vector<std::pair<uint32_t, PcStats>> v(s.pcs.begin(), s.pcs.end());
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
            uint64_t tl = 0, tb = 0, tm = 0, tns = 0;
            for (const auto &e : v)
                tl += e.second.lifted, tb += e.second.bailed, tm += e.second.missing, tns += e.second.ns;
            std::fprintf(stderr, "[vulift-stats] 10 s: lifted %llu bailed %llu not-lifted %llu, %.2f ms host; verified %llu mismatches %llu\n",
                         (unsigned long long)tl, (unsigned long long)tb, (unsigned long long)tm, tns / 1e6,
                         (unsigned long long)s.verified, (unsigned long long)s.mismatches);
            for (size_t k = 0; k < v.size() && k < 24; ++k)
                std::fprintf(stderr, "[vulift-stats]   VU%u pc %04x: lifted %llu bailed %llu not-lifted %llu, %.2f ms, %llu cycles\n",
                             v[k].first >> 16, v[k].first & 0xFFFFu, (unsigned long long)v[k].second.lifted,
                             (unsigned long long)v[k].second.bailed, (unsigned long long)v[k].second.missing,
                             v[k].second.ns / 1e6, (unsigned long long)v[k].second.cycles);
            s.pcs.clear();
        }
    }
}

// Engine pieces the dispatcher needs (friend)
struct VuLiftEngine
{
    static const uint64_t &cycleRef(const VU1Interpreter &vu) { return vu.m_cycle; }
    static void runExact(VU1Interpreter &vu, VU1Interpreter::StepContext &c)
    {
        while (vu.stepInterpreted(c))
        {
        }
    }
    static void tail(VU1Interpreter &vu, VU1Interpreter::StepContext &c)
    {
        // what run() does after its loop, for the verify comparisons
        if (c.programEnded)
        {
            vu.flushPipelines();
            vu.m_state.ebit = false;
            vu.m_state.haltAfterDelaySlot = false;
            vu.m_pendingHaltD = false;
            vu.m_pendingHaltT = false;
        }
        vu.m_lastRunHitBudget = !c.programEnded && !vu.m_stopRequested && vu.m_cycle >= c.budgetEnd;
        vu.commitReadyFlags();
        vu.m_state.cycles = vu.m_cycle;
    }
    static bool &silent(VU1Interpreter &vu) { return vu.m_verifySilent; }
    static bool stopped(const VU1Interpreter &vu) { return vu.m_stopRequested; }
    static uint32_t unit(const VU1Interpreter &vu) { return vu.m_unit == VU1Interpreter::Unit::VU1 ? 1u : 0u; }
    static uint64_t ready(const VU1Interpreter &vu, uint32_t lane)
    {
        const uint64_t v = lane < 128 ? vu.m_vfReady[lane / 4][lane % 4] : lane < 144 ? vu.m_viReady[lane - 128] : vu.m_accReady[lane - 144];
        return std::max(v, vu.m_cycle);
    }
    static std::string compare(const VU1Interpreter &a, const VU1Interpreter &b)
    {
        std::string why;
        char buf[160];
        const VU1State &x = a.m_state, &y = b.m_state;
        auto w = [](float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; };
        for (int r = 0; r < 32 && why.size() < 400; ++r)
            for (int c = 0; c < 4; ++c)
                if (w(x.vf[r][c]) != w(y.vf[r][c]))
                {
                    std::snprintf(buf, sizeof(buf), " vf%d.%c %08x/%08x", r, "xyzw"[c], w(x.vf[r][c]), w(y.vf[r][c]));
                    why += buf;
                }
        for (int r = 0; r < 16; ++r)
            if (x.vi[r] != y.vi[r])
            {
                std::snprintf(buf, sizeof(buf), " vi%d %x/%x", r, x.vi[r], y.vi[r]);
                why += buf;
            }
        for (int c = 0; c < 4; ++c)
            if (w(x.acc[c]) != w(y.acc[c]))
            {
                std::snprintf(buf, sizeof(buf), " acc.%c %08x/%08x", "xyzw"[c], w(x.acc[c]), w(y.acc[c]));
                why += buf;
            }
        const struct { const char *n; uint64_t u, v; } scalars[] = {
            {"q", w(x.q), w(y.q)}, {"p", w(x.p), w(y.p)}, {"i", w(x.i), w(y.i)}, {"r", x.r, y.r}, {"pc", x.pc, y.pc},
            {"mac", x.mac, y.mac}, {"clip", x.clip, y.clip}, {"status", x.status, y.status}, {"cycles", x.cycles, y.cycles},
            {"ebit", x.ebit, y.ebit}, {"stopD", x.stoppedByD, y.stoppedByD}, {"stopT", x.stoppedByT, y.stoppedByT},
            {"branchPending", x.branchPending, y.branchPending}, {"m_cycle", a.m_cycle, b.m_cycle},
            {"workingClip", a.m_workingClip, b.m_workingClip}, {"budget", a.m_lastRunHitBudget, b.m_lastRunHitBudget},
            {"kickActive", a.m_xgkick.active, b.m_xgkick.active}, {"stop", a.m_stopRequested, b.m_stopRequested},
            {"efuRes", std::max(a.m_efuResourceReady, a.m_cycle), std::max(b.m_efuResourceReady, b.m_cycle)},
            {"bkValid", a.m_viBranchBackupValid, b.m_viBranchBackupValid},
            {"bkReg", a.m_viBranchBackupValid ? a.m_viBranchBackupReg : 0u, b.m_viBranchBackupValid ? b.m_viBranchBackupReg : 0u},
            {"bkVal", a.m_viBranchBackupValid ? static_cast<uint32_t>(a.m_viBranchBackupValue) : 0u,
             b.m_viBranchBackupValid ? static_cast<uint32_t>(b.m_viBranchBackupValue) : 0u},
        };
        for (const auto &s : scalars)
            if (s.u != s.v)
            {
                std::snprintf(buf, sizeof(buf), " %s %llx/%llx", s.n, (unsigned long long)s.u, (unsigned long long)s.v);
                why += buf;
            }
        for (uint32_t lane = 0; lane < 148; ++lane)
            if (ready(a, lane) != ready(b, lane))
            {
                std::snprintf(buf, sizeof(buf), " ready[%u] %llu/%llu", lane, (unsigned long long)ready(a, lane),
                              (unsigned long long)ready(b, lane));
                why += buf;
                break;
            }
        return why;
    }
};

void vuLiftTestSetCode(uint32_t unit, const uint8_t *code, uint64_t generation)
{
    g_testCode[unit & 1u] = code;
    g_testGeneration[unit & 1u] = generation;
}

uint32_t vuLiftTestEntries(uint32_t unit, uint16_t *out, uint32_t max)
{
    uint32_t n = 0;
    for (const VuLiftProgramDesc *p : g_units[unit & 1u].programs)
        for (uint32_t k = 0; k < p->entryCount && n < max; ++k)
            out[n++] = p->entries[k];
    return n;
}

void vuLiftTestSetSilent(VU1Interpreter &vu, bool silent) { VuLiftEngine::silent(vu) = silent; }

uint64_t vuLiftTestCounter(int which)
{
    std::lock_guard<std::mutex> lock(g_statsLock);
    return which == 0 ? stats().verified : which == 1 ? stats().mismatches : stats().rejected;
}

bool vuLiftRun(VU1Interpreter &vu, VU1Interpreter::StepContext &context, bool &ended)
{
    Options &o = options();
    if (!o.enabled || !VuLiftAccess::directWrites(vu) && VuLiftEngine::unit(vu) == 1u)
        return false;
    const uint32_t unit = VuLiftEngine::unit(vu);
    VU1State &s = vu.state();
    const uint32_t pc = s.pc;
    const VuLiftProgramDesc *prog = lookup(vu, unit, context, pc);
    const auto t0 = o.stats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint64_t cyc0 = VuLiftAccess::cycle(vu);
    if (!prog)
    {
        if (!VuLiftEngine::silent(vu)) ps2_recomp_audit::event(unit, false);
        if (o.stats)
            noteStats(unit, pc, 2, 0, 0);
        static std::atomic<int> shown{0};
        static std::mutex lock;
        static std::vector<uint32_t> seen;
        if (o.log && !std::getenv("PS2X_VULIFT_QUIET"))
        {
            std::lock_guard<std::mutex> g(lock);
            const uint32_t key = unit << 16 | pc;
            if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64)
            {
                seen.push_back(key);
                std::fprintf(stderr, "[vulift] VU%u: no lifted program at pc %04x (exact engine)\n", unit, pc);
            }
        }
        return false;
    }

    auto liftedOnce = [&](VU1Interpreter::StepContext &c) -> bool
    {
        VuLiftCtx x;
        x.vu = &vu;
        x.s = &s;
        x.data = c.vuData;
        x.dmask = c.dataSize - 1u;
        x.c0 = VuLiftAccess::cycle(vu);
        x.budgetEnd = c.budgetEnd;
        uint32_t r;
        {
            // End before exact-engine handover: the path cycle counts are disjoint.
            ps2_recomp_audit::Scope audit(unit, ps2_recomp_audit::Lifted, VuLiftEngine::cycleRef(vu),
                                         !VuLiftEngine::silent(vu));
            r = prog->fn(x, pc);
        }
        if (VuLiftEngine::stopped(vu))
        {
            // a rejected XGKICK packet (broken GIF tag) stops the engine at its next pair; the lifted code does not
            // model that stop's position (never seen with the game's real packets): say so
            static std::atomic<int> shown{0};
            if (shown.fetch_add(1) < 8)
                std::fprintf(stderr, "[vulift] VU%u program %04x: XGKICK packet rejected (stop position not exact)\n", unit, pc);
        }
        if (r == kVuLiftEnded)
        {
            c.programEnded = true;
            return true;
        }
        if (r == kVuLiftBailed)
        {
            if (!VuLiftEngine::silent(vu)) ps2_recomp_audit::event(unit, true);
            static std::atomic<int> shown{0};
            if (o.log && shown.fetch_add(1) < 16)
                std::fprintf(stderr, "[vulift] VU%u program %04x handed to the exact engine at pc %04x (why %u)\n", unit, pc,
                             x.bailPc, x.bailWhy);
            VuLiftEngine::runExact(vu, c);
            return false;
        }
        // kVuLiftNoEntry: not in this function (table error); nothing was done
        VuLiftEngine::runExact(vu, c);
        return false;
    };

    if (o.verify != 0 && takeVerificationRun())
    {
        // A: the exact engine alone. B: the lifted code (+ the engine after a hand-over). Same entry state, silent
        // (XGKICK packets captured instead of sent), then compared bit for bit.
        const std::vector<uint8_t> dataSave(context.vuData, context.vuData + context.dataSize);
        auto saved = std::make_unique<VU1Interpreter>(vu);
        const bool silentBefore = VuLiftEngine::silent(vu);

        VuLiftEngine::silent(vu) = true;
        std::vector<uint8_t> *packets = ps2VuVerifyCapture(true);
        packets->clear();
        VU1Interpreter::StepContext ca = context;
        VuLiftEngine::runExact(vu, ca);
        VuLiftEngine::tail(vu, ca);
        auto stateA = std::make_unique<VU1Interpreter>(vu);
        const std::vector<uint8_t> dataA(context.vuData, context.vuData + context.dataSize);
        const std::vector<uint8_t> packetsA = *packets;

        vu = *saved;
        std::memcpy(context.vuData, dataSave.data(), context.dataSize);
        VuLiftEngine::silent(vu) = true;
        packets->clear();
        VU1Interpreter::StepContext cb = context;
        liftedOnce(cb);
        VuLiftEngine::tail(vu, cb);
        const std::vector<uint8_t> dataB(context.vuData, context.vuData + context.dataSize);
        const std::vector<uint8_t> packetsB = *packets;
        ps2VuVerifyCapture(false);

        std::string why = VuLiftEngine::compare(*stateA, vu);
        if (ca.programEnded != cb.programEnded)
            why += " ended";
        unsigned memDiff = 0;
        for (uint32_t i = 0; i < context.dataSize; i += 4)
            if (std::memcmp(dataA.data() + i, dataB.data() + i, 4) != 0)
            {
                if (memDiff++ < 3)
                {
                    uint32_t a, b;
                    std::memcpy(&a, dataA.data() + i, 4);
                    std::memcpy(&b, dataB.data() + i, 4);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), " mem[%x] %08x/%08x", i, a, b);
                    why += buf;
                }
            }
        if (memDiff)
            why += " memWords=" + std::to_string(memDiff);
        if (packetsA != packetsB)
        {
            size_t at = 0;
            while (at < packetsA.size() && at < packetsB.size() && packetsA[at] == packetsB[at])
                ++at;
            why += " packets " + std::to_string(packetsA.size()) + "/" + std::to_string(packetsB.size()) + " first diff at " +
                   std::to_string(at);
        }
        {
            std::lock_guard<std::mutex> lock(g_statsLock);
            Stats &st = stats();
            if (VuLiftEngine::stopped(*stateA))
            {
                // the engine stopped on a rejected XGKICK packet (broken GIF tag): the lifted code does not model where
                // in the program that stop lands; counted apart (never seen with the game's real packets)
                if (st.rejected++ < 8)
                    std::fprintf(stderr, "[vulift-verify] VU%u pc %04x: XGKICK packet rejected by the engine (not compared)\n", unit, pc);
                why.clear();
            }
            else
                ++st.verified;
            if (!why.empty())
                ++st.mismatches;
            if (!why.empty() && st.mismatches <= 60)
                std::fprintf(stderr, "[vulift-verify] VU%u pc %04x run %llu MISMATCH:%s\n", unit, pc,
                             (unsigned long long)st.verified, why.c_str());
            else if (st.verified % 20000u == 0u)
                std::fprintf(stderr, "[vulift-verify] %llu runs checked, %llu mismatches\n", (unsigned long long)st.verified,
                             (unsigned long long)st.mismatches);
        }
        // the real run, from the same entry state
        vu = *saved;
        std::memcpy(context.vuData, dataSave.data(), context.dataSize);
        VuLiftEngine::silent(vu) = silentBefore;
    }

    const bool done = liftedOnce(context);
    ended = context.programEnded;
    if (o.stats)
    {
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        noteStats(unit, pc, done ? 0 : 1, ns, VuLiftAccess::cycle(vu) - cyc0);
    }
    return true;
}
