"""Which code references an address range? JAL/J targets and lui+addiu/ori pairs from code below CODE_END.

    python -I tools/refs_into.py <start> <end> [code_end]
"""
import struct
import sys

elf = open("game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
words = struct.unpack_from(f"<{filesz // 4}I", elf, off)
lo, hi = int(sys.argv[1], 0), int(sys.argv[2], 0)
code_end = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x208000
jal, ptr = {}, {}
for i, w in enumerate(words):
    a = va + i * 4
    if a >= code_end:
        break
    op = w >> 26
    if op in (2, 3):
        t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if lo <= t < hi:
            jal.setdefault(t, []).append(a)
    if op == 0x0F:  # lui
        rt, imm = (w >> 16) & 31, w & 0xFFFF
        for j in range(1, 8):
            if i + j >= len(words):
                break
            w2 = words[i + j]
            op2 = w2 >> 26
            if op2 in (0x09, 0x0D) and (w2 >> 21) & 31 == rt:
                v = (imm << 16) + (((w2 & 0xFFFF) ^ 0x8000) - 0x8000 if op2 == 0x09 else (w2 & 0xFFFF))
                v &= 0xFFFFFFFF
                if lo <= v < hi:
                    ptr.setdefault(v, []).append(a)
                break
print(f"J/JAL targets in [0x{lo:x},0x{hi:x}): {len(jal)}")
for t in sorted(jal)[:40]:
    print(f"  0x{t:08x} <- " + " ".join(f"0x{x:x}" for x in jal[t][:4]))
print(f"lui/addiu|ori addresses in range: {len(ptr)}")
for t in sorted(ptr)[:40]:
    print(f"  0x{t:08x} <- " + " ".join(f"0x{x:x}" for x in ptr[t][:4]))
# data words in the whole image that point into the range (function pointer tables)
dp = sorted({w for w in words if lo <= w < hi and w % 4 == 0})
print(f"aligned data words pointing into range: {len(dp)} (first: {[hex(x) for x in dp[:12]]})")
