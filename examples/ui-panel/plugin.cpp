// M23 example 2 of 3 — a UI mod (one panel in the loader's F2 menu).
//
// This is a complete, documented plugin: it needs no game hook, no patch and no
// game symbol, so it is the safest place to start.  Build it with
// examples/build.sh (or scripts/build-modloader.sh) and install it as
//
//     <game>/mods/ui-panel/plugin.dll
//     <game>/mods/config/org.example.ui-panel.ini
//
// The panel appears under F2 (or Insert) in game.  Everything here is the
// pattern every UI mod in this repo follows:
//
//   1. declare the mod with CDE_MOD(...)
//   2. read config in the init callback (cde::ConfigXxx)
//   3. register a panel (cde_ui_panel) from that same init
//   4. draw widgets only inside the panel callback, and write config back when
//      a value changes, so the ini matches what the panel shows
//
// The widgets available are in src/modloader/cdemod_ui.h: checkbox, slider_int,
// slider_float, button, text, separator, same_line, plus a draggable window and
// screen-space line/rect primitives for HUDs (see cde_ui_hud).

static void up_init(void);
static void up_panel(void);

#define CDE_USER_INIT() up_init()

#include "cdemod.hpp"
#include "cdemod_ui.h"

#include <cstdio>

// ---- state mirrored from the ini ------------------------------------------
static int   g_enabled = 1;
static int   g_count = 5;         // "how many rows to show"
static float g_scale = 1.0f;      // an example float setting
static int   g_clicks = 0;        // runtime state, not persisted
static bool  g_drew = false;      // the panel callback has run

static void up_init(void) {
    // Defaults are what a fresh install gets; they are written back only when
    // the user changes something in the panel.
    g_enabled = cde::ConfigBool("enabled", 1);
    g_count = cde::ConfigInt("rows", 5);
    g_scale = cde::ConfigFloat("scale", 1.0f);
    if (g_count < 1) g_count = 1;
    if (g_count > 20) g_count = 20;

    CDE_INFO("ui-panel example: ready (enabled=%d rows=%d scale=%.2f)",
             g_enabled, g_count, static_cast<double>(g_scale));

    // Register the panel.  Safe even when the overlay is unavailable: the call
    // is a no-op and the mod still loads.
    cde_ui_panel("Example: UI panel", up_panel);
}

static void up_panel(void) {
    if (!g_drew) {
        g_drew = true;
        // Logging once from the draw callback is the cheap way to prove the
        // panel really rendered (the loader draws it inside its F2 menu).
        CDE_INFO("ui-panel example: panel drawn");
    }
    cde_ui_text("This panel is an example mod.  It changes nothing in the game.");
    cde_ui_separator();

    if (!cde_ui_available()) {
        cde_ui_text("The overlay is not available on this machine.");
        return;
    }

    int en = g_enabled;
    if (cde_ui_checkbox("enabled", &en)) {
        g_enabled = en;
        cde::ConfigSetBool("enabled", en);
    }

    if (!g_enabled) return;

    int rows = g_count;
    if (cde_ui_slider_int("rows", &rows, 1, 20)) {
        g_count = rows;
        cde::ConfigSetInt("rows", rows);
    }
    float scale = g_scale;
    if (cde_ui_slider_float("scale", &scale, 0.1f, 4.0f)) {
        g_scale = scale;
        cde::ConfigSetFloat("scale", scale);
    }
    if (cde_ui_button("count a click")) ++g_clicks;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "clicks this session: %d   (rows=%d, scale=%.2f)",
                  g_clicks, g_count, static_cast<double>(g_scale));
    cde_ui_text(buf);

    cde_ui_separator();
    cde_ui_text("config: mods/config/org.example.ui-panel.ini");
    cde_ui_text("source: examples/ui-panel/plugin.cpp");
}

CDE_MOD("org.example.ui-panel", "Example: UI panel", "1.0.0", "coade-re")
