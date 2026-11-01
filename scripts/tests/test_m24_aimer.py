#!/usr/bin/env python3
"""M24: the aim-wander model and the laser-jitter mod's clamp."""

from __future__ import annotations

import math
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))

import aimer_sim as A  # noqa: E402


def unit(rng):
    v = (rng.gauss(0, 1), rng.gauss(0, 1), rng.gauss(0, 1))
    n = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) or 1.0
    return (v[0] / n, v[1] / n, v[2] / n)


def sample(rng, half_extent, tight_radius, range_km):
    return A.get_aimer_offset_km(half_extent, tight_radius, range_km,
                                 u=rng.random(), r_unit=unit(rng),
                                 dir_unit=unit(rng), spot_unit=unit(rng))


class ConversionTests(unittest.TestCase):
    def test_arcsec_constant(self):
        # 1 rad = 206 264.8 arcsec
        self.assertAlmostEqual(1.0 / A.ARCSEC_RAD, 206264.806, places=3)

    def test_diffraction_limit(self):
        # 1.22 * 380 nm / (2 * 0.1 m) = 2.318 urad = 0.478 arcsec
        half = A.diffraction_half_angle(380.0, 0.1)
        self.assertAlmostEqual(half, 2.318e-6, places=9)
        self.assertAlmostEqual(half / A.ARCSEC_RAD, 0.478, places=2)

    def test_default_budget_in_metres(self):
        # 0.5 arcsec at 1000 km is 2.42 m; at 25 000 km it is 60.6 m
        self.assertAlmostEqual(1000.0 * math.tan(A.DEFAULT_ARCSEC * A.ARCSEC_RAD), 2.424068e-3, places=9)
        self.assertAlmostEqual(25000.0 * math.tan(A.DEFAULT_ARCSEC * A.ARCSEC_RAD), 0.0606, places=4)


class StockModelTests(unittest.TestCase):
    def setUp(self):
        self.rng = random.Random(1234)

    def test_stock_wander_is_bounded_by_the_hull(self):
        # |offset| <= half extent + tight radius; never larger than the target.
        for _ in range(500):
            off = A.random_aim_offset_km(0.05, 0.04, 0.0, self.rng.random(),
                                         unit(self.rng), unit(self.rng))
            self.assertLessEqual(A._norm(off), 0.05 + 0.04 + 1e-12)

    def test_stock_wander_shrinks_with_the_line_of_sight_term(self):
        # Large t eats the sphere of random directions but keeps the axis term.
        off = A.random_aim_offset_km(0.05, 0.04, 0.04, 0.5,
                                     (0.0, 0.0, 1.0), (1.0, 0.0, 0.0))
        self.assertAlmostEqual(A._norm(off), 0.0, places=12)

    def test_stock_angle_is_large_at_close_range(self):
        # A 100 m long target at 200 km: stock error above 10 arcsec.
        errs = [A.angle_arcsec(sample(self.rng, 0.05, 0.02, 200.0), 200.0)
                for _ in range(200)]
        self.assertGreater(max(errs), 10.0)
        self.assertGreater(max(errs), 20.0)

    def test_stock_angle_is_small_at_long_range(self):
        # The same target at 25 000 km is already about a beam half-width: the
        # hull term collapses to ~0.4 arcsec and the spot term is ~0.24 arcsec.
        errs = [A.angle_arcsec(sample(self.rng, 0.05, 0.02, 25000.0), 25000.0)
                for _ in range(200)]
        self.assertLess(max(errs), 1.0)


class ModTests(unittest.TestCase):
    def setUp(self):
        self.rng = random.Random(99)

    def test_absolute_mode_bounds_the_angle(self):
        for arcsec in (0.05, 0.2, 0.5, 3.0, 20.0):
            for range_km in (10.0, 200.0, 5000.0, 25000.0):
                for _ in range(60):
                    stock = sample(self.rng, 0.3, 0.2, range_km)
                    out = A.apply_mod(stock, range_km, arcsec=arcsec)
                    self.assertLessEqual(A.angle_arcsec(out, range_km),
                                         arcsec + 1e-6)

    def test_absolute_mode_never_amplifies(self):
        for range_km in (100.0, 10000.0):
            for _ in range(200):
                stock = sample(self.rng, 0.02, 0.01, range_km)
                out = A.apply_mod(stock, range_km, arcsec=0.5)
                self.assertLessEqual(A._norm(out), A._norm(stock) + 1e-15)

    def test_absolute_mode_is_inert_when_stock_is_already_tight(self):
        # 1 m offset at 25 000 km = 0.008 arcsec < 0.5: untouched.
        stock = (0.001, 0.0, 0.0)
        self.assertIs(A.apply_mod(stock, 25000.0), stock)

    def test_absolute_mode_is_strongest_up_close(self):
        # At 100 km a 50 m scatter is 103 arcsec; the clamp cuts it to 0.5.
        stock = (0.05, 0.0, 0.0)
        out = A.apply_mod(stock, 100.0, arcsec=0.5)
        self.assertAlmostEqual(A.angle_arcsec(out, 100.0), 0.5, places=6)
        self.assertLess(A._norm(out), A._norm(stock) / 100.0)

    def test_relative_mode_scales(self):
        stock = (0.05, 0.0, 0.0)
        out = A.apply_mod(stock, 1000.0, absolute=False, scale=0.1)
        self.assertAlmostEqual(A._norm(out), 0.005, places=12)

    def test_disabled_is_a_no_op(self):
        stock = (0.05, 0.0, 0.0)
        self.assertEqual(A.apply_mod(stock, 100.0, enabled=False), stock)

    def test_larger_arcsec_is_weaker(self):
        stock = (0.05, 0.0, 0.0)
        tight = A._norm(A.apply_mod(stock, 1000.0, arcsec=0.2))
        loose = A._norm(A.apply_mod(stock, 1000.0, arcsec=2.0))
        self.assertLess(tight, loose)
        self.assertLess(loose, stock[0])
        # Both are the requested budget: 1000 km * tan(arcsec)
        self.assertAlmostEqual(tight, 1000.0 * math.tan(0.2 * A.ARCSEC_RAD), places=9)
        self.assertAlmostEqual(loose, 1000.0 * math.tan(2.0 * A.ARCSEC_RAD), places=9)

    def test_bad_range_passes_through(self):
        # A zero or absurd range means the argument layout is not what the mod
        # expects; the detour must not touch the offset.
        stock = (0.05, 0.0, 0.0)
        self.assertEqual(A.apply_mod(stock, 0.0), stock)
        self.assertEqual(A.apply_mod(stock, 1e12), stock)


if __name__ == "__main__":
    unittest.main()
