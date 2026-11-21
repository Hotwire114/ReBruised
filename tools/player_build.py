#!/usr/bin/env python3
"""Build Black & Bruised on the player's PC from their own disc (the launcher runs this; nothing from the game ships).

    python -I kit/tools/player_build.py --root <ReBruised folder> --elf <SLES_516.20 taken from the player's disc>
                                        [--jobs N]

The release (tools/make_release.py) carries the runtime already compiled (kit/lib, kit/obj: no game code), the
recompiler (kit/bin/ps2_recomp.exe), the recompiler's config (function addresses and names, no code), llvm-mingw
(toolchain/) and Python. Here, from the player's ELF:

  1. ps2_recomp.exe turns the game's MIPS code into C++            -> build-game/generated
  2. tools/patch_generated.py, tools/make_batches.py                -> build-game/batches
  3. the VU microcode is cut out of the ELF (config/vucode_recipe.json: offsets only) and lifted to C++
     (tools/vu/vu_lift.py)                                          -> build-game/vulift
  4. clang++ compiles that code, lld links it with the kit          -> BlackAndBruised.exe next to the launcher

build-game/progress.txt holds "<percent>\\n<text>" (or "error\\n<text>") for the launcher's progress bar;
build-game/built_id.txt records the kit it was built with (kit/build_id.txt), so a new release rebuilds.
An object whose source and flags did not change is kept (a second build compiles nothing).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

KIT = Path(__file__).resolve().parents[1]
NO_WINDOW = 0x08000000 if os.name == "nt" else 0
JOB_MB = 1500          # one clang++ -O1 on a 1.5 MB batch of generated code (measured ~0.6-1.2 GB)


class BuildError(Exception):
    pass


class Progress:
    def __init__(self, path):
        self.path = path

    def set(self, pct, text):
        print(f"[{pct:3d}%] {text}", flush=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(f"{pct}\n{text}\n", encoding="utf-8")
        os.replace(tmp, self.path)

    def error(self, text):
        print("ERROR: " + text, flush=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(f"error\n{text}\n", encoding="utf-8")
        os.replace(tmp, self.path)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def run(cmd, cwd=None, env=None, what=""):
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, capture_output=True, text=True, errors="replace",
                       creationflags=NO_WINDOW)
    if r.stdout:
        print(r.stdout.rstrip()[-4000:], flush=True)
    if r.returncode != 0:
        print(r.stderr.rstrip()[-8000:], flush=True)
        raise BuildError(f"{what} failed (exit {r.returncode}); see build-game\\build.log")
    return r


def free_mb():
    if os.name != "nt":
        return 8192
    import ctypes

    class MemStatus(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

    m = MemStatus()
    m.dwLength = ctypes.sizeof(m)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.ullAvailPhys // (1024 * 1024)


def recompile(elf, build, env, prog):
    prog.set(2, "Recompiling the game's code from your disc")
    gen = build / "generated"
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    # ps2_recomp opens files through the C runtime's narrow (ANSI code page) paths: a folder name outside that code
    # page (a Windows user name such as "JÃ¶hn" or "Ð˜Ð²Ð°Ð½" holds the disc files) cannot be opened. So it gets only
    # relative ASCII names, run from build-game with its inputs copied there.
    shutil.copyfile(elf, build / "SLES_516.20")
    shutil.copyfile(KIT / "config" / "function_map.csv", build / "function_map.csv")
    toml = (KIT / "config" / "bnb.toml").read_text(encoding="utf-8")
    lines = []
    for line in toml.splitlines():
        key = line.split("=", 1)[0].strip()
        if key == "input":
            line = 'input = "SLES_516.20"'
        elif key == "output":
            line = 'output = "generated/"'
        elif key == "ghidra_output":
            line = 'ghidra_output = "function_map.csv"'
        lines.append(line)
    cfg = build / "bnb.toml"
    cfg.write_text("\n".join(lines) + "\n", encoding="utf-8")
    t = time.time()
    run([KIT / "bin" / "ps2_recomp.exe", "bnb.toml"], cwd=build, env=env, what="The recompiler")
    count = sum(1 for _ in gen.glob("*.cpp"))
    if count < 1000:
        raise BuildError(f"The recompiler wrote only {count} files")
    print(f"recompiled {count} files in {time.time() - t:.0f} s", flush=True)
    py = [sys.executable, "-I", "-X", "utf8"]   # UTF-8 output: paths outside the ANSI code page
    genv = dict(env, BNB_GEN_DIR=str(gen), BNB_BATCH_DIR=str(build / "batches"))
    prog.set(14, "Applying ReBruised's fixes")
    run(py + [KIT / "tools" / "patch_generated.py"], env=genv, what="patch_generated")
    run(py + [KIT / "tools" / "make_batches.py"], env=genv, what="make_batches")


def lift_vu(elf_bytes, build, env, prog):
    prog.set(17, "Translating the VU programs")
    recipe = json.loads((KIT / "config" / "vucode_recipe.json").read_text(encoding="utf-8"))
    vucode = build / "vucode"
    if vucode.exists():
        shutil.rmtree(vucode)
    for name, img in recipe["images"].items():
        data = bytearray(img["size"])
        for dst, src, n in img["segments"]:
            data[dst:dst + n] = elf_bytes[src:src + n]
        if sha256_bytes(bytes(data)) != img["sha256"]:
            raise BuildError("VU microcode from the disc does not match (" + name + ")")
        out = vucode / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(bytes(data))
    lift = build / "vulift"
    if lift.exists():
        shutil.rmtree(lift)
    cmd = [sys.executable, "-I", "-X", "utf8", KIT / "tools" / "vu" / "vu_lift.py", lift, "--vu1", vucode / "vu1"]
    if (vucode / "vu0").is_dir():
        cmd += ["--vu0", vucode / "vu0"]
    cmd += ["--parts", "4", "--heads", KIT / "config" / "vulift_heads.txt", "--report", build / "vulift_report.txt"]
    run(cmd, env=env, what="The VU translator")


def source_stamp(src, flags):
    h = hashlib.sha256("\0".join(flags).encode("utf-8"))
    text = src.read_bytes()
    h.update(text)
    for line in text.decode("utf-8", "replace").splitlines():   # a batch is a list of #include "<generated file>"
        if line.startswith('#include "'):
            inc = Path(line[10:-1])
            if inc.is_file():
                h.update(inc.read_bytes())
    return h.hexdigest()


def compile_all(root, build, kitinfo, build_id, jobs, env, prog):
    tc = root / "toolchain" / "bin"
    cxx = tc / "clang++.exe"
    inc = []
    for d in kitinfo["include"]:
        inc += ["-I", str(KIT / d)]
    inc += ["-I", str(build / "generated")]
    base = kitinfo["defines"] + inc + kitinfo["cxxflags"]
    jobs_src = [(p, base + kitinfo["generated_flags"]) for p in sorted((build / "batches").glob("batch_*.cpp"))]
    jobs_src += [(p, base) for p in sorted((build / "vulift").glob("vulift_part*.cpp"))]
    objdir = build / "obj"
    objdir.mkdir(exist_ok=True)
    wanted = {p.stem + ".o" for p, _ in jobs_src}
    for stale in objdir.glob("*.o"):
        if stale.name not in wanted:
            stale.unlink()
    todo, objs = [], []
    for src, flags in jobs_src:
        obj = objdir / (src.stem + ".o")
        stamp = source_stamp(src, flags + [build_id])
        objs.append(obj)
        sfile = obj.with_suffix(".stamp")
        if obj.is_file() and sfile.is_file() and sfile.read_text(encoding="utf-8") == stamp:
            continue
        todo.append((src, flags, obj, stamp))
    if jobs <= 0:
        jobs = max(1, min(os.cpu_count() or 1, int(max(0, free_mb() - 1024) // JOB_MB)))
    print(f"compiling {len(todo)} of {len(jobs_src)} sources with {jobs} jobs", flush=True)
    done = 0
    t0 = time.time()

    def one(item):
        src, flags, obj, stamp = item
        tmp = obj.with_suffix(".o.tmp")
        r = subprocess.run([str(cxx)] + flags + ["-c", str(src), "-o", str(tmp)], capture_output=True, text=True,
                           errors="replace", env=env, creationflags=NO_WINDOW)
        if r.returncode != 0:
            raise BuildError(f"compiling {src.name} failed:\n{r.stderr[-4000:]}")
        os.replace(tmp, obj)
        obj.with_suffix(".stamp").write_text(stamp, encoding="utf-8")
        return src.name

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(one, item) for item in todo]
        for f in concurrent.futures.as_completed(futures):
            name = f.result()
            done += 1
            left = ""
            if done >= 3:
                rest = (time.time() - t0) / done * (len(todo) - done)
                left = f" - about {max(1, round(rest / 60))} min left" if rest > 45 else " - almost done"
            prog.set(20 + 75 * done // max(1, len(todo)), f"Compiling {done} of {len(todo)}{left}")
            print(f"[{done}/{len(todo)}] {name}", flush=True)

    prog.set(96, "Linking")
    exe = root / "BlackAndBruised.exe"
    tmp = root / "BlackAndBruised.exe.new"
    link = ([str(cxx)] + kitinfo["linkflags"] + [str(KIT / o) for o in kitinfo["objs"]] + [str(o) for o in objs] +
            [str(KIT / l) if l.startswith("lib/") else l for l in kitinfo["libs"]] + ["-o", str(tmp)])
    rsp = build / "link.rsp"
    rsp.write_text(" ".join('"' + a.replace("\\", "/") + '"' for a in link[1:]), encoding="utf-8")
    run([cxx, "@" + str(rsp)], env=env, what="Linking")
    os.replace(tmp, exe)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--jobs", type=int, default=0)
    a = ap.parse_args()
    for stream in (sys.stdout, sys.stderr):   # the log is a file: any path (e.g. "Ð˜Ð²Ð°Ð½") must print
        stream.reconfigure(encoding="utf-8", errors="replace")
    root = Path(a.root).resolve()
    build = root / "build-game"
    build.mkdir(parents=True, exist_ok=True)
    prog = Progress(build / "progress.txt")
    try:
        prog.set(0, "Checking your disc")
        kitinfo = json.loads((KIT / "kit.json").read_text(encoding="utf-8"))
        build_id = (KIT / "build_id.txt").read_text(encoding="utf-8").strip()
        elf = Path(a.elf).resolve()
        if not elf.is_file():
            raise BuildError("The game's program from your disc is missing: select your disc image again (GAME DISC).")
        elf_bytes = elf.read_bytes()
        if sha256_bytes(elf_bytes) != kitinfo["elf_sha256"]:
            raise BuildError("This disc's program is not the PAL Black & Bruised (SLES-51620) this release supports.")
        free_gb = shutil.disk_usage(build).free / 1e9
        if free_gb < 1.0:   # build-game ~0.2 GB + the exe ~60 MB, plus headroom
            raise BuildError(f"Not enough free disk space: {free_gb:.1f} GB free on this drive, the build needs about 1 GB.")
        (build / "built_id.txt").unlink(missing_ok=True)
        env = dict(os.environ)
        env["PATH"] = str(root / "toolchain" / "bin") + os.pathsep + env.get("PATH", "")
        t = time.time()
        recompile(elf, build, env, prog)
        lift_vu(elf_bytes, build, env, prog)
        compile_all(root, build, kitinfo, build_id, a.jobs, env, prog)
        (build / "built_id.txt").write_text(build_id + "\n", encoding="utf-8")
        prog.set(100, f"Done in {max(1, round((time.time() - t) / 60))} min")
        return 0
    except BuildError as e:
        prog.error(str(e))
        return 1
    except Exception as e:   # anything unexpected still ends the progress bar with a message
        prog.error(f"{type(e).__name__}: {e}")
        raise


if __name__ == "__main__":
    sys.exit(main())
