"""Histogram of R5900 MMI instructions in .text (op 0x1C), with a few example addresses each.

    python -I tools/mmi_usage.py
"""
import collections
import struct

MMI0 = ["PADDW", "PSUBW", "PCGTW", "PMAXW", "PADDH", "PSUBH", "PCGTH", "PMAXH", "PADDB", "PSUBB", "PCGTB", "?", "?",
        "?", "?", "?", "PADDSW", "PSUBSW", "PEXTLW", "PPACW", "PADDSH", "PSUBSH", "PEXTLH", "PPACH", "PADDSB", "PSUBSB",
        "PEXTLB", "PPACB", "?", "?", "PEXT5", "PPAC5"]
MMI1 = ["?", "PABSW", "PCEQW", "PMINW", "PADSBH", "PABSH", "PCEQH", "PMINH", "?", "?", "PCEQB", "?", "?", "?", "?",
        "?", "PADDUW", "PSUBUW", "PEXTUW", "?", "PADDUH", "PSUBUH", "PEXTUH", "?", "PADDUB", "PSUBUB", "PEXTUB", "QFSRV",
        "?", "?", "?", "?"]
MMI2 = ["PMADDW", "?", "PSLLVW", "PSRLVW", "PMSUBW", "?", "?", "?", "PMFHI", "PMFLO", "PINTH", "?", "PMULTW", "PDIVW",
        "PCPYLD", "?", "PMADDH", "PHMADH", "PAND", "PXOR", "PMSUBH", "PHMSBH", "?", "?", "?", "?", "PEXEH", "PREVH",
        "PMULTH", "PDIVBW", "PEXEW", "PROT3W"]
MMI3 = ["PMADDUW", "?", "?", "PSRAVW", "?", "?", "?", "?", "PMTHI", "PMTLO", "PINTEH", "?", "PMULTUW", "PDIVUW",
        "PCPYUD", "?", "?", "?", "POR", "PNOR", "?", "?", "?", "?", "?", "?", "PEXCH", "PCPYH", "?", "?", "PEXCW", "?"]
FUNCT = {0x00: "MADD", 0x01: "MADDU", 0x04: "PLZCW", 0x10: "MFHI1", 0x11: "MTHI1", 0x12: "MFLO1", 0x13: "MTLO1",
         0x18: "MULT1", 0x19: "MULTU1", 0x1A: "DIV1", 0x1B: "DIVU1", 0x20: "MADD1", 0x21: "MADDU1", 0x30: "PMFHL",
         0x31: "PMTHL", 0x34: "PSLLH", 0x36: "PSRLH", 0x37: "PSRAH", 0x3C: "PSLLW", 0x3E: "PSRLW", 0x3F: "PSRAW"}

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
    if w >> 26 != 0x1C:
        continue
    f = w & 0x3F
    sa = (w >> 6) & 0x1F
    if f == 0x08:
        name = MMI0[sa]
    elif f == 0x28:
        name = MMI1[sa]
    elif f == 0x09:
        name = MMI2[sa]
    elif f == 0x29:
        name = MMI3[sa]
    else:
        name = FUNCT.get(f, f"funct{f:02x}")
    hist[name] += 1
    if len(where[name]) < 4:
        where[name].append(a)
for name, n in hist.most_common():
    print(f"{name:8s} {n:5d}  " + " ".join(f"0x{a:x}" for a in where[name]))
