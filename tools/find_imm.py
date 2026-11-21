"""Find instructions in .text whose 16-bit immediate equals one of the given values (addiu/ori/li/lui...).

    python -I tools/find_imm.py 0x2840 0x2848 0x284c
"""
import struct
import sys

elf = open("game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
words = struct.unpack_from(f"<{filesz // 4}I", elf, off)
vals = {int(v, 0) for v in sys.argv[1:]}
ops = {0x09: "addiu", 0x0D: "ori", 0x0F: "lui", 0x19: "daddiu", 0x0C: "andi"}
for i, w in enumerate(words):
    a = va + i * 4
    if a >= 0x206090:
        break
    op = w >> 26
    if op in ops and (w & 0xFFFF) in vals:
        print(f"0x{a:08x}: {ops[op]} r{(w >> 16) & 31}, r{(w >> 21) & 31}, 0x{w & 0xFFFF:x}")
