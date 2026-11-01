#!/usr/bin/env python3
"""Build a function/name address table from the matched PDB, and rank classes.

Read-only. Uses `llvm-pdbutil` (read the PDB) and `llvm-undname` (demangle
MSVC names). Writes a TSV to decomp/symbols/ and prints a class ranking, to help
pick a self-contained reconstruction target.

VA = image_base + section_rva + offset, where publics store `section:offset`.

Usage: scripts/pdb-functions.py [PDB] [--all]
  default: only S_PUB32 records with `flags = function`
  --all:   every S_PUB32 record (code + data)
"""
import os
import re
import subprocess
import sys
import collections

IMAGE_BASE = 0x400000
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(cmd, data=None):
    p = subprocess.run(cmd, input=data, capture_output=True, text=True)
    if p.returncode != 0 and not p.stdout:
        sys.stderr.write(p.stderr[:500] + "\n")
    return p.stdout


def section_rvas(pdb):
    rvas, cur = {}, None
    for ln in run(["llvm-pdbutil", "dump", "-section-headers", pdb]).splitlines():
        m = re.match(r"\s*SECTION HEADER #(\d+)", ln)
        if m:
            cur = int(m.group(1))
            continue
        m = re.match(r"\s*([0-9A-Fa-f]+)\s+virtual address", ln)
        if m and cur is not None:
            rvas[cur] = int(m.group(1), 16)
    return rvas


def publics(pdb):
    lines = run(["llvm-pdbutil", "dump", "-publics", pdb]).splitlines()
    recs = []
    for i, ln in enumerate(lines):
        m = re.search(r"S_PUB32 \[size = \d+\] `(.*)`\s*$", ln)
        if not m:
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        a = re.search(r"flags = (.*?), addr = ([0-9A-Fa-f]+):([0-9A-Fa-f]+)", nxt)
        if not a:
            continue
        flags = a.group(1).strip()
        sec = int(a.group(2), 16)
        off = int(a.group(3), 10)  # llvm-pdbutil prints the section offset in decimal
        recs.append((flags, sec, off, m.group(1)))
    return recs


def demangle(names):
    res = {}
    B = 2000
    uniq = list(dict.fromkeys(names))
    for i in range(0, len(uniq), B):
        chunk = uniq[i:i + B]
        out = run(["llvm-undname"], data="\n".join(chunk) + "\n").splitlines()
        j = 0
        while j < len(out):
            if out[j].startswith("?") and j + 1 < len(out):
                res[out[j]] = out[j + 1]
                j += 2
            else:
                j += 1
    return res


def class_of(demangled):
    head = demangled.split("(", 1)[0]
    head = re.sub(r"^(public|private|protected):\s*", "", head).strip()
    head = re.sub(r"^(virtual|static|inline)\s+", "", head).strip()
    if "::" not in head:
        return "(global)", head
    cls, meth = head.rsplit("::", 1)
    cls = cls.split()[-1] if " " in cls else cls
    return cls, meth


def main(argv):
    pdb = os.path.join(ROOT, "artifacts", "CDE.pdb")
    include_all = False
    for a in argv[1:]:
        if a == "--all":
            include_all = True
        elif not a.startswith("-"):
            pdb = a
    if not os.path.isfile(pdb):
        raise SystemExit(f"PDB not found: {pdb}")

    rvas = section_rvas(pdb)
    recs = publics(pdb)
    funcs = [r for r in recs if include_all or r[0] == "function"]
    names = [r[3] for r in funcs]
    dem = demangle(names)

    rows = []
    for flags, sec, off, name in funcs:
        va = IMAGE_BASE + rvas.get(sec, 0) + off
        d = dem.get(name, "")
        cls, meth = class_of(d) if d else ("(unknown)", "")
        rows.append((va, sec, off, flags, name, d, cls, meth))
    rows.sort()

    outdir = os.path.join(ROOT, "decomp", "symbols")
    os.makedirs(outdir, exist_ok=True)
    outpath = os.path.join(outdir, "functions.tsv")
    with open(outpath, "w") as f:
        f.write("va\tsection\toffset\tflags\tmangled\tdemangled\tclass\tmethod\n")
        for va, sec, off, flags, name, d, cls, meth in rows:
            f.write(f"0x{va:08x}\t{sec}\t0x{off:x}\t{flags}\t{name}\t{d}\t{cls}\t{meth}\n")

    byclass = collections.Counter(r[6] for r in rows)
    print(f"records: {len(recs)}  functions: {len(rows)}  -> {outpath}")
    print(f"distinct classes: {len(byclass)}")
    print("\nclasses with 1-6 functions (candidate small targets):")
    small = [(c, n) for c, n in byclass.items() if 1 <= n <= 6 and c not in ("(global)", "(unknown)")]
    for c, n in sorted(small, key=lambda x: x[0])[:60]:
        print(f"  {n:>2}  {c}")


if __name__ == "__main__":
    main(sys.argv)
