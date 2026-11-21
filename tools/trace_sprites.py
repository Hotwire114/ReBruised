"""Summarise a BNB_GS_TRACETBP / BNB_GS_TRACEALL log: per texture block, the frames where a sprite's box or alpha
changes (prim 6 = sprite only unless --all).

    python -I tools/trace_sprites.py logs/run-trc.err 3d60 3e60 [--all] [--max 40]
"""
import re
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
limit = 40
for f in flags:
    if f.startswith("--max="):
        limit = int(f[6:])
path, tbps = args[0], [t.lower() for t in args[1:]]
pat = re.compile(r"p=(\d+) b=(\d+) tbp=(\w+) fbp=(\d+) prim=(\d+) v=(\d+) box=(\S+) a=(\S+)")
rows = {}
for line in open(path, encoding="utf-8", errors="replace"):
    m = pat.search(line)
    if not m:
        continue
    p, b, tbp, fbp, prim, v, box, a = m.groups()
    if "--all" not in flags and prim != "6":
        continue
    if tbps and tbp not in tbps:
        continue
    rows.setdefault(tbp, []).append((int(p), int(b), box, a, int(v)))
for tbp, rs in rows.items():
    print(f"== {tbp}: {len(rs)} draws, frames {rs[0][0]}..{rs[-1][0]}")
    prev, shown = None, 0
    for p, b, box, a, v in rs:
        key = (box, a)
        if key != prev:
            print(f"  p{p} b{b} v{v} box={box} a={a}")
            prev = key
            shown += 1
            if shown >= limit:
                print("  ...")
                break
