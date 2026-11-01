"""Mod linter ("Mod Doctor"): follow the game's load order and report problems.

The checks mirror the community Mod Doctor (see ``README-ModDoctor.txt``) and
the rules in ``MODDING.md``:

* format errors (spaces instead of tabs, a missing blank line between entries);
* duplicate entries inside one file;
* unknown entry types / references to entries that don't exist;
* overrides placed in the wrong file (so the stock file replaces them again);
* overrides that drop properties of the stock entry;
* conflicts: the same entry defined by several mods (later one wins).

Which properties are references is *learned* from the stock data (a property
whose stock values name other entries).  With no stock data supplied, only the
format checks run.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from . import datum as dm
from .datum import Datum, Problem


@dataclass
class Entry:
    datum: Datum
    path: str          # absolute path
    reldir: str        # path under Data/, e.g. "Materials/Minerals.txt"
    source: str        # "stock" | "local" | "workshop:<id>"


@dataclass
class Index:
    entries: Dict[Tuple[str, str], Entry] = field(default_factory=dict)
    types: Set[str] = field(default_factory=set)
    name_types: Dict[str, Set[str]] = field(default_factory=dict)
    ref_props: Dict[str, Set[str]] = field(default_factory=dict)  # prop -> types

    def add(self, entry: Entry) -> None:
        d = entry.datum
        self.types.add(d.type)
        self.name_types.setdefault(d.name, set()).add(d.type)
        # Later entries win, matching the game.
        self.entries[(d.type, d.name)] = entry

    def finish_references(self) -> None:
        names = set(self.name_types)
        for entry in self.entries.values():
            for prop in entry.datum.properties:
                for value in prop.values:
                    if value in names:
                        self.ref_props.setdefault(prop.key, set()).update(
                            self.name_types[value])


def _reldir(path: str, data_root: str) -> str:
    try:
        rel = os.path.relpath(path, data_root)
    except ValueError:
        rel = path
    return rel.replace("\\", "/")


def index_directory(root: str, source: str, subdir: str = "Data") -> Tuple[Index, List[Problem]]:
    """Index every datum under ``root/<subdir>``."""
    index = Index()
    problems: List[Problem] = []
    base = os.path.join(root, subdir)
    if not os.path.isdir(base):
        return index, problems
    for dirpath, _dirs, files in os.walk(base):
        for fn in sorted(files):
            if not fn.lower().endswith(".txt"):
                continue
            path = os.path.join(dirpath, fn)
            datums, probs = dm.parse_file(path)
            problems.extend(probs)
            reldir = _reldir(path, base)
            for d in datums:
                index.add(Entry(d, path, reldir, source))
    index.finish_references()
    return index, problems


def _format_checks(datums: List[Datum], path: str, problems: List[Problem]) -> None:
    seen: Set[Tuple[str, str]] = set()
    for d in datums:
        if d.key in seen:
            problems.append(Problem("warning",
                                    "duplicate entry %s %r in one file" % (d.type, d.name),
                                    path, d.line))
        seen.add(d.key)


def lint_data_dir(root: str, stock: Optional[Index], known_names: Optional[Set[str]],
                  source: str = "mod", subdir: str = "Data") -> List[Problem]:
    """Lint one mod directory; ``stock`` enables the content checks."""
    problems: List[Problem] = []
    base = os.path.join(root, subdir)
    if not os.path.isdir(base):
        problems.append(Problem("error", "no %s folder in %s" % (subdir, root), root))
        return problems

    all_names = set(known_names or ())
    data_root = base

    for dirpath, _dirs, files in os.walk(base):
        for fn in sorted(files):
            if not fn.lower().endswith(".txt"):
                continue
            path = os.path.join(dirpath, fn)
            datums, probs = dm.parse_file(path)
            problems.extend(probs)
            _format_checks(datums, path, problems)
            reldir = _reldir(path, data_root)
            if stock is None:
                continue
            for d in datums:
                key = d.key
                if d.type not in stock.types:
                    problems.append(Problem("warning",
                                            "unknown entry type %r" % d.type, path, d.line))
                existing = stock.entries.get(key)
                if existing is not None:
                    if existing.reldir.lower() != reldir.lower():
                        problems.append(Problem(
                            "warning",
                            "override %s %r is in %s but the stock entry is in %s; "
                            "the stock file loads later and replaces it" %
                            (d.type, d.name, reldir, existing.reldir), path, d.line))
                    stock_props = set(existing.datum.prop_keys)
                    mod_props = set(d.prop_keys)
                    dropped = sorted(stock_props - mod_props)
                    if dropped:
                        problems.append(Problem(
                            "warning",
                            "override %s %r drops stock properties: %s" %
                            (d.type, d.name, ", ".join(dropped)), path, d.line))
                for prop in d.properties:
                    referenced = stock.ref_props.get(prop.key)
                    if not referenced:
                        continue
                    for value in prop.values:
                        if value in ("null", "") or value in all_names:
                            continue
                        if _is_literal(value):
                            continue
                        problems.append(Problem(
                            "warning",
                            "reference %s %r -> %r: no such entry" %
                            (d.type, d.name, value), path, prop.line))
    return problems


def _is_literal(value: str) -> bool:
    low = value.lower()
    if low in ("true", "false", "null"):
        return True
    try:
        float(value)
        return True
    except ValueError:
        return False


def lint_mods(mod_dirs: List[Tuple[str, str]], stock: Optional[Index],
              subdir: str = "Data") -> List[Problem]:
    """Lint several mod directories; report cross-mod conflicts too."""
    problems: List[Problem] = []
    known_names: Set[str] = set(stock.name_types) if stock else set()
    owners: Dict[Tuple[str, str], str] = {}
    for root, source in mod_dirs:
        # Collect names first so references to entries defined by another mod
        # are not flagged.
        datums, _p = dm.parse_dir(root, subdir)
        for d in datums:
            known_names.add(d.name)
    for root, source in mod_dirs:
        problems.extend(lint_data_dir(root, stock, known_names, source, subdir))
        datums, _p = dm.parse_dir(root, subdir)
        for d in datums:
            prev = owners.get(d.key)
            if prev is not None and prev != source:
                problems.append(Problem(
                    "warning",
                    "entry %s %r is defined by both %s and %s; %s wins" %
                    (d.type, d.name, prev, source, source)))
            owners[d.key] = source
    return problems


def format_report(problems: List[Problem]) -> str:
    if not problems:
        return "No problems found.\n"
    lines = ["%d problem(s):" % len(problems), ""]
    for p in problems:
        lines.append(str(p))
    lines.append("")
    return "\n".join(lines)
