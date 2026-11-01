#!/usr/bin/env python3
"""M25: the Flyby At Any Range mod hooks two functions; check their prologues.

The mod installs two hooks (Itinerary::IsFleetNearToNode and
util::Darray<ActorDropdownAction>::Add) and refuses to run when either fails to
resolve.  This test pins the prologue bytes of both targets against
artifacts/CDE.exe.original, so a wrong address or a different build fails here
rather than silently doing nothing in game.  Pure stdlib PE section mapper.
"""

from __future__ import annotations

import os
import struct
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.path.join(ROOT, "artifacts", "CDE.exe.original")
IMAGE_BASE = 0x400000

# (name, VA, expected prologue, purpose)
TARGETS = [
    ("Itinerary::IsFleetNearToNode", 0x00744730,
     bytes([0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x98, 0x01, 0x00, 0x00]),
     "bool __thiscall(node const&, WeakPtr<Fleet>), ret 0xC"),
    ("util::Darray<ActorDropdownAction>::Add", 0x007727D0,
     bytes([0x55, 0x8B, 0xEC, 0x51, 0x56, 0x57]),
     "bool __thiscall(action const&), ret 4"),
]


class PeSections:
    """Minimal RVA -> file offset mapper for a PE32 image."""

    def __init__(self, data: bytes):
        self.data = data
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        coff = e_lfanew + 4
        n_sections = struct.unpack_from("<H", data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.sections = []
        sec = opt + opt_size
        for i in range(n_sections):
            base = sec + i * 40
            va = struct.unpack_from("<I", data, base + 12)[0]
            size = struct.unpack_from("<I", data, base + 8)[0]
            raw = struct.unpack_from("<I", data, base + 20)[0]
            self.sections.append((va, size, raw))

    def offset(self, va: int) -> int:
        rva = va - self.image_base
        for va0, size, raw in self.sections:
            if va0 <= rva < va0 + size:
                return raw + (rva - va0)
        raise KeyError("VA %08x is not in any section" % va)


@unittest.skipUnless(os.path.isfile(EXE), "needs artifacts/CDE.exe.original")
class HookTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(EXE, "rb") as fh:
            cls.data = fh.read()
        cls.pe = PeSections(cls.data)

    def read(self, va: int, n: int) -> bytes:
        off = self.pe.offset(va)
        return self.data[off:off + n]

    def test_prologues(self):
        for name, va, expect, abi in TARGETS:
            with self.subTest(name=name):
                self.assertEqual(self.read(va, len(expect)), expect,
                                 "%s: prologue differs (expected %s)" % (name, abi))

    def test_targets_are_distinct_and_padded(self):
        # Two hooks, two different functions, each with enough bytes to splice a
        # 5-byte jump plus the instructions it covers.
        seen = set()
        for name, va, expect, _abi in TARGETS:
            with self.subTest(name=name):
                self.assertNotIn(va, seen)
                seen.add(va)
                self.assertGreaterEqual(len(expect), 6)


if __name__ == "__main__":
    unittest.main()
