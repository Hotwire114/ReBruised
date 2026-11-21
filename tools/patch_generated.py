"""Post-recompile fixes to generated/ that the prebuilt runtime cannot provide.

    python -I tools/patch_generated.py

* mfc0 Count -> bnb_cop0_count() (tools/gamedll/include/bnb_runtime_ext.h): the runtime never advances
  ctx->cop0_count, and the game's frame limiter waits for it.
"""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = Path(os.environ.get("BNB_GEN_DIR") or (ROOT / "generated"))
OLD = "(int32_t)ctx->cop0_count"
NEW = "(int32_t)bnb_cop0_count()"
INCLUDE = '#include "bnb_runtime_ext.h"\n'

# Code inserted before the instruction at a guest pc (C++ in the generated function's context).
GUEST_HEAP_END = 0x01C00000
INSERTS = {
    # crt0 SetupHeap(0x2AA200, -1) (syscall 0x3d at 0x100194) would give the runtime's own guest heap
    # (host stubs: SIF packets, thread stacks, MPEG/GS buffers) the same start as the game's newlib sbrk, which
    # grows from _end = 0x2AA200 too: the two allocators overwrote each other. The runtime gets 0x1C00000-0x1F00000.
    0x100194: f"SET_GPR_S32(ctx, 4, (int32_t)0x{GUEST_HEAP_END:X}u); SET_GPR_S32(ctx, 5, (int32_t)0x00300000u); "
              "/* bnb: runtime heap above the game's sbrk heap */",
    # game sbrk (0x1c4da0) after EndOfHeap: the game's heap ends where the runtime's begins
    0x1c4df8: f"if (GPR_U32(ctx, 2) > 0x{GUEST_HEAP_END:X}u) SET_GPR_S32(ctx, 2, (int32_t)0x{GUEST_HEAP_END:X}u); "
              "/* bnb: sbrk limit */",
    # WORKAROUND (root cause open): the title-screen script calls FUN_001ba050(x, obj) -> obj->vtbl[0x14](obj, 6)
    # with obj = 0 (an int-0 script handle, builtin at 0x1f12b4); the game then jumps to 0. Return 0 instead.
    0x1ba058: "if (GPR_U32(ctx, 4) == 0u) { BNB_PROBE(ctx, rdram, 0x1ba058u, 5); SET_GPR_S32(ctx, 2, 0); "
              "ctx->pc = GPR_U32(ctx, 31); return; } /* bnb: null object guard */",
    # frame cap setter FUN_0013afb0(fps): the PAL build sets 50 after the intro movies; settings.ini framerate = 60
    # keeps 60 (BNB_FPS). The vblank rate follows (main.cpp sets PS2X_VBLANK_HZ).
    0x13afb0: "BNB_FPS_CAP(ctx); /* bnb: frame rate setting */",
    # frame limiter entry: game frames per second (BNB_FPS_LOG=1)
    0x13b5d0: "bnb_frame_tick(); /* bnb: fps log */",
    # Voices / streamed audio (MultiStream): the sound update FUN_001246c0 acks the IOP's pending stream data
    # request when the status reply's sequence (gp-0x7da0) changed since its last call, then sends the next nowait
    # status RPC and stores the sequence. On hardware that reply lands later; here the RPC and its end callback
    # finish inside the call, so the stored sequence already includes the new reply and the ack is never sent:
    # every voice / music stream waits forever. Store the sequence seen before the RPC instead.
    0x1246e0: "bnb_ms_seq = READ32(ADD32(GPR_U32(ctx, 28), 0xFFFF8260u)); /* bnb: stream ack, see 0x124714 */",
    0x124714: "if (bnb_stream_ack_on()) SET_GPR_S32(ctx, 3, (int32_t)bnb_ms_seq); "
              "/* bnb: stream ack, sequence before the status RPC */",
}

# Probes: config/probes.txt lines "<pc> [limit]" -> BNB_PROBE before the instruction at <pc> (every function
# that contains a label for it). Output: "[bnb-probe] pc=... ra=... a0=..." on stderr.
# "<pc> [limit] [reg offset]": with reg/offset also dumps 8 words at GPR[reg] + offset (BNB_PROBE_MEM).
probes = {}
pf = ROOT / "config/probes.txt"
for line in pf.read_text(encoding="utf-8-sig").splitlines() if pf.is_file() else []:
    parts = line.split("#", 1)[0].split()
    if parts:
        limit = int(parts[1]) if len(parts) > 1 else 20
        mem = (int(parts[2], 0), int(parts[3], 0)) if len(parts) > 3 else None
        probes[int(parts[0], 0)] = (limit, mem)

patched = sites = probed = inserted = 0
for path in GEN.glob("*.cpp"):
    text = path.read_text(encoding="utf-8-sig")
    changed = False
    for pc, code in INSERTS.items():
        label = f"    // 0x{pc:x}: "
        if label in text and code not in text:
            text = text.replace(label, f"    {code}\n" + label, 1)
            inserted += 1
            changed = True
    if OLD in text:
        sites += text.count(OLD)
        text = text.replace(OLD, NEW)
        changed = True
    for pc, (limit, mem) in probes.items():
        label = f"    // 0x{pc:x}: "
        if label in text:
            call = (f"BNB_PROBE_MEM(ctx, rdram, 0x{pc:x}u, {limit}, {mem[0]}, {mem[1]});" if mem
                    else f"BNB_PROBE(ctx, rdram, 0x{pc:x}u, {limit});")
            text = text.replace(label, f"    {call}\n" + label, 1)
            probed += 1
            changed = True
    if changed:
        if INCLUDE not in text:
            text = INCLUDE + text
        path.write_text(text, encoding="utf-8")
        patched += 1
print(f"patch_generated: Count reads {sites}, inserts {inserted}, probes {probed} (of {len(probes)} pcs), "
      f"{patched} files")
