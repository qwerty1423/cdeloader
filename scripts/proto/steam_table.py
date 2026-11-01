#!/usr/bin/env python3
"""M10 spike: compact steam table for a future in-game Rankine port.

The game's flow model is ideal-gas only, so a real water/steam path needs
properties.  Rather than porting all of IAPWS-IF97 into the mod, generate a
table offline and interpolate:

    h(P, T), s(P, T), v(P, T)

The generator writes JSON (and can dump CSV); `SteamTable` validates itself
against IAPWS-IF97 at random points.  The eventual C++ port can embed the same
grid or ship it as a data file.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys

try:
    from iapws import IAPWS97
except ImportError:  # pragma: no cover
    IAPWS97 = None

# Grid: log-spaced pressure, linear temperature.  Kept small enough to embed.
PRESSURES_MPA = [0.001, 0.002, 0.003, 0.005, 0.008, 0.01, 0.02, 0.03, 0.05,
                 0.08, 0.1, 0.2, 0.3, 0.5, 0.8, 1.0, 2.0, 3.0, 5.0, 8.0, 10.0,
                 15.0, 20.0, 22.0]
TEMPERATURES_C = list(range(50, 751, 10))


def _state(p_mpa: float, t_c: float):
    return IAPWS97(P=p_mpa, T=t_c + 273.15)


def build(pressures=PRESSURES_MPA, temperatures=TEMPERATURES_C) -> dict:
    """Separate saturation line and superheated grid (no phase-crossing)."""
    if IAPWS97 is None:
        raise RuntimeError("iapws is required (.venv/bin/pip install iapws)")
    sat = {}
    superheated = {}
    for p in pressures:
        try:
            liq = IAPWS97(P=p, x=0)
            vap = IAPWS97(P=p, x=1)
        except Exception:
            continue
        sat["%.6g" % p] = {
            "tsat_c": liq.T - 273.15,
            "hf": liq.h, "hg": vap.h,
            "sf": liq.s, "sg": vap.s,
            "vf": liq.v, "vg": vap.v,
        }
        for t in temperatures:
            if t < sat["%.6g" % p]["tsat_c"] + 5.0:
                continue
            try:
                st = IAPWS97(P=p, T=t + 273.15)
            except Exception:
                continue
            if not all(math.isfinite(x) for x in (st.h, st.s, st.v)):
                continue
            superheated.setdefault("%.6g" % p, {})["%.6g" % t] = {
                "h": st.h, "s": st.s, "v": st.v,
            }
    return {"pressures_mpa": pressures, "temperatures_c": temperatures,
            "sat": sat, "superheated": superheated}


class SteamTable:
    def __init__(self, data: dict):
        self.sat = {float(k): v for k, v in data["sat"].items()}
        self.super = {float(p): {float(t): v for t, v in row.items()}
                      for p, row in data["superheated"].items()}
        self.pressures = sorted(self.sat)

    def saturation(self, p: float) -> dict:
        """Clamp to the nearest tabulated pressure (no interpolation across P)."""
        p = min(max(p, self.pressures[0]), self.pressures[-1])
        best = min(self.pressures, key=lambda q: abs(q - p))
        return self.sat[best]

    def superheated_lookup(self, p: float, t: float, prop: str = "h") -> float:
        """Bilinear interpolation in the superheated region; clamps to edges."""
        ps = sorted(p for p in self.super if len(self.super[p]) >= 2)
        if not ps:
            raise ValueError("no superheated data")
        p = min(max(p, ps[0]), ps[-1])
        if t < self.saturation(p)["tsat_c"]:
            raise ValueError("temperature is below saturation; use the saturation table")

        def bracket(vals, x):
            for i in range(len(vals) - 1):
                if vals[i] <= x <= vals[i + 1]:
                    return i, i + 1, (x - vals[i]) / (vals[i + 1] - vals[i])
            return len(vals) - 1, len(vals) - 1, 0.0

        logs = [math.log(q) for q in ps]
        i0, i1, fx = bracket(logs, math.log(p))
        ts0 = sorted(self.super[ps[i0]])
        ts1 = sorted(self.super[ps[i1]])
        common = [t for t in ts0 if t in ts1]
        if len(common) < 2:
            raise ValueError("grid rows do not overlap")
        t = min(max(t, common[0]), common[-1])
        j0, j1, fy = bracket(common, t)
        v00 = self.super[ps[i0]][common[j0]][prop]
        v10 = self.super[ps[i1]][common[j0]][prop]
        v01 = self.super[ps[i0]][common[j1]][prop]
        v11 = self.super[ps[i1]][common[j1]][prop]
        a = v00 + (v10 - v00) * fx
        b = v01 + (v11 - v01) * fx
        return a + (b - a) * fy


def validate(data: dict, samples: int = 200) -> dict:
    table = SteamTable(data)
    random.seed(1)
    worst = {"h": 0.0, "s": 0.0}
    for _ in range(samples):
        p = random.uniform(0.01, 15.0)
        t = random.uniform(250.0, 600.0)
        # Validate away from the saturation boundary; interpolation across the
        # phase edge is deliberately not supported.
        if t < table.saturation(p)["tsat_c"] + 30.0:
            continue
        row = table.super.get(min(table.super, key=lambda q: abs(q - p)))
        if not row or t < min(row) or t > max(row):
            continue
        try:
            st = IAPWS97(P=p, T=t + 273.15)
        except Exception:
            continue
        if getattr(st, "phase", "") == "Two phases":
            continue
        for prop in ("h", "s"):
            raw = getattr(st, prop)
            if not math.isfinite(raw) or abs(raw) < 1e-6:
                continue
            err = abs(table.superheated_lookup(p, t, prop) - raw) / abs(raw)
            worst[prop] = max(worst[prop], err)
    return worst


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="generate a compact steam table (M10)")
    ap.add_argument("--out", default=None, help="write JSON to this path")
    ap.add_argument("--csv", default=None, help="write CSV to this path")
    args = ap.parse_args(argv)
    data = build()
    errors = validate(data)
    print("worst relative error vs IAPWS-IF97: h %.4f%%, s %.4f%%"
          % (errors["h"] * 100.0, errors["s"] * 100.0))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(data, fh, indent=1)
        print("wrote", args.out)
    if args.csv:
        with open(args.csv, "w", encoding="utf-8") as fh:
            fh.write("p_mpa,t_c,h_kj_kg,s_kj_kgk,v_m3_kg\n")
            for p_key, vals in data["sat"].items():
                fh.write("sat,%s,%.4f,%.4f,%.4f,%.6f,%.6f\n" % (
                    p_key, vals["tsat_c"], vals["hf"], vals["hg"], vals["sf"], vals["sg"]))
            for p_key, row in data["superheated"].items():
                for t_key, vals in row.items():
                    fh.write("%s,%s,%.6f,%.6f,%.9g\n" % (p_key, t_key,
                                                       vals["h"], vals["s"], vals["v"]))
        print("wrote", args.csv)
    return 0 if errors["h"] < 0.02 and errors["s"] < 0.05 else 1


if __name__ == "__main__":
    sys.exit(main())
