#!/usr/bin/env python3
"""M1: the limited-delta-v intercept plan, mirrored from the mod.

The mod (``org.cdere.intercept-planner``) computes, for every arrival time the
player asks about:

    r  = target position - my position            (km)
    v  = target velocity - my velocity            (km/s)
    required closing speed  V = |r| / t
    required delta-v        dv = |v + r/t|

``v + r/t`` is the change in *relative* velocity needed to make the relative
velocity point straight down the line of sight, i.e. to put the ship on a
collision course, in the no-gravity linear approximation.  This module mirrors
the arithmetic so the mod's numbers can be checked without the game; it is not
a trajectory optimiser and knows nothing about gravity, finite burns, or plane
changes (see notes/M1-PLANNER.md).
"""

from __future__ import annotations

import math

def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])

def _scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)

def _len(a):
    return math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])

def plan(r_km, v_kms, hours):
    """Return {'ok', 't_s', 'range_km', 'closing_kms', 'dv_kms'}."""
    t = hours * 3600.0
    out = {"ok": False, "t_s": t, "range_km": _len(r_km),
           "closing_kms": 0.0, "dv_kms": 0.0}
    if t <= 0.0:
        return out
    d = _add(v_kms, _scale(r_km, 1.0 / t))
    out["closing_kms"] = _len(r_km) / t
    out["dv_kms"] = _len(d)
    out["ok"] = math.isfinite(out["dv_kms"])
    return out

def plan_table(r_km, v_kms, horizons_h):
    return [plan(r_km, v_kms, h) for h in horizons_h]
