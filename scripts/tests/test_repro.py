#!/usr/bin/env python3
"""Reproducibility: the same inputs must produce the same zip bytes."""

from __future__ import annotations

import hashlib
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from cdetool import package  # noqa: E402

DELTA = os.path.join(ROOT, "build", "framework", "cdeloader.delta.json")


class ReproTests(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(DELTA), "framework delta not built")
    def test_zip_is_deterministic(self):
        digests = []
        for _ in range(2):
            out = tempfile.mktemp(suffix=".zip")
            try:
                package.make_package(DELTA, out, name="Repro", version="0.0.0")
                with open(out, "rb") as fh:
                    digests.append(hashlib.sha256(fh.read()).hexdigest())
            finally:
                if os.path.exists(out):
                    os.unlink(out)
        self.assertEqual(digests[0], digests[1])


if __name__ == "__main__":
    unittest.main()
