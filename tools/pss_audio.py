"""Reference decode of a PSS / FMV movie's sound track (no game code involved), for comparing with captures.

    py -3 tools/pss_audio.py <movie.FMV> <out.wav> [channel=0]

Sony PSS audio: MPEG-PS private_stream_1 packets, payload sub-header ff <type> 00 <channel>; the first payload of a
channel starts with "SShd" (type, rate, channels, interleave) then "SSbd" <size>; the data is <interleave> bytes of
left then right samples (16-bit LE PCM).
"""
import struct
import sys
import wave


def demux(data, want_ch):
    out = bytearray()
    i, n = 0, len(data)
    while i + 4 <= n:
        if data[i:i + 3] != b"\x00\x00\x01":
            i += 1
            continue
        sid = data[i + 3]
        if sid == 0xB9:
            break
        if sid == 0xBA:
            i += 14 + (data[i + 13] & 7) if (data[i + 4] & 0xC0) == 0x40 else 12
            continue
        ln = struct.unpack(">H", data[i + 4:i + 6])[0]
        end = i + 6 + ln
        if sid == 0xBD:
            p = i + 6
            if (data[p] & 0xC0) == 0x80:  # MPEG-2 PES header
                p += 3 + data[p + 2]
            else:  # MPEG-1
                while data[p] == 0xFF:
                    p += 1
                if (data[p] & 0xC0) == 0x40:
                    p += 2
                p += 5 if (data[p] & 0xF0) == 0x20 else 10 if (data[p] & 0xF0) == 0x30 else 1
            if data[p] == 0xFF and data[p + 3] == want_ch:
                out += data[p + 4:end]
        i = end
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    ch = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    raw = demux(open(src, "rb").read(), ch)
    if raw[:4] != b"SShd":
        sys.exit(f"no SShd header (got {raw[:16].hex()})")
    hsize = struct.unpack("<I", raw[4:8])[0]
    typ, rate, chans, inter = struct.unpack("<IIII", raw[8:24])
    body = raw[8 + hsize:]
    assert body[:4] == b"SSbd", body[:16].hex()
    size = struct.unpack("<I", body[4:8])[0]
    pcm = body[8:8 + size]
    print(f"type={typ:#x} rate={rate} channels={chans} interleave={inter:#x} bytes={len(pcm)}")
    frames = bytearray()
    blk = inter * chans
    for b in range(0, len(pcm) - blk + 1, blk):
        L = pcm[b:b + inter]
        R = pcm[b + inter:b + 2 * inter]
        for k in range(0, inter, 2):
            frames += L[k:k + 2] + R[k:k + 2]
    with wave.open(dst, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))
    print(f"{dst}: {len(frames) // 4 / rate:.2f} s")


main()
