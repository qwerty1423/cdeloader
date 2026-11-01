// laser_jitter.cpp — org.cdere.laser-jitter
//
// Pointing wander, in two layers (both inside WeaponCompartment):
//
//   RandomAimer::GetRandomAimOffset_km (VA 0x663fd0)
//       scatters the aim point inside the target's own silhouette: up to half
//       the bounding cylinder along the target's motion axis plus a sphere of
//       the "tight radius", in km.  At combat ranges that is several to tens of
//       arcseconds of pointing error, but it shrinks with the target.
//
//   WeaponCompartment::GetAimerOffset_km (VA 0x6615e0)
//       takes that offset and adds a second one: a random direction times
//       tan(diffraction limit / 2) * range (the beam spot at the target), then
//       returns the total aim-point offset in km.  Its 4th argument is the
//       range in km, so this is the one place where the *angular* pointing
//       error can be measured.
//
// Hooking the outer function bounds both layers at once, and the range
// argument lets the mod work in real angular units: the aim point may wander
// at most `arcsec` away from the true intercept point, i.e. a stabilised mount
// of that pointing accuracy.  Real optical systems: large telescope mounts
// ~1 arcsec, space telescopes ~0.01 arcsec, lasercom fine steering 0.2-0.8
// arcsec; the game's own diffraction limit is 1.22*lambda/D (0.48 arcsec half
// angle for a 380 nm laser on a 0.1 m mirror).  See notes/M24-LASER-JITTER.md.
//
// Minimum 0.05 arcsec, maximum 60.  Relative mode keeps the shipped behaviour
// scaled by a percentage instead, for comparison.
//
// ABI: `util::Vec3<double> WeaponCompartment::GetAimerOffset_km(util::WeakPtr<Body>,
// util::Maybe<util::Vec3<double>>, util::Vec3<double>, double) const` — MSVC
// __thiscall with a hidden struct-return pointer: ECX = this, then the stack
// holds (Vec3* out, WeakPtr.ptr, WeakPtr.ctrl, Maybe<Vec3> 32 bytes, Vec3 24
// bytes, double range_km); the function ends in `ret 0x4c`, which is exactly
// 76 bytes of arguments.  The __fastcall bridge below maps ECX/EDX and puts the
// remaining arguments on the stack in that order.  EAX returns `out`.
//
// Config (mods/config/org.cdere.laser-jitter.ini):
//   enabled = 1
//   absolute = 1        1 = bound the angular error, 0 = scale the stock offset
//   arcsec  = 0.5       absolute mode: peak pointing error (telescope class)
//   scale   = 0.1       relative mode: fraction of the stock offset
//   log     = 0         one sample per N calls (1 = on, useful for tuning)
//
// The panel toggle and sliders take effect immediately; the ini is written
// back when they move.

static void jt_init(void);
static void jt_panel(void);

#define CDE_USER_INIT() jt_init()

#include "cdemod.hpp"

#include <windows.h>
#include "cdemod_ui.h"

#include <cmath>

struct JtVec3 { double x, y, z; };
// util::Maybe<util::Vec3<double>>: the 24-byte value, then IsValid() at +0x18.
struct JtMaybeVec3 { double x, y, z; unsigned char valid; unsigned char pad[7]; };

// RandomAimer::GetRandomAimOffset_km (0x663fd0) returns the scatter of the aim point
// inside the target, which is what lets a shot land on a component.  Its result is
// cached here so the outer hook can tell the two terms apart: the aimer returns
// scatter + jitter, and only the jitter is reduced.
typedef JtVec3* (__fastcall *GetRandomAimOffsetFn)(void* self, void* edx, JtVec3* out,
                                                   void* target, void* target_ctrl,
                                                   double beam_radius_km);
static GetRandomAimOffsetFn g_scatter_orig = nullptr;
static JtVec3 g_scatter_last;          // most recent scatter, km
static unsigned long g_scatter_seq = 0;   // bumped on every call
static unsigned long g_scatter_seen = 0;  // last sequence the outer hook consumed

typedef JtVec3* (__fastcall *GetAimerOffsetFn)(void* self, void* edx, JtVec3* out,
                                               void* target, void* target_ctrl,
                                               JtMaybeVec3 aim_point, JtVec3 aim_dir,
                                               double range_km);
static GetAimerOffsetFn g_orig = nullptr;
static void* g_hook = nullptr;

static int    g_enabled = 1;
static int    g_absolute = 1;
static double g_arcsec = 0.5;
static double g_scale = 0.1;
static int    g_log = 0;
static long   g_calls = 0;
static DWORD  g_last_log_ms = 0;

// Last sample, for the panel readout (display only; a torn read is harmless).
static double g_last_range = 0.0;
static double g_last_km_before = 0.0;
static double g_last_km_after = 0.0;
static double g_last_as_before = 0.0;
static double g_last_as_after = 0.0;
static double g_last_scatter_km = 0.0;

static const double kPi = 3.14159265358979323846;
static const double kArcsec = kPi / (180.0 * 3600.0);   // radians per arcsecond

static double jt_len(const JtVec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

static JtVec3* __fastcall jt_scatter_detour(void* self, void* edx, JtVec3* out,
                                            void* target, void* target_ctrl,
                                            double beam_radius_km) {
    g_scatter_orig(self, edx, out, target, target_ctrl, beam_radius_km);
    if (out) {
        g_scatter_last = *out;
        ++g_scatter_seq;
    }
    return out;
}

static JtVec3* __fastcall jt_detour(void* self, void* edx, JtVec3* out, void* target,
                                    void* target_ctrl, JtMaybeVec3 aim_point,
                                    JtVec3 aim_dir, double range_km) {
    g_orig(self, edx, out, target, target_ctrl, aim_point, aim_dir, range_km);
    if (!out) return out;
    ++g_calls;

    const double total = jt_len(*out);
    // A sane range is required for the angular units; a bad one means the argument
    // layout is not what this build expects, so do not touch anything.
    const bool usable = range_km > 1e-6 && range_km < 1e9;

    // The aim point has two possible sources.  When the caller supplies one (a chosen
    // point on the target, which is what a shot at a component looks like), that is
    // the scatter, converted from metres to km.  Otherwise the game asks the random
    // aimer, whose result the other hook caches.  Either way, what is left after
    // subtracting it from the returned offset is the jitter.
    JtVec3 scatter = {};
    bool have_scatter = false;
    const char* scatter_source = "none";
    if (aim_point.valid) {
        scatter = JtVec3{aim_point.x / 1000.0, aim_point.y / 1000.0, aim_point.z / 1000.0};
        have_scatter = true;
        scatter_source = "aim-point";
    } else if (g_scatter_seq != g_scatter_seen) {
        scatter = g_scatter_last;
        have_scatter = true;
        scatter_source = "aimer";
        g_scatter_seen = g_scatter_seq;
    }

    double jitter_before = 0.0;
    double jitter_after = 0.0;

    if (g_enabled && usable && have_scatter) {
        JtVec3 jitter = {out->x - scatter.x, out->y - scatter.y, out->z - scatter.z};
        jitter_before = jt_len(jitter);
        double limit;
        if (g_absolute) {
            limit = range_km * std::tan(g_arcsec * kArcsec);
        } else {
            limit = jitter_before * g_scale;
        }
        if (jitter_before > limit) {
            const double k = jitter_before > 0.0 ? limit / jitter_before : 0.0;
            jitter.x *= k;
            jitter.y *= k;
            jitter.z *= k;
        }
        jitter_after = jt_len(jitter);
        out->x = scatter.x + jitter.x;
        out->y = scatter.y + jitter.y;
        out->z = scatter.z + jitter.z;
    } else if (g_enabled && usable && !have_scatter && !g_absolute) {
        // No scatter seen: relative mode falls back to scaling the whole offset, and
        // absolute mode leaves the value alone rather than guessing which part is
        // which.
        out->x *= g_scale;
        out->y *= g_scale;
        out->z *= g_scale;
        jitter_after = jt_len(*out);
    } else {
        jitter_after = jitter_before;
    }

    const double after = jt_len(*out);
    if (usable) {
        g_last_range = range_km;
        g_last_km_before = jitter_before;
        g_last_km_after = jitter_after;
        g_last_as_before = jitter_before / range_km / kArcsec;
        g_last_as_after = jitter_after / range_km / kArcsec;
        g_last_scatter_km = jt_len(scatter);
    }
    const DWORD now_ms = GetTickCount();
    if (g_log && (g_calls <= 5 || now_ms - g_last_log_ms > 2000)) {
        g_last_log_ms = now_ms;
        const char* mode = !g_enabled            ? "off"
                         : !usable                ? "no-range"
                         : have_scatter           ? "jitter-only"
                         : g_absolute             ? "no-scatter, untouched"
                                                  : "no-scatter, scaled";
        cde::Logf(CDE_LOG_INFO,
                  "laser_jitter: call %ld mode=%s from=%s range=%.1f km "
                  "jitter %.4g -> %.4g km (%.3f -> %.3f arcsec), scatter(%.4g) %.4g km, "
                  "total %.4g km",
                  g_calls, mode, scatter_source, range_km,
                  jitter_before, jitter_after,
                  usable ? g_last_as_before : 0.0, usable ? g_last_as_after : 0.0,
                  jt_len(scatter), jt_len(scatter), after);
    }
    return out;
}

static void jt_init(void) {
    g_enabled = cde::ConfigBool("enabled", 1);
    g_absolute = cde::ConfigBool("absolute", 1);
    g_arcsec = cde::ConfigFloat("arcsec", 0.5f);
    g_scale = cde::ConfigFloat("scale", 0.1f);
    g_log = cde::ConfigBool("log", 0);
    if (g_arcsec < 0.01) g_arcsec = 0.01;
    if (g_arcsec > 60.0) g_arcsec = 60.0;
    if (g_scale < 0.0) g_scale = 0.0;
    if (g_scale > 1.0) g_scale = 1.0;

    uintptr_t fn = cde::Resolve("WeaponCompartment::GetAimerOffset_km");
    if (!fn) {
        cde::Log(CDE_LOG_ERROR, "laser_jitter: cannot resolve WeaponCompartment::GetAimerOffset_km");
    } else {
        g_orig = reinterpret_cast<GetAimerOffsetFn>(
            cde::Hook("laser_jitter.aimer_offset", reinterpret_cast<void*>(fn),
                      reinterpret_cast<void*>(&jt_detour)));
        if (!g_orig)
            cde::Log(CDE_LOG_ERROR, "laser_jitter: hook refused");
    }
    // The scatter hook is what makes the split possible; without it only relative
    // mode can do anything.
    uintptr_t scatter_fn = cde::Resolve("RandomAimer::GetRandomAimOffset_km");
    if (scatter_fn) {
        g_scatter_orig = reinterpret_cast<GetRandomAimOffsetFn>(
            cde::Hook("laser_jitter.scatter", reinterpret_cast<void*>(scatter_fn),
                      reinterpret_cast<void*>(&jt_scatter_detour)));
    }
    g_hook = nullptr;
    cde::Logf(CDE_LOG_INFO,
              "laser_jitter: enabled=%d mode=%s arcsec=%.3f scale=%.3f log=%d "
              "hooks=%p/%p",
              g_enabled, g_absolute ? "absolute" : "relative", g_arcsec, g_scale,
              g_log, reinterpret_cast<void*>(g_orig),
              reinterpret_cast<void*>(g_scatter_orig));
    cde_ui_panel("Laser Jitter Reduction", jt_panel);
}

static void jt_panel(void) {
    if (!g_orig) {
        cde_ui_text("not this exe build; the hook is not installed");
        return;
    }
    int en = g_enabled;
    if (cde_ui_checkbox("enabled", &en)) {
        g_enabled = en;
        cde::ConfigSetBool("enabled", en);
    }
    cde_ui_help("reduces the turret aim jitter; stock when off");

    int ab = g_absolute;
    if (cde_ui_checkbox("limit in arcseconds", &ab)) {
        g_absolute = ab;
        cde::ConfigSetBool("absolute", ab);
    }
    cde_ui_help("on: cap the pointing error in arcseconds. off: percent of the stock "
                "aim offset.");
    // The whole range is reachable and typed: 1e-6 arcsec is far below the game's
    // own diffraction limit, and 120 arcsec is twice anything useful.  The entry box
    // is what makes a value like 0.0045 arcsec actually settable.
    if (g_absolute) {
        float as = static_cast<float>(g_arcsec);
        const float as_default = 0.5f;
        if (cde_ui_slider_row("max jitter (arcsec)", &as, 1e-6f, 120.0f, "%.6f",
                              &as_default, /*logarithmic=*/1)) {
            g_arcsec = as;
            cde::ConfigSetFloat("arcsec", as);
        }
        cde_ui_help("arcseconds: 0.5 is a telescope-class mount, 0.2 is laser comms fine "
                    "steering, 0.05 is a space telescope, 0 leaves the aim point exactly "
                    "on the solution");
    } else {
        float sc = static_cast<float>(g_scale * 100.0);
        const float sc_default = 10.0f;
        if (cde_ui_slider_row("jitter (% of stock)", &sc, 0.0f, 200.0f, "%.3f",
                              &sc_default, /*logarithmic=*/0)) {
            g_scale = sc / 100.0f;
            cde::ConfigSetFloat("scale", g_scale);
        }
        cde_ui_help("percent of the stock jitter; 0 and 200 are both accepted");
    }

    cde_ui_text_disabled("limits the jitter; the aim point on the target is kept");

    if (g_last_range > 0.0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "jitter %.3f arcsec, was %.3f arcsec",
                      g_last_as_after, g_last_as_before);
        cde_ui_text(buf);
        cde_ui_help("measured on the last turret solution, with the target's own scatter "
                    "excluded");
        std::snprintf(buf, sizeof(buf), "aim point offset %.2f m at the target",
                      g_last_scatter_km * 1000.0);
        cde_ui_text_disabled(buf);
        cde_ui_help("the part that is left alone: where on the target the shot is aimed, "
                    "which is what lets it hit a component");
        std::snprintf(buf, sizeof(buf), "range %.0f km, %.2f m of jitter at target",
                      g_last_range, g_last_km_after * 1000.0);
        cde_ui_text(buf);
    } else {
        cde_ui_text_disabled("stock jitter: no turret solution seen yet");
    }
    int lg = g_log;
    if (cde_ui_checkbox("log", &lg)) {
        g_log = lg;
        cde::ConfigSetBool("log", lg);
    }
    cde_ui_help("one line per second in cdeloader.log");
}

CDE_MOD("org.cdere.laser-jitter", "Laser Jitter Reduction", "0.5.1", "coade-re")
