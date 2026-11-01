// M23 example 3 of 3 — a DLL-hook mod.
//
// Hooks one game function, counts how often it is called, and does nothing
// else.  It is deliberately the safest possible hook:
//
//   StrategicManager::RunTurn   ?RunTurn@StrategicManager@@QAEXXZ
//   VA 0x0076b450, __thiscall, void, no arguments
//
// The game calls it once per "Run Turn" click, so the panel's counter is an
// honest, easy-to-trigger demonstration.  Nothing is patched, nothing is
// written; the detour calls the original through the loader's trampoline and
// returns.
//
// Why this shape
//   * Resolve by PDB name, never by hardcoded address: cde::Resolve returns 0
//     when cdeloader.symbols does not match the exe, and the mod then logs an
//     error instead of hooking the wrong thing.
//   * The detour uses the __fastcall bridge (ECX = this, EDX free, stack args
//     after that) because MSVC's __thiscall is fastcall-shaped.
//   * Never change a return value unless you must: for a hook that only
//     observes, call the original and return exactly what it returned.
//   * A second hook on the same address is refused by the loader and both mod
//     names are logged, so one mod never silently breaks another.
//
// Install:
//     <game>/mods/hook-example/plugin.dll
//     <game>/mods/config/org.example.hook.ini
//
// Then start a game, open the strategic map, click "Run Turn" and open the F2
// menu: the panel counts the calls and cdeloader.log gets one line per call
// (keep log = 1 only while you are looking, the game calls it rarely).

static void hk_init(void);
static void hk_panel(void);

#define CDE_USER_INIT() hk_init()

#include "cdemod.hpp"
#include "cdemod_ui.h"

#include <cstdio>

// void __thiscall StrategicManager::RunTurn()
typedef void (__fastcall *RunTurnFn)(void* self, void* edx);
static RunTurnFn g_orig = nullptr;

static int  g_enabled = 1;
static int  g_log = 0;
static long g_calls = 0;
static void* g_last_self = nullptr;

static void __fastcall hk_detour(void* self, void* edx) {
    ++g_calls;
    g_last_self = self;
    if (g_log && (g_calls <= 3 || (g_calls % 25) == 0)) {
        CDE_INFO("hook example: RunTurn call %ld (this=%p)", g_calls, self);
    }
    // Call through to the real function.  Runs the turn as usual.
    g_orig(self, edx);
}

static void hk_init(void) {
    g_enabled = cde::ConfigBool("enabled", 1);
    g_log = cde::ConfigBool("log", 0);

    if (!g_enabled) {
        CDE_INFO("hook example: disabled in the ini");
    } else if (uintptr_t fn = cde::Resolve("StrategicManager::RunTurn")) {
        g_orig = reinterpret_cast<RunTurnFn>(
            cde::Hook("example.run_turn", reinterpret_cast<void*>(fn),
                      reinterpret_cast<void*>(&hk_detour)));
        if (!g_orig) {
            CDE_ERROR("hook example: the loader refused the hook (already taken?)");
        }
    } else {
        CDE_ERROR("hook example: cannot resolve StrategicManager::RunTurn "
                  "(is cdeloader.symbols next to the exe, and the same build?)");
    }
    CDE_INFO("hook example: enabled=%d log=%d hook=%p",
             g_enabled, g_log, reinterpret_cast<void*>(g_orig));
    cde_ui_panel("Example: hook", hk_panel);
}

static void hk_panel(void) {
    cde_ui_text("Counts calls to StrategicManager::RunTurn.");
    cde_ui_separator();
    if (g_enabled && !g_orig) {
        cde_ui_text("The hook is not installed.");
        cde_ui_text("Check that cdeloader.symbols matches your exe.");
        return;
    }
    if (!g_enabled) {
        cde_ui_text("Disabled in mods/config/org.example.hook.ini.");
        return;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "RunTurn calls: %ld   last manager: %p",
                  g_calls, g_last_self);
    cde_ui_text(buf);
    cde_ui_text("Click Run Turn on the strategic map to see it move.");
    int lg = g_log;
    if (cde_ui_checkbox("log each call", &lg)) {
        g_log = lg;
        cde::ConfigSetBool("log", lg);
    }
    cde_ui_text("source: examples/hook/plugin.cpp");
}

CDE_MOD("org.example.hook", "Example: hook", "1.0.0", "coade-re")
