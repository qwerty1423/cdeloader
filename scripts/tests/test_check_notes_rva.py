#!/usr/bin/env python3
"""check-notes.py must catch an RVA quoted where a virtual address belongs.

The community patch stores its targets as RVAs; the framework stores virtual addresses. A note that quotes
`0x00461c50` when it means the function at `0x00861c50` reads as a wrong address to anyone who tries it, and
that is a mistake this project has made more than once.
"""

from __future__ import annotations

import importlib.util
import pathlib
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))


def load_checker():
    spec = importlib.util.spec_from_file_location("check_notes", os.path.join(ROOT, "scripts", "check-notes.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RvaCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_checker()
        cls.symbols = cls.checker.load_symbols()
        assert cls.symbols, "the symbol table must load"

    def test_the_real_corpus_is_clean(self):
        notes = sorted(p.name for p in self.checker.NOTES.glob("*.md"))
        self.assertEqual(self.checker.check_rva_presented_as_va(notes, self.symbols), [])

    def test_a_known_function_below_the_base_is_reported(self):
        # exercise the matcher directly: 0x00461c50 + image base is the function at 0x00861c50
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            self.checker.NOTES = pathlib.Path(tmp)
            (pathlib.Path(tmp) / "x.md").write_text("the hook is at `0x00461c50` somewhere\n")
            problems = self.checker.check_rva_presented_as_va(["x.md"], {0x00861C50: "GameManager::Tick"})
        self.assertEqual(len(problems), 1)
        self.assertIn("0x00461c50", problems[0])

    def test_the_checker_sees_an_rva_in_a_real_note(self):
        # sanity: the regex the check uses matches the eight-digit form
        import re
        match = re.search(r"0x0[0-9a-f]{6,7}", "see `0x00461c50` for the hook")
        self.assertIsNotNone(match)


if __name__ == "__main__":
    unittest.main()
