// UI-only plugin for scripts/test-modloader.sh.
//
// Unlike sample_plugin.cpp it does no symbol resolution or hooking, so it can be
// loaded into the OpenGL test host (which has no matching symbol table).  It
// registers an overlay panel and, from the tick thread, reports how many times
// the loader drew it — proving the overlay actually runs a plugin's panel.

#include <cstdint>

static void ui_init(void);
static void ui_frame(void);

#define CDE_USER_INIT()  ui_init()
#define CDE_USER_FRAME() ui_frame()

#include "cdemod.hpp"
#include "cdemod_ui.h"

static volatile int g_draws = 0;
static volatile int g_hud_draws = 0;
static int g_reported = 0;
static int g_counter = 0;

static void ui_panel(void) {
    ++g_draws;
    cde_ui_text("This panel is drawn by the loader's ImGui overlay.");
    cde_ui_slider_int("counter", &g_counter, 0, 100);
}

static void ui_hud(void) {
    ++g_hud_draws;
    cde_ui_overlay_text(8.0f, 8.0f, 0xFF00FF00u, "hud test");
}

static void ui_init(void) {
    cde_ui_panel("UI Test", ui_panel);
    cde_ui_hud("UI Test HUD", ui_hud);
    CDE_INFO("ui plugin init; overlay available=%d", cde_ui_available());
}

static void ui_frame(void) {
    int n = g_draws;
    if (n != g_reported) {
        g_reported = n;
        CDE_INFO("panel drawn %d time(s)", n);
    }
    static int hud_reported = 0;
    int h = g_hud_draws;
    if (h != hud_reported && h > 5) {
        hud_reported = h;
        CDE_INFO("hud drawn %d time(s)", h);
    }
}

CDE_MOD("org.example.ui", "UI Test", "1.0.0", "coade-re")
