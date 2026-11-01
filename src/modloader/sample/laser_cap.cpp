// laser_cap.cpp — org.cdere.laser-cap
//
// Removes the armour photon-damage clamp that stops a laser from carving deeper
// than the clamped fraction.  The stock code computes the damage depth, clamps
// it with `minsd xmm3, xmm5` where xmm5 is 1.0, then multiplies by the plate
// thickness.  The widely circulated "laser cap removal" exe patch NOPs exactly
// that instruction (its old file offset belongs to another build; in this build
// it is VA 0x675355, bytes F2 0F 5D DD).
//
// The patch is applied and restored at runtime (both directions use the same
// expected-byte checks), so the checkbox in the F2 panel takes effect
// immediately and the last state is written back to the mod ini.  It stacks
// with the community patch; the second min (damage vs remaining plate,
// 0x675369) is left alone.
//
// Config (mods/config/org.cdere.laser-cap.ini):
//   enabled = 1            master switch and startup state (F2 toggles live);
//                          on by default on a fresh install
//
// Evidence: notes/M7-LASER-CAP.md.

static void lc_init(void);
static void lc_panel(void);

#define CDE_USER_INIT() lc_init()

#include "cdemod.hpp"
#include "cdemod_ui.h"

static const uintptr_t kCapSite = 0x00675355;  // minsd xmm3, xmm5 (1.0)
static const unsigned char kStock[4] = {0xF2, 0x0F, 0x5D, 0xDD};
static const unsigned char kNoCap[4] = {0x90, 0x90, 0x90, 0x90};

static int g_want = 0;       // requested state (config / checkbox)
static int g_patched = 0;    // clamp removed in memory right now
static int g_site_ok = 0;    // the site looks like one of the two known states

static int lc_read(unsigned char* out) {
    return cde::ReadMem(kCapSite, out, 4);
}

static int lc_bytes_are(const unsigned char* cur, const unsigned char* want) {
    for (int i = 0; i < 4; ++i)
        if (cur[i] != want[i]) return 0;
    return 1;
}

// Apply the requested state.  Only touches the site when it currently holds a
// known state, and always verifies after writing.  Returns 1 on success.
static int lc_apply(int want) {
    unsigned char cur[4];
    if (!lc_read(cur)) {
        cde::Log(CDE_LOG_ERROR, "laser_cap: cannot read the clamp site");
        return 0;
    }
    const int is_stock = lc_bytes_are(cur, kStock);
    const int is_patched = lc_bytes_are(cur, kNoCap);
    if (!is_stock && !is_patched) {
        cde::Logf(CDE_LOG_ERROR,
                  "laser_cap: unexpected bytes %02x %02x %02x %02x at 0x%08x",
                  cur[0], cur[1], cur[2], cur[3], (unsigned)kCapSite);
        return 0;
    }
    if ((want && is_patched) || (!want && is_stock)) {
        g_patched = is_patched;
        return 1;
    }
    const unsigned char* target = want ? kNoCap : kStock;
    if (!cde::WriteMem(kCapSite, target, 4)) {
        cde::Log(CDE_LOG_ERROR, "laser_cap: write failed");
        return 0;
    }
    if (!lc_read(cur) || !lc_bytes_are(cur, target)) {
        cde::Log(CDE_LOG_ERROR, "laser_cap: verify failed after write");
        return 0;
    }
    g_patched = want ? 1 : 0;
    cde::Logf(CDE_LOG_INFO, "laser_cap: %s (live)",
              want ? "clamp removed" : "stock clamp restored");
    return 1;
}

static void lc_init(void) {
    g_want = cde::ConfigBool("enabled", 1);
    unsigned char cur[4];
    g_site_ok = lc_read(cur) && (lc_bytes_are(cur, kStock) || lc_bytes_are(cur, kNoCap));
    if (g_want && !lc_apply(1)) {
        g_want = 0;
    } else if (!g_want) {
        lc_apply(0);  // restore if some other tool left the site patched
    }
    cde::Logf(CDE_LOG_INFO, "laser_cap: enabled=%d patched=%d site=%s",
              g_want, g_patched, g_site_ok ? "ok" : "unexpected");
    // Register unconditionally: the overlay finishes initialising after plugins,
    // so cde_ui_available() is still 0 here.  cde_ui_panel stores the callback.
    cde_ui_panel("Laser Ablation Cap Removal", lc_panel);
}

static void lc_panel(void) {
    if (!g_site_ok) {
        cde_ui_text("not this exe build; nothing patched");
        return;
    }
    int want = g_want;
    if (cde_ui_checkbox("enabled", &want)) {
        if (lc_apply(want)) {
            g_want = want;
            cde::ConfigSetBool("enabled", want);
        }
    }
    cde_ui_help("removes the 1.0 depth cap in laser armour ablation");
}

CDE_MOD("org.cdere.laser-cap", "Laser Ablation Cap Removal", "0.2.1", "coade-re")
