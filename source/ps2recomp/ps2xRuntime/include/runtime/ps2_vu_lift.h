// Tekken 4 port (t4vurecomp): statically recompiled VU microprograms.
//
// tools/vu/vu_lift.py (Tekken 4 repo) turns the player's own VU micro memory images into C++ at build time: every
// microprogram from its entry to its end, with the exact engine's pipeline timing (stalls, MAC/status/clip flag
// latency, Q/P latency, the XGKICK transfer) worked out per code path while generating, so nothing of the timing model
// runs at run time. The generated code is checked bit for bit against the exact engine (VU1Interpreter's step path):
// PS2X_VULIFT_VERIFY=<runs>. The generated C++ is game code: it stays in the build folder, never in a repository.
//
// Run time: VU1Interpreter::run() asks vuLiftRun() first. A program is used when its entry pc is known and every pair
// it can execute is byte-identical to what was lifted (checked once per code upload). Everything else (unknown entry,
// code changed, JR to a target that was not seen when lifting, cycle budget close) continues in the exact engine with
// the pipeline state handed over exactly (VuLiftAccess::exportState), so results never depend on what was lifted.
#ifndef PS2_VU_LIFT_H
#define PS2_VU_LIFT_H

#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <immintrin.h>

struct VuLiftCtx;
using VuLiftFn = uint32_t (*)(VuLiftCtx &, uint32_t entryPc);

// Return value of a lifted function
constexpr uint32_t kVuLiftEnded = 0u;    // program ended (E bit), state final, pipelines flushed
constexpr uint32_t kVuLiftBailed = 1u;   // state handed to the exact engine at a pair boundary: continue stepping
constexpr uint32_t kVuLiftNoEntry = 2u;  // entry pc not in this function: nothing was done

struct VuLiftRange
{
    uint16_t begin, end; // byte offsets in micro memory, end exclusive
};

struct VuLiftProgramDesc
{
    uint8_t unit;               // 0 = VU0, 1 = VU1
    uint32_t imageHash;         // FNV-1a 32 of the covered pairs (see vuLiftRangeHash)
    const VuLiftRange *ranges;  // pairs this function may execute
    uint32_t rangeCount;
    const uint16_t *entries;    // entry pcs
    uint32_t entryCount;
    VuLiftFn fn;
    const char *name;
};

// Called by the generated tables' static initialisers.
void vuLiftRegister(const VuLiftProgramDesc *programs, uint32_t count);
uint32_t vuLiftRangeHash(const uint8_t *code, const VuLiftRange *ranges, uint32_t count);

// Pending flag writer handed over to the exact engine (in issue order)
struct VuLiftFlagOut
{
    uint64_t ready;
    uint32_t mac, cur, extra, clip;
    uint8_t writesMac, writesStatus, writesClip;
};

// State of one lifted run. The register file lives in the interpreter's VU1State (VF, ACC in memory, VI in the
// generated function's locals, written back at every exit).
struct VuLiftCtx
{
    VU1Interpreter *vu = nullptr;
    VU1State *s = nullptr;
    uint8_t *data = nullptr;
    uint32_t dmask = 0;           // data size - 1
    uint64_t c0 = 0;              // engine cycle when the run started
    uint64_t budgetEnd = 0;       // engine cycle the run must not reach
    // hand-over to the exact engine (exportState)
    uint64_t lane[148];           // pipeline lane ready cycles (VF reg*4+comp, VI 128+reg, ACC 144+comp); 0 = ready
    uint64_t qReady = 0, pReady[2] = {0, 0}, efuRes = 0;
    float qValue = 0.f, pValue[2] = {0.f, 0.f};
    uint32_t qDi = 0;
    bool qValid = false, pValid[2] = {false, false};
    VuLiftFlagOut flags[8];
    uint32_t flagCount = 0;
    uint32_t workingClip = 0;
    int32_t branchBackupValue = 0;
    uint8_t branchBackupReg = 0;
    // statistics
    uint32_t bailPc = 0;
    uint32_t bailWhy = 0;         // 1 budget, 2 JR target, 3 other
};

// Run hook (ps2_vu1_core.cpp, VU1Interpreter::run): returns true when the lifted code ran; *ended tells whether the
// program ended (false: the exact engine continues from the handed-over state).
#if !defined(PS2X_GAME_DLL_VIEW)
bool vuLiftRun(VU1Interpreter &vu, VU1Interpreter::StepContext &context, bool &ended);
#endif
// tests: lifted code for an image that is not in a PS2Memory; silent XGKICK capture; verify counters (0 = runs
// checked, 1 = mismatches)
void vuLiftTestSetCode(uint32_t unit, const uint8_t *code, uint64_t generation);
void vuLiftTestSetSilent(VU1Interpreter &vu, bool silent);
uint64_t vuLiftTestCounter(int which);
uint32_t vuLiftTestEntries(uint32_t unit, uint16_t *out, uint32_t max);

// Friend of VU1Interpreter: the few engine pieces lifted code shares (XGKICK transfer, hand-over, end of program).
struct VuLiftAccess
{
    // XGKICK: the engine's own transfer (XgkickPipeline + progressXgkick/finishXgkick), advanced lazily to the cycle
    // the lifted code is at. kickCycle = the cycle up to which the transfer has been advanced.
    static bool kickActive(const VU1Interpreter &vu);
    static void kickStart(VU1Interpreter &vu, uint32_t qwordAddress, uint64_t cycle, uint64_t &kickCycle);
    static void kickAdvance(VU1Interpreter &vu, uint64_t cycle, uint64_t &kickCycle);
    // Runs the active transfer to its end with the memory as it is; returns the cycle of its last qword copy
    // (the engine's cycle at which PATH1 goes idle), or 0 when no transfer was active.
    static uint64_t kickFinish(VU1Interpreter &vu, uint64_t &kickCycle);
    static void exportState(VU1Interpreter &vu, VuLiftCtx &x, uint64_t cycle, uint32_t pc);
    static void finishRun(VU1Interpreter &vu, VuLiftCtx &x, uint64_t endCycle, uint32_t pc);
    static uint64_t cycle(const VU1Interpreter &vu);
    static bool directWrites(const VU1Interpreter &vu);
};

#endif
