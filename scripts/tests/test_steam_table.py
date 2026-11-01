import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))

try:
    import iapws  # noqa: F401
    from steam_table import build, validate, SteamTable
    HAVE_IAPWS = True
except Exception:
    HAVE_IAPWS = False


@unittest.skipUnless(HAVE_IAPWS, "iapws not installed (use .venv/bin/python)")
class SteamTableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = build()
        cls.table = SteamTable(cls.data)

    def test_interpolation_error_is_small(self):
        errors = validate(self.data, samples=150)
        self.assertLess(errors["h"], 0.02)
        self.assertLess(errors["s"], 0.03)

    def test_saturation_temperature_rises_with_pressure(self):
        low = self.table.saturation(0.01)["tsat_c"]
        high = self.table.saturation(10.0)["tsat_c"]
        self.assertGreater(high, low)

    def test_superheated_enthalpy_rises_with_temperature(self):
        # 5 MPa is superheated above ~264 C.
        h1 = self.table.superheated_lookup(5.0, 350.0, "h")
        h2 = self.table.superheated_lookup(5.0, 450.0, "h")
        self.assertGreater(h2, h1)

    def test_no_data_below_saturation(self):
        with self.assertRaises(ValueError):
            self.table.superheated_lookup(5.0, 100.0, "h")
