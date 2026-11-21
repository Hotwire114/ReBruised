"""Merge Ghidra's export with the analyzer's SDK signature names into config/bnb.toml.

    python -I tools/make_config.py

Inputs (all produced from the player's own SLES_516.20, never shipped):
  config/ghidra_export.toml   ExportPS2Functions.java: function boundaries, jump tables, Ghidra-named stubs
  config/ghidra_functions.csv the function map ps2_recomp reads (ghidra_output)
  config/analyzer.toml        ps2_analyzer: Sony SDK functions matched from the embedded signature database
Output:
  config/bnb.toml             what ps2_recomp.exe runs on (relative paths, run from the project root)

Ghidra's names win where Ghidra recognised a library function itself; the analyzer's signature names fill the rest.
An address is a stub (runtime handler) only once; every other named address the analyzer found stays an entry point.
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG = ROOT / "config"
ITEM = re.compile(r'"([^"@]+)@0x([0-9A-Fa-f]+)"')


def list_block(text, key):
    """Return (items, span) of a top-level `key = [ ... ]` string list."""
    m = re.search(r'^' + key + r' = \[(.*?)^\]', text, flags=re.M | re.S)
    if not m:
        return [], None
    return [(n, int(a, 16)) for n, a in ITEM.findall(m.group(1))], m.span()


def fmt(key, items):
    body = "".join(f'  "{n}@0x{a:08X}",\n' for n, a in items)
    return f"{key} = [\n{body}]"


def main():
    ghidra = (CFG / "ghidra_export.toml").read_text(encoding="utf-8-sig")
    analyzer = (CFG / "analyzer.toml").read_text(encoding="utf-8-sig")
    g_stubs, _ = list_block(ghidra, "stubs")
    a_stubs, _ = list_block(analyzer, "stubs")
    a_entries, _ = list_block(analyzer, "entry_points")
    g_entries, _ = list_block(ghidra, "entry_points")

    guest = set()
    gi = CFG / "guest_impl.txt"
    if gi.is_file():
        guest = {l.split("#", 1)[0].strip() for l in gi.read_text(encoding="utf-8-sig").splitlines()} - {""}
    # A stub can only be a host handler if the prebuilt runner exports it (tools/gamedll/ps2_runtime_exports.def,
    # MSVC-mangled ?name@ps2_stubs@@... / ?name@ps2_syscalls@@...). Anything else runs as recompiled game code.
    exported = set()
    for line in (ROOT / "tools/gamedll/ps2_runtime_exports.def").read_text(encoding="utf-8-sig").splitlines():
        m = re.match(r'\s*\?(\w+)@(ps2_stubs|ps2_syscalls)@@', line)
        if m:
            exported.add(m.group(1))
    # BNB_FULL_RUNTIME=1: the program build (runtime compiled with the game) has every handler, no export filter.
    full = os.environ.get("BNB_FULL_RUNTIME") == "1"
    unexported = [] if full else sorted({n for n, _ in g_stubs + a_stubs if n not in exported and n not in guest})
    if unexported:
        print(f"{len(unexported)} stub names not exported by the runner (run as game code): {', '.join(unexported)}")
    guest |= set(unexported)
    alias = {}
    sa = CFG / "stub_alias.txt"
    for line in sa.read_text(encoding="utf-8-sig").splitlines() if sa.is_file() else []:
        parts = line.split("#", 1)[0].split()
        if len(parts) == 2:
            alias[int(parts[0], 0)] = parts[1]
    g_stubs = [(alias.get(a, n), a) for n, a in g_stubs]
    a_stubs = [(alias.get(a, n), a) for n, a in a_stubs]
    stubs = {}
    for n, a in g_stubs:
        if n not in guest:
            stubs.setdefault(a, n)
    added = 0
    for n, a in a_stubs:
        if a not in stubs and n not in guest:
            stubs[a] = n
            added += 1
    # stub_alias.txt entries at addresses neither list named: add them (functions the scan did not match at all)
    for a, n in alias.items():
        if a not in stubs and n not in guest and (full or n in exported):
            stubs[a] = n
            added += 1
    entries = {}
    for n, a in g_entries + a_entries:
        if a not in stubs:
            entries.setdefault(a, n)

    # Function map without the ranges listed in config/not_code.txt (data Ghidra took for code).
    not_code = set()
    nc = CFG / "not_code.txt"
    if nc.is_file():
        for line in nc.read_text(encoding="utf-8-sig").splitlines():
            value = line.split("#", 1)[0].strip()
            if value:
                not_code.add(int(value, 0))
    fixes = {}
    fr = CFG / "fix_ranges.txt"
    if fr.is_file():
        for line in fr.read_text(encoding="utf-8-sig").splitlines():
            parts = line.split("#", 1)[0].split()
            if len(parts) == 2:
                fixes[int(parts[0], 0)] = int(parts[1], 0)
    rows = (CFG / "ghidra_functions.csv").read_text(encoding="utf-8-sig").splitlines()
    kept = [rows[0]]
    for r in rows[1:]:
        f = r.split(",")
        start = int(f[1], 16)
        if start in not_code:
            continue
        if start in fixes:
            f[2], f[3] = f"0x{fixes[start]:08X}", str(fixes[start] - start)
            r = ",".join(f)
        kept.append(r)
    dropped = len(rows) - len(kept)
    extra = CFG / "extra_functions.csv"
    if extra.is_file():
        kept += extra.read_text(encoding="utf-8-sig").splitlines()[1:]
    (CFG / "function_map.csv").write_text("\n".join(kept) + "\n", encoding="utf-8")

    out = ghidra
    out = re.sub(r'^input = .*$', 'input = "game/disc/SLES_516.20"', out, flags=re.M)
    out = re.sub(r'^output = .*$', 'output = "generated/"', out, flags=re.M)
    out = re.sub(r'^ghidra_output = .*$', 'ghidra_output = "config/function_map.csv"', out, flags=re.M)
    _, span = list_block(out, "stubs")
    if span is None:
        sys.exit("ghidra_export.toml has no stubs list")
    out = out[:span[0]] + fmt("stubs", [(n, a) for a, n in sorted(stubs.items())]) + out[span[1]:]
    _, span = list_block(out, "entry_points")
    block = fmt("entry_points", [(n, a) for a, n in sorted(entries.items())])
    if span is None:
        # insert after the stubs list
        _, sspan = list_block(out, "stubs")
        out = out[:sspan[1]] + "\n\n" + block + out[sspan[1]:]
    else:
        out = out[:span[0]] + block + out[span[1]:]
    # ExportPS2Functions.java writes no jump tables; the analyzer's detected tables are carried over.
    tables = 0
    if "[jump_tables]" not in out:
        m = re.search(r'^\[jump_tables\].*?(?=^\[(?!\[)|\Z)', analyzer, flags=re.M | re.S)
        if m:
            out = out.rstrip() + "\n\n" + m.group(0).rstrip() + "\n"
            tables = m.group(0).count("[[jump_tables.table]]")
    (CFG / "bnb.toml").write_text(out, encoding="utf-8")
    print(f"stubs: {len(g_stubs)} from Ghidra + {added} SDK signatures = {len(stubs)}; entry points: {len(entries)}; "
          f"jump tables from analyzer: {tables}; map rows dropped as data: {dropped}")


if __name__ == "__main__":
    main()
