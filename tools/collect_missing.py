"""Append missing-target addresses from run logs to config/extra_entries.txt (resume points / undiscovered code).

    python -I tools/collect_missing.py logs/run-x.err [...]
Prints the number of new addresses (0 = nothing new). Targets outside the code regions are listed but not added.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTRA = ROOT / "config" / "extra_entries.txt"
CODE = [(0x100000, 0x206090), (0x25CB00, 0x25E470)]
have = set()
if EXTRA.is_file():
    for line in EXTRA.read_text(encoding="utf-8-sig").splitlines():
        v = line.split("#", 1)[0].strip()
        if v:
            have.add(int(v, 0))
new, rejected = [], []
for log in sys.argv[1:]:
    text = Path(log).read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r"\[guest-branch:missing-target\] kind=(\S+) op=(.*?) source=0x([0-9a-f]+) target=0x([0-9a-f]+)", text):
        kind, op, src, tgt = m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4), 16)
        if tgt in have or tgt in [n for n, _ in new]:
            continue
        if tgt % 4 or not any(lo <= tgt < hi for lo, hi in CODE):
            rejected.append((tgt, kind, op, src))
            continue
        new.append((tgt, f"{kind} {op} from 0x{src:x}"))
if new:
    with EXTRA.open("a", encoding="utf-8") as f:
        for tgt, why in new:
            f.write(f"0x{tgt:08X}  # {why}\n")
for tgt, why in new:
    print(f"added 0x{tgt:08x} ({why})")
for tgt, kind, op, src in rejected:
    print(f"NOT CODE 0x{tgt:08x} ({kind} {op} from 0x{src:x})")
print(f"collect_missing: {len(new)} new")
