"""Reference decode of a Black & Bruised .STM / .VOC entry (SPU ADPCM, mono) to a 48 kHz WAV (linear resampling).

    py -3 tools/stm_decode.py <file> <entry index|list> [out.wav]

Header: u32 0x0c, u32 header size, then entries of 0x34 bytes from offset 8: u32 index, u32 rate, u32 size,
u32 offset, u32 ?, char name[32] (layout read from the files; 'list' prints them).
"""
import struct
import sys
import wave

import numpy as np

F = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]


def entries(d):
    out = []
    hdr = struct.unpack_from("<I", d, 4)[0]
    p = 8
    while p + 0x34 <= hdr + 8:
        idx, rate, size, off, _ = struct.unpack_from("<5I", d, p)
        name = d[p + 0x14:p + 0x34].split(b"\0")[0]
        if not name or rate == 0 or rate > 96000:
            break
        out.append((name.decode("latin1"), rate, size, off))
        p += 0x34
    return out


def adpcm(data):
    h1 = h2 = 0
    out = []
    for b in range(0, len(data) - 15, 16):
        sh = data[b] & 15
        fi = min((data[b] >> 4) & 7, 4)
        if sh > 12:
            sh = 9
        for i in range(2, 16):
            for nib in (data[b + i] & 15, data[b + i] >> 4):
                s = ((nib << 12) & 0xFFFF)
                s = (s - 0x10000 if s & 0x8000 else s) >> sh
                s += (h1 * F[fi][0] + h2 * F[fi][1] + 32) >> 6
                s = max(-32768, min(32767, s))
                h2, h1 = h1, s
                out.append(s)
    return np.array(out, dtype=np.float64)


d = open(sys.argv[1], "rb").read()
es = entries(d)
if sys.argv[2] == "list":
    for i, e in enumerate(es):
        print(i, e)
    sys.exit()
name, rate, size, off = es[int(sys.argv[2])]
x = adpcm(d[off:off + size])
n = int(len(x) * 48000 / rate)
y = np.interp(np.arange(n) * rate / 48000, np.arange(len(x)), x)
out = sys.argv[3] if len(sys.argv) > 3 else name + ".wav"
st = np.repeat(y[:, None], 2, axis=1).astype("<i2")
with wave.open(out, "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(st.tobytes())
print(name, rate, f"{len(x) / rate:.1f} s ->", out)
