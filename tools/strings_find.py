"""Print NUL-terminated strings in the ELF that contain any of the given substrings, with their EE addresses,
and the code that references each (lui/addiu pairs).

    python -I tools/strings_find.py .STG .CWD STAGES
"""
import re
import struct
import sys

elf = open("game/disc/SLES_516.20", "rb").read()
phoff, = struct.unpack_from("<I", elf, 0x1C)
_t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
img = elf[off:off + filesz]
words = struct.unpack_from(f"<{filesz // 4}I", img)
hits = []
for m in re.finditer(rb"[\x20-\x7e]{3,}", img):
    s = m.group().decode()
    if any(k in s for k in sys.argv[1:]):
        hits.append((va + m.start(), s))
refs = {a: [] for a, _ in hits}
for i, w in enumerate(words):
    if (w >> 26) != 0x0F:
        continue
    rt, hi = (w >> 16) & 31, w & 0xFFFF
    for j in range(1, 8):
        if i + j >= len(words):
            break
        w2 = words[i + j]
        if (w2 >> 26) == 0x09 and (w2 >> 21) & 31 == rt:
            v = ((hi << 16) + (((w2 & 0xFFFF) ^ 0x8000) - 0x8000)) & 0xFFFFFFFF
            if v in refs:
                refs[v].append(va + i * 4)
            break
for a, s in hits:
    print(f"0x{a:08x}: {s!r}  <- " + " ".join(f"0x{x:x}" for x in refs[a][:6]))
