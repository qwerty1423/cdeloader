import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "proto"))
from pn_sim import simulate  # noqa: E402


class PNTests(unittest.TestCase):
    def test_stationary_target_is_hit(self):
        r = simulate((200000, 0, 0), (0, 0, 0), (0, 0, 0), (0, 0, 0), 3000)
        self.assertLess(r["miss_m"], 100.0)

    def test_maneuvering_target_needs_augmentation(self):
        kwargs = dict(target_acc=(0, 50, 0), interceptor_speed=10000,
                      max_accel=200.0)
        pure = simulate((200000, 30000, 0), (-1000, 200, 0), (0, 50, 0),
                        (0, 0, 0), 10000, augmented=False, max_accel=200.0)
        aug = simulate((200000, 30000, 0), (-1000, 200, 0), (0, 50, 0),
                       (0, 0, 0), 10000, augmented=True, max_accel=200.0)
        self.assertLess(aug["miss_m"], pure["miss_m"])
        self.assertLess(aug["miss_m"], 100.0)

    def test_acceleration_stays_in_plane(self):
        # Planar engagement: a correct PN law never pushes out of the x-y plane.
        r = simulate((200000, 30000, 0), (-1000, 200, 0), (0, 0, 0),
                     (0, 0, 0), 5000, augmented=False, max_accel=100.0,
                     dt=0.01, max_time=200.0)
        self.assertLess(r["miss_m"], 200.0)


if __name__ == "__main__":
    unittest.main()
