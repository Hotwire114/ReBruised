"""Functions Ghidra never found: code addresses that only data points at (C++ vtables, callback tables).

    python -I tools/find_missing_functions.py        -> config/extra_functions.csv (read by make_config.py)

A candidate is an aligned 32-bit word anywhere in the loaded image whose value lies in a code region, is not covered
by any function of config/ghidra_functions.csv, and starts with a non-zero word. Each becomes a function from the
candidate to the next known function start or candidate (whichever comes first). Also adds any address listed in
config/extra_entries.txt (found at run time: [guest-branch:missing-target]).
"""
import bisect
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ELF = ROOT / "game/disc/SLES_516.20"
# Code regions (tools/code_extent.py): main .text and the static-constructor island before the .ctors table.
# The island starts at the first entry of the .ctors table at 0x25e470 (0x25cb00); 0x25c000-0x25cb00 is data.
CODE = [(0x100000, 0x206090), (0x25CB00, 0x25E470)]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


def main():
    elf = ELF.read_bytes()
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    _t, off, va, _pa, filesz, *_ = struct.unpack_from("<8I", elf, phoff)
    words = struct.unpack_from(f"<{filesz // 4}I", elf, off)

    def word(a):
        return words[(a - va) // 4]

    rows = (ROOT / "config/ghidra_functions.csv").read_text(encoding="utf-8-sig").splitlines()[1:]
    not_code = set()
    nc = ROOT / "config/not_code.txt"
    for line in nc.read_text(encoding="utf-8-sig").splitlines() if nc.is_file() else []:
        v = line.split("#", 1)[0].strip()
        if v:
            not_code.add(int(v, 0))
    spans = []
    for r in rows:
        f = r.split(",")
        s, e = int(f[1], 16), int(f[2], 16)
        if s not in not_code and e > s:
            spans.append((s, e))
    spans.sort()
    # union of covered intervals
    covered = []
    for s, e in spans:
        if covered and s <= covered[-1][1]:
            covered[-1][1] = max(covered[-1][1], e)
        else:
            covered.append([s, e])
    cstarts = [c[0] for c in covered]
    starts = sorted({s for s, _ in spans})

    def is_covered(a):
        i = bisect.bisect_right(cstarts, a) - 1
        return i >= 0 and a < covered[i][1]

    start_set = set(starts)

    def wanted(a):
        """A referenced code address that is not a function start yet: uncovered, or inside a merged range right
        after a `jr ra` + delay slot / zero padding (Ghidra ran the previous function over it)."""
        if a % 4 or not in_code(a) or a in start_set or word(a) == 0:
            return False
        if not is_covered(a):
            return True
        return word(a - 8) == 0x03E00008 or word(a - 4) == 0

    cands = set()
    for w in set(words):
        if wanted(w):
            cands.add(w)
    # lui/addiu|ori pointer constants built in code (vtables filled by constructors at run time)
    for i, w in enumerate(words):
        a = va + i * 4
        if not in_code(a) or (w >> 26) != 0x0F:
            continue
        rt, imm = (w >> 16) & 31, w & 0xFFFF
        for j in range(1, 8):
            w2 = words[i + j]
            op2 = w2 >> 26
            if op2 in (0x09, 0x0D) and (w2 >> 21) & 31 == rt:
                lo = w2 & 0xFFFF
                v = ((imm << 16) + ((lo ^ 0x8000) - 0x8000 if op2 == 0x09 else lo)) & 0xFFFFFFFF
                if wanted(v):
                    cands.add(v)
                break
    # Uncovered stretches inside .text: every non-empty run, split after `jr ra` / `j` + delay slot.
    gap_funcs = 0
    for lo_r, hi_r in CODE:
        a = lo_r
        while a < hi_r:
            if is_covered(a) or word(a) == 0:
                a += 4
                continue
            start = a
            if start not in cands:
                cands.add(start)
                gap_funcs += 1
            while a < hi_r and not is_covered(a):
                w = word(a)
                a += 4
                if w == 0x03E00008 or (w >> 26) == 0x02:   # jr ra / j: function ends after the delay slot
                    a += 4
                    break
    extra = ROOT / "config/extra_entries.txt"
    for line in extra.read_text(encoding="utf-8-sig").splitlines() if extra.is_file() else []:
        v = line.split("#", 1)[0].strip()
        if v:
            cands.add(int(v, 0))
    cands = sorted(cands)
    out = ["Name,Start,End,Size"]
    for i, c in enumerate(cands):
        nxt = [x for x in (starts[bisect.bisect_right(starts, c)] if bisect.bisect_right(starts, c) < len(starts) else None,
                           cands[i + 1] if i + 1 < len(cands) else None) if x is not None]
        end = min(nxt) if nxt else c + 4
        region_end = next(hi for lo, hi in CODE if lo <= c < hi) if in_code(c) else end
        end = min(end, region_end)
        # trim trailing zero padding
        while end - 4 > c and word(end - 4) == 0:
            end -= 4
        out.append(f"VT_{c:08x},0x{c:08X},0x{end:08X},{end - c}")
    (ROOT / "config/extra_functions.csv").write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{len(cands)} missing functions ({gap_funcs} from uncovered .text runs) -> config/extra_functions.csv")
    for line in out[1:11]:
        print("  " + line)


if __name__ == "__main__":
    main()
