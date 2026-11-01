// Probe plugin: demonstrates the SDK against the *real* game (not the synthetic
// test host in host.c).  It resolves a handful of PDB names and lists them in an
// overlay panel.  Read-only: it patches nothing and hooks nothing.

#include <cstdint>
#include <cstdio>

static void probe_init(void);
static void probe_frame(void);

#define CDE_USER_INIT()  probe_init()
#define CDE_USER_FRAME() probe_frame()

#include "cdemod.hpp"
#include "cdemod_ui.h"

static const char* const kNames[] = {
    "StrategicManager::GetMinimumBattleRange_km",
    "GameModel::GameModel",
    "Module::GetTag",
    "SpacecraftShader::GetStatic",
    "does::Not::Exist",
};
static const size_t kCount = sizeof(kNames) / sizeof(kNames[0]);

static uintptr_t g_va[sizeof(kNames) / sizeof(kNames[0])];
static int g_found = 0;
static int g_frames = 0;

static void probe_panel(void) {
    cde_ui_text("Read-only PDB lookups (no patch, no hook):");
    char line[192];
    for (size_t i = 0; i < kCount; ++i) {
        if (g_va[i])
            std::snprintf(line, sizeof(line), "%s  @ %08X", kNames[i],
                          static_cast<unsigned>(g_va[i]));
        else
            std::snprintf(line, sizeof(line), "%s  (not found)", kNames[i]);
        cde_ui_text(line);
    }
    std::snprintf(line, sizeof(line), "resolved %d/%u", g_found,
                  static_cast<unsigned>(kCount));
    cde_ui_text(line);
}

static void probe_init(void) {
    for (size_t i = 0; i < kCount; ++i) {
        g_va[i] = cde::Resolve(kNames[i]);
        if (g_va[i]) ++g_found;
        CDE_INFO("probe: resolve '%s' -> %p", kNames[i],
                 reinterpret_cast<void*>(g_va[i]));
    }
    CDE_INFO("probe: %d/%u names resolved from cdeloader.symbols", g_found,
             static_cast<unsigned>(kCount));
    cde_ui_panel("PDB Probe", probe_panel);
}

static void probe_frame(void) {
    if (++g_frames == 120) CDE_INFO("probe: still alive after 120 frames");
}

CDE_MOD("org.example.probe", "PDB Probe", "1.0.0", "coade-re")
