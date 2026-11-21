#!/usr/bin/env python3
"""Maintainer tool: package ReBruised for players -- with nothing from the game in it.

    python -I tools/make_release.py --version 0.2.0 [--out dist] [--toolchain <llvm-mingw>] [--no-zip]

Run after tools/build_program.ps1 (it reuses that build). The package holds:

  ReBruised.exe + DLLs + assets   the launcher and the libraries the game uses (FFmpeg, libc++)
  kit/obj, kit/lib                the PS2Recomp runtime and our code, compiled here; no recompiled game code
  kit/include                     headers the generated code compiles against
  kit/bin/ps2_recomp.exe          the recompiler, run on the player's disc
  kit/config                      the recompiler config (function addresses and names) and the VU microcode recipe
                                  (offsets into the player's ELF), no code or data from the disc
  kit/tools                       tools/player_build.py and the scripts it runs
  toolchain/                      llvm-mingw (clang, lld, mingw-w64), only what an x86_64 build needs
  python/                         Python for the build scripts

On first PLAY the launcher asks for the player's disc and runs kit/tools/player_build.py, which recompiles the game
from it into BlackAndBruised.exe.

Release gate (fails the package): no disc files or game-derived names/types (ISO, ELF, IRX, generated code, batches,
lifted VU code, the built game exe, logs, saves, settings); no file containing bytes of the game's ELF or IRX modules
(sampled windows); no generated-function symbols (name_0x<addr>) in anything we compiled.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / "build" / "program"
SOURCE_URL = "https://github.com/Hotwire114/ReBruised"
DEFAULT_TC = str(ROOT / "llvm-mingw")   # BNB_TOOLCHAIN or --toolchain overrides
HEADER_EXT = {".h", ".hpp", ".hh", ".hxx", ".inl", ".inc", ".ipp", ".tcc"}

# Never in a release (lower-case names / parts / suffixes).
BAD_PARTS = {"game", "generated", "batches", "vulift", "vucode", "logs", "save", "saves", "build-game", "mc0", "mc1",
             "__pycache__", ".git"}
BAD_NAMES = {"sles_516.20", "system.cnf", "blackandbruised.exe", "settings.ini", "ps2_recompiled_functions.h",
             "ps2_recompiled_stubs.h", "register_functions.cpp", "bnb.ico"}
BAD_SUFFIXES = {".iso", ".elf", ".irx", ".bin", ".pss", ".cso", ".chd", ".mcd", ".ps2", ".log", ".dmp", ".pdb"}
# Recompiled functions are "<name>_0x<addr>" (C++-mangled: _Z17SetAlarm_0x1c4420Ph...); lifted VU programs carry
# "_u1_<image hash>_" / "vu1_<image hash>".
GAME_SYMBOL = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*_0x[0-9a-fA-F]{5,8}(?![0-9a-fA-F])|_u[01]_[0-9a-f]{8}_|vu[01]_[0-9a-f]{8}")
WINDOW = 32          # bytes per indexed window of the game's files
MAX_HITS = 64        # windows a file may share with the game (common tables the filter below lets through)

TOOLS = ["player_build.py", "patch_generated.py", "make_batches.py", "vu/vu_lift.py", "vu/vu_isa.py",
         "vu1_micro_from_elf.py"]   # the last: vu_lift also takes VU1 entry points from its address list
CONFIG = ["bnb.toml", "function_map.csv", "vulift_heads.txt", "vucode_recipe.json", "probes.txt"]


def say(text):
    print(text, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


# ---- what the program build compiled and linked (build/program/build.ninja) ----
def ninja_exe_edge():
    lines = (PROGRAM / "build.ninja").read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("build bin/BlackAndBruised.exe:"):
            inputs = line.split(":", 1)[1].split("|", 1)[0].split()[1:]
            vars_ = {}
            for v in lines[i + 1:i + 20]:
                if not v.startswith("  "):
                    break
                k, _, val = v.strip().partition(" = ")
                vars_[k] = val
            return inputs, vars_
    raise SystemExit("ERROR: no BlackAndBruised.exe link edge in build/program/build.ninja (build it first)")


def ninja_compile(obj_suffix):
    lines = (PROGRAM / "build.ninja").read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("build ") and obj_suffix in line.split(":", 1)[0]:
            vars_ = {}
            for v in lines[i + 1:i + 12]:
                if not v.startswith("  "):
                    break
                k, _, val = v.strip().partition(" = ")
                vars_[k] = val
            return vars_
    raise SystemExit("ERROR: no compile edge for " + obj_suffix)


def kit_path_for_include(d):
    p = Path(d).resolve()
    for base, prefix in ((PROGRAM / "ThirdParty" / "ffmpeg-prefix" / "src" / "ffmpeg_external", "ffmpeg"),
                         (ROOT, "")):
        try:
            rel = p.relative_to(base)
        except ValueError:
            continue
        return (Path(prefix) / rel) if prefix else rel
    raise SystemExit("ERROR: include dir outside the project: " + d)


def stage_kit(stage):
    kit = stage / "kit"
    inputs, link = ninja_exe_edge()
    objs, kit_objs = [i for i in inputs if "/batches/" not in i and "/vulift/" not in i], []
    for o in objs:
        name = Path(o).name.replace(".cpp.obj", ".o").replace(".rc.obj", ".res.o")
        copy(PROGRAM / o, kit / "obj" / name)
        kit_objs.append("obj/" + name)
    libs = []
    for tok in link["LINK_LIBRARIES"].split():
        if tok.startswith("-"):
            libs.append(tok)
        else:
            copy(PROGRAM / tok, kit / "lib" / Path(tok).name)
            libs.append("lib/" + Path(tok).name)
    # compile flags of one batch (generated code) and one lifted VU part
    batch = ninja_compile("batch_000.cpp.obj")
    vu = ninja_compile("vulift_part0.cpp.obj")
    defines = batch["DEFINES"].split()
    includes = []
    for tok in shlex.split(batch["INCLUDES"], posix=False):
        if tok in ("-isystem",):
            continue
        d = tok[2:] if tok.startswith("-I") else tok
        if Path(d).resolve() == (ROOT / "generated").resolve():
            continue
        rel = kit_path_for_include(d)
        src = Path(d)
        for f in src.rglob("*"):
            if f.is_file() and f.suffix.lower() in HEADER_EXT:
                copy(f, kit / rel / f.relative_to(src))
        includes.append(rel.as_posix())
    vu_flags = vu["FLAGS"].split()
    gen_flags = [f for f in batch["FLAGS"].split()]
    extra = gen_flags[len(vu_flags):] if gen_flags[:len(vu_flags)] == vu_flags else []
    if not extra:
        raise SystemExit("ERROR: generated-code flags not found after the common flags: " + batch["FLAGS"])
    for t in TOOLS:
        copy(ROOT / "tools" / t, kit / "tools" / t)
    for c in CONFIG:
        copy(ROOT / "config" / c, kit / "config" / c)
    copy(ROOT / "build" / "host-tools" / "ps2xRecomp" / "ps2_recomp.exe", kit / "bin" / "ps2_recomp.exe")
    recipe = json.loads((ROOT / "config" / "vucode_recipe.json").read_text(encoding="utf-8"))
    info = {
        "elf_sha256": recipe["elf_sha256"],
        "defines": defines, "include": includes, "cxxflags": vu_flags, "generated_flags": extra,
        "linkflags": link.get("FLAGS", "").split() + link.get("LINK_FLAGS", "").split(),
        "objs": kit_objs, "libs": libs,
    }
    (kit / "kit.json").write_text(json.dumps(info, indent=1) + "\n", encoding="utf-8")
    h = hashlib.sha256()
    for f in sorted(kit.rglob("*")):
        if f.is_file():
            h.update(f.relative_to(kit).as_posix().encode() + b"\0" + bytes.fromhex(sha256(f)))
    (kit / "build_id.txt").write_text(h.hexdigest()[:16] + "\n", encoding="utf-8")
    say(f"kit: {len(kit_objs)} objects, {sum(1 for l in libs if l.startswith('lib/'))} libraries, "
        f"{len(includes)} include dirs, build id {h.hexdigest()[:16]}")


def stage_toolchain(tc, stage):
    tc = Path(tc)
    out = stage / "toolchain"
    keep_bin = {"clang.exe", "clang++.exe", "clang-22.exe", "clang-target-wrapper.exe", "ld.lld.exe", "lld.exe",
                "libLLVM-22.dll", "libclang-cpp.dll", "libc++.dll", "libunwind.dll", "libwinpthread-1.dll",
                "libffi-8.dll", "x86_64-w64-mingw32-clang.exe", "x86_64-w64-mingw32-clang++.exe",
                "x86_64-w64-windows-gnu.cfg", "x86_64-pc-windows-gnu.cfg", "mingw32-common.cfg"}
    for f in (tc / "bin").iterdir():
        if f.name in keep_bin:
            copy(f, out / "bin" / f.name)
    for sub in ("include", "x86_64-w64-mingw32"):
        shutil.copytree(tc / sub, out / sub)
    res = next((tc / "lib" / "clang").iterdir())
    shutil.copytree(res / "include", out / "lib" / "clang" / res.name / "include")
    for f in (res / "lib").rglob("*x86_64*"):
        if f.is_file():
            copy(f, out / "lib" / "clang" / res.name / "lib" / f.relative_to(res / "lib"))
    for name in ("LICENSE.TXT", "README.md"):
        if (tc / name).is_file():
            copy(tc / name, out / name)


def stage_python(stage):
    skip = {"tcl", "tkinter", "turtledemo", "idlelib", "test", "__pycache__", "lib2to3", "msilib", "ensurepip"}
    src = ROOT / "python"
    for f in src.rglob("*"):
        rel = f.relative_to(src)
        if f.is_file() and not any(p.lower() in skip for p in rel.parts) and f.suffix != ".pyc":
            copy(f, stage / "python" / rel)


def stage_front(stage, version):
    bin_ = PROGRAM / "bin"
    copy(bin_ / "BlackAndBruisedLauncher.exe", stage / "ReBruised.exe")
    for f in bin_.glob("*.dll"):
        copy(f, stage / f.name)
    for f in (bin_ / "assets").glob("*"):
        copy(f, stage / "assets" / f.name)
    lic = stage / "licenses"
    copy(ROOT / "source" / "ps2recomp" / "LICENSE", lic / "PS2Recomp.txt")
    for d in (ROOT / "source" / "fetch").iterdir():
        for f in d.iterdir():
            if f.is_file() and re.match(r"(LICEN[CS]E|COPYING)", f.name, re.I):
                copy(f, lic / f"{d.name}.txt")
    (lic / "FFmpeg.txt").write_text(
        "FFmpeg (avcodec, avformat, avutil, swresample, swscale, ...) shared libraries: LGPL version 3 or later.\n"
        "Build: BtbN FFmpeg-Builds (https://github.com/BtbN/FFmpeg-Builds), source: https://ffmpeg.org/download.html\n",
        encoding="utf-8")
    (lic / "README.txt").write_text(
        "toolchain/LICENSE.TXT: llvm-mingw (LLVM: Apache-2.0 WITH LLVM-exception; mingw-w64: ZPL and others).\n"
        "python/LICENSE.txt: Python (PSF License).\n", encoding="utf-8")
    copy(ROOT / "LICENSE", stage / "LICENSE.txt")
    (stage / "README.txt").write_text(
        f"ReBruised {version} - a native PC recompilation of Black & Bruised (PAL, SLES-51620)\n"
        f"Source code and help: {SOURCE_URL}\n\n"
        "This package contains no part of the game. You need your own copy of the PAL disc as an .iso image.\n\n"
        "1. Extract the whole zip to a folder you can write to (for example Documents\\ReBruised; not Program Files).\n"
        "2. Start ReBruised.exe and select your disc image (GAME DISC). The launcher checks it is the PAL disc.\n"
        "3. The first time, ReBruised builds the game from your disc into BlackAndBruised.exe (a few minutes,\n"
        "   up to 10 on slower PCs; nothing is downloaded). Then press PLAY.\n\n"
        "Needs Windows 10 1903+ or 11 (64-bit), an SSE4.1 CPU, OpenGL 3.3 graphics, about 1 GB of free disk space.\n"
        "Settings: settings.ini (the launcher's OPTIONS). Logs: logs\\. The build: build-game\\build.log.\n"
        "Something wrong? The launcher's REPORT A PROBLEM page saves the logs to your Desktop.\n\n"
        "ReBruised is free software under the GNU GPL v3.0 (LICENSE.txt); its source is at the address above.\n"
        "Third-party licences: licenses\\. Not affiliated with Digital Fiction, Majesco or Sony.\n",
        encoding="utf-8")


# ---- the release gate ----
def distinctive(w):
    """A window worth indexing: varied bytes, mostly not text, not a ramp or a repeating pattern. Shared strings
    (newlib, SDK messages) and tables every program has would otherwise match unrelated libraries."""
    if len(set(w)) < 16:
        return False
    if sum(32 <= b < 127 for b in w) > len(w) // 2:
        return False
    return len({(w[i + 1] - w[i]) & 0xFF for i in range(len(w) - 1)}) > 4


def game_index():
    """Every distinctive 32-byte window (at every offset) of the game's ELF and IRX modules. A package file is read
    at a 4-byte stride: any copied run of 35+ bytes of the game lands on an indexed window."""
    sources = [ROOT / "game" / "disc" / "SLES_516.20"] + sorted((ROOT / "game" / "disc" / "MODULES").glob("*.IRX"))
    index = set()
    for src in sources:
        if not src.is_file():
            continue
        data = src.read_bytes()
        for off in range(0, len(data) - WINDOW + 1):
            w = data[off:off + WINDOW]
            if w not in index and distinctive(w):
                index.add(w)
    if not index:
        raise SystemExit("ERROR: the release gate needs game\\disc (the owner's copy) to check against")
    return index


def game_hits(data, index):
    hits = []
    for off in range(0, len(data) - WINDOW + 1, 4):
        w = data[off:off + WINDOW]
        if w in index:
            hits.append(off)
            if len(hits) > MAX_HITS:
                break
    return hits


def gate(stage):
    bad = []
    for f in stage.rglob("*"):
        rel = f.relative_to(stage)
        parts = [p.lower() for p in rel.parts]
        if any(p in BAD_PARTS for p in parts[:-1]) or (f.is_file() and (
                parts[-1] in BAD_NAMES or f.suffix.lower() in BAD_SUFFIXES)):
            bad.append(f"{rel}: name not allowed in a release")
    index = game_index()
    # Prebuilt third-party binaries (FFmpeg, the toolchain's C++ runtime) must equal their package's file; they are not
    # byte-scanned (their many generic tables would need a looser limit for everything else).
    upstream = {}
    for d in (PROGRAM / "ThirdParty" / "ffmpeg-prefix" / "src" / "ffmpeg_external" / "bin",
              Path(os.environ.get("BNB_TOOLCHAIN", DEFAULT_TC)) / "bin"):
        for f in d.glob("*.dll"):
            upstream.setdefault(f.name.lower(), f)
    ours, third = [], 0
    for f in stage.rglob("*"):
        if not f.is_file() or f.relative_to(stage).parts[0] in ("toolchain", "python"):
            continue
        up = upstream.get(f.name.lower()) if f.parent == stage else None
        if up is not None:
            if sha256(up) != sha256(f):
                bad.append(f"{f.relative_to(stage)}: differs from the upstream file {up}")
            third += 1
            continue
        ours.append(f)
    shared = 0
    for f in ours:
        data = f.read_bytes()
        hits = game_hits(data, index)
        if len(hits) > MAX_HITS:
            bad.append(f"{f.relative_to(stage)}: contains bytes of the game (at 0x{hits[0]:x}: {data[hits[0]:hits[0] + 32]!r})")
        shared += len(hits)
        rel = f.relative_to(stage).as_posix()
        if rel.startswith(("kit/obj/", "kit/lib/", "kit/bin/")) or rel == "ReBruised.exe":
            syms = sorted({m.group(0).decode() for m in GAME_SYMBOL.finditer(data)})
            if syms:
                bad.append(f"{rel}: generated-function symbols {syms[:5]}")
    # the toolchain and Python are third-party: only checked for game bytes in a cheap way (names above)
    if bad:
        for b in bad[:40]:
            say("  GATE: " + b)
        raise SystemExit(f"ERROR: release gate failed ({len(bad)} problems)")
    say(f"gate: {sum(1 for _ in stage.rglob('*') if _.is_file())} files, none from the game "
        f"({len(index)} game windows checked in {len(ours)} files; {shared} shared windows in all, under the limit; "
        f"{third} upstream DLLs verified by hash)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", default=str(ROOT / "dist"))
    ap.add_argument("--toolchain", default=os.environ.get("BNB_TOOLCHAIN", DEFAULT_TC))
    ap.add_argument("--no-zip", action="store_true")
    a = ap.parse_args()
    out = Path(a.out).resolve()
    stage = out / f"ReBruised-{a.version}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    stage_front(stage, a.version)
    stage_kit(stage)
    stage_python(stage)
    stage_toolchain(a.toolchain, stage)
    (stage / "kit" / "version.txt").write_text(a.version + "\n", encoding="utf-8")
    gate(stage)
    size = sum(f.stat().st_size for f in stage.rglob("*") if f.is_file())
    say(f"staged {stage} ({size / 1e6:.0f} MB)")
    if not a.no_zip:
        z = out / f"ReBruised-{a.version}.zip"
        with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for f in sorted(stage.rglob("*")):
                if f.is_file():
                    zf.write(f, f"ReBruised/{f.relative_to(stage).as_posix()}")
        say(f"zip {z} ({z.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
