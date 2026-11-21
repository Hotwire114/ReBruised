"""Histogram of COP2 (VU0 macro mode) instructions in .text with example addresses.

    python -I tools/cop2_usage.py
"""
import collections
import struct

UPPER = {0x00: "VADDbc", 0x04: "VSUBbc", 0x08: "VMADDbc", 0x0C: "VMSUBbc", 0x10: "VMAXbc", 0x14: "VMINIbc",
         0x18: "VMULbc", 0x1C: "VMULq", 0x1D: "VMAXi", 0x1E: "VMULi", 0x1F: "VMINIi", 0x20: "VADDq", 0x21: "VMADDq",
         0x22: "VADDi", 0x23: "VMADDi", 0x24: "VSUBq", 0x25: "VMSUBq", 0x26: "VSUBi", 0x27: "VMSUBi", 0x28: "VADD",
         0x29: "VMADD", 0x2A: "VMUL", 0x2B: "VMAX", 0x2C: "VSUB", 0x2D: "VMSUB", 0x2E: "VOPMSUB", 0x2F: "VMINI",
         0x30: "VIADD", 0x31: "VISUB", 0x32: "VIADDI", 0x34: "VIAND", 0x35: "VIOR", 0x38: "VCALLMS", 0x39: "VCALLMSR"}
SPECIAL2 = {0x00: "VADDAbc", 0x04: "VSUBAbc", 0x08: "VMADDAbc", 0x0C: "VMSUBAbc", 0x10: "VITOF0", 0x11: "VITOF4",
            0x12: "VITOF12", 0x13: "VITOF15", 0x14: "VFTOI0", 0x15: "VFTOI4", 0x16: "VFTOI12", 0x17: "VFTOI15",
            0x18: "VMULAbc", 0x1C: "VMULAq", 0x1D: "VABS", 0x1E: "VMULAi", 0x1F: "VCLIPw", 0x20: "VADDAq",
            0x21: "VMADDAq", 0x22: "VADDAi", 0x23: "VMADDAi", 0x24: "VSUBAq", 0x25: "VMSUBAq", 0x26: "VSUBAi",
            0x27: "VMSUBAi", 0x28: "VADDA", 0x29: "VMADDA", 0x2A: "VMULA", 0x2C: "VSUBA", 0x2D: "VMSUBA",
            0x2E: "VOPMULA", 0x2F: "VNOP", 0x30: "VMOVE", 0x31: "VMR32", 0x34: "VLQI", 0x35: "VSQI", 0x36: "VLQD",
            0x37: "VSQD", 0x38: "VDIV", 0x39: "VSQRT", 0x3A: "VRSQRT", 0x3B: "VWAITQ", 0x3C: "VMTIR", 0x3D: "VMFIR",
            0x3E: "VILWR", 0x3F: "VISWR", 0x40: "VRNEXT", 0x41: "VRGET", 0x42: "VRINIT", 0x43: "VRXOR"}
elf = open("game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
words = struct.unpack_from(f"<{filesz // 4}I", elf, off)
hist = collections.Counter()
where = collections.defaultdict(list)
for i, w in enumerate(words):
    a = va + i * 4
    if not (0x100000 <= a < 0x206090 or 0x25CB00 <= a < 0x25E470):
        continue
    op = w >> 26
    if op == 0x36:
        name = "LQC2"
    elif op == 0x3E:
        name = "SQC2"
    elif op != 0x12:
        continue
    else:
        fmt = (w >> 21) & 0x1F
        if fmt < 0x10:
            name = {1: "QMFC2", 2: "CFC2", 5: "QMTC2", 6: "CTC2", 8: "BC2x"}.get(fmt, f"cop2fmt{fmt:x}")
        else:
            f = w & 0x3F
            if f >= 0x3C:
                s2 = ((w >> 6) & 0x1F) << 2 | (w & 3)
                key = s2 & ~3 if s2 < 0x30 and (s2 & ~3) in (0x00, 0x04, 0x08, 0x0C, 0x18) else s2
                name = SPECIAL2.get(key, f"sp2_{s2:x}")
            else:
                key = f & ~3 if f < 0x1C else f
                name = UPPER.get(key, f"up_{f:x}")
    hist[name] += 1
    if len(where[name]) < 4:
        where[name].append(a)
for name, n in hist.most_common():
    print(f"{name:9s} {n:5d}  " + " ".join(f"0x{a:x}" for a in where[name]))
