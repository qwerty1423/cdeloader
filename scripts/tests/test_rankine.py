import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))

try:
    import iapws  # noqa: F401
    from rankine import rankine
    HAVE_IAPWS = True
except Exception:  # iapws is a venv-only dependency
    HAVE_IAPWS = False


@unittest.skipUnless(HAVE_IAPWS, "iapws not installed (use .venv/bin/python)")
class RankineTests(unittest.TestCase):
    def test_superheated_cycle_is_plausible(self):
        r = rankine(5.0, 300.0, 0.005)
        self.assertGreater(r.thermal_efficiency, 0.25)
        self.assertLess(r.thermal_efficiency, 0.40)
        self.assertLess(r.thermal_efficiency, r.carnot_efficiency)
        self.assertGreater(r.specific_net_work_kj_kg, 500.0)

    def test_pump_work_is_small(self):
        r = rankine(5.0, 300.0, 0.005)
        self.assertLess(r.pump_work_kj_kg, 0.05 * r.turbine_work_kj_kg)

    def test_energy_balance(self):
        r = rankine(5.0, 300.0, 0.005)
        # boiler heat + pump work = turbine work + condenser heat
        self.assertAlmostEqual(r.boiler_heat_kj_kg + r.pump_work_kj_kg,
                               r.turbine_work_kj_kg + r.condenser_heat_kj_kg,
                               delta=1e-6 * r.boiler_heat_kj_kg + 1e-3)

    def test_low_temperature_is_worse(self):
        cold = rankine(5.0, 200.0, 0.005)
        hot = rankine(5.0, 300.0, 0.005)
        self.assertLess(cold.thermal_efficiency, hot.thermal_efficiency)

    def test_rejects_bad_pressures(self):
        with self.assertRaises(ValueError):
            rankine(0.005, 300.0, 0.005)


if __name__ == "__main__":
    unittest.main()


@unittest.skipUnless(HAVE_IAPWS, "iapws not installed (use .venv/bin/python)")
class RadiatorTests(unittest.TestCase):
    def test_radiator_balances_at_low_temperature(self):
        from rankine import solve_condenser
        p = solve_condenser(5.0, 300.0, 5000.0, 1.0)
        self.assertTrue(p["balanced"])
        self.assertGreater(p["t_cond_c"], 5.0)
        self.assertLess(p["t_cond_c"], 60.0)
        self.assertAlmostEqual(p["q_rad_mw"], p["q_cond_mw"], delta=0.02 * p["q_cond_mw"])
        self.assertGreater(p["efficiency"], 0.25)

    def test_oversized_radiator_is_cold_limited(self):
        from rankine import solve_condenser
        p = solve_condenser(5.0, 300.0, 8000.0, 1.0)
        self.assertFalse(p["balanced"])
        self.assertIn("oversized", p["reason"])
        self.assertEqual(p["t_cond_c"], 5.0)

    def test_small_radiator_reports_required_area(self):
        from rankine import solve_condenser
        p = solve_condenser(5.0, 300.0, 2000.0, 1.0)
        self.assertFalse(p["balanced"])
        self.assertIn("too small", p["reason"])
        self.assertGreater(p["required_area_m2"], 2000.0)


@unittest.skipUnless(HAVE_IAPWS, "module import needs iapws")
class BraytonTests(unittest.TestCase):
    def test_helium_cycle_is_plausible(self):
        from rankine import brayton_helium
        p = brayton_helium(3.0, 900.0)
        self.assertGreater(p["efficiency"], 0.20)
        self.assertLess(p["efficiency"], 0.45)
        self.assertGreater(p["net_kj_kg"], 0.0)

    def test_regenerator_improves_efficiency(self):
        from rankine import brayton_helium
        plain = brayton_helium(3.0, 900.0)
        regen = brayton_helium(3.0, 900.0, regenerator_eff=0.8)
        self.assertGreater(regen["efficiency"], plain["efficiency"])

    def test_higher_pressure_ratio_improves_efficiency(self):
        from rankine import brayton_helium
        low = brayton_helium(2.0, 900.0)
        high = brayton_helium(4.0, 900.0)
        self.assertGreater(high["efficiency"], low["efficiency"])

    def test_energy_balance(self):
        from rankine import brayton_helium
        p = brayton_helium(3.0, 900.0, regenerator_eff=0.5)
        self.assertAlmostEqual(p["q_in_kj_kg"] - p["q_reject_kj_kg"],
                               p["net_kj_kg"], delta=1e-9)


@unittest.skipUnless(HAVE_IAPWS, "module import needs iapws")
class RadiatorAreaTests(unittest.TestCase):
    def test_hotter_radiator_needs_less_area(self):
        from rankine import radiator_area_m2
        cold = radiator_area_m2(2.0, 400.0)
        hot = radiator_area_m2(2.0, 800.0)
        self.assertLess(hot, cold)

    def test_helium_exhaust_is_compact(self):
        from rankine import brayton_helium
        p = brayton_helium(3.0, 900.0, regenerator_eff=0.8)
        self.assertLess(p["radiator_area_m2_per_mw"], 200.0)

    def test_water_condenser_is_large(self):
        from rankine import solve_condenser
        p = solve_condenser(5.0, 300.0, 5000.0, 1.0)
        self.assertTrue(p["balanced"])
        self.assertGreater(p["required_area_m2"], 1000.0)
