#!/usr/bin/env python3
"""End-to-end tests for the cdetool patch toolchain.

Run with ``python3 tools/tests/test_toolchain.py`` (stdlib unittest; no deps).

The strongest tests need ``artifacts/CDE.exe.original``; they skip cleanly when
it is not present (it is proprietary and not committed).  The expected target
hashes are the published CDE Community Patch build checksums.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from cdetool import inject  # noqa: E402
from cdetool.delta import apply_delta, create_delta, load_delta, save_delta  # noqa: E402
from cdetool.package import make_package  # noqa: E402
from cdetool.pe import PE, sha256  # noqa: E402
from cdetool.spec import build_from_spec  # noqa: E402
from cdetool.symbols import Symbols  # noqa: E402

ORIGINAL = os.path.join(ROOT, "artifacts", "CDE.exe.original")
ORIGINAL_SHA = "04fa7f2218211cbcc8f041bcd81077cc4b0c7f5466603d7b26dae9c9e8bf3db2"
TARGETS = {
    "fixes-only": "485696dec8b0f8d0b8c08433cfd184df698449d29563b2c79539649e4f40f211",
    "complete": "26c70b79fcac69d61ffb23dfe5c2fc4c8611a2a4924a53b4fe4a46f863292465",
}


def have_original() -> bool:
    if not os.path.isfile(ORIGINAL):
        return False
    with open(ORIGINAL, "rb") as fh:
        return sha256(fh.read()) == ORIGINAL_SHA


def read(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


class TestDelta(unittest.TestCase):
    def test_roundtrip_coalesces_runs(self):
        src = bytes(range(256)) * 4
        tgt = bytearray(src)
        tgt[10:13] = b"ABC"
        tgt[100] = 0xFF
        tgt += b"tail data"
        delta = create_delta(src, bytes(tgt), variant="x", version="1")
        self.assertEqual([(c["offset"], c["bytes"]) for c in delta["changes"]],
                         [(10, "414243"), (100, "ff")])
        self.assertEqual(delta["append"], b"tail data".hex())
        self.assertEqual(apply_delta(src, delta), bytes(tgt))

    def test_rejects_wrong_source(self):
        src = b"a" * 16
        delta = create_delta(src, src + b"b")
        with self.assertRaises(Exception):
            apply_delta(b"c" * 16, delta)


class TestInject(unittest.TestCase):
    def test_layout_matches_known_geometry(self):
        if not have_original():
            self.skipTest("artifacts/CDE.exe.original not present")
        src = read(ORIGINAL)
        pe = PE(src)
        report = inject.inject(
            pe,
            imports=[("cdepatch.dll", ["CdeInit"])],
            exports=[{"name": "AmdPowerXpressRequestHighPerformance",
                      "kind": "data_u32", "value": 1},
                     {"name": "NvOptimusEnablement", "kind": "data_u32", "value": 1}],
            module_name="CDE.exe", large_address_aware=True)
        out = pe.to_bytes()
        self.assertEqual(len(out), 6727680)
        self.assertEqual(report["sections"], {".cdei": 0x6A0000, ".cdep": 0x6A1000})
        self.assertEqual(report["iat_rvas"], [0x6A01FC])
        # DataDirectory[IMPORT] points at the new table with the cdepatch extra.
        imp_rva, imp_size = PE(out).directory(1)
        self.assertEqual((imp_rva, imp_size), (0x6A0000, 0x1F4))
        # The new import is visible.
        names = [f.get("name") for d in PE(out).iter_imports()
                 if d["dll"] == "cdepatch.dll" for f in d["functions"]]
        self.assertEqual(names, ["CdeInit"])
        # The two GPU globals are exported.
        exported = {e["name"] for e in PE(out).iter_exports()}
        self.assertIn("NvOptimusEnablement", exported)


class TestPackage(unittest.TestCase):
    def test_bundle_is_self_contained(self):
        import zipfile
        with tempfile.TemporaryDirectory() as tmp:
            src = b"original game bytes" * 8
            tgt = bytearray(src)
            tgt[5:7] = b"XY"
            tgt += b"appended section"
            delta = create_delta(src, bytes(tgt), variant="demo", version="9.9")
            delta_path = os.path.join(tmp, "d.delta.json")
            save_delta(delta, delta_path)
            out = os.path.join(tmp, "patch.zip")
            make_package(delta_path, out, name="Demo Patch", version="9.9")
            with zipfile.ZipFile(out) as zf:
                names = set(zf.namelist())
            for expected in ("installer.py", "install.sh", "install.cmd",
                             "install.ps1", "uninstall.sh", "uninstall.cmd",
                             "README.txt", "CDE-demo.delta.json"):
                self.assertIn(expected, names)
            with zipfile.ZipFile(out) as zf:
                installer = zf.read("installer.py").decode()
                readme = zf.read("README.txt").decode()
            self.assertNotIn("@@", installer)
            self.assertNotIn("@@", readme)
            self.assertIn("Demo Patch", installer)
            self.assertIn(delta["target_sha256"], readme)


class TestShippedPatchReproduction(unittest.TestCase):
    """The toolchain must rebuild the community patch exactly."""

    def _spec(self, variant):
        return os.path.join(ROOT, "patches",
                            "cde-community-%s.spec.json" % variant)

    def test_specs_reproduce_published_targets(self):
        if not have_original():
            self.skipTest("artifacts/CDE.exe.original not present")
        src = read(ORIGINAL)
        symbols = Symbols.load()
        for variant, target_sha in TARGETS.items():
            with self.subTest(variant=variant):
                with open(self._spec(variant), "r", encoding="utf-8") as fh:
                    spec = json.load(fh)
                delta = build_from_spec(spec, src, symbols=symbols)
                self.assertEqual(delta["target_sha256"], target_sha)
                # Applying the freshly built delta lands on the same bytes.
                self.assertEqual(sha256(apply_delta(src, delta)), target_sha)

    def test_loader_import_stacks_on_community_patch(self):
        """Our loader can be injected on top of an already-patched exe
        (cdepatch.dll), preserving the existing import — the coexistence path."""
        if not have_original():
            self.skipTest("artifacts/CDE.exe.original not present")
        src = read(ORIGINAL)
        with open(self._spec("complete"), encoding="utf-8") as fh:
            spec = json.load(fh)
        patched = apply_delta(src, build_from_spec(spec, src, symbols=Symbols.load()))
        self.assertEqual(sha256(patched), TARGETS["complete"])

        pe = PE(patched)
        inject.inject(pe, imports=[("cdeloader.dll", ["CdeModInit"])])
        stacked = pe.to_bytes()

        imports = {i["dll"].lower() for i in PE(stacked).iter_imports()}
        self.assertIn("cdepatch.dll", imports)
        self.assertIn("cdeloader.dll", imports)
        # Only headers and appended sections changed; the image base is intact.
        self.assertEqual(PE(stacked).image_base, PE(patched).image_base)

    def test_apply_then_revert(self):
        if not have_original():
            self.skipTest("artifacts/CDE.exe.original not present")
        with tempfile.TemporaryDirectory() as tmp:
            src = read(ORIGINAL)
            symbols = Symbols.load()
            with open(self._spec("fixes-only"), encoding="utf-8") as fh:
                spec = json.load(fh)
            delta = build_from_spec(spec, src, symbols=symbols)
            delta_path = os.path.join(tmp, "p.delta.json")
            save_delta(delta, delta_path)
            exe = os.path.join(tmp, "CDE.exe")
            with open(exe, "wb") as fh:
                fh.write(src)
            from cdetool.cli import main as cli_main
            self.assertEqual(cli_main(["apply", exe, delta_path]), 0)
            self.assertEqual(sha256(read(exe)), TARGETS["fixes-only"])
            # Applying again is a no-op (idempotent).
            self.assertEqual(cli_main(["apply", exe, delta_path]), 0)
            self.assertEqual(sha256(read(exe)), TARGETS["fixes-only"])
            self.assertEqual(cli_main(["revert", exe]), 0)
            self.assertEqual(sha256(read(exe)), ORIGINAL_SHA)


if __name__ == "__main__":
    unittest.main(verbosity=2)
