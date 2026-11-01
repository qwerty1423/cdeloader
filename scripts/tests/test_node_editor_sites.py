#!/usr/bin/env python3
"""The Manoeuvre Node Editor depends on three hooks and three offsets; pin them all.

The hooks are the game calls the editor makes (edit a burn, refresh the manager's copy of the
selection, and the tick the calls are marshalled onto). The offsets are the ones the editor reads
to find the fleet's itinerary and the selected node.

Every byte pattern here is taken from artifacts/CDE.exe.original, so a wrong address, a stale
offset or a different game build fails this test instead of quietly making the editor do nothing.
"""

from __future__ import annotations

import os
import struct
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
EXE = os.path.join(ROOT, "artifacts", "CDE.exe.original")
IMAGE_BASE = 0x400000

# (name, VA, expected bytes, why)
HOOKS = [
    ("Itinerary::AlterBurn", 0x00738700,
     bytes([0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C]),
     "void __thiscall(node const&, burn), the game's own burn path"),
    ("StrategicManager::SelectItineraryNode", 0x0076B620,
     bytes([0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA4, 0x02, 0x00, 0x00]),
     "node by value, ret 0x150; refreshes the manager's selection copy"),
    ("StrategicManager::Tick", 0x0076DAC0,
     bytes([0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1]),
     "the tick the editor marshals its edits onto"),
]

BODIES = [
    ("Fleet::GetMissionItinerary", 0x007317A0,
     bytes([0x8D, 0x81, 0xC0, 0x02, 0x00, 0x00, 0xC3]),
     "lea eax, [ecx+0x2c0]; ret  -- the Maybe<Itinerary> offset"),
    # inside Maybe<Itinerary>::operator-> (which begins 56 8B F1), not at a function entry
    ("Maybe<Itinerary> validity read", 0x00771903,
     bytes([0x80, 0xBE, 0x90, 0x02, 0x00, 0x00, 0x00]),
     "cmp byte [esi+0x290], 0  -- the validity byte"),
]


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
            offset = raw_off + (va - start)
            return data[offset:offset + count]
    raise AssertionError("address %#x is not inside the image" % va)


class NodeEditorSitesTest(unittest.TestCase):
    def test_the_game_exe_is_present(self):
        self.assertTrue(os.path.isfile(EXE), "artifacts/CDE.exe.original is missing")

    def test_hook_targets_are_where_the_editor_thinks(self):
        for name, va, expected, why in HOOKS:
            with self.subTest(target=name):
                actual = read(va, len(expected))
                self.assertEqual(actual, expected,
                                 "%s at %#x: %s\n got %s\nwant %s" %
                                 (name, va, why, actual.hex(" "), expected.hex(" ")))

    def test_offset_reading_instructions_match(self):
        for name, va, expected, why in BODIES:
            with self.subTest(pattern=name):
                actual = read(va, len(expected))
                self.assertEqual(actual, expected,
                                 "%s at %#x: %s\n got %s\nwant %s" %
                                 (name, va, why, actual.hex(" "), expected.hex(" ")))


if __name__ == "__main__":
    unittest.main()
