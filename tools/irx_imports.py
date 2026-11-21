"""List the imports of an IOP module (.IRX): library name, version and function ordinals.

    python -I tools/irx_imports.py <file.IRX>
Import tables start with the magic 0x41E00000, a version word, 8 bytes of library name, then
(jr ra; addiu zero, zero, ordinal) pairs.
"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
i = 0
while i + 20 <= len(d):
    if struct.unpack_from("<I", d, i)[0] == 0x41E00000:
        ver = struct.unpack_from("<I", d, i + 8)[0]
        name = d[i + 12:i + 20].split(b"\0")[0].decode("ascii", "replace")
        j = i + 20
        ords = []
        while j + 8 <= len(d):
            a, b = struct.unpack_from("<II", d, j)
            if a != 0x03E00008 or (b & 0xFFFF0000) != 0x24000000:
                break
            ords.append(b & 0xFFFF)
            j += 8
        print(f"{name:10s} v{ver:#06x}: {ords}")
        i = j
    else:
        i += 4
