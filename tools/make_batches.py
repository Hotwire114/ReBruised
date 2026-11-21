"""Group generated/*.cpp into compilation batches (build/batches/batch_NNN.cpp) for the llvm-mingw program build.

    python -I tools/make_batches.py [max_bytes]
"""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEN = Path(os.environ.get("BNB_GEN_DIR") or (ROOT / "generated"))
OUT = Path(os.environ.get("BNB_BATCH_DIR") or (ROOT / "build" / "batches"))
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 1_500_000

OUT.mkdir(parents=True, exist_ok=True)
files = sorted(GEN.glob("*.cpp"))
batches, cur, size = [], [], 0
for f in files:
    s = f.stat().st_size
    if cur and size + s > LIMIT:
        batches.append(cur)
        cur, size = [], 0
    cur.append(f)
    size += s
if cur:
    batches.append(cur)
wanted = set()
for i, group in enumerate(batches):
    path = OUT / f"batch_{i:03d}.cpp"
    wanted.add(path.name)
    text = "".join(f'#include "{p.as_posix()}"\n' for p in group)
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
for stale in OUT.glob("batch_*.cpp"):
    if stale.name not in wanted:
        stale.unlink()
print(f"{len(files)} generated files in {len(batches)} batches -> {OUT}")
