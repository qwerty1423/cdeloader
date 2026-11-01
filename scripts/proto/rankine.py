#!/usr/bin/env python3
"""M10 spike: water Rankine cycle for the turboelectric generator.

Prototype only: this is the thermodynamic model we would port into the game if
the numbers look sane.  It uses IAPWS-IF97 (``iapws``) for water properties and
models a simple cycle:

    pump -> boiler (P_boiler, T_boiler) -> turbine (eta_t) -> condenser
    (P_cond) -> pump

Coolant/working fluid assumptions: liquid water is pumped to boiler pressure,
boiled/superheated, expanded to condenser pressure, condensed against the ship's
radiators, and returned.  The interesting question for CoaDE is not the exact
efficiency but whether a real Rankine cycle can replace the blackbox
``P = Q * eta_carnot * fraction`` and produce plausible power/heat at the
temperatures the game's reactors and radiators allow.

Run:
    .venv/bin/python scripts/proto/rankine.py --table
    .venv/bin/python scripts/proto/rankine.py --p-boiler 5 --t-boiler 300
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass

try:
    from iapws import IAPWS97
except ImportError:  # pragma: no cover - environment hint
    IAPWS97 = None


@dataclass
class Result:
    p_boiler_mpa: float
    t_boiler_c: float
    p_cond_mpa: float
    t_cond_c: float
    pump_work_kj_kg: float
    boiler_heat_kj_kg: float
    turbine_work_kj_kg: float
    condenser_heat_kj_kg: float
    thermal_efficiency: float
    carnot_efficiency: float
    specific_net_work_kj_kg: float
    mass_flow_for_1mw_kg_s: float


def _sat_liquid(p_mpa: float):
    return IAPWS97(P=p_mpa, x=0)


def _sat_vapor(p_mpa: float):
    return IAPWS97(P=p_mpa, x=1)


def _state_tp(t_c: float, p_mpa: float):
    return IAPWS97(T=t_c + 273.15, P=p_mpa)


def rankine(p_boiler_mpa: float, t_boiler_c: float, p_cond_mpa: float,
            eta_turbine: float = 0.85, eta_pump: float = 0.75,
            condenser_subcool_c: float = 2.0) -> Result:
    """Compute one steady-state cycle (all specific values in kJ/kg)."""
    if IAPWS97 is None:
        raise RuntimeError("iapws is required: .venv/bin/pip install iapws")
    if p_boiler_mpa <= p_cond_mpa:
        raise ValueError("boiler pressure must exceed condenser pressure")

    h1 = _sat_liquid(p_cond_mpa).h
    t_cond_c = _sat_liquid(p_cond_mpa).T - 273.15 - condenser_subcool_c
    # Pump: incompressible liquid, v * dp, plus pump efficiency.
    v1 = 1.0 / _sat_liquid(p_cond_mpa).rho  # m3/kg
    pump = v1 * (p_boiler_mpa - p_cond_mpa) * 1000.0 / eta_pump  # kPa*m3/kg = kJ/kg
    h2 = h1 + pump

    h3 = _state_tp(t_boiler_c, p_boiler_mpa).h
    s3 = _state_tp(t_boiler_c, p_boiler_mpa).s

    # Isentropic expansion to condenser pressure.
    st = IAPWS97(P=p_cond_mpa, s=s3)
    h4s = st.h
    h4 = h3 - eta_turbine * (h3 - h4s)

    turbine = h3 - h4
    condenser = h4 - h1
    boiler = h3 - h2
    net = turbine - pump
    if boiler <= 0.0:
        raise ValueError("boiler heat is not positive; check T_boiler vs P_boiler")

    t_hot = t_boiler_c + 273.15
    t_cold = _sat_liquid(p_cond_mpa).T
    carnot = 1.0 - t_cold / t_hot

    return Result(
        p_boiler_mpa=p_boiler_mpa,
        t_boiler_c=t_boiler_c,
        p_cond_mpa=p_cond_mpa,
        t_cond_c=t_cond_c,
        pump_work_kj_kg=pump,
        boiler_heat_kj_kg=boiler,
        turbine_work_kj_kg=turbine,
        condenser_heat_kj_kg=condenser,
        thermal_efficiency=net / boiler,
        carnot_efficiency=carnot,
        specific_net_work_kj_kg=net,
        mass_flow_for_1mw_kg_s=1000.0 / net if net > 0 else float("inf"),
    )


def brayton_helium(p_ratio: float, t_hot_c: float, t_cold_c: float = 30.0,
                   eta_turbine: float = 0.90, eta_compressor: float = 0.85,
                   regenerator_eff: float = 0.0) -> dict:
    """Closed helium Brayton cycle (ideal gas, cp constant).

    This is the gas-coolant branch of the generator: compressor -> recuperator
    (optional) -> reactor heat -> turbine -> recuperator -> cooler.
    """
    cp = 5.193  # kJ/kg/K, helium
    gamma = 5.0 / 3.0
    t1 = t_cold_c + 273.15
    t3 = t_hot_c + 273.15
    if p_ratio <= 1.0 or t3 <= t1:
        raise ValueError("invalid pressure ratio or temperatures")
    k = (gamma - 1.0) / gamma
    t2 = t1 * (1.0 + (p_ratio ** k - 1.0) / eta_compressor)
    t4 = t3 * (1.0 - eta_turbine * (1.0 - p_ratio ** (-k)))
    w_t = cp * (t3 - t4)
    w_c = cp * (t2 - t1)
    # Recuperator: transfer from turbine exhaust (t4) to compressor outlet (t2).
    q_regen = 0.0
    if regenerator_eff > 0.0 and t4 > t2:
        q_regen = regenerator_eff * cp * (t4 - t2)
    q_in = cp * (t3 - t2) - q_regen
    q_out = cp * (t4 - t1) - q_regen
    net = w_t - w_c
    return {
        "p_ratio": p_ratio,
        "t_hot_c": t_hot_c,
        "t_cold_c": t_cold_c,
        "t2_k": t2,
        "t4_k": t4,
        "turbine_kj_kg": w_t,
        "compressor_kj_kg": w_c,
        "net_kj_kg": net,
        "q_in_kj_kg": q_in,
        "q_reject_kj_kg": q_out,
        "efficiency": net / q_in if q_in > 0 else 0.0,
        "carnot": 1.0 - t1 / t3,
        "mass_flow_for_1mw_kg_s": 1000.0 / net if net > 0 else float("inf"),
        "radiator_area_m2_per_mw": (radiator_area_m2(q_out / (net / 1000.0) / 1000.0,
                                                     t4) if net > 0 and q_out > 0 else float("inf")),
    }


SIGMA = 5.670374419e-8  # W/m2/K4


def saturation_pressure_mpa(t_c: float) -> float:
    """Saturation pressure of water at t_c (IAPWS-IF97)."""
    return IAPWS97(T=t_c + 273.15, x=0).P


def radiator_capacity_w(area_m2: float, t_rad_k: float, emissivity: float = 0.9,
                        t_space_k: float = 3.0) -> float:
    """Blackbody rejection of a two-sided? no: one-sided projected area."""
    return (emissivity * SIGMA * area_m2 *
            (t_rad_k ** 4 - t_space_k ** 4))


def radiator_area_m2(q_mw: float, t_rad_k: float, emissivity: float = 0.9,
                     t_space_k: float = 3.0) -> float:
    """Area needed to reject q_mw at t_rad_k (one-sided blackbody)."""
    per_m2 = radiator_capacity_w(1.0, t_rad_k, emissivity, t_space_k)
    if per_m2 <= 0.0:
        return float("inf")
    return q_mw * 1.0e6 / per_m2


def solve_condenser(p_boiler_mpa: float, t_boiler_c: float, area_m2: float,
                    power_mw: float = 1.0, emissivity: float = 0.9,
                    approach_k: float = 5.0) -> dict:
    """Find the condenser temperature where radiator rejection balances Q_cond.

    The cycle runs with p_cond = psat(T_cond); the radiator runs at
    T_cond + approach and must reject the condenser duty of `power_mw`.
    The net heat `q_rad - q_cond` can change sign twice (near the cold bound and
    near the critical point), so scan for the first + to - crossing (the stable
    operating point) and bisect inside it.
    """
    target_w = power_mw * 1.0e6
    t_sat_boiler = IAPWS97(P=p_boiler_mpa, x=0).T - 273.15
    lo = 5.0
    hi = min(250.0, t_sat_boiler - 1.0)

    def evaluate(t_cond_c: float) -> dict:
        p_cond = saturation_pressure_mpa(t_cond_c)
        r = rankine(p_boiler_mpa, t_boiler_c, p_cond)
        mass = target_w / (r.specific_net_work_kj_kg * 1000.0)
        q_cond_w = r.condenser_heat_kj_kg * 1000.0 * mass
        q_rad_one_m2 = radiator_capacity_w(1.0, t_cond_c + 273.15 + approach_k,
                                           emissivity)
        return {
            "t_cond_c": t_cond_c,
            "p_cond_mpa": p_cond,
            "efficiency": r.thermal_efficiency,
            "mass_flow_kg_s": mass,
            "q_cond_mw": q_cond_w / 1e6,
            "q_rad_mw": q_rad_one_m2 * area_m2 / 1e6,
            "required_area_m2": q_cond_w / q_rad_one_m2 if q_rad_one_m2 > 0 else float("inf"),
        }

    def net(t: float) -> float:
        point = evaluate(t)
        return point["q_rad_mw"] - point["q_cond_mw"]

    # A radiator larger than needed at the cold bound simply runs colder; report
    # that limit instead of the hot branch near the critical point.
    if net(lo) >= 0.0:
        point = evaluate(lo)
        point["balanced"] = False
        point["reason"] = "radiator oversized: cold-bound limited"
        return point

    prev_t = lo
    prev_f = net(prev_t)
    crossing = None
    t = lo + 1.0
    while t <= hi:
        f = net(t)
        if (prev_f < 0.0 <= f) or (prev_f >= 0.0 > f):
            crossing = (prev_t, t)
            break
        prev_t, prev_f = t, f
        t += 1.0

    if crossing is None:
        point = evaluate(hi)
        point["balanced"] = False
        point["reason"] = ("radiator too small for this power at boiler pressure"
                           if net(hi) < 0 else "no stable balance found")
        return point

    a, b = crossing
    best = evaluate(0.5 * (a + b))
    for _ in range(50):
        mid = 0.5 * (a + b)
        point = evaluate(mid)
        best = point
        if point["q_rad_mw"] > point["q_cond_mw"]:
            a = mid
        else:
            b = mid
    best["balanced"] = True
    best["reason"] = "radiator/condenser balance"
    return best


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="water Rankine cycle prototype (M10)")
    ap.add_argument("--p-boiler", type=float, default=5.0, help="boiler pressure, MPa")
    ap.add_argument("--t-boiler", type=float, default=300.0, help="turbine inlet temperature, C")
    ap.add_argument("--p-cond", type=float, default=0.005, help="condenser pressure, MPa")
    ap.add_argument("--eta-turbine", type=float, default=0.85)
    ap.add_argument("--eta-pump", type=float, default=0.75)
    ap.add_argument("--table", action="store_true", help="sweep boiler temperature")
    ap.add_argument("--helium", action="store_true",
                    help="helium Brayton path instead of water Rankine")
    ap.add_argument("--pressure-ratio", type=float, default=3.0)
    ap.add_argument("--regenerator", type=float, default=0.0,
                    help="recuperator effectiveness 0..1")
    ap.add_argument("--area", type=float, default=0.0,
                    help="radiator area m2; if set, solve the condenser point")
    ap.add_argument("--power", type=float, default=1.0, help="target MWe for --area")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    if args.helium:
        point = brayton_helium(args.pressure_ratio, args.t_boiler,
                               30.0, regenerator_eff=args.regenerator)
        if args.json:
            print(json.dumps(point, indent=2))
        else:
            for k, v in point.items():
                print(f"{k:26s} {v:10.4f}")
        return 0

    if args.area > 0.0:
        point = solve_condenser(args.p_boiler, args.t_boiler, args.area, args.power)
        if args.json:
            print(json.dumps(point, indent=2))
        else:
            print(f"radiator area {args.area:.0f} m2, target {args.power:.2f} MWe")
            for k, v in point.items():
                print(f"{k:18s} {v}" if isinstance(v, str) else f"{k:18s} {v:10.4f}")
        return 0

    if args.table:
        rows = []
        for t in (150, 200, 250, 300, 350, 400, 500):
            try:
                rows.append(rankine(args.p_boiler, t, args.p_cond,
                                    args.eta_turbine, args.eta_pump))
            except Exception as exc:  # invalid T/P combination
                rows.append({"t_boiler_c": t, "error": str(exc)})
        if args.json:
            print(json.dumps([r if isinstance(r, dict) else asdict(r) for r in rows], indent=2))
            return 0
        print(f"water Rankine: p_boiler={args.p_boiler} MPa, p_cond={args.p_cond} MPa")
        print(f"{'T_C':>5} {'eta':>7} {'carnot':>7} {'net kJ/kg':>10} {'Qcond kJ/kg':>12} {'kg/s/MW':>9}")
        for r in rows:
            if isinstance(r, dict):
                print(f"{r.get('t_boiler_c', 0):5.0f}  {r.get('error', '')}")
                continue
            print(f"{r.t_boiler_c:5.0f} {r.thermal_efficiency:7.3f} {r.carnot_efficiency:7.3f} "
                  f"{r.specific_net_work_kj_kg:10.1f} {r.condenser_heat_kj_kg:12.1f} "
                  f"{r.mass_flow_for_1mw_kg_s:9.3f}")
        return 0

    result = rankine(args.p_boiler, args.t_boiler, args.p_cond,
                     args.eta_turbine, args.eta_pump)
    if args.json:
        print(json.dumps(asdict(result), indent=2))
    else:
        for k, v in asdict(result).items():
            print(f"{k:28s} {v:10.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
