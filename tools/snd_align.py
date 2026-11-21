"""Piecewise alignment of a capture against a reference: for each window of the capture, the reference offset that
matches best (search +-range around the running offset), the local SNR and gain. Shows drops/repeats/drift.

    py -3 tools/snd_align.py <capture.wav|.raw> <reference.wav> <capture start s> <ref start s> [win ms=20] [count=200]
"""
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from snd_compare_lib import load  # noqa: E402


def main():
    cap, _ = load(sys.argv[1])
    ref, _ = load(sys.argv[2])
    c0 = int(float(sys.argv[3]) * 48000)
    r0 = int(float(sys.argv[4]) * 48000)
    win = int(float(sys.argv[5]) * 48) if len(sys.argv) > 5 else 960
    count = int(sys.argv[6]) if len(sys.argv) > 6 else 200
    search = 4096
    off = r0 - c0
    for n in range(count):
        cs = c0 + n * win
        x = cap[cs:cs + win]
        if len(x) < win:
            break
        best = None
        for ch in (0,):
            pass
        lo = max(0, cs + off - search)
        hi = min(len(ref) - win, cs + off + search)
        if hi <= lo:
            break
        seg = ref[lo:hi + win, 0]
        xc = x[:, 0]
        corr = np.correlate(seg, xc, mode="valid")
        en = np.sqrt(np.convolve(seg * seg, np.ones(win), mode="valid").clip(1e-9))
        sc = corr / en / (np.linalg.norm(xc) + 1e-9)
        k = int(np.argmax(sc))
        roff = lo + k
        y = ref[roff:roff + win]
        res = []
        for ch in range(2):
            g = np.dot(x[:, ch], y[:, ch]) / (np.dot(y[:, ch], y[:, ch]) + 1e-9)
            e = x[:, ch] - g * y[:, ch]
            res.append((g, 10 * np.log10((np.dot(g * y[:, ch], g * y[:, ch]) + 1e-9) / (np.dot(e, e) + 1e-9))))
        z = int(np.sum(np.all(x == 0, axis=1)))
        print(f"{cs / 48000:8.3f}s ref {roff / 48000:8.3f}s d={roff - cs - off:+6d} score {sc[k]:.3f} "
              f"L g={res[0][0]:.2f} {res[0][1]:5.1f}dB  R g={res[1][0]:.2f} {res[1][1]:5.1f}dB zero={z}")
        off = roff - cs


main()
