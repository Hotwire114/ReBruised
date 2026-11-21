#pragma once
// Opt-in execution-path audit. Per-host-thread counters exclude silent oracle runs.
// Diagnostic host state only: no guest state is changed.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
namespace ps2_recomp_audit {
inline bool enabled() {
    static const bool on = [] { const char *e = std::getenv("PS2X_RECOMP_AUDIT"); return e && e[0] == '1'; }();
    return on;
}
inline std::atomic<uint64_t> missingNativeDispatch{0};
inline void noteEeMissing() {
    if (enabled()) missingNativeDispatch.fetch_add(1, std::memory_order_relaxed);
}
enum Path { Run, Lifted, NativeBlocks, CompiledPairs, Interpreted, Count };
struct Counter { uint64_t calls = 0, cycles = 0; };
struct Unit {
    Counter paths[Count]{};
    uint64_t missing = 0, handovers = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
inline Unit &unit(unsigned u) { static thread_local Unit units[2]; return units[u & 1u]; }
inline unsigned threadId() {
    static std::atomic<unsigned> next{1};
    static thread_local unsigned id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}
inline void event(unsigned u, bool handover) {
    if (enabled()) { auto &s = unit(u); if (handover) ++s.handovers; else ++s.missing; }
}
inline void report(unsigned u) {
    auto &s = unit(u);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - s.last).count();
    if (seconds < 1.0) return;
    const auto &r = s.paths[Run], &l = s.paths[Lifted], &n = s.paths[NativeBlocks],
               &c = s.paths[CompiledPairs], &i = s.paths[Interpreted];
    std::fprintf(stderr, "[recomp-audit] thread=%u VU%u seconds=%.6f runs=%llu cycles=%llu "
                 "lifted_calls=%llu lifted_cycles=%llu native_calls=%llu native_cycles=%llu "
                 "compiled_calls=%llu compiled_cycles=%llu interpreted_pairs=%llu interpreted_cycles=%llu "
                 "missing=%llu handovers=%llu missing_native_dispatch_total=%llu ee_interpreter=absent guest_jit=0\n", threadId(), u, seconds,
                 (unsigned long long)r.calls, (unsigned long long)r.cycles,
                 (unsigned long long)l.calls, (unsigned long long)l.cycles,
                 (unsigned long long)n.calls, (unsigned long long)n.cycles,
                 (unsigned long long)c.calls, (unsigned long long)c.cycles,
                 (unsigned long long)i.calls, (unsigned long long)i.cycles,
                 (unsigned long long)s.missing, (unsigned long long)s.handovers,
                 (unsigned long long)missingNativeDispatch.load(std::memory_order_relaxed));
    for (auto &p : s.paths) p = {};
    s.missing = s.handovers = 0;
    s.last = now;
}
class Scope {
    const uint64_t *cycle;
    uint64_t start;
    unsigned u;
    Path path;
public:
    Scope(unsigned unitNumber, Path p, const uint64_t &currentCycle, bool real = true)
        : cycle(real && enabled() ? &currentCycle : nullptr), start(cycle ? *cycle : 0), u(unitNumber), path(p) {}
    ~Scope() {
        if (!cycle) return;
        const uint64_t delta = *cycle - start;
        // A rejected native lookup is not a call into native game code.
        if (path != NativeBlocks || delta) { auto &p = unit(u).paths[path]; ++p.calls; p.cycles += delta; }
        if (path == Run) report(u);
    }
};
}
