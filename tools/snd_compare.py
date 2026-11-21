"""Find a reference sound (WAV) inside a capture (WAV or raw s16le stereo 48 kHz) and measure how well it matches.

    py -3 tools/snd_compare.py <capture.wav|.raw> <reference.wav> [seconds of reference to use=8] [ref offset s=0]

Prints the offset found, gain, SNR of the aligned capture against the reference, and the share of
frames that are exact zero (gaps). Also writes <capture>-spec.png (spectrogram of capture vs reference).
"""
import sys
import wave

import numpy as np
from scipy import signal


def load(path):
    if path.lower().endswith(".wav"):
        with wave.open(path, "rb") as w:
            rate = w.getframerate()
            ch = w.getnchannels()
            data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch)
            return data.astype(np.float64), rate
    data = np.fromfile(path, dtype="<i2")
    return data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.float64), 48000


def main():
    cap, rate = load(sys.argv[1])
    ref, rrate = load(sys.argv[2])
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 8.0
    roff = int(float(sys.argv[4]) * rrate) if len(sys.argv) > 4 else 0
    assert rate == rrate == 48000
    seg = ref[roff:roff + int(secs * rate)]
    # coarse alignment on the mono mix
    a = cap.mean(axis=1)
    b = seg.mean(axis=1)
    corr = signal.fftconvolve(a, b[::-1], mode="valid")
    energy = np.sqrt(signal.fftconvolve(a * a, np.ones(len(b)), mode="valid").clip(1e-9))
    score = np.nan_to_num(corr / energy / (np.linalg.norm(b) + 1e-9))
    k = int(np.argmax(score))
    print(f"capture {len(cap) / rate:.1f} s; best match at {k / rate:.3f} s, normalised correlation {score[k]:.4f}")
    c = cap[k:k + len(seg)]
    for ch in range(2):
        x, y = c[:, ch], seg[:, ch]
        g = np.dot(x, y) / (np.dot(y, y) + 1e-9)
        err = x - g * y
        snr = 10 * np.log10(np.dot(g * y, g * y) / (np.dot(err, err) + 1e-9))
        print(f"  ch{ch}: gain {g:.3f}  SNR {snr:.1f} dB")
    zero = np.mean(np.all(c == 0, axis=1))
    print(f"  zero frames in the matched span: {zero * 100:.2f} %")
    # residual over time (100 ms windows)
    x, y = c.mean(axis=1), seg.mean(axis=1)
    g = np.dot(x, y) / (np.dot(y, y) + 1e-9)
    win = rate // 10
    worst = []
    for i in range(0, len(x) - win, win):
        e = x[i:i + win] - g * y[i:i + win]
        s = g * y[i:i + win]
        worst.append(10 * np.log10((np.dot(s, s) + 1e-9) / (np.dot(e, e) + 1e-9)))
    worst = np.array(worst)
    print(f"  per-100ms SNR: median {np.median(worst):.1f} dB, min {worst.min():.1f} dB, "
          f"windows < 10 dB: {np.sum(worst < 10)} of {len(worst)}")
    try:
        from PIL import Image

        def spec(v):
            f, t, s = signal.spectrogram(v, rate, nperseg=1024, noverlap=512)
            s = 10 * np.log10(s + 1e-6)
            s = np.clip((s - s.max() + 90) / 90, 0, 1)
            return (s[::-1] * 255).astype(np.uint8)

        img = np.vstack([spec(x), np.full((8, spec(x).shape[1]), 128, np.uint8), spec(y)])
        out = sys.argv[1].rsplit(".", 1)[0] + "-spec.png"
        Image.fromarray(img).save(out)
        print(f"  spectrogram (top capture, bottom reference): {out}")
    except Exception as e:  # noqa
        print("  (no spectrogram)", e)


main()
