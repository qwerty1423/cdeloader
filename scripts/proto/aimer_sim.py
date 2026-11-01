#!/usr/bin/env python3
"""M24 spike: the game's turret aim-point wander, and the mod's clamp.

This mirrors the two decompiled layers (notes/M24-LASER-JITTER.md) so the mod's
behaviour can be checked without the game:

  RandomAimer::GetRandomAimOffset_km   scatter sized by the target's hull
  WeaponCompartment::GetAimerOffset_km adds a spot-sized scatter, and its 4th
                                       argument is the range in km
  this mod                             bounds the total offset in angle

Everything here is a model of observed code, not a measurement of the game; the
constants (0.5 arcsec default, 1.22 lambda/D diffraction) are documented in the
note.
"""

from __future__ import annotations

import math

ARCSEC_RAD = math.pi / (180.0 * 3600.0)          # radians per arcsecond
DEFAULT_ARCSEC = 0.5                             # mod default

def _norm(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])

def _scale(v, s):
    return (v[0] * s, v[1] * s, v[2] * s)

def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])

def random_aim_offset_km(half_extent_km: float, tight_radius_km: float,
                         t_km: float, u: float, r_unit, dir_unit) -> tuple:
    """`RandomAimer::GetRandomAimOffset_km` (0x663fd0), decoded.

    half_extent_km  Body bounding cylinder length / 2 (`Body+0x178 * 0.5`)
    tight_radius_km Body tight radius, km (`Body+0x188`)
    t_km            line-of-sight term from the caller (angle * range)
    u               T oscillator, normalised to [0, 1]
    r_unit          uniform unit vector (Theta/Z oscillators)
    dir_unit        target direction vector from the Body accessor
    """
    d0 = max(0.0, tight_radius_km - t_km)
    d = max(0.0, half_extent_km - d0)
    along = _scale(dir_unit, (2.0 * u - 1.0) * d)
    return _add(along, _scale(r_unit, d0))

def diffraction_half_angle(laser_nm: float, aperture_m: float) -> float:
    """1.22 * lambda / (2 D) — the game's own beam half-angle (0x68c610)."""
    return laser_nm * 1e-9 * 1.22 / (2.0 * aperture_m)

def get_aimer_offset_km(half_extent_km: float, tight_radius_km: float,
                        range_km: float, u: float, r_unit, dir_unit,
                        spot_unit, laser_nm: float = 380.0,
                        aperture_m: float = 0.1) -> tuple:
    """`WeaponCompartment::GetAimerOffset_km` (0x6615e0): layer 1 + spot scatter."""
    # The caller passes t = (angle/2) * range; use the full diffraction half
    # angle as the amplitude (the sin() draw only shrinks it).
    t = diffraction_half_angle(laser_nm, aperture_m) * range_km
    base = random_aim_offset_km(half_extent_km, tight_radius_km, t, u, r_unit,
                                dir_unit)
    spot = _scale(spot_unit, math.tan(diffraction_half_angle(laser_nm, aperture_m) / 2.0) * range_km)
    return _add(base, spot)

def angle_arcsec(offset_km, range_km: float) -> float:
    """Angular size of an aim-point offset seen from the shooter."""
    if range_km <= 0.0:
        return 0.0
    return _norm(offset_km) / range_km / ARCSEC_RAD

def apply_mod(offset_km, range_km: float, *, enabled: bool = True,
              absolute: bool = True, arcsec: float = DEFAULT_ARCSEC,
              scale: float = 0.1):
    """The mod's post-processing of the returned offset."""
    if not enabled or range_km <= 0.0 or range_km >= 1e9:
        return offset_km
    if absolute:
        limit = range_km * math.tan(min(max(arcsec, 0.01), 60.0) * ARCSEC_RAD)
        n = _norm(offset_km)
        if n > limit and n > 0.0:
            return _scale(offset_km, limit / n)
        return offset_km
    return _scale(offset_km, min(max(scale, 0.0), 1.0))
