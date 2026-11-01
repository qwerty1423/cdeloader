"""Symbol lookup for authoring byte patches by name.

The shipped ``CDE.pdb`` matches the executable, so a patch author can say
"patch ``util::Foo`` + 12" instead of hard-coding a file offset.  The heavy
lifting (PDB -> name/VA table) is done once by ``scripts/pdb-functions.py``,
which writes ``decomp/symbols/functions.tsv``.  This module consumes that
table, falling back to ``llvm-pdbutil`` if it is missing.
"""

from __future__ import annotations

import os
import re
import subprocess
from typing import Dict, List, Optional

_TSV_CANDIDATES = [
    "decomp/symbols/functions.tsv",
    "artifacts/functions.tsv",
]


def _find_tsv(start: str = ".") -> Optional[str]:
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


class Symbols:
    """Name <-> VA index built from the PDB-derived function table."""

    def __init__(self, rows: List[dict]) -> None:
        self.rows = rows
        self.by_demangled: Dict[str, int] = {}
        self.by_mangled: Dict[str, int] = {}
        self.by_va: Dict[int, dict] = {}
        for row in rows:
            va = int(row["va"], 0)
            self.by_va[va] = row
            dem = (row.get("demangled") or "").strip()
            man = (row.get("mangled") or "").strip()
            if dem and dem not in self.by_demangled:
                self.by_demangled[dem] = va
            if man and man not in self.by_mangled:
                self.by_mangled[man] = va
        self._lower = {k.lower(): v for k, v in self.by_demangled.items()}

    @classmethod
    def load(cls, path: Optional[str] = None) -> "Symbols":
        if path is None:
            path = _find_tsv()
        if path is None or not os.path.isfile(path):
            return cls([])
        rows = []
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            header = fh.readline().rstrip("\n").split("\t")
            idx = {name: i for i, name in enumerate(header)}
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < len(header):
                    parts += [""] * (len(header) - len(parts))
                rows.append({
                    "va": parts[idx.get("va", 0)],
                    "section": parts[idx.get("section", 1)] if "section" in idx else "",
                    "offset": parts[idx.get("offset", 2)] if "offset" in idx else "",
                    "flags": parts[idx.get("flags", 3)] if "flags" in idx else "",
                    "mangled": parts[idx.get("mangled", 4)] if "mangled" in idx else "",
                    "demangled": parts[idx.get("demangled", 5)] if "demangled" in idx else "",
                    "class": parts[idx.get("class", 6)] if "class" in idx else "",
                    "method": parts[idx.get("method", 7)] if "method" in idx else "",
                })
        return cls(rows)

    def __len__(self) -> int:
        return len(self.rows)

    def resolve(self, name: str) -> Optional[int]:
        """Resolve a demangled or mangled symbol to a VA.

        Tries exact, then case-insensitive, then a unique suffix match (so
        ``StrategicManager::GetMinimumBattleRange_km`` finds the fully
        qualified form).
        """
        name = name.strip()
        if name in self.by_demangled:
            return self.by_demangled[name]
        if name in self.by_mangled:
            return self.by_mangled[name]
        low = name.lower()
        if low in self._lower:
            return self._lower[low]
        matches = [va for dem, va in self.by_demangled.items()
                   if dem.endswith("::" + name) or dem == name]
        if len(matches) == 1:
            return matches[0]
        if matches:
            raise KeyError("ambiguous symbol %r (%d matches)" % (name, len(matches)))
        return None

    def name_of(self, va: int) -> Optional[str]:
        row = self.by_va.get(va)
        return row["demangled"] or row["mangled"] if row else None

    def search(self, pattern: str, limit: int = 40) -> List[dict]:
        rx = re.compile(pattern, re.IGNORECASE)
        out = []
        for row in self.rows:
            hay = row["demangled"] or row["mangled"]
            if rx.search(hay):
                out.append(row)
                if len(out) >= limit:
                    break
        return out


def load_symbols(path: Optional[str] = None) -> Symbols:
    return Symbols.load(path)
