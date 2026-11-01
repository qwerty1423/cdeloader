#!/usr/bin/env python3
"""Proxy-mode install: layout, idempotency and restore (no game needed)."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "scripts"))

from cdefw import proxy  # noqa: E402

PROXY_DLL = os.path.join(ROOT, "build", "proxy", "vcruntime140.dll")


class ProxyInstallTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="cde-proxy-test-")
        self.loader_dir = os.path.join(self.dir, "loader")
        os.makedirs(self.loader_dir)
        with open(os.path.join(self.loader_dir, "cdeloader.dll"), "wb") as fh:
            fh.write(b"MZ loader")
        self.symbols = os.path.join(self.dir, "cdeloader.symbols")
        with open(self.symbols, "wb") as fh:
            fh.write(b"# symbols\n")
        self.ini = os.path.join(self.dir, "cdeloader.ini")
        with open(self.ini, "w") as fh:
            fh.write("[loader]\n")
        self.game = os.path.join(self.dir, "game")
        os.makedirs(self.game)
        self.runtime = os.path.join(self.game, "vcruntime140.dll")
        with open(self.runtime, "wb") as fh:
            fh.write(b"MZ real runtime")
        self.stock = os.path.join(self.dir, "CDE.exe.original")
        with open(self.stock, "wb") as fh:
            fh.write(b"MZ stock")

    def test_install_rename_copy_and_restore(self):
        if not os.path.isfile(PROXY_DLL):
            self.skipTest("proxy DLL not built (run scripts/build-proxy.sh)")
        report = proxy.install(
            self.game, proxy_dll=PROXY_DLL,
            loader=os.path.join(self.loader_dir, "cdeloader.dll"),
            symbols=self.symbols, ini=self.ini, stock_exe=self.stock)
        self.assertTrue(proxy.is_proxy(self.runtime))
        self.assertTrue(os.path.isfile(os.path.join(self.game,
                                                    "vcruntime140_orig.dll")))
        self.assertTrue(os.path.isfile(os.path.join(self.game, "cdeloader.dll")))
        self.assertTrue(os.path.isfile(os.path.join(self.game,
                                                    "CDE.exe.original")))
        self.assertTrue(report["actions"])

        # Second run must not lose the renamed original.
        proxy.install(self.game, proxy_dll=PROXY_DLL,
                      loader=os.path.join(self.loader_dir, "cdeloader.dll"),
                      symbols=self.symbols, ini=self.ini, stock_exe=self.stock)
        self.assertTrue(os.path.isfile(os.path.join(self.game,
                                                    "vcruntime140_orig.dll")))

        proxy.restore(self.game)
        self.assertFalse(proxy.is_proxy(self.runtime))
        self.assertFalse(os.path.exists(os.path.join(
            self.game, "vcruntime140_orig.dll")))

    def test_refuses_ambiguous_runtime(self):
        if not os.path.isfile(PROXY_DLL):
            self.skipTest("proxy DLL not built")
        with open(os.path.join(self.game, "vcruntime140_orig.dll"), "wb") as fh:
            fh.write(b"MZ other")
        with self.assertRaises(proxy.ProxyError):
            proxy.install(self.game, proxy_dll=PROXY_DLL,
                          loader=os.path.join(self.loader_dir, "cdeloader.dll"))


if __name__ == "__main__":
    unittest.main()
