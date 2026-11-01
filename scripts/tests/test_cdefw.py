#!/usr/bin/env python3
"""Tests for the cdefw mod-framework tooling (stdlib unittest; no deps).

Run with ``python3 scripts/tests/test_cdefw.py``.  The symbol-table tests use a
synthetic table and a synthetic exe, so they need no proprietary artefacts.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # scripts/

from cdefw import framework  # noqa: E402
from cdefw import sdk as sdk_mod  # noqa: E402
from cdefw.scaffold import scaffold  # noqa: E402

REPO = sdk_mod.find_repo_root(HERE)
STOCK_EXE = os.path.join(REPO, "artifacts", "CDE.exe.original")
STOCK_SHA = "04fa7f2218211cbcc8f041bcd81077cc4b0c7f5466603d7b26dae9c9e8bf3db2"

TSV_HEADER = "va\tsection\toffset\tflags\tmangled\tdemangled\tclass\tmethod\n"


def write_tsv(path: str, rows) -> None:
    """rows: (va, mangled, demangled, class, method)."""
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(TSV_HEADER)
        for va, man, dem, cls, meth in rows:
            fh.write("0x%08x\t1\t0x0\tfunction\t%s\t%s\t%s\t%s\n"
                     % (va, man, dem, cls, meth))


class SdkTests(unittest.TestCase):
    def test_generates_sorted_symbols_with_header(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "functions.tsv")
            exe = os.path.join(d, "CDE.exe")
            out = os.path.join(d, "cdeloader.symbols")
            write_tsv(tsv, [
                (0x004d0e70, "?NormalizeAngle@@YA",
                 "double __cdecl util::NormalizeAngle<double>(double)",
                 "util", "NormalizeAngle<double>"),
                (0x00401000, "?Foo@@YA", "Foo", "(global)", "Foo"),
            ])
            with open(exe, "wb") as fh:
                fh.write(b"fake exe")
            sha, count = sdk_mod.build(tsv_path=tsv, out_path=out, exe_path=exe,
                                       repo_root=d)
            self.assertEqual(count, 2)
            self.assertEqual(sha, hashlib.sha256(b"fake exe").hexdigest())

            with open(out, encoding="utf-8") as fh:
                lines = fh.read().splitlines()
            self.assertTrue(lines[0].startswith("# cdeloader.symbols v1 sha256="))
            self.assertIn("base=0x00400000", lines[0])
            self.assertIn(sha, lines[0])
            # sorted by VA; the first (friendly) key is Class::Method
            self.assertEqual(lines[1].split("\t")[0], "0x00401000")
            self.assertEqual(lines[1].split("\t")[1], "Foo")
            self.assertEqual(lines[2].split("\t")[0], "0x004d0e70")
            self.assertEqual(lines[2].split("\t")[1], "util::NormalizeAngle<double>")
            # the full demangled signature is emitted as an alternate key
            self.assertEqual(lines[3].split("\t")[0], "0x004d0e70")
            self.assertIn("util::NormalizeAngle<double>(double)", lines[3].split("\t")[1])

    def test_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "functions.tsv")
            write_tsv(tsv, [
                (0x401000, "?A@@YA", "B::A", "B", "A"),
                (0x402000, "?B@@YA", "A::B", "A", "B"),
            ])
            a = os.path.join(d, "a.symbols")
            b = os.path.join(d, "b.symbols")
            sdk_mod.build(tsv_path=tsv, out_path=a, exe_path=None, repo_root=d)
            sdk_mod.build(tsv_path=tsv, out_path=b, exe_path=None, repo_root=d)
            with open(a, "rb") as fa, open(b, "rb") as fb:
                self.assertEqual(fa.read(), fb.read())

    def test_missing_exe_records_placeholder(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            tsv = os.path.join(d, "functions.tsv")
            out = os.path.join(d, "cdeloader.symbols")
            write_tsv(tsv, [(0x401000, "?A@@YA", "A", "(global)", "A")])
            sha, _ = sdk_mod.build(tsv_path=tsv, out_path=out, exe_path=None, repo_root=d)
            self.assertEqual(sha, "")
            with open(out, encoding="utf-8") as fh:
                self.assertIn("sha256=?", fh.readline())


class ScaffoldTests(unittest.TestCase):
    def test_scaffold_creates_buildable_project(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            target = scaffold("My Mod", d, mod_id="org.test.mymod", author="tester")
            self.assertTrue(os.path.isfile(os.path.join(target, "plugin.cpp")))
            self.assertTrue(os.path.isfile(os.path.join(target, "build.sh")))
            with open(os.path.join(target, "plugin.cpp"), encoding="utf-8") as fh:
                src = fh.read()
            self.assertIn('CDE_MOD("org.test.mymod"', src)
            self.assertIn("#include \"cdemod.hpp\"", src)
            # macros must be set before the header include
            self.assertLess(src.index("#define CDE_USER_INIT()"),
                            src.index('#include "cdemod.hpp"'))

    @unittest.skipUnless(shutil.which("i686-w64-mingw32-g++"),
                         "i686-w64-mingw32-g++ not installed")
    def test_scaffold_project_builds(self) -> None:
        """The generated build.sh must actually compile the plugin."""
        root = sdk_mod.find_repo_root(HERE)
        with tempfile.TemporaryDirectory() as d:
            target = scaffold("Build Me", d, mod_id="org.test.builde", author="tester")
            env = dict(os.environ, SDK=os.path.join(root, "src", "modloader"))
            env.pop("LOADER_IMPLIB", None)  # build without the loader import lib
            proc = subprocess.run(["./build.sh"], cwd=target, env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(proc.returncode, 0, proc.stdout.decode("utf-8", "replace"))
            self.assertTrue(os.path.isfile(os.path.join(target, "plugin.dll")))

    def test_scaffold_data_mod(self) -> None:
        from cdefw.scaffold import scaffold_data_mod
        with tempfile.TemporaryDirectory() as d:
            target = scaffold_data_mod(
                "Better Engines", d, mod_id="org.test.engines",
                author="tester", files=[r"Resources\Data\Modules\engines.txt"])
            self.assertTrue(os.path.isfile(
                os.path.join(target, "Data", "Resources", "Data", "Modules", "engines.txt")))
            self.assertTrue(os.path.isfile(os.path.join(target, "mod.ini")))
            self.assertTrue(os.path.isfile(os.path.join(target, "README.md")))
            with open(os.path.join(target, "mod.ini"), encoding="utf-8") as fh:
                self.assertIn("id = org.test.engines", fh.read())


@unittest.skipUnless(os.path.isfile(STOCK_EXE), "artifacts/CDE.exe.original not present")
class FrameworkBuildTests(unittest.TestCase):
    def test_delta_reproducible_and_injects_loader(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            a = os.path.join(d, "a.json")
            b = os.path.join(d, "b.json")
            da = framework.build_delta(STOCK_EXE, a)
            db = framework.build_delta(STOCK_EXE, b)
            with open(a, "rb") as fa, open(b, "rb") as fb:
                self.assertEqual(fa.read(), fb.read())
            self.assertEqual(da["source_sha256"], STOCK_SHA)
            self.assertEqual(da["variant"], "framework")
            # the delta itself only carries bytes; apply it and inspect the PE to
            # prove the loader import was injected
            from cdetool.delta import apply_delta
            from cdetool.pe import PE
            with open(STOCK_EXE, "rb") as fh:
                target = apply_delta(fh.read(), da)
            summary = PE(target).summary()
            self.assertIn("cdeloader.dll", summary["imports"])

    def test_framework_package_layout(self) -> None:
        """The framework zip is the loader, its settings and a README.  Mods, the
        installer and the source tree are separate downloads."""
        loader = os.path.join(REPO, "build", "modloader", "cdeloader.dll")
        if not os.path.isfile(loader):
            self.skipTest("cdeloader.dll not built")
        with tempfile.TemporaryDirectory() as d:
            symbols = os.path.join(d, "cdeloader.symbols")
            with open(symbols, "w", encoding="utf-8") as fh:
                fh.write("# cdeloader.symbols v1 sha256=? base=0x00400000\n")
            mod = os.path.join(d, "plugin.dll")
            with open(mod, "wb") as fh:
                fh.write(b"MZ")
            zip_path = os.path.join(d, "pkg.zip")
            framework.make_framework_package(
                STOCK_EXE, zip_path, loader=loader, symbols=symbols,
                mod_files=[(mod, "mods/mymod/mymod.dll")], all_mods=False)
            names = set(zipfile.ZipFile(zip_path).namelist())
            for required in ("cdeloader.dll", "cdeloader.symbols", "cdeloader.ini",
                             "README.md", "LICENSE", "vcruntime140.dll",
                             "mods/mymod/mymod.dll"):
                self.assertIn(required, names)
            # no executable patching and no source tree in this archive
            for absent in ("cdeloader.delta.json", "CDE-framework.delta.json",
                           "install.sh", "install.cmd", "SOURCE.txt"):
                self.assertNotIn(absent, names)
            self.assertFalse(any(n.startswith("src/") for n in names), names)
            readme = zipfile.ZipFile(zip_path).read("README.md").decode()
            self.assertIn("vcruntime140_orig.dll", readme)

    def test_dist_layout(self) -> None:
        loader = os.path.join(REPO, "build", "modloader", "cdeloader.dll")
        if not os.path.isfile(loader):
            self.skipTest("cdeloader.dll not built")
        with tempfile.TemporaryDirectory() as d:
            dist = framework.assemble_dist(d, loader=loader, mod_files=[])
            self.assertTrue(os.path.isfile(os.path.join(dist, "cdeloader.dll")))
            self.assertTrue(os.path.isfile(os.path.join(dist, "cdeloader.ini")))


class LoaderSourceTests(unittest.TestCase):
    def test_abi_header_and_loader_present(self) -> None:
        root = sdk_mod.find_repo_root(HERE)
        for rel in ("src/modloader/cdemod_api.h", "src/modloader/cdemod.hpp",
                    "src/modloader/modloader.cpp"):
            self.assertTrue(os.path.isfile(os.path.join(root, rel)), rel)
        with open(os.path.join(root, "src/modloader/cdemod_api.h"), encoding="utf-8") as fh:
            head = fh.read()
        self.assertIn("CDEMOD_ABI_VERSION", head)
        self.assertIn("CdeHostApi", head)
        self.assertIn("CdeModInfo", head)


if __name__ == "__main__":
    unittest.main(verbosity=2)
