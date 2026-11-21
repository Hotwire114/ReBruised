"""Copy build/generated.new into generated/ touching only files whose content changed (keeps ninja incremental).

    python -I tools/sync_generated.py
"""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NEW = ROOT / "build" / "generated.new"
DST = ROOT / "generated"
DST.mkdir(exist_ok=True)
changed = added = removed = 0
names = set()
for src in NEW.iterdir():
    if not src.is_file():
        continue
    names.add(src.name)
    dst = DST / src.name
    data = src.read_bytes()
    if dst.is_file():
        if dst.read_bytes() == data:
            continue
        changed += 1
    else:
        added += 1
    dst.write_bytes(data)
for old in DST.iterdir():
    if old.is_file() and old.name not in names:
        old.unlink()
        removed += 1
shutil.rmtree(NEW)
print(f"sync_generated: {added} added, {changed} changed, {removed} removed, {len(names)} total")
