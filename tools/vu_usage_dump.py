"""Register usage of VU microprograms (no mnemonics): per pair, VF/VI read/write and the lower opcode.

    python -I tools/vu_usage_dump.py logs/vucap/vu0_xxxx.bin 0x8f 0xa1 ...   (start addresses in instruction units)
Walks from each start to the first pair with the E bit (+ its delay slot).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "vu"))
from vu_isa import Pair  # noqa: E402

LOWER = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ", 0x11: "FCSET",
         0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR", 0x18: "FMEQ",
         0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR", 0x25: "JALR", 0x28: "IBEQ",
         0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ", 0x40: "LOWER-SPECIAL"}
LANES = "xyzw"


def lanes(m):
    return "".join(LANES[i] for i in range(4) if m & (8 >> i))


code = open(sys.argv[1], "rb").read()
for arg in sys.argv[2:]:
    start = int(arg, 0)
    print(f"=== program at {start:#x} (byte {start * 8:#x})")
    pc = start * 8
    end_after = None
    while pc + 8 <= len(code):
        lower, upper = struct.unpack_from("<II", code, pc)
        p = Pair(pc, lower, upper, vu1=False)
        lop = (lower >> 25) & 0x7F
        name = "I-imm" if p.ibit else LOWER.get(lop, f"op{lop:02x}")
        vr = " ".join(f"vf{r}.{lanes(l)}" for r, l in p.uu.vf_read + p.lu.vf_read)
        vw = [f"vf{r}.{lanes(l)}" for r, l in (p.uu.vf_write, p.lu.vf_write) if r]
        vir = [f"vi{i}" for i in range(16) if p.lu.vi_read & (1 << i)]
        viw = [f"vi{i}" for i in range(16) if p.lu.vi_write & (1 << i)]
        flags = ("E" if p.ebit else "") + ("I" if p.ibit else "")
        print(f"  {pc:#06x} {flags:2s} up={upper & 0x3F:02x} low={name:8s} R[{vr} {' '.join(vir)}] W[{' '.join(vw + viw)}]"
              f"{' clip' if p.uu.writes_clip else ''}")
        if end_after is not None:
            break
        if p.ebit:
            end_after = pc
        pc += 8
