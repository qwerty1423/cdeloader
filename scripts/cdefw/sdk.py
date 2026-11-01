"""Generate the mod loader's symbol table from the PDB-derived function table.

Inputs
------
``decomp/symbols/functions.tsv`` (built by ``scripts/pdb-functions.py``), with a
header and one row per function::

    va  section  offset  flags  mangled  demangled  class  method

Output
------
A ``cdeloader.symbols`` text file the loader parses at startup::

    # cdeloader.symbols v1 sha256=<exe sha256> base=0x00400000
    0x004d0e70<TAB>util::NormalizeAngle<double><TAB>?NormalizeAngle@...

The first column is a VA (image base 0x400000); the second a name plugins may
pass to ``resolve_symbol`` — the friendly ``Class::Method`` first, then the full
demangled signature when it differs — and the third the mangled name.  Rows are
sorted by VA so the output is reproducible for a given input.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from typing import List, Optional, Tuple

IMAGE_BASE = 0x400000
_TSV_CANDIDATES = [
    "decomp/symbols/functions.tsv",
    "artifacts/functions.tsv",
]


def find_repo_root(start: str = ".") -> str:
    here = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(here, "scripts")) and os.path.isdir(os.path.join(here, "docs")):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            return os.path.abspath(start)
        here = parent


def find_functions_tsv(start: str = ".") -> Optional[str]:
    here = os.path.abspath(start)
    while True:
        for rel in _TSV_CANDIDATES:
            path = os.path.join(here, rel)
            if os.path.isfile(path):
                return path
        parent = os.path.dirname(here)
        if parent == here:
            return None
        here = parent


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _cell(parts: List[str], idx: dict, key: str, default: str = "") -> str:
    i = idx.get(key)
    return parts[i] if i is not None and i < len(parts) else default


def load_rows(tsv_path: str) -> List[Tuple[int, List[str], str]]:
    """Return ``(va, names, mangled)`` per symbol.

    ``names`` are the keys plugins may pass to ``resolve_symbol``: the friendly
    ``Class::Method`` first (what authors actually type), then the full demangled
    signature if it differs.  The mangled name is kept separately.
    """
    rows: List[Tuple[int, List[str], str]] = []
    with open(tsv_path, "r", encoding="utf-8", errors="replace") as fh:
        header = fh.readline().rstrip("\n").split("\t")
        idx = {name: i for i, name in enumerate(header)}
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            try:
                va = int(_cell(parts, idx, "va", "0") or "0", 0)
            except ValueError:
                continue
            dem = _cell(parts, idx, "demangled")
            man = _cell(parts, idx, "mangled")
            cls = _cell(parts, idx, "class")
            meth = _cell(parts, idx, "method")

            names: List[str] = []
            if cls and meth and cls not in ("(global)", "(unknown)", ""):
                names.append(cls + "::" + meth)
            if dem and dem not in names:
                names.append(dem)
            if not names and man:
                names.append(man)
            if not va or not names:
                continue
            rows.append((va, names, man))
    rows.sort(key=lambda r: (r[0], r[1][0]))
    return rows


def build(tsv_path: Optional[str] = None, out_path: str = "cdeloader.symbols",
          exe_path: Optional[str] = None, pdb_path: Optional[str] = None,
          repo_root: Optional[str] = None) -> Tuple[str, int]:
    """Write ``out_path``; return (sha256 used, row count)."""
    root = repo_root or find_repo_root()
    if tsv_path is None:
        tsv_path = find_functions_tsv()
    if tsv_path is None or not os.path.isfile(tsv_path):
        if pdb_path is None:
            pdb_path = os.path.join(root, "artifacts", "CDE.pdb")
        gen = os.path.join(root, "scripts", "pdb-functions.py")
        if not os.path.isfile(pdb_path) or not os.path.isfile(gen):
            raise FileNotFoundError(
                "no functions.tsv and no PDB to build one from "
                "(run scripts/pdb-functions.py first)")
        subprocess.run([sys.executable, gen, pdb_path], check=True, cwd=root)
        tsv_path = find_functions_tsv(root)

    rows = load_rows(tsv_path)

    sha = ""
    if exe_path is None:
        default_exe = os.path.join(root, "artifacts", "CDE.exe.original")
        exe_path = default_exe if os.path.isfile(default_exe) else None
    if exe_path and os.path.isfile(exe_path):
        sha = sha256_file(exe_path)

    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# cdeloader.symbols v1 sha256=%s base=0x%08x\n" % (sha or "?", IMAGE_BASE))
        for va, names, man in rows:
            for name in names:
                fh.write("0x%08x\t%s\t%s\n" % (va, name, man))
    return sha, len(rows)
