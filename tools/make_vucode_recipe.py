"""Maintainer tool: describe the captured VU micro memory images (build/vucode/vu0|vu1/*.bin) as byte ranges of the
game's ELF, so the player's PC can rebuild them from their own disc (tools/player_build.py) and nothing from the game
has to ship.

    python -I tools/make_vucode_recipe.py [elf]      -> config/vucode_recipe.json

The recipe holds only offsets, lengths and SHA-256s; a range that is not in the ELF fails here (it would have to ship).
"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ELF = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "game/disc/SLES_516.20"


def segments(image, elf):
    out, i = [], 0
    while i < len(image):
        if image[i:i + 8] == b"\0" * 8:
            i += 8
            continue
        key, best = image[i:i + 16], (0, -1)
        s = elf.find(key)
        while s >= 0:
            n = 0
            while i + n < len(image) and s + n < len(elf) and image[i + n] == elf[s + n]:
                n += 1
            if n > best[0]:
                best = (n, s)
            s = elf.find(key, s + 1)
        n = best[0] - best[0] % 8
        if n < 8:
            raise SystemExit(f"ERROR: bytes at 0x{i:x} are not in the ELF")
        out.append([i, best[1], n])
        i += n
    return out


def main():
    elf = ELF.read_bytes()
    recipe = {"elf_sha256": hashlib.sha256(elf).hexdigest(), "images": {}}
    for path in sorted((ROOT / "build/vucode").glob("vu[01]/*.bin")):
        image = path.read_bytes()
        recipe["images"][f"{path.parent.name}/{path.name}"] = {
            "size": len(image), "sha256": hashlib.sha256(image).hexdigest(), "segments": segments(image, elf)}
    if not recipe["images"]:
        raise SystemExit("ERROR: no build/vucode/vu0|vu1/*.bin")
    out = ROOT / "config/vucode_recipe.json"
    out.write_text(json.dumps(recipe, indent=1) + "\n", encoding="utf-8")
    print(f"{len(recipe['images'])} images -> {out}")


if __name__ == "__main__":
    main()
