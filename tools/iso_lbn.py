"""Map disc sectors (LBNs) to files of the ISO9660 image.

    python -I tools/iso_lbn.py game/BnB.iso logs/run-x.err      # every sceCdStStart / sceCdRead LBN in a log
    python -I tools/iso_lbn.py game/BnB.iso 0x14534 0x1a9b5      # given LBNs
"""
import re
import struct
import sys

iso = open(sys.argv[1], "rb")


def sector(n):
    iso.seek(n * 2048)
    return iso.read(2048)


pvd = sector(16)
root = pvd[156:156 + 34]
files = []


def walk(lbn, size, path):
    data = b"".join(sector(lbn + i) for i in range((size + 2047) // 2048))
    off = 0
    while off < len(data):
        n = data[off]
        if n == 0:
            off = (off // 2048 + 1) * 2048
            continue
        rec = data[off:off + n]
        ext, = struct.unpack_from("<I", rec, 2)
        ln, = struct.unpack_from("<I", rec, 10)
        flags = rec[25]
        name = rec[33:33 + rec[32]].decode("latin1")
        if name not in ("\x00", "\x01"):
            full = f"{path}/{name.split(';')[0]}"
            if flags & 2:
                walk(ext, ln, full)
            else:
                files.append((ext, ext + (ln + 2047) // 2048, full, ln))
        off += n


walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
files.sort()


def find(lbn):
    for lo, hi, name, ln in files:
        if lo <= lbn < hi:
            return f"{name} (+{lbn - lo} of {hi - lo} sectors)"
    return "?"


args = sys.argv[2:]
lbns = []
if args and not args[0].startswith("0x") and not args[0].isdigit():
    text = open(args[0], encoding="utf-8", errors="replace").read()
    seen = set()
    for m in re.finditer(r"\[sceCdSt(?:Start|Seek)\] lbn=0x([0-9a-f]+)|sceCdRead\S* lbn=0x([0-9a-f]+)", text):
        v = int(m.group(1) or m.group(2), 16)
        if v not in seen:
            seen.add(v)
            lbns.append(v)
else:
    lbns = [int(a, 0) for a in args]
for v in lbns:
    print(f"0x{v:x}: {find(v)}")
