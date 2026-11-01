// turboelectric_fix.cpp — make the hidden turboelectric fission reactor usable.
//
// Background
// ----------
// The retail game ships a complete TurboelectricFissionReactorModule but hides
// it: its `IsEnabled()` override is literally `return 0;` (the base
// `Module::IsEnabled` returns 1), so it never appears in Module Design.  The
// community unlocks it by importing a module design from
// `%APPDATA%/CDE/Mods/Data/Imports`.
//
// The naive fix — hook the override's address — is wrong: MSVC's identical-code
// folding merged that `return 0` with ~80 other methods (including
// `AudioSettings::ReflectionInfoStruct::ShouldZeroInitialize` and
// `IsPreReleaseVersion`), so hooking it corrupts preference loading.  Instead we
// patch the single *vtable slot* for this class:
//
//   TurboelectricFissionReactorModule primary vtable @ VA 0x0092c4b0
//   Module::IsEnabled is vftable offset 64  -> slot VA 0x0092c4f0
//   slot currently contains 0x004957b0 (the shared `return 0`)
//
// Only this class's virtual dispatch changes; every other user of the shared
// function is untouched.
//
#include <windows.h>

// This mod also clamps the net electrical output to the **Carnot limit**:
//   eta <= 1 - T_cold/T_hot
// with Q = NuclearReactorSubModule::GetThermalHeatGenerated_W, T_cold = the
// cached cold inlet (module +0x1f0), and T_hot = T_cold +
// NuclearReactorSubModule::GetTemperatureJump_K.  If an accessor is unavailable
// it falls back to eta <= 100%; NaN -> 0.
// See notes/TURBOELECTRIC.md for the model and open work.
//
// Build: scripts/build-modloader.sh

#include <cmath>
#include <cstdint>

static void tb_init(void);
static void tb_frame(void);
static void tb_shutdown(void);
static void tb_selftest(void);

#define CDE_USER_INIT()  tb_init()
#define CDE_USER_FRAME() tb_frame()
#define CDE_USER_SHUTDOWN() tb_shutdown()

#include "cdemod.hpp"

// --- 1. unlock (one vtable slot) -------------------------------------------
typedef bool(__thiscall* IsEnabledFn)(const void*);

static const uintptr_t kIsEnabledSlotVA = 0x0092c4f0u;  // vtable + 64
static const uint32_t kSharedReturnZero = 0x004957b0u;  // expected slot value

static unsigned g_enabled_calls = 0;
static volatile int g_frames = 0;
static int g_unlock_after = 0;
static bool g_log_clamps = true;
static bool g_logged_first_clamp = false;
static bool g_clamp_power = true;
static bool g_unlock_active = false;
static double g_last_p = 0.0;
static double g_last_q = 0.0;

static bool __thiscall detour_enabled(const void* self) {
    ++g_enabled_calls;
    (void)self;  // the override is a constant; no instance state is read
    return g_frames >= g_unlock_after;
}

static bool install_unlock(void) {
    uint32_t replacement = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&detour_enabled));
    if (!cde::Patch("TurboelectricFissionReactorModule::IsEnabled (vtable slot)",
                    kIsEnabledSlotVA, &kSharedReturnZero, &replacement, 4)) {
        CDE_ERROR("turboelectric: vtable slot 0x%08x did not contain 0x%08x "
                  "(unsupported CDE build?)",
                  static_cast<unsigned>(kIsEnabledSlotVA),
                  static_cast<unsigned>(kSharedReturnZero));
        return false;
    }
    CDE_INFO("patched IsEnabled vtable slot @ %p -> %p",
             reinterpret_cast<void*>(kIsEnabledSlotVA),
             reinterpret_cast<void*>(&detour_enabled));
    g_unlock_active = true;
    return true;
}

static void set_unlock(bool on) {
    uint32_t val = on ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&detour_enabled))
                      : kSharedReturnZero;
    if (cde::WriteMem(kIsEnabledSlotVA, &val, sizeof(val))) {
        g_unlock_active = on;
        CDE_INFO("turboelectric: unlock %s", on ? "on" : "off");
    }
}

// --- 2. power sanity / blackbox -------------------------------------------
typedef double(__thiscall* PowerFn)(const void*);
static PowerFn g_orig_power = nullptr;
static PowerFn g_orig_heat = nullptr;     // module GetHeatGenerated_W (blackbox)
static PowerFn g_reactor_heat = nullptr;  // NuclearReactorSubModule::GetThermalHeatGenerated_W
static PowerFn g_reactor_dT = nullptr;    // NuclearReactorSubModule::GetTemperatureJump_K
static unsigned g_power_clamps = 0;
static bool g_blackbox = true;
static float g_carnot_fraction = 0.7f;

typedef double(__thiscall* TorqueFn)(const void*);
static TorqueFn g_orig_torque = nullptr;
static bool g_fix_pump = false;
static bool g_logged_torque = false;

struct FlowPoint {
    double cp;    // J/kg/K
    double R;     // J/kg/K
    double T;     // K
    double P;     // Pa
    double mdot;  // kg/s
    double area;  // m2
};
typedef void(__thiscall* FlowPointFn)(const void*, FlowPoint* out);
static FlowPointFn g_turbopump_flow = nullptr;
static int g_auto_gas = 0;
static bool g_logged_auto_gas = false;

typedef double(__thiscall* MachFn)(const void*, double);
static MachFn g_orig_mach = nullptr;   // CompressibleFlowPoint::GetExitMachNumber
typedef double(__thiscall* CritFn)(const void*);
static CritFn g_orig_crit = nullptr;   // CompressibleFlowPoint::GetCriticalArea_m2
static bool g_fix_mach = false;
static int g_probe_every_ms = 0;
static DWORD g_last_probe = 0;
static bool g_logged_mach = false;
static bool g_logged_high_mach = false;

// Blackbox model: keep the reactor's fission heat (physical) and replace the
// broken turbine/pump loop with P = Q * (1 - Tc/Th) * fraction.  This is a
// deliberate simplification, not a repair of the flow solver: the loop's
// pump/turbine temperatures and Mach numbers remain stock.
static bool blackbox_values(const void* self, double* q_out, double* p_out) {
    if (!self || !g_reactor_heat) return false;
    const char* base = reinterpret_cast<const char*>(self);
    void* reactor = const_cast<char*>(base + 0x80);
    const double q = g_reactor_heat(reactor);
    const double t_cold = *reinterpret_cast<const double*>(base + 0x1f0);
    if (!(q > 0.0) || q > 1.0e15 || !(t_cold > 1.0) || t_cold > 100000.0)
        return false;

    double eta = 0.5;  // conservative fallback if the jump is unavailable
    if (g_reactor_dT) {
        const double dT = g_reactor_dT(reactor);
        if (dT > 0.0 && dT < 1.0e6) {
            const double t_hot = t_cold + dT;
            if (t_hot > t_cold) eta = 1.0 - t_cold / t_hot;  // Carnot
        }
    }
    eta *= static_cast<double>(g_carnot_fraction);
    if (eta < 0.0) eta = 0.0;
    if (eta > 1.0) eta = 1.0;
    *q_out = q;
    *p_out = q * eta;
    return true;
}

// Gas coolants are represented correctly by the stock ideal-gas flow model, so
// they should not be clamped.  Detect a gas from the turbopump exit flow point:
// gamma = cp/(cp-R), which is ~1.12 for liquid water's inconsistent pair and
// 1.4-1.67 for real gases.
static bool looks_like_gas(const void* self) {
    if (!g_turbopump_flow || !self) return false;
    FlowPoint fp = {0, 0, 0, 0, 0, 0};
    g_turbopump_flow(self, &fp);
    if (!(fp.cp > 1.0) || !(fp.R > 1.0) || !(fp.cp > fp.R) ||
        !(fp.T > 1.0) || !(fp.T < 1.0e6))
        return false;
    const double gamma = fp.cp / (fp.cp - fp.R);
    return gamma > 1.25 && gamma < 1.75;
}

// auto_gas: gas designs use the stock (real) model, liquid designs keep the
// clamp; otherwise the explicit blackbox switch wins.
static bool use_blackbox(const void* self) {
    if (!g_auto_gas) return g_blackbox;
    if (looks_like_gas(self)) {
        if (!g_logged_auto_gas) {
            g_logged_auto_gas = true;
            CDE_INFO("turboelectric: gas coolant detected; stock model (no blackbox)");
        }
        return false;
    }
    return true;
}

static double __thiscall detour_power(const void* self) {
    if (g_probe_every_ms > 0 && g_orig_power && self) {
        const DWORD now = GetTickCount();
        if (now - g_last_probe >= (DWORD)g_probe_every_ms) {
            g_last_probe = now;
            const double stock = g_orig_power(self);
            double q = 0.0, p_bb = 0.0;
            double tc = 0.0, dt = 0.0;
            blackbox_values(self, &q, &p_bb);
            const char* base = static_cast<const char*>(self);
            tc = *reinterpret_cast<const double*>(base + 0x1f0);
            if (g_reactor_dT)
                dt = g_reactor_dT(static_cast<const char*>(self) + 0x80);
            CDE_INFO("turbo probe: stockP=%.5g W blackboxP=%.5g W Q=%.5g W "
                     "Tc=%.1f K dT=%.1f K (ratio stock/Q=%.3f)",
                     stock, p_bb, q, tc, dt, q > 0.0 ? stock / q : 0.0);
        }
    }
    if (use_blackbox(self)) {
        double q = 0.0, p = 0.0;
        if (blackbox_values(self, &q, &p)) {
            g_last_p = p;
            g_last_q = q;
            if (g_log_clamps && !g_logged_first_clamp) {
                g_logged_first_clamp = true;
                CDE_INFO("turboelectric: blackbox P=%.4g W (Q=%.4g W, fraction=%.2f)",
                         p, q, static_cast<double>(g_carnot_fraction));
            }
            return p;
        }
    }
    double p = g_orig_power(self);
    if (p != p) {  // NaN
        ++g_power_clamps;
        return 0.0;
    }
    if (p < 0.0) p = 0.0;
    if (!self) return p;
    if (!g_clamp_power) return p;

    // The reactor submodule is *embedded* at +0x80: the engine's
    // GetEfficiency_Percent does `lea ecx, [esi+0x80]` before calling
    // GetThermalHeatGenerated_W.  (+0xd0 is the fuel-material pointer field
    // inside that submodule -- using it as `this` crashed Module Design.)
    const char* base = reinterpret_cast<const char*>(self);
    void* reactor = const_cast<char*>(base + 0x80);
    if (!g_reactor_heat) return p;

    const double q = g_reactor_heat(reactor);
    if (!(q > 0.0) || q > 1.0e15) return p;  // not initialized yet

    // Carnot: eta <= 1 - T_cold/T_hot.  T_cold is the cached cold inlet at
    // +0x1f0 (the temperature GetHeatGenerated_W subtracts); the reactor's
    // temperature jump gives T_hot = T_cold + dT.
    double pmax = q;  // fallback: energy conservation (eta <= 100%)
    const double t_cold = *reinterpret_cast<const double*>(base + 0x1f0);
    if (g_reactor_dT && t_cold > 1.0 && t_cold < 100000.0) {
        const double dT = g_reactor_dT(reactor);
        const double t_hot = t_cold + dT;
        if (dT > 0.0 && dT < 1.0e6 && t_hot > t_cold) {
            pmax = q * (1.0 - t_cold / t_hot);
        }
    }

    if (p > pmax) {
        ++g_power_clamps;
        if (g_log_clamps && !g_logged_first_clamp) {
            g_logged_first_clamp = true;
            CDE_INFO("turboelectric: first clamp %.4g -> %.4g W (Q=%.4g W, Tc=%.1f K)",
                     p, pmax, q, t_cold);
        }
        p = pmax;
    }
    return p;
}

static double __thiscall detour_heat(const void* self) {
    if (use_blackbox(self)) {
        double q = 0.0, p = 0.0;
        if (blackbox_values(self, &q, &p)) return q - p;
    }
    return g_orig_heat ? g_orig_heat(self) : 0.0;
}

// Pump Euler fix.  The coded velocity triangle uses a large inlet pre-whirl
// (v_t,in = v_n/tan30) and a smaller outlet tangential component, so Euler's
// torque is negative and the "pump" cools the coolant.  A real centrifugal
// pump has ~zero inlet swirl; with c_u1 = 0:
//     T = mdot * r_m * v_t,out  (> 0)
// Hooking GetTorque_N_m also fixes GetPowerRequired_W and
// GetSpecificEnthalpyIncreaseAcrossTurbopump, which both call it.
static double __thiscall detour_torque(const void* self) {
    if (!g_fix_pump || !self) return g_orig_torque(self);
    const char* p = reinterpret_cast<const char*>(self);
    const double R = *reinterpret_cast<const double*>(p + 0x18);
    const double rpm = *reinterpret_cast<const double*>(p + 0x20);
    const double rho = *reinterpret_cast<const double*>(p + 0x30);
    if (!(R > 0.0) || !(rpm > 0.0) || !(rho > 0.0)) return g_orig_torque(self);

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTan30 = 0.5773502691896257;
    constexpr double kTan60 = 1.7320508075688772;
    const double r_h = 0.2 * R;
    const double r_m = 0.6 * R;
    const double area = kPi * R * R - kPi * r_h * r_h - r_m * 0.001;
    const double omega = rpm * 2.0 * kPi / 60.0;
    const double u = omega * r_m;
    const double v_n = u / (kTan30 + 1.0 / kTan30);
    const double mdot = area * v_n * rho;
    const double alpha_out = std::atan(u / v_n - 1.0 / kTan60);
    const double vt_out = v_n / std::tan(alpha_out);
    const double torque = mdot * r_m * vt_out;

    if (g_log_clamps && !g_logged_torque) {
        g_logged_torque = true;
        CDE_INFO("turboelectric: pump Euler fix T=%.4g N m (stock %.4g), mdot=%.4g kg/s",
                 torque, g_orig_torque(self), mdot);
    }
    return torque;
}

// The isentropic area-Mach relation has two roots for A/A* > 1.  The pump and
// turbine flow points are subsonic, but the stock solver returns the
// supersonic root (Mach 45), which makes the static temperature/pressure
// nonsense.  Return the subsonic root instead.
// EXPERIMENTAL: this hooks the global CompressibleFlowPoint, so it also
// affects supersonic nozzles; keep fix_mach off unless testing.
static double __thiscall detour_mach(const void* self, double area) {
    const double orig = g_orig_mach(self, area);
    if (!g_fix_mach || !self) return orig;
    const char* p = reinterpret_cast<const char*>(self);
    const double cp = *reinterpret_cast<const double*>(p + 0);
    const double R = *reinterpret_cast<const double*>(p + 8);
    if (!(cp > 0.0) || !(R > 0.0) || !(cp > R)) return orig;
    const double gamma = cp / (cp - R);
    const double astar = g_orig_crit ? g_orig_crit(self) : 0.0;
    if (!(astar > 0.0) || !(area > 0.0)) return orig;
    const double ratio = area / astar;
    if (orig > 10.0 && !g_logged_high_mach) {
        g_logged_high_mach = true;
        const double T = *reinterpret_cast<const double*>(p + 16);
        const double P = *reinterpret_cast<const double*>(p + 24);
        const double mdot = *reinterpret_cast<const double*>(p + 32);
        const double hint = *reinterpret_cast<const double*>(p + 48);
        CDE_INFO("turboelectric: HIGH MACH %.4g: cp=%.4g R=%.4g T=%.4g P=%.4g "
                 "mdot=%.4g area=%.4g A*=%.4g ratio=%.4g hint=%.4g",
                 orig, cp, R, T, P, mdot, area, astar, ratio, hint);
    }
    if (ratio <= 1.0) return 1.0;  // choked; the subsonic root is sonic

    double lo = 1e-9, hi = 1.0;
    for (int i = 0; i < 80; ++i) {
        const double mid = 0.5 * (lo + hi);
        const double t = 1.0 + 0.5 * (gamma - 1.0) * mid * mid;
        const double ar = (1.0 / mid) *
            std::pow((2.0 / (gamma + 1.0)) * t,
                     (gamma + 1.0) / (2.0 * (gamma - 1.0)));
        if (ar > ratio) lo = mid; else hi = mid;
    }
    const double m = 0.5 * (lo + hi);
    if (g_log_clamps && !g_logged_mach) {
        g_logged_mach = true;
        CDE_INFO("turboelectric: area-Mach subsonic M=%.4g (stock %.4g), "
                 "A/A*=%.4g gamma=%.3f", m, orig, ratio, gamma);
    }
    return m;
}

// --- helpers ---------------------------------------------------------------
template <typename T>
static T install(const char* label, const char* symbol, T detour) {
    uintptr_t va = cde::Resolve(symbol);
    if (!va) {
        CDE_WARN("turboelectric: symbol not found: %s", label);
        return nullptr;
    }
    T orig = cde::Hook(label, reinterpret_cast<T>(va), detour);
    if (orig) {
        CDE_INFO("hooked %s @ %p -> %p", label, reinterpret_cast<void*>(va),
                 reinterpret_cast<void*>(detour));
    } else {
        CDE_ERROR("HOOK FAILED %s @ %p", label, reinterpret_cast<void*>(va));
    }
    return orig;
}

static void tb_ui(void) {
    cde_ui_text("Turboelectric model (applies to ALL turbo reactors)");
    cde_ui_text("Blackbox is the physically consistent model; stock is unphysical.");
    int bb = g_blackbox ? 1 : 0;
    if (cde_ui_checkbox("Blackbox: P = Q * Carnot * fraction", &bb))
        g_blackbox = bb != 0;
    cde_ui_text("Untick only to compare with the stock formulas (designer).");
    cde_ui_slider_float("Carnot fraction", &g_carnot_fraction, 0.10f, 1.00f);
    int cl = g_clamp_power ? 1 : 0;
    if (cde_ui_checkbox("Clamp stock power to Carnot", &cl))
        g_clamp_power = cl != 0;
    cde_ui_separator();
    int un = g_unlock_active ? 1 : 0;
    if (cde_ui_checkbox("Unlock in Module Design", &un))
        set_unlock(un != 0);
    int lg = g_log_clamps ? 1 : 0;
    if (cde_ui_checkbox("Log clamps", &lg))
        g_log_clamps = lg != 0;
    cde_ui_separator();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "last: P = %.4g W, Q = %.4g W", g_last_p, g_last_q);
    cde_ui_text(buf);
}

static void tb_init(void) {
    const bool unlock = cde::ConfigBool("unlock", 1) != 0;
    g_clamp_power = cde::ConfigBool("clamp_power", 1) != 0;
    g_blackbox = cde::ConfigBool("blackbox", 1) != 0;
    g_fix_pump = cde::ConfigBool("fix_pump", 0) != 0;
    g_probe_every_ms = cde::ConfigInt("probe_every_ms", 0);
    g_auto_gas = cde::ConfigBool("auto_gas", 0) != 0;
    if (g_auto_gas) {
        g_turbopump_flow = reinterpret_cast<FlowPointFn>(cde::Resolve(
            "TurboelectricFissionReactorModule::GetTurbopumpExitFlowPoint"));
        if (!g_turbopump_flow)
            CDE_WARN("turboelectric: auto_gas requested but the flow-point "
                     "accessor did not resolve");
    }
    g_fix_mach = cde::ConfigBool("fix_mach", 0) != 0;
    g_carnot_fraction = cde::ConfigFloat("carnot_fraction", 0.7f);
    if (g_carnot_fraction < 0.0f) g_carnot_fraction = 0.0f;
    if (g_carnot_fraction > 1.0f) g_carnot_fraction = 1.0f;
    g_unlock_after = cde::ConfigInt("unlock_after_frames", 0);
    if (g_unlock_after < 0) g_unlock_after = 0;
    g_log_clamps = cde::ConfigBool("log_clamps", 1) != 0;

    if (unlock && !install_unlock()) {
        CDE_WARN("turboelectric: module stays hidden");
    }

    if (g_fix_pump) {
        g_orig_torque = install("TurbopumpSubModule::GetTorque_N_m",
                                "?GetTorque_N_m@TurbopumpSubModule@@QBENXZ",
                                &detour_torque);
        if (!g_orig_torque) CDE_WARN("turboelectric: pump torque hook failed");
    }

    if (g_fix_mach) {
        uintptr_t crit_va =
            cde::Resolve("?GetCriticalArea_m2@CompressibleFlowPoint@@QBENXZ");
        if (crit_va) g_orig_crit = reinterpret_cast<CritFn>(crit_va);
        g_orig_mach = install("CompressibleFlowPoint::GetExitMachNumber",
                              "?GetExitMachNumber@CompressibleFlowPoint@@QBENN@Z",
                              &detour_mach);
        if (!g_orig_mach) CDE_WARN("turboelectric: area-Mach hook failed");
    }

    {
        // Always install the power/heat hooks so the in-game panel can toggle
        // the model live; the detours fall back to the originals when off.
        uintptr_t q_va =
            cde::Resolve("?GetThermalHeatGenerated_W@NuclearReactorSubModule@@QBENXZ");
        if (q_va) g_reactor_heat = reinterpret_cast<PowerFn>(q_va);
        else CDE_WARN("turboelectric: GetThermalHeatGenerated_W not resolved");
        uintptr_t dt_va =
            cde::Resolve("?GetTemperatureJump_K@NuclearReactorSubModule@@QBENXZ");
        if (dt_va) g_reactor_dT = reinterpret_cast<PowerFn>(dt_va);
        else CDE_WARN("turboelectric: GetTemperatureJump_K not resolved; "
                      "Carnot clamp falls back to eta<=100%%");
        g_orig_power = install(
            "TurboelectricFissionReactorModule::GetPowerGenerated_W",
            "?GetPowerGenerated_W@TurboelectricFissionReactorModule@@UBENXZ",
            &detour_power);
        if (!g_orig_power) CDE_ERROR("turboelectric: power hook failed");
        g_orig_heat = install(
            "TurboelectricFissionReactorModule::GetHeatGenerated_W",
            "?GetHeatGenerated_W@TurboelectricFissionReactorModule@@UBENXZ",
            &detour_heat);
        if (!g_orig_heat) CDE_ERROR("turboelectric: heat hook failed");
    }

    cde::UI("Turboelectric Reactor", tb_ui);

    if (cde::ConfigBool("selftest", 0)) tb_selftest();
}

static void tb_frame(void) {
    if (g_frames < 1000000) ++g_frames;
    static bool announced = false;
    if (!announced && g_frames >= g_unlock_after) {
        announced = true;
        CDE_INFO("turboelectric: unlock active (after %d frames)", g_unlock_after);
    }
}

static void tb_shutdown(void) {
    if (g_log_clamps && g_power_clamps > 0) {
        CDE_INFO("turboelectric: %u power clamp(s) this session", g_power_clamps);
    }
}

// Call the patched vtable slot directly (safe: the detour reads no instance
// state).  After the delay it must return 1; the stock slot returns 0.
static void tb_selftest(void) {
    uint32_t fn = 0;
    if (!cde::ReadMem(kIsEnabledSlotVA, &fn, sizeof(fn)) || !fn) {
        CDE_WARN("selftest: could not read IsEnabled slot");
        return;
    }
    CDE_INFO("selftest: slot contains %p (detour is %p)", reinterpret_cast<void*>(fn),
             reinterpret_cast<void*>(&detour_enabled));
    bool r = reinterpret_cast<IsEnabledFn>(fn)(nullptr);
    CDE_INFO("selftest: IsEnabled vtable slot -> %d (expect 1; delay=%d frames)",
             r ? 1 : 0, g_unlock_after);
}

CDE_MOD("org.cdere.turboelectric", "Turboelectric Reactor Unlock + Fix", "0.3.4",
        "coade-re")
