#!/usr/bin/env python3
"""Tests for the mod tooling (parser, linter, scaffolder). Stdlib only."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from cdemod import datum as dm  # noqa: E402
from cdemod.lint import index_directory, lint_mods  # noqa: E402
from cdemod.scaffold import create_mod  # noqa: E402


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


STOCK_MINERALS = (
    "Material Wustite\n"
    "Elements\tFe\tO\n"
    "ElementCount\t1\t1\n"
    "Density_kg__m3\t5700\n"
    "\n"
)
STOCK_ALLOYS = (
    "Alloy Steel\n"
    "Component\tWustite\n"
    "Density_kg__m3\t7800\n"
    "\n"
)


class TestParser(unittest.TestCase):
    def test_roundtrip(self):
        text = "Material Wustite\nElements\tFe\tO\nDensity_kg__m3\t5700\n\n"
        datums, problems = dm.parse_text(text)
        self.assertEqual(problems, [])
        self.assertEqual(len(datums), 1)
        self.assertEqual(datums[0].key, ("Material", "Wustite"))
        self.assertEqual(datums[0].prop("Elements").values, ["Fe", "O"])
        self.assertEqual(dm.serialize(datums), text)

    def test_missing_blank_line(self):
        text = "Material A\nDensity\t1\nMaterial B\nDensity\t2\n"
        _datums, problems = dm.parse_text(text)
        self.assertTrue(any("blank line" in p.message for p in problems))

    def test_spaces_instead_of_tabs(self):
        text = "Material A\nDensity_kg__m3    100\n\n"
        _datums, problems = dm.parse_text(text)
        self.assertTrue(any("spaces instead of a tab" in p.message for p in problems))

    def test_nested_block_preserved(self):
        text = "Part Wing\n\tComponent\tWustite\n\tCount\t2\n\n"
        datums, _ = dm.parse_text(text)
        self.assertEqual(datums[0].properties[0].indent, 1)
        self.assertEqual(dm.serialize(datums), text)


class TestLinter(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        write(os.path.join(self.tmp, "game", "Resources", "Data", "Materials",
                           "Minerals.txt"), STOCK_MINERALS)
        write(os.path.join(self.tmp, "game", "Resources", "Data", "Materials",
                           "Alloys.txt"), STOCK_ALLOYS)
        stock, _ = index_directory(os.path.join(self.tmp, "game", "Resources"),
                                   "stock", "Data")
        self.stock = stock

    def tearDown(self):
        import shutil
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_wrong_file_override_dropped_and_missing_ref(self):
        mod = os.path.join(self.tmp, "MyMod")
        write(os.path.join(mod, "Data", "Compounds.txt"),
              "Alloy Steel\nComponent\tUnobtainium\n\n")
        problems = lint_mods([(mod, "MyMod")], self.stock)
        msgs = [p.message for p in problems]
        self.assertTrue(any("Compounds.txt" in m and "stock entry is in" in m for m in msgs),
                        msgs)
        self.assertTrue(any("drops stock properties" in m for m in msgs), msgs)
        self.assertTrue(any("no such entry" in m for m in msgs), msgs)

    def test_correct_override_is_clean(self):
        mod = os.path.join(self.tmp, "GoodMod")
        write(os.path.join(mod, "Data", "Materials", "Alloys.txt"),
              "Alloy Steel\nComponent\tWustite\nDensity_kg__m3\t9000\n\n")
        problems = lint_mods([(mod, "GoodMod")], self.stock)
        self.assertEqual([p.message for p in problems], [])

    def test_cross_mod_conflict(self):
        a = os.path.join(self.tmp, "ModA")
        b = os.path.join(self.tmp, "ModB")
        text = "Alloy Steel\nComponent\tWustite\nDensity_kg__m3\t1\n\n"
        write(os.path.join(a, "Data", "Materials", "Alloys.txt"), text)
        write(os.path.join(b, "Data", "Materials", "Alloys.txt"), text)
        problems = lint_mods([(a, "ModA"), (b, "ModB")], self.stock)
        self.assertTrue(any("defined by both" in p.message for p in problems))


class TestScaffold(unittest.TestCase):
    def test_create(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = create_mod("CoolMod", tmp)
            self.assertTrue(os.path.isdir(os.path.join(root, "Data", "Imports")))
            self.assertTrue(os.path.isfile(os.path.join(root, "README.md")))
            self.assertTrue(os.path.isfile(
                os.path.join(root, "Data", "Materials", "MyMaterial.txt")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
