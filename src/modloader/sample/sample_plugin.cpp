// Example CDE plugin, used by scripts/test-modloader.sh.
//
// It exercises every Phase-A host service: logging, per-mod config, symbol
// resolution, pattern scanning, an inline hook (with the original callable via
// the returned trampoline), and a per-frame callback.  Built as a 32-bit DLL
// exporting CdeMod_GetInfo.
//
// Build: scripts/build-modloader.sh

#include <cstdint>

// Declare the entry points the SDK macro will call.
static void sample_init(void);
static void sample_frame(void);

#define CDE_USER_INIT()  sample_init()
#define CDE_USER_FRAME() sample_frame()

#include "cdemod.hpp"
#include "cdemod_ui.h"

static int (CDEMOD_CALL *g_orig_target)(int) = nullptr;
static int g_frames = 0;

// State for the example overlay panel.
static int g_ui_enabled = 1;
static int g_ui_counter = 0;
static float g_ui_scale = 1.0f;

static void sample_ui(void) {
    cde_ui_text("Panel drawn by the loader's ImGui overlay (F10).");
    cde_ui_checkbox("enabled", &g_ui_enabled);
    cde_ui_slider_int("counter", &g_ui_counter, 0, 100);
    cde_ui_slider_float("scale", &g_ui_scale, 0.1f, 4.0f);
    if (cde_ui_button("log a line"))
        CDE_INFO("UI button pressed (counter=%d)", g_ui_counter);
}

// The host's HostTarget() is hooked to add 100; calling the trampoline must still
// produce the original result (+1).
static int CDEMOD_CALL detour_target(int x) {
    CDE_INFO("hook fired: HostTarget(%d)", x);
    return g_orig_target(x) + 100;
}

static void sample_init(void) {
    CDE_INFO("sample plugin init (abi %u, struct %u)", CDEMOD_ABI_VERSION,
             static_cast<unsigned>(sizeof(CdeHostApi)));
    CDE_INFO("config answer = %d", cde::ConfigInt("answer", -1));

    cde_ui_panel("Sample Mod", sample_ui);

    uintptr_t pattern = cde::Pattern("55 89 e5 8b 45 08");
    CDE_INFO("pattern '55 89 e5 8b 45 08' at %p", reinterpret_cast<void*>(pattern));

    uintptr_t target = cde::Resolve("HostTarget");
    if (!target) {
        CDE_ERROR("could not resolve HostTarget (is cdeloader.symbols present?)");
        return;
    }
    CDE_INFO("resolved HostTarget @ %p", reinterpret_cast<void*>(target));

    g_orig_target = cde::Hook("HostTarget", reinterpret_cast<int (CDEMOD_CALL*)(int)>(target),
                              &detour_target);
    if (!g_orig_target) CDE_ERROR("hook of HostTarget failed");
}

static void sample_frame(void) {
    if (++g_frames == 3) CDE_INFO("frame callback ticked");
}

CDE_MOD("org.example.sample", "Sample Mod", "1.0.0", "coade-re")
