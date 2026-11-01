#!/usr/bin/env python3
"""Every symbol name a mod asks the loader to resolve must exist in the table.

F77 was exactly this bug: `cdegame.hpp` asked for the non-virtual form of a
`Darray` destructor (`...QAE@XZ`) while the shipped table spells it `...UAE@XZ`,
so the lookup returned null and `Fleets()` leaked one array per call. Nothing
failed loudly. This test reads the names out of the SDK headers and checks each
against `decomp/symbols/functions.tsv` (mangled or demangled column), so a typo or
a wrong calling-convention spelling fails here instead of leaking in game.
"""

from __future__ import annotations

import os
import re
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# The file the loader actually reads: VA \t NAME \t MANGLED, where NAME is the
# short form the SDK asks for ("StrategicManager::GetFleets").
TABLE = os.environ.get(
    "CDE_SYMBOLS",
    "<path>")
HEADERS = [
    os.path.join(ROOT, "src", "modloader", "cdegame.hpp"),
    os.path.join(ROOT, "src", "modloader", "sample", "laser_m2.cpp"),
    os.path.join(ROOT, "src", "modloader", "sample", "laser_jitter.cpp"),
    os.path.join(ROOT, "src", "modloader", "sample", "auto_intercept.cpp"),
]

# `detail::Sym<T>("Name")` in cdegame.hpp, `cde::Resolve("Name")` in plugins.
NAME_RE = re.compile(r'(?:Sym<[^>]*>|Resolve)\(\s*"([^"]+)"')


def load_table():
    """Return (short names, mangled names) as the loader sees them."""
    names, mangled = set(), set()
    with open(TABLE, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2 and parts[1]:
                names.add(parts[1])
            if len(parts) >= 3 and parts[2]:
                mangled.add(parts[2])
    return names, mangled


def requested_names():
    seen = []
    for path in HEADERS:
        if not os.path.isfile(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for name in NAME_RE.findall(text):
            if name not in seen:
                seen.append(name)
    return seen


@unittest.skipUnless(os.path.isfile(TABLE), "needs decomp/symbols/functions.tsv")
class SymbolNameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.names, cls.mangled = load_table()

    def test_requested_names_resolve(self):
        wanted = requested_names()
        self.assertGreater(len(wanted), 10, "expected the SDK to name its symbols")
        unresolved = [n for n in wanted if n not in self.names and n not in self.mangled]
        self.assertEqual(unresolved, [],
                         "the loader would resolve these to null: %r" % (unresolved,))


if __name__ == "__main__":
    unittest.main()
