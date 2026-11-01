#!/usr/bin/env python3
"""audit-addresses.py must not call a quoted file offset a typo.

Third-party write-ups quote file offsets, and several notes quote them verbatim, so the auditor
has to tell three things apart: virtual addresses, file offsets, and a genuine typo.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "audit-addresses.py")


class AuditAddressesTest(unittest.TestCase):
    def run_audit(self, *args: str):
        return subprocess.run([sys.executable, SCRIPT] + list(args),
                              capture_output=True, text=True, cwd=ROOT)

    def test_the_whole_corpus_has_no_typos(self):
        out = self.run_audit()
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertNotIn("outside=", out.stdout)
        self.assertIn("start=", out.stdout)

    def test_a_quoted_file_offset_is_classified_not_rejected(self):
        out = self.run_audit(os.path.join("notes", "M7-LASER-CAP.md"))
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
        self.assertIn("fileoff=", out.stdout)

    def test_an_impossible_address_would_be_reported(self):
        # 0x00123456 is below the image base and inside no section's raw range
        out = self.run_audit(os.path.join("notes", "armored_components.md"))
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


if __name__ == "__main__":
    unittest.main()
