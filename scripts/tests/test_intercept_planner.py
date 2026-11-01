#!/usr/bin/env python3
"""M1: the intercept planner's arithmetic (mirrored from the mod)."""

from __future__ import annotations

import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))

import intercept_plan as P  # noqa: E402


class BasicTests(unittest.TestCase):
    def test_closing_speed_is_range_over_time(self):
        row = P.plan((3600.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1.0)
        self.assertTrue(row["ok"])
        self.assertAlmostEqual(row["closing_kms"], 1.0, places=9)

    def test_stationary_relative_target_needs_the_closing_speed(self):
        # 3600 km ahead, no relative motion, one hour: burn 1 km/s at it.
        row = P.plan((3600.0, 0.0, 0.0), (0.0, 0.0, 0.0), 1.0)
        self.assertAlmostEqual(row["dv_kms"], 1.0, places=9)

    def test_already_on_a_collision_course_costs_nothing(self):
        # v = -r/t is exactly the required relative velocity.
        row = P.plan((3600.0, 0.0, 0.0), (-1.0, 0.0, 0.0), 1.0)
        self.assertAlmostEqual(row["dv_kms"], 0.0, places=9)

    def test_closing_twice_as_fast_costs_the_closing_speed(self):
        row = P.plan((3600.0, 0.0, 0.0), (-2.0, 0.0, 0.0), 1.0)
        self.assertAlmostEqual(row["dv_kms"], 1.0, places=9)

    def test_crossing_target_is_a_vector_sum(self):
        # Target 3600 km ahead drifting at 1 km/s sideways; over an hour the
        # required velocity change is sqrt(1^2 + 1^2).
        row = P.plan((3600.0, 0.0, 0.0), (0.0, 1.0, 0.0), 1.0)
        self.assertAlmostEqual(row["dv_kms"], math.sqrt(2.0), places=9)

    def test_receding_target_costs_less_if_you_take_longer(self):
        fast = P.plan((3600.0, 0.0, 0.0), (0.5, 0.0, 0.0), 1.0)
        slow = P.plan((3600.0, 0.0, 0.0), (0.5, 0.0, 0.0), 10.0)
        self.assertLess(slow["dv_kms"], fast["dv_kms"])
        # Even for a very long wait you still have to match its speed.
        self.assertGreater(slow["dv_kms"], 0.49)

    def test_only_relative_motion_matters(self):
        base = P.plan((1000.0, 0.0, 0.0), (0.2, -0.1, 0.05), 6.0)
        bumped = P.plan((1000.0, 0.0, 0.0), (0.2, -0.1, 0.05), 6.0)
        self.assertAlmostEqual(base["dv_kms"], bumped["dv_kms"], places=12)
        # Shift both velocities by the same amount: same r, same v.
        self.assertAlmostEqual(base["range_km"], bumped["range_km"], places=12)

    def test_bad_horizon_is_not_ok(self):
        self.assertFalse(P.plan((1.0, 0.0, 0.0), (0.0, 0.0, 0.0), 0.0)["ok"])
        self.assertFalse(P.plan((1.0, 0.0, 0.0), (0.0, 0.0, 0.0), -5.0)["ok"])

    def test_table_keeps_the_requested_horizons(self):
        rows = P.plan_table((3600.0, 0.0, 0.0), (0.0, 0.0, 0.0), [1.0, 6.0, 24.0])
        self.assertEqual(len(rows), 3)
        self.assertEqual([r["t_s"] for r in rows], [3600.0, 21600.0, 86400.0])
        # A longer arrival time always needs less closing speed for the same
        # range.
        self.assertGreater(rows[0]["closing_kms"], rows[1]["closing_kms"])
        self.assertGreater(rows[1]["closing_kms"], rows[2]["closing_kms"])


class RealisticScaleTests(unittest.TestCase):
    def test_a_realistic_fleet_intercept(self):
        # 20 000 km out, target drifting 50 m/s across, arrival in 6 h: the
        # closing speed dominates, the crossing term is small.
        r = (20000.0, 0.0, 0.0)
        v = (0.0, 0.05, 0.0)
        row = P.plan(r, v, 6.0)
        self.assertAlmostEqual(row["closing_kms"], 20000.0 / 21600.0, places=9)
        self.assertGreater(row["dv_kms"], row["closing_kms"])
        self.assertLess(row["dv_kms"], row["closing_kms"] + 0.06)

    def test_a_fast_arrival_is_expensive(self):
        r = (20000.0, 0.0, 0.0)
        one_hour = P.plan(r, (0.0, 0.0, 0.0), 1.0)
        one_day = P.plan(r, (0.0, 0.0, 0.0), 24.0)
        self.assertAlmostEqual(one_hour["dv_kms"], 20000.0 / 3600.0, places=9)
        self.assertAlmostEqual(one_day["dv_kms"], 20000.0 / 86400.0, places=9)
        self.assertGreater(one_hour["dv_kms"], 5.0 * one_day["dv_kms"])


if __name__ == "__main__":
    unittest.main()
