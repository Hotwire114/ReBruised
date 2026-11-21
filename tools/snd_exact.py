"""Sample-exact map of a capture onto a reference: for each capture position, the reference position whose next K
samples of one channel are identical. Prints runs (capture start, length, reference start) and the jumps between.

    py -3 tools/snd_exact.py <capture> <reference.wav> <cap start s> <cap len s> <ref start s> <ref len s> [ch=0] [K=6]
"""
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from snd_compare_lib import load  # noqa: E402

cap, _ = load(sys.argv[1])
ref, _ = load(sys.argv[2])
c0, cn = int(float(sys.argv[3]) * 48000), int(float(sys.argv[4]) * 48000)
r0, rn = int(float(sys.argv[5]) * 48000), int(float(sys.argv[6]) * 48000)
ch = int(sys.argv[7]) if len(sys.argv) > 7 else 0
K = int(sys.argv[8]) if len(sys.argv) > 8 else 6
c = cap[c0:c0 + cn, ch].astype(np.int64)
r = ref[r0:r0 + rn, ch].astype(np.int64)
index = {}
for j in range(len(r) - K):
    index.setdefault(tuple(r[j:j + K]), j)
runs = []
i = 0
while i < len(c) - K:
    j = index.get(tuple(c[i:i + K]))
    if j is None:
        runs.append((i, None))
        i += 1
        continue
    n = 0
    while i + n < len(c) and j + n < len(r) and c[i + n] == r[j + n]:
        n += 1
    runs.append((i, (j, n)))
    i += max(n, 1)
# merge unmatched
prev_end = None
out = []
for i, m in runs:
    if m is None:
        if out and out[-1][0] == "x" and out[-1][1] + out[-1][2] == i:
            out[-1] = ("x", out[-1][1], out[-1][2] + 1)
        else:
            out.append(("x", i, 1))
    else:
        out.append(("m", i, m[1], m[0]))
for o in out[:400]:
    if o[0] == "x":
        print(f"cap {o[1] + c0:9d} ({(o[1] + c0) / 48000:8.4f}s) unmatched {o[2]}")
    else:
        print(f"cap {o[1] + c0:9d} ({(o[1] + c0) / 48000:8.4f}s) len {o[2]:6d} -> ref {o[3] + r0:9d} "
              f"(cap-ref {o[1] + c0 - o[3] - r0})")
