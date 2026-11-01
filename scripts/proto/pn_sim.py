#!/usr/bin/env python3
"""M1 spike: 3-D proportional-navigation simulator (pure vs augmented PN).

This is the tuning sandbox the roadmap asks for before touching the game's
midcourse guidance.  Point-mass interceptor, constant-acceleration target,
augmented PN law:

    a_cmd = N * V_c * lambda_dot + 0.5 * a_target

where `V_c` is the closing velocity and `lambda_dot` the LOS rotation vector.
The second term compensates a target that keeps accelerating during a long burn
(the case stock midcourse guidance misses for fusion missiles).
"""

from __future__ import annotations

import argparse
import json
import math


def _sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def _mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def _dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])
def _norm(a): return math.sqrt(_dot(a, a))


def simulate(target_pos, target_vel, target_acc, interceptor_pos,
             interceptor_speed, n_law=4.0, augmented=True, max_accel=50.0,
             dt=0.01, max_time=600.0) -> dict:
    """Run one engagement; accelerations in m/s^2, distances in metres."""
    ip = interceptor_pos
    tp = target_pos
    tv = target_vel
    ta = target_acc
    # Initial heading: straight at the target.
    r = _sub(tp, ip)
    dist = _norm(r)
    if dist <= 0.0:
        raise ValueError("interceptor starts on the target")
    iv = _mul(r, interceptor_speed / dist)
    t = 0.0
    min_dist = dist
    while t < max_time:
        r = _sub(tp, ip)
        dist = _norm(r)
        if dist < min_dist:
            min_dist = dist
        if dist < 1.0:
            break
        v_rel = _sub(tv, iv)
        closing = -_dot(r, v_rel) / dist
        if closing <= 0.0 and t > 1.0:
            break  # opening: closest approach already passed
        # LOS rotation vector: (r x v_rel) / |r|^2
        lam_dot = _mul(_cross(r, v_rel), 1.0 / (dist * dist))
        r_hat = _mul(r, 1.0 / dist)
        # PN acceleration is N*Vc*(omega x r_hat): in the engagement plane,
        # perpendicular to the LOS.  (Applying omega directly is out of plane.)
        acc = _mul(_cross(lam_dot, r_hat), n_law * closing)
        if augmented:
            acc = _add(acc, _mul(ta, 0.5))
        an = _norm(acc)
        if an > max_accel:
            acc = _mul(acc, max_accel / an)
        iv = _add(iv, _mul(acc, dt))
        ip = _add(ip, _mul(iv, dt))
        tv = _add(tv, _mul(ta, dt))
        tp = _add(tp, _mul(tv, dt))
        t += dt
    return {
        "miss_m": min_dist,
        "time_s": t,
        "n_law": n_law,
        "augmented": augmented,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="3-D PN tuning simulator (M1)")
    ap.add_argument("--n", type=float, default=4.0, help="navigation constant")
    ap.add_argument("--target-accel", type=float, default=50.0,
                    help="target acceleration, m/s^2")
    ap.add_argument("--speed", type=float, default=3000.0,
                    help="interceptor speed, m/s")
    ap.add_argument("--max-accel", type=float, default=50.0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    target_pos = (200_000.0, 30_000.0, 0.0)
    target_vel = (-1000.0, 200.0, 0.0)
    target_acc = (0.0, args.target_accel, 0.0)
    start = (0.0, 0.0, 0.0)

    rows = []
    for augmented in (False, True):
        rows.append(simulate(target_pos, target_vel, target_acc, start,
                             args.speed, n_law=args.n, augmented=augmented,
                             max_accel=args.max_accel))
    if args.json:
        print(json.dumps(rows, indent=2))
        return 0
    print(f"target accel {args.target_accel:.0f} m/s^2, N={args.n}, "
          f"interceptor {args.speed:.0f} m/s")
    for r in rows:
        print(f"{'augmented' if r['augmented'] else 'pure PN':10s} "
              f"miss {r['miss_m']:9.1f} m  time {r['time_s']:6.1f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
