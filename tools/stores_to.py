"""Find code that stores to / loads from an absolute address through `lui base, hi` + `sw/lw rt, lo(base)`.

    python -I tools/stores_to.py 0x262180 [0x262184]
"""
import struct
import sys

elf = open("game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
words = struct.unpack_from(f"<{filesz // 4}I", elf, off)
lo_a = int(sys.argv[1], 0)
hi_a = int(sys.argv[2], 0) if len(sys.argv) > 2 else lo_a + 4
OPS = {0x23: "lw", 0x2B: "sw", 0x37: "ld", 0x3F: "sd", 0x1E: "lq", 0x1F: "sq", 0x21: "lh", 0x29: "sh", 0x20: "lb", 0x28: "sb", 0x24: "lbu", 0x25: "lhu"}
for i, w in enumerate(words):
    if (w >> 26) != 0x0F:
        continue
    base, hi = (w >> 16) & 31, w & 0xFFFF
    for j in range(1, 12):
        if i + j >= len(words):
            break
        w2 = words[i + j]
        op = w2 >> 26
        if op in OPS and (w2 >> 21) & 31 == base:
            imm = ((w2 & 0xFFFF) ^ 0x8000) - 0x8000
            addr = ((hi << 16) + imm) & 0xFFFFFFFF
            if lo_a <= addr < hi_a:
                print(f"0x{va + (i + j) * 4:08x}: {OPS[op]} r{(w2 >> 16) & 31}, 0x{addr:08x}  (lui at 0x{va + i * 4:08x})")
        # base register overwritten -> stop
        if ((w2 >> 26) in (0x09, 0x0D, 0x0F, 0x23) and (w2 >> 16) & 31 == base) or \
           ((w2 >> 26) == 0 and (w2 >> 11) & 31 == base):
            break
