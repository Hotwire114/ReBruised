"""Minimal MIPS R3000 disassembler for IOP modules (.IRX, relocatable ELF with symbols).

    python -I tools/irx_dis.py <file.IRX> <function name | hex offset> [count]
Addresses are module-relative (add the load base from the IOP log). jal targets and import stubs are named.
"""
import struct
import sys

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

d = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from("<I", d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
shstr = secs[shstrndx]


def secname(s):
    o = shstr[4] + s[0]
    return d[o:d.index(b"\0", o)].decode()


byname = {secname(s): s for s in secs}
text = byname[".text"]
code = d[text[4]:text[4] + text[5]]
syms = {}
symtab, strtab = byname[".symtab"], byname[".strtab"]
for i in range(symtab[5] // 16):
    nm, val, size, info, oth, shn = struct.unpack_from("<IIIBBH", d, symtab[4] + i * 16)
    if nm:
        name = d[strtab[4] + nm:d.index(b"\0", strtab[4] + nm)].decode()
        if (info & 0xF) in (1, 2) or shn == secs.index(text):
            syms.setdefault(val, name)
# import stubs: magic 0x41E00000 tables, (jr ra; addiu zero,zero,ord) pairs
i = 0
while i + 20 <= len(code):
    if struct.unpack_from("<I", code, i)[0] == 0x41E00000:
        lib = code[i + 12:i + 20].split(b"\0")[0].decode("ascii", "replace")
        j = i + 20
        while j + 8 <= len(code):
            a, b = struct.unpack_from("<II", code, j)
            if a != 0x03E00008 or (b & 0xFFFF0000) != 0x24000000:
                break
            syms[j] = f"{lib}#{b & 0xFFFF}"
            j += 8
        i = j
    else:
        i += 4


def name(a):
    return syms.get(a, f"{a:#x}")


def dis(pc, w):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    br = pc + 4 + (simm << 2)
    if w == 0:
        return "nop"
    if op == 0:
        n = {0: "sll", 2: "srl", 3: "sra"}.get(fn)
        if n:
            return f"{n} {R[rd]}, {R[rt]}, {sa}"
        n = {4: "sllv", 6: "srlv", 7: "srav"}.get(fn)
        if n:
            return f"{n} {R[rd]}, {R[rt]}, {R[rs]}"
        if fn == 8:
            return f"jr {R[rs]}"
        if fn == 9:
            return f"jalr {R[rd]}, {R[rs]}"
        if fn == 12:
            return "syscall"
        n = {16: "mfhi", 18: "mflo"}.get(fn)
        if n:
            return f"{n} {R[rd]}"
        n = {17: "mthi", 19: "mtlo"}.get(fn)
        if n:
            return f"{n} {R[rs]}"
        n = {24: "mult", 25: "multu", 26: "div", 27: "divu"}.get(fn)
        if n:
            return f"{n} {R[rs]}, {R[rt]}"
        n = {32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt",
             43: "sltu"}.get(fn)
        if n:
            return f"{n} {R[rd]}, {R[rs]}, {R[rt]}"
    if op == 1:
        n = {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt, "regimm?")
        return f"{n} {R[rs]}, {br:#x}"
    if op in (2, 3):
        t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return f"{'j' if op == 2 else 'jal'} {name(t)}"
    if op in (4, 5):
        return f"{'beq' if op == 4 else 'bne'} {R[rs]}, {R[rt]}, {br:#x}"
    if op in (6, 7):
        return f"{'blez' if op == 6 else 'bgtz'} {R[rs]}, {br:#x}"
    n = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu"}.get(op)
    if n:
        return f"{n} {R[rt]}, {R[rs]}, {simm}"
    n = {12: "andi", 13: "ori", 14: "xori"}.get(op)
    if n:
        return f"{n} {R[rt]}, {R[rs]}, {imm:#x}"
    if op == 15:
        return f"lui {R[rt]}, {imm:#x}"
    if op == 16:
        return f"cop0 {w:#010x}"
    n = {32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl",
         43: "sw", 46: "swr"}.get(op)
    if n:
        return f"{n} {R[rt]}, {simm}({R[rs]})"
    return f".word {w:#010x}"


arg = sys.argv[2]
start = next((a for a, n in syms.items() if n == arg), None)
if start is None:
    start = int(arg, 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 64
for pc in range(start, min(start + count * 4, len(code)), 4):
    if pc in syms:
        print(f"{syms[pc]}:")
    w, = struct.unpack_from("<I", code, pc)
    print(f"  {pc:6x}: {w:08x}  {dis(pc, w)}")
