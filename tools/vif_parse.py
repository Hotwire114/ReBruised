"""Parse a [bnb-vif0] payload line (tag + data words) into VIFcodes.

    python -I tools/vif_parse.py logs/vif0-payload.txt
"""
import re
import sys

NAMES = {0x00: "NOP", 0x01: "STCYCL", 0x02: "OFFSET", 0x03: "BASE", 0x04: "ITOP", 0x05: "STMOD", 0x06: "MSKPATH3",
         0x07: "MARK", 0x10: "FLUSHE", 0x11: "FLUSH", 0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT",
         0x20: "STMASK", 0x30: "STROW", 0x31: "STCOL", 0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}
FMT = {0: "S", 1: "V2", 2: "V3", 3: "V4"}
SIZE = {0: 32, 1: 16, 2: 8, 3: 16}

for line in open(sys.argv[1], encoding="utf-8"):
    words = [int(w, 16) for w in re.findall(r"\b[0-9a-f]{8}\b", line.split("tag:", 1)[1])]
    # tag: 4 words (CNT tag lo, hi, then 2 VIFcode words transferred with TTE); data follows
    stream = words[2:]
    print(f"--- {len(stream)} words")
    i = 0
    cl = wl = 1
    while i < len(stream):
        code = stream[i]
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        i += 1
        if cmd & 0x60 == 0x60:
            vn, vl, m = (cmd >> 2) & 3, cmd & 3, (cmd >> 4) & 1
            n = num or 256
            comps = vn + 1
            bits = 16 if (vn == 3 and vl == 3) else comps * SIZE[vl]
            src_vecs = n if cl >= wl else (n // wl) * cl + min(n % wl, cl)
            nwords = (src_vecs * bits + 31) // 32
            print(f"  [{i - 1:3d}] UNPACK {FMT[vn]}-{SIZE[vl] if not (vn == 3 and vl == 3) else 5} m={m} num={n} "
                  f"addr={imm & 0x3FF:#x} usn={(imm >> 14) & 1} flg={(imm >> 15) & 1}  ({nwords} words)  cl={cl} wl={wl}")
            sample = stream[i:i + min(nwords, 8)]
            print("        data: " + " ".join(f"{w:08x}" for w in sample))
            i += nwords
            continue
        name = NAMES.get(cmd, f"cmd{cmd:02x}")
        extra = ""
        if cmd == 0x01:
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
            extra = f" cl={cl} wl={wl}"
        elif cmd == 0x20:
            extra = f" mask={stream[i]:08x}"
            i += 1
        elif cmd in (0x30, 0x31):
            extra = " " + " ".join(f"{w:08x}" for w in stream[i:i + 4])
            i += 4
        elif cmd == 0x05:
            extra = f" mode={imm & 3}"
        elif cmd in (0x14, 0x15, 0x17):
            extra = f" addr={imm:#x}"
        print(f"  [{i - 1:3d}] {name} num={num} imm={imm:#x}{extra}")
