// laser_m2.cpp — org.cdere.laser-m2
//
// M22: remove the M^2 = 3 beam-quality floor.
//
// The game never lets a laser design be better than M^2 = 3, and that is not a
// GUI limit: every place that uses the beam quality computes
//
//     M2 = sqrt(q*q + 9)          // q = LaserModule::GetThermalLensingQualityFactor()
//
// so M2 >= 3 always.  The 9 is one shared .rdata double at VA 0x00925568, and
// exactly four instructions add it:
//
//     0x0063d034  LaserModule::GetRadiusAtDistance_m   (beam radius -> damage)
//     0x0063d7cc  LaserModule::GetStatuses             (design status readout)
//     0x006450c4  LaserModule::GetBeamWaistRadius_m    (waist)
//     0x00646473  LaserModule::GetLaserInfo            (the info panel's M^2)
//
// all `F2 0F 58 05 68 55 92 00` (addsd xmm0, [0x925568]).  This mod retargets
// those four displacements at its own double, so the floor becomes a live
// setting: 1.0 gives a diffraction-limited beam (M2 = max(1, q)), 3.0 is stock.
// The shared constant itself is not touched (one other instruction multiplies
// by it), the original bytes are verified before each write, and writing the
// stock value back is an exact restore.
//
// Effects, all from the same value: M2 shown in the design UI and the info
// panel, the beam waist, the beam radius at range, and therefore the damage
// (intensity scales with 1/waist^2, so M2 3 -> 1 is up to ~9x at long range).
//
// The M^2 still depends on the design: a sloppy laser keeps its high value, a
// clean one is limited only by the floor.  The floor is the perfection limit,
// never below 1 (a diffraction-limited beam), and the mod is a toggle between
// the physical limit (M^2 >= 1, default) and the vanilla one (M^2 >= 3).
//
// Config (mods/config/org.cdere.laser-m2.ini):
//   enabled = 1
//   limit   = 1       1 = perfection limit (default), 3 = vanilla limit
//   log     = 0
//
// Live in the F2 panel.  Stock-exe VAs; the patch refuses if the bytes differ.

static void lm_init(void);
static void lm_panel(void);

#define CDE_USER_INIT() lm_init()

#include "cdemod.hpp"
#include "cdemod_ui.h"

#include <cmath>

// The four `addsd xmm0, qword ptr [0x00925568]` sites.
struct LmSite { uintptr_t va; const char* what; };
static const LmSite kSites[] = {
    {0x0063d034, "GetRadiusAtDistance_m"},
    {0x0063d7cc, "GetStatuses"},
    {0x006450c4, "GetBeamWaistRadius_m"},
    {0x00646473, "GetLaserInfo"},
};
static const int kSiteCount = static_cast<int>(sizeof(kSites) / sizeof(kSites[0]));

// The value the rest of the game reads once the four sites point here.
// 9.0 = stock (sqrt(q^2 + 9) => M2 >= 3); 1.0 => M2 >= 1.
static double g_floor_sq = 9.0;

static int    g_enabled = 1;
static int    g_limit = 1;      // 1 = perfection limit, 3 = vanilla
static int    g_log = 0;
static int    g_patched = 0;

static void lm_apply(void) {
    double floor = 9.0;             // vanilla (sqrt(q^2 + 9) => M2 >= 3)
    if (g_enabled) {
        const double limit = (g_limit >= 3) ? 3.0 : 1.0;
        floor = limit * limit;
    }
    g_floor_sq = floor;
}

static void lm_init(void) {
    g_enabled = cde::ConfigBool("enabled", 1);
    g_limit = cde::ConfigInt("limit", 1);
    if (g_limit >= 3) g_limit = 3;
    else g_limit = 1;
    g_log = cde::ConfigBool("log", 0);
    lm_apply();

    // The replacement bytes: the same addsd, but reading our own double.
    const uintptr_t target = reinterpret_cast<uintptr_t>(&g_floor_sq);
    const unsigned char expect[8] = {0xF2, 0x0F, 0x58, 0x05, 0x68, 0x55, 0x92, 0x00};
    unsigned char repl[8] = {0xF2, 0x0F, 0x58, 0x05, 0, 0, 0, 0};
    repl[4] = static_cast<unsigned char>(target & 0xFF);
    repl[5] = static_cast<unsigned char>((target >> 8) & 0xFF);
    repl[6] = static_cast<unsigned char>((target >> 16) & 0xFF);
    repl[7] = static_cast<unsigned char>((target >> 24) & 0xFF);

    int ok = 0;
    for (int i = 0; i < kSiteCount; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "laser_m2.%s", kSites[i].what);
        if (cde::Patch(name, kSites[i].va, expect, repl, 8)) {
            ++ok;
            if (g_log)
                cde::Logf(CDE_LOG_INFO, "laser_m2: %s @ 0x%08lx -> floor^2 var 0x%08lx",
                          kSites[i].what, static_cast<unsigned long>(kSites[i].va),
                          static_cast<unsigned long>(target));
        } else {
            cde::Logf(CDE_LOG_ERROR, "laser_m2: %s @ 0x%08lx did not match (not this build?)",
                      kSites[i].what, static_cast<unsigned long>(kSites[i].va));
        }
    }
    g_patched = ok;
    cde::Logf(CDE_LOG_INFO,
              "laser_m2: enabled=%d limit=M2>=%d floor^2=%.2f sites=%d/%d",
              g_enabled, g_limit, g_floor_sq, ok, kSiteCount);
    cde_ui_panel("Laser M^2 Limit Removal", lm_panel);
}

static void lm_panel(void) {
    if (g_patched <= 0) {
        cde_ui_text("not this exe build; nothing patched");
        return;
    }
    int en = g_enabled;
    if (cde_ui_checkbox("enabled", &en)) {
        g_enabled = en;
        lm_apply();
        cde::ConfigSetBool("enabled", en);
    }
    cde_ui_help("reduces the M^2 = 3 limit to M^2 = 1");
    int lg = g_log;
    if (cde_ui_checkbox("log", &lg)) {
        g_log = lg;
        cde::ConfigSetBool("log", lg);
    }
    cde_ui_help("one line per second in cdeloader.log");
}

CDE_MOD("org.cdere.laser-m2", "Laser M^2 Limit Removal", "0.1.1", "coade-re")
