"""Where does real code end in the ELF? Histogram of prologues (addiu sp,sp,-N) and `jr ra` per 64 KiB.

    python -I tools/code_extent.py [elf]
"""
import struct
import sys

elf = open(sys.argv[1] if len(sys.argv) > 1 else "game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, memsz, _f, _a = struct.unpack_from("<8I", elf, phoff)
words = struct.unpack_from(f"<{filesz // 4}I", elf, off)
BUCKET = 0x4000
rows = {}
for i, w in enumerate(words):
    a = va + i * 4
    b = a - a % BUCKET
    r = rows.setdefault(b, [0, 0, 0])
    if (w & 0xFFFF8000) == 0x27BD8000:      # addiu sp,sp,-N
        r[0] += 1
    if w == 0x03E00008:                       # jr ra
        r[1] += 1
    if w == 0:
        r[2] += 1
print(f"segment va=0x{va:x} filesz=0x{filesz:x} memsz=0x{memsz:x}")
print("bucket      prologue  jr_ra  zero_words")
for b in sorted(rows):
    p, j, z = rows[b]
    print(f"0x{b:08x}  {p:8d}  {j:5d}  {z:6d}")
last = max(i for i, w in enumerate(words) if w == 0x03E00008)
print(f"last jr ra at 0x{va + last * 4:08x}")
