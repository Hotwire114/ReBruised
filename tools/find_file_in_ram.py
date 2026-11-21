"""Does a RAM dump contain pieces of a disc file? Reads <file> from the ISO by LBN and size, takes 64-byte probes every
<step> bytes and reports where each probe occurs in the dump.

    python -I tools/find_file_in_ram.py <iso> <lbn> <size> <ram.bin> [step]
"""
import sys

iso, lbn, size, ram = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
step = int(sys.argv[5], 0) if len(sys.argv) > 5 else 8192
with open(iso, "rb") as f:
    f.seek(lbn * 2048)
    data = f.read(size)
mem = open(ram, "rb").read()
found = 0
probes = 0
for off in range(0, len(data) - 64, step):
    probe = data[off:off + 64]
    if probe.count(probe[0]) == 64:
        continue
    probes += 1
    at = mem.find(probe)
    if at >= 0:
        found += 1
        if found <= 10:
            print(f"file+{off:#x} at ram {at:#x}")
print(f"{found} of {probes} probes found")
