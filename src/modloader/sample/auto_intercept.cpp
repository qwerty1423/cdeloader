// auto_intercept.cpp — org.cdere.auto-intercept ("Flyby At Any Range")
//
// The fleet dropdown only lists fleets inside engagement range, and each listed
// enemy fleet gets exactly two rows: Intercept (type 2, labelled Join when allied)
// and Flyby (type 3).  The user wants long-range *flyby only*: for a far fleet,
// drop the Intercept/Join row and keep the Flyby row; for a near fleet, change
// nothing at all.
//
// The action type is decided in the fleet-action builder (0x76f5a0: PUSH 0 ->
// ctor -> Add for type 2, then PUSH 1 -> ctor -> Add for type 3 when the fleet is
// hostile), and it has no distance input — the range verdict is computed one
// function earlier by Itinerary::IsFleetNearToNode (0x744730, called from the
// list filter 0x735aa0 at 0x735b3e) and thrown away.  So no byte patch can split
// the two rows per fleet; this mod uses two hooks and keeps the stock verdict as
// data:
//
//   H2  Itinerary::IsFleetNearToNode  record (fleet -> near?) and return true, so
//                                     every fleet is listed.
//   H3  util::Darray<ActorDropdownAction>::Add  drop a type-2 row when its fleet
//                                     was recorded far; the Flyby row survives.
//
// Hooks rather than patches: enabling/disabling writes no bytes, and a near
// fleet's rows are exactly what the game would have built itself.
//
// Evidence: notes/M25-AUTO-INTERCEPT-RANGE.md §12.

static void ai_init(void);
static void ai_panel(void);

#define CDE_USER_INIT() ai_init()

#include "cdemod.hpp"
#include "cdemod_ui.h"

namespace {

typedef unsigned char (__fastcall *NearFn)(void* self, void* edx, const void* node,
                                          void* fleet, void* fleet_ctrl);
typedef unsigned char (__fastcall *AddFn)(void* self, void* edx, const void* action);

NearFn g_near_orig = nullptr;
AddFn  g_add_orig = nullptr;

// ActorDropdownAction: Type at +0x2B8, Fleet at +0x2BC (sizeof 0x2C8, verified).
const uintptr_t kOffType = 0x2B8;
const uintptr_t kOffFleet = 0x2BC;
// ItineraryNode carries its parent fleet as a WeakPtr at +0xF0.
const uintptr_t kOffNodeParent = 0xF0;

struct FleetVerdict {
    void* fleet;
    void* ctrl;
    bool near;
};
FleetVerdict g_verdicts[256];
int g_verdict_count = 0;

int g_enabled = 1;
int g_hooks = 0;
int g_rows_skipped = 0;

void record_verdict(void* fleet, void* ctrl, bool near) {
    if (!fleet) return;
    for (int i = 0; i < g_verdict_count; ++i) {
        if (g_verdicts[i].fleet == fleet && g_verdicts[i].ctrl == ctrl) {
            g_verdicts[i].near = near;
            return;
        }
    }
    if (g_verdict_count < static_cast<int>(sizeof g_verdicts / sizeof g_verdicts[0])) {
        g_verdicts[g_verdict_count].fleet = fleet;
        g_verdicts[g_verdict_count].ctrl = ctrl;
        g_verdicts[g_verdict_count].near = near;
        ++g_verdict_count;
    }
}

// A fleet recorded "far" during the current list build.  Fleets we never saw are
// treated as near, so nothing changes unless the filter actually ran on them.
bool recorded_far(void* fleet) {
    for (int i = 0; i < g_verdict_count; ++i) {
        if (g_verdicts[i].fleet == fleet) return !g_verdicts[i].near;
    }
    return false;
}

unsigned char __fastcall h2_is_near(void* self, void* edx, const void* node, void* fleet,
                                    void* fleet_ctrl) {
    const unsigned char is_near = g_near_orig(self, edx, node, fleet, fleet_ctrl);
    if (!g_enabled) return is_near;

    // Keep the game's own early-out (an invalid node has no parent fleet) so we do
    // not list things the filter would have rejected for a different reason.
    void* parent = nullptr;
    void* parent_ctrl = nullptr;
    if (node) {
        cde::ReadMem(reinterpret_cast<uintptr_t>(node) + kOffNodeParent, &parent, sizeof parent);
        cde::ReadMem(reinterpret_cast<uintptr_t>(node) + kOffNodeParent + 4, &parent_ctrl,
                     sizeof parent_ctrl);
    }
    if (!parent || !parent_ctrl) return is_near;

    record_verdict(fleet, fleet_ctrl, is_near != 0);
    return 1;   // list every fleet; H3 decides which rows survive
}

unsigned char __fastcall h3_add(void* self, void* edx, const void* action) {
    if (g_enabled && action) {
        int type = -1;
        void* fleet = nullptr;
        cde::ReadMem(reinterpret_cast<uintptr_t>(action) + kOffType, &type, sizeof type);
        cde::ReadMem(reinterpret_cast<uintptr_t>(action) + kOffFleet, &fleet, sizeof fleet);
        // Type 2 is Intercept, or Join when the fleet is allied.  Only that row is
        // suppressed, and only for a fleet the filter called far.
        if (type == 2 && fleet && recorded_far(fleet)) {
            ++g_rows_skipped;
            return 0;
        }
    }
    return g_add_orig(self, edx, action);
}

}  // namespace

static void ai_init(void) {
    g_enabled = cde::ConfigBool("enabled", 1);

    const uintptr_t near_fn = cde::Resolve("Itinerary::IsFleetNearToNode");
    // The demangled form is truncated by the symbol table, so resolve by mangling.
    const uintptr_t add_fn = cde::Resolve(
        "?Add@?$Darray@UActorDropdownAction@@@util@@QAE_NABUActorDropdownAction@@@Z");

    if (near_fn) {
        g_near_orig = reinterpret_cast<NearFn>(cde::Hook(
            "auto_intercept.is_fleet_near", reinterpret_cast<void*>(near_fn),
            reinterpret_cast<void*>(&h2_is_near)));
        if (g_near_orig) ++g_hooks;
    }
    if (add_fn) {
        g_add_orig = reinterpret_cast<AddFn>(cde::Hook(
            "auto_intercept.action_add", reinterpret_cast<void*>(add_fn),
            reinterpret_cast<void*>(&h3_add)));
        if (g_add_orig) ++g_hooks;
    }
    if (g_hooks != 2) {
        CDE_WARN("auto_intercept: %d of 2 hooks installed; flyby stays stock", g_hooks);
        g_enabled = 0;
    }
    CDE_INFO("auto_intercept: hooks=%d enabled=%d (is_fleet_near=%p action_add=%p)", g_hooks,
             g_enabled, reinterpret_cast<void*>(near_fn), reinterpret_cast<void*>(add_fn));
    cde_ui_panel("Flyby At Any Range", ai_panel);
}

static void ai_panel(void) {
    if (g_hooks != 2) {
        cde_ui_text("not this exe build; nothing hooked");
        return;
    }
    int want = g_enabled;
    if (cde_ui_checkbox("enabled", &want)) {
        g_enabled = want;
        cde::ConfigSetBool("enabled", want);
    }
    cde_ui_help("lists every fleet and offers its flyby row at any range; the "
                "intercept and join rows appear only for fleets in engagement range, "
                "exactly as stock");
    if (g_rows_skipped) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d rows hidden", g_rows_skipped);
        cde_ui_text_disabled(buf);
    }
}

CDE_MOD("org.cdere.auto-intercept", "Flyby At Any Range", "0.4.0", "coade-re")
