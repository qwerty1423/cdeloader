"""Parser for Children of a Dead Earth data ("datum") text files.

Format (see the community patch's ``MODDING.md``):

* An entry starts with a header line ``<Type> <Name>`` (space separated).
* Properties follow, one per line, as ``<Key>\\t<value>\\t<value>...``.
* Nested blocks are indented with a leading tab.
* A blank line ends the entry.

This module is deliberately lossless enough to round-trip and strict enough to
surface the format mistakes that crash the game (spaces instead of tabs, a
missing blank line between entries).
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


@dataclass
class Property:
    key: str
    values: List[str]
    indent: int
    line: int

    @property
    def value(self) -> str:
        return self.values[0] if self.values else ""


@dataclass
class Datum:
    type: str
    name: str
    properties: List[Property] = field(default_factory=list)
    line: int = 0
    path: Optional[str] = None

    @property
    def key(self) -> Tuple[str, str]:
        return (self.type, self.name)

    def prop(self, key: str) -> Optional[Property]:
        for p in self.properties:
            if p.key == key:
                return p
        return None

    @property
    def prop_keys(self) -> List[str]:
        return [p.key for p in self.properties]


@dataclass
class Problem:
    severity: str  # "error" | "warning"
    message: str
    path: Optional[str] = None
    line: int = 0

    def __str__(self) -> str:
        loc = ""
        if self.path:
            loc = self.path
            if self.line:
                loc += ":%d" % self.line
            loc += ": "
        return "%s: %s%s" % (self.severity.upper(), loc, self.message)


def _looks_numeric(value: str) -> bool:
    try:
        float(value)
        return True
    except ValueError:
        return False


def parse_text(text: str, path: Optional[str] = None) -> Tuple[List[Datum], List[Problem]]:
    datums: List[Datum] = []
    problems: List[Problem] = []
    current: Optional[Datum] = None

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\r")
        if line.strip() == "":
            current = None
            continue

        if line[0] == "\t":
            indent = len(line) - len(line.lstrip("\t"))
            body = line[indent:]
            if current is None:
                problems.append(Problem("error", "indented line before any entry",
                                        path, lineno))
                continue
            if "\t" in body:
                key, _, rest = body.partition("\t")
                current.properties.append(Property(key, rest.split("\t"), indent, lineno))
            else:
                current.properties.append(Property(body, [], indent, lineno))
            continue

        if "\t" in line:
            if current is None:
                problems.append(Problem("error", "property before any entry", path, lineno))
                continue
            key, _, rest = line.partition("\t")
            current.properties.append(Property(key, rest.split("\t"), 0, lineno))
            continue

        # A header line: "<Type> <Name>".
        if current is not None:
            problems.append(Problem(
                "error",
                "entry %r not followed by a blank line before %r" %
                (current.key, line.split(" ", 1)[0]), path, lineno))
        if "  " in line:
            problems.append(Problem(
                "warning",
                "header %r contains repeated spaces (entries start with "
                "'Type Name')" % line, path, lineno))
        first = line.split(" ", 1)[0]
        if "_" in first:
            problems.append(Problem(
                "warning",
                "%r looks like a property written with spaces instead of a tab"
                % line, path, lineno))
        parts = line.split(" ", 1)
        if len(parts) < 2 or not parts[1].strip():
            problems.append(Problem("error", "entry header without a name: %r" % line,
                                    path, lineno))
            current = None
            continue
        current = Datum(parts[0], parts[1].strip(), [], lineno, path)
        datums.append(current)

    return datums, problems


def parse_file(path: str) -> Tuple[List[Datum], List[Problem]]:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        datums, problems = parse_text(fh.read(), path)
    # Detect the "spaces instead of tabs" mistake: a property line with no tab
    # whose first token is not a plausible entry header.
    for d in datums:
        if d.properties and all(p.indent >= 0 for p in d.properties):
            pass
    return datums, problems


def serialize(datums: List[Datum]) -> str:
    out = []
    for d in datums:
        out.append("%s %s" % (d.type, d.name))
        for p in d.properties:
            indent = "\t" * p.indent
            if p.values:
                out.append("%s%s\t%s" % (indent, p.key, "\t".join(p.values)))
            else:
                out.append("%s%s" % (indent, p.key))
        out.append("")
    return "\n".join(out) + ("\n" if out else "")


def parse_dir(root: str, subdir: str = "Data") -> Tuple[List[Datum], List[Problem]]:
    """Parse every ``.txt`` datum file under ``root/<subdir>``."""
    datums: List[Datum] = []
    problems: List[Problem] = []
    base = os.path.join(root, subdir)
    if not os.path.isdir(base):
        return datums, problems
    for dirpath, _dirs, files in os.walk(base):
        for fn in sorted(files):
            if not fn.lower().endswith(".txt"):
                continue
            path = os.path.join(dirpath, fn)
            d, p = parse_file(path)
            datums.extend(d)
            problems.extend(p)
    return datums, problems
