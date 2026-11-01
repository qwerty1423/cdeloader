#!/usr/bin/env python3
"""The two laser mods patch specific bytes; pin every one of them.

Laser Ablation Cap Removal rewrites one `minsd` in the armour damage path, and the M^2 Limit Removal
retargets four `addsd` instructions that all reference the shared 9.0 double. If a game update moved
any of them, the mods would refuse to install (they check the bytes themselves) - but only at runtime,
in game. This test says so at build time against artifacts/CDE.exe.original.
"""

from __future__ import annotations

import os
import struct
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.path.join(ROOT, "artifacts", "CDE.exe.original")
IMAGE_BASE = 0x400000

# The laser-jitter mod's two hooks, by prologue.
JITTER_HOOKS = [
    ("WeaponCompartment::GetAimerOffset_km", 0x006615E0,
     bytes([0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00])),
    ("RandomAimer::GetRandomAimOffset_km", 0x00663FD0,
     bytes([0x55, 0x8B, 0xEC])),
]

# The ablation cap site the laser-cap mod rewrites: minsd xmm3, xmm5.
CAP_SITE = ("ArmorPlate::ApplyLaserDamage cap", 0x00675355, bytes([0xF2, 0x0F, 0x5D, 0xDD]))

# The four addsd sites the M^2 mod retargets; all reference the same 9.0 double.
M2_SITES = [
    ("LaserModule::GetRadiusAtDistance_m", 0x0063D034),
    ("LaserModule::GetStatuses", 0x0063D7CC),
    ("LaserModule::GetBeamWaistRadius_m", 0x006450C4),
    ("LaserModule::GetLaserInfo", 0x00646473),
]
M2_ADDSD = bytes([0xF2, 0x0F, 0x58, 0x05, 0x68, 0x55, 0x92, 0x00])


def read(va: int, count: int) -> bytes:
    with open(EXE, "rb") as fh:
        data = fh.read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    table = opt + struct.unpack_from("<H", data, pe + 20)[0]
    for index in range(sections):
        base = table + 40 * index
        vsize, vaddr, raw_size, raw_off = struct.unpack_from("<IIII", data, base + 8)
        start = IMAGE_BASE + vaddr
        if start <= va < start + max(vsize, raw_size):
            return data[raw_off + (va - start):raw_off + (va - start) + count]
    raise AssertionError("address %#x is not inside the image" % va)


class LaserModSitesTest(unittest.TestCase):
    def test_jitter_hook_prologues(self):
        for name, va, expected in JITTER_HOOKS:
            with self.subTest(target=name):
                self.assertEqual(read(va, len(expected)), expected,
                                 "%s at %#x" % (name, va))

    def test_ablation_cap_site(self):
        name, va, expected = CAP_SITE
        self.assertEqual(read(va, len(expected)), expected, "%s at %#x" % (name, va))

    def test_m2_addsd_sites(self):
        for name, va in M2_SITES:
            with self.subTest(site=name):
                self.assertEqual(read(va, len(M2_ADDSD)), M2_ADDSD,
                                 "%s at %#x no longer addsd the shared 9.0" % (name, va))


if __name__ == "__main__":
    unittest.main()
