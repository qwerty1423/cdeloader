// Manoeuvre Node Editor: shows the node the game has selected and nudges its burn.
//
// There is no fleet or node picker: the game's own selection decides what is edited.
// Select a node in the itinerary and the window follows it.
//
// The edit goes through Itinerary::AlterBurn, which is the game's own burn path, and
// then through StrategicManager::SelectItineraryNode so the manager's copy of the
// selection matches the node that changed.  Both run on the game thread: mod
// callbacks run on the loader's thread, and touching the itinerary from there would
// race the game.
static void ne_init(void);

#define CDE_USER_INIT() ne_init()

#include "cdemod.hpp"
#include "cdegame.hpp"
#include "cdemod_ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kNodeSize = 0x150;
constexpr int kOffNodeTime = 0x70;    // double, seconds
constexpr int kOffNodeBurn = 0x78;    // double[3], km/s
constexpr int kOffNodeType = 0xA8;    // int
constexpr int kOffNodeState = 64;      // util::PhysicalState<Vec3<double>>: Position, Velocity
constexpr int kOffItineraryValid = 0x290;
constexpr int kOffManagerSelection = 0x660;

using GetItineraryFn = void* (__fastcall*)(void* fleet, void* edx);
using TickFn = void (__fastcall*)(void* self, void* edx, double dt);

struct NodeView {
    double time_s;
    int type;
    double burn[3];
};

struct Snapshot {
    bool have_node;
    bool have_plan;
    int index;
    NodeView node;
};

Snapshot g_snap;
cde::game::Runtime* g_runtime = nullptr;
GetItineraryFn g_get_itinerary = nullptr;
TickFn g_tick_orig = nullptr;

// Written by the window callback, applied on the game thread.
struct Pending {
    bool queued;
    int axis;         // 0..2 = burn x/y/z, -1 = reset all
    double delta;
};
Pending g_pending;

int g_enabled = 1;
int g_window = 0;
double g_step = 0.001;    // km/s per click
double g_step_default = 0.001;
int g_log = 0;   // off by default: the log is a diagnostic, not a record of use
int g_edits = 0;
int g_ticks = 0;
int g_last_log_tick = -60;

struct Vec3 {
    double x, y, z;
};

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

Vec3 scaled(const Vec3& v, double k) { return Vec3{v.x * k, v.y * k, v.z * k}; }

// Local axes at the node: x along the trajectory, z along the orbit normal, and y the
// in-plane direction perpendicular to both (which is the outward radial direction for a
// circular orbit; the game's own drag axes come from HUDItineraryNodeDragger::GetDirections,
// which needs dragger state we do not have).  A node's burn is stored in the world frame,
// so a nudge along one of these has to be rotated into world coordinates before it is added.
bool node_axes(const unsigned char* node, Vec3* prograde, Vec3* radial, Vec3* normal) {
    Vec3 pos = {0, 0, 0};
    Vec3 vel = {0, 0, 0};
    cde::ReadMem(reinterpret_cast<uintptr_t>(node) + kOffNodeState + 0, &pos, sizeof pos);
    cde::ReadMem(reinterpret_cast<uintptr_t>(node) + kOffNodeState + 24, &vel, sizeof vel);
    const double speed = length(vel);
    const Vec3 h = cross(pos, vel);
    const double h_len = length(h);
    if (!(speed > 1e-12) || !(h_len > 1e-12)) return false;
    *prograde = scaled(vel, 1.0 / speed);
    *normal = scaled(h, 1.0 / h_len);
    *radial = cross(*normal, *prograde);
    return true;
}

unsigned char* node_ptr(void* itinerary, int index) {
    unsigned char* base = static_cast<unsigned char*>(itinerary) + 8;   // Darray<ItineraryNode>
    void* data = nullptr;
    int count = 0;
    if (!cde::ReadMem(reinterpret_cast<uintptr_t>(base) + 4, &data, sizeof data)) return nullptr;
    if (!cde::ReadMem(reinterpret_cast<uintptr_t>(base) + 8, &count, sizeof count)) return nullptr;
    if (!data || index < 0 || index >= count || count > 4096) return nullptr;
    return static_cast<unsigned char*>(data) + index * kNodeSize;
}

// The manager keeps a copy of the selected node; its time identifies the node.
bool selected_node_time(void* manager, double* time_s) {
    if (!manager || !time_s) return false;
    return cde::ReadMem(reinterpret_cast<uintptr_t>(manager) + kOffManagerSelection +
                            kOffNodeTime, time_s, sizeof *time_s);
}

// Find the plan node whose time matches the selection.  False when the selection is
// not a plan node, in which case nothing is edited.
bool locate_selected(void* manager, NodeView* out, int* index_out) {
    if (!manager || !g_runtime || !out) return false;
    double want = 0.0;
    if (!selected_node_time(manager, &want)) return false;
    if (!std::isfinite(want) || want <= 0.0) return false;

    cde::game::FleetInfo fleets[32];
    const int n = g_runtime->Fleets(fleets, 32);
    for (int f = 0; f < n; ++f) {
        void* fleet = fleets[f].object;
        void* maybe = fleet ? g_get_itinerary(fleet, nullptr) : nullptr;
        if (!maybe) continue;
        unsigned char valid = 0;
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(maybe) + kOffItineraryValid, &valid, 1) ||
            !valid)
            continue;
        unsigned char* base = static_cast<unsigned char*>(maybe) + 8;
        void* data = nullptr;
        int count = 0;
        cde::ReadMem(reinterpret_cast<uintptr_t>(base) + 4, &data, sizeof data);
        cde::ReadMem(reinterpret_cast<uintptr_t>(base) + 8, &count, sizeof count);
        if (!data || count <= 0 || count > 4096) continue;
        for (int i = 0; i < count; ++i) {
            double t = 0.0;
            cde::ReadMem(reinterpret_cast<uintptr_t>(data) + i * kNodeSize + kOffNodeTime, &t,
                         sizeof t);
            if (std::fabs(t - want) > 0.001) continue;
            out->time_s = t;
            cde::ReadMem(reinterpret_cast<uintptr_t>(data) + i * kNodeSize + kOffNodeBurn,
                         out->burn, sizeof out->burn);
            cde::ReadMem(reinterpret_cast<uintptr_t>(data) + i * kNodeSize + kOffNodeType,
                         &out->type, sizeof out->type);
            if (index_out) *index_out = i;
            return true;
        }
    }
    return false;
}

void refresh_snapshot(void* manager) {
    g_snap.have_plan = false;
    g_snap.have_node = false;
    g_snap.index = -1;
    if (!manager || !g_get_itinerary) return;
    g_snap.have_node = locate_selected(manager, &g_snap.node, &g_snap.index);
    g_snap.have_plan = g_snap.have_node;
}

// Runs on the game thread.
void apply_pending(void) {
    if (!g_pending.queued) return;
    g_pending.queued = false;
    if (!g_enabled) return;

    void* manager = g_runtime ? g_runtime->StrategicManager() : nullptr;
    if (!manager) return;
    NodeView node;
    int index = -1;
    if (!locate_selected(manager, &node, &index)) {
        if (g_log) CDE_INFO("node_editor: nothing selected, edit dropped");
        return;
    }

    using AlterBurnFn = void (__fastcall*)(void* self, void* edx, void* out_node,
                                           const void* in_node, double dx, double dy,
                                           double dz);
    static AlterBurnFn alter_burn = nullptr;
    if (!alter_burn) alter_burn = reinterpret_cast<AlterBurnFn>(
        cde::Resolve("Itinerary::AlterBurn"));
    if (!alter_burn) return;

    // ItineraryNode is passed by value (336 bytes on the stack, ret 0x150).
    struct RawNode {
        unsigned char bytes[kNodeSize];
    };
    using SelectFn = void (__fastcall*)(void* self, void* edx, RawNode node);
    static SelectFn select_node = nullptr;
    if (!select_node) select_node = reinterpret_cast<SelectFn>(
        cde::Resolve("StrategicManager::SelectItineraryNode"));

    cde::game::FleetInfo fleets[32];
    const int n = g_runtime->Fleets(fleets, 32);
    for (int f = 0; f < n; ++f) {
        void* fleet = fleets[f].object;
        void* maybe = fleet ? g_get_itinerary(fleet, nullptr) : nullptr;
        if (!maybe) continue;
        unsigned char* target = node_ptr(maybe, index);
        if (!target) continue;
        double t = 0.0;
        cde::ReadMem(reinterpret_cast<uintptr_t>(target) + kOffNodeTime, &t, sizeof t);
        if (std::fabs(t - node.time_s) > 0.001) continue;   // another fleet's plan

        unsigned char copy[kNodeSize];
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(target), copy, sizeof copy)) return;
        double burn[3] = {0, 0, 0};
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(target) + kOffNodeBurn, burn,
                          sizeof burn))
            return;
        Vec3 p_axis = {1, 0, 0};
        Vec3 r_axis = {0, 1, 0};
        Vec3 n_axis = {0, 0, 1};
        const bool local = node_axes(target, &p_axis, &r_axis, &n_axis);
        if (g_pending.axis < 0) {
            burn[0] = burn[1] = burn[2] = 0.0;
        } else if (local) {
            const Vec3 step = g_pending.axis == 0 ? p_axis
                            : g_pending.axis == 1 ? r_axis : n_axis;
            burn[0] += step.x * g_pending.delta;
            burn[1] += step.y * g_pending.delta;
            burn[2] += step.z * g_pending.delta;
            if (!std::isfinite(burn[0]) || !std::isfinite(burn[1]) ||
                !std::isfinite(burn[2]))
                return;
        } else {
            // No usable state vectors: fall back to the world axes and say so once.
            if (g_log) CDE_WARN("node_editor: no orbit frame at this node, using world axes");
            burn[g_pending.axis] += g_pending.delta;
            if (!std::isfinite(burn[g_pending.axis])) return;
        }

        unsigned char out[kNodeSize];
        alter_burn(maybe, nullptr, out, copy, burn[0], burn[1], burn[2]);

        double new_time = 0.0;
        void* parent = nullptr;
        void* parent_ctrl = nullptr;
        cde::ReadMem(reinterpret_cast<uintptr_t>(out) + kOffNodeTime, &new_time,
                     sizeof new_time);
        cde::ReadMem(reinterpret_cast<uintptr_t>(out) + 0xF0, &parent, sizeof parent);
        cde::ReadMem(reinterpret_cast<uintptr_t>(out) + 0xF4, &parent_ctrl, sizeof parent_ctrl);
        if (!std::isfinite(new_time) || !parent || !parent_ctrl) return;

        if (select_node) {
            // The node edited is the node the manager already holds, so refreshing its
            // copy cannot move the selection to another node.
            double current = 0.0;
            if (selected_node_time(manager, &current) &&
                std::fabs(current - new_time) < 0.001) {
                RawNode by_value;
                std::memcpy(by_value.bytes, out, kNodeSize);
                select_node(manager, nullptr, by_value);
            }
        }
        ++g_edits;
        if (g_log && g_edits <= 20) {
            CDE_INFO("node_editor: edit axis=%d delta=%.9f time=%.1f burn=(%.6f %.6f %.6f)",
                     g_pending.axis, g_pending.delta, new_time, burn[0], burn[1], burn[2]);
        } else if (g_log && g_edits == 21) {
            CDE_INFO("node_editor: 20 edits logged; further edits are not");
        }
        return;
    }
}

void __fastcall hook_tick(void* self, void* edx, double dt) {
    ++g_ticks;
    if (g_enabled && g_runtime) {
        apply_pending();
        if ((g_ticks % 15) == 0) {
            refresh_snapshot(g_runtime->StrategicManager());
            if (g_log && g_snap.have_node && g_ticks - g_last_log_tick >= 60) {
                g_last_log_tick = g_ticks;
                const NodeView& v = g_snap.node;
                CDE_INFO("node_editor: node %d time=%.1f type=%d burn=(%.6f %.6f %.6f)",
                         g_snap.index, v.time_s, v.type, v.burn[0], v.burn[1], v.burn[2]);
            }
        }
    }
    g_tick_orig(self, edx, dt);
}

const char* type_name(int t) {
    switch (t) {
        case 1: return "match orbit";
        case 2: return "join fleet";
        case 3: return "intercept";
        case 4: return "flyby";
        case 5: return "circularise";
        default: return "burn";
    }
}

void toggle_window(void) { g_window = g_window ? 0 : 1; }

void ne_panel(void) {
    int want = g_enabled;
    if (cde_ui_checkbox("enabled", &want)) {
        g_enabled = want;
        cde::ConfigSetBool("enabled", want);
    }
    cde_ui_help("nudges the selected node's burn through the game's own burn path");
    int open = g_window;
    if (cde_ui_checkbox("window (F3)", &open)) g_window = open;
    cde_ui_help("the editor window; F3 toggles it as well");
    int logging = g_log;
    if (cde_ui_checkbox("log", &logging)) {
        g_log = logging;
        cde::ConfigSetBool("log", logging);
    }
    cde_ui_help("writes the selected node and every edit to cdeloader.log; off unless "
                "something needs diagnosing");
    if (!g_snap.have_node) cde_ui_text_disabled("select a flight-plan node");
}

void ne_hud(void) {
    if (!g_window) return;
    int open_flag = g_window;
    if (!cde_ui_window_begin("Manoeuvre Node Editor (F3)", &open_flag, 0)) {
        if (!open_flag) g_window = 0;
        cde_ui_window_end();
        return;
    }
    if (!open_flag) g_window = 0;

    if (!g_snap.have_node) {
        cde_ui_text_disabled("select a flight-plan node");
        cde_ui_help("the window follows the game's selection");
        cde_ui_window_end();
        return;
    }

    char buf[192];
    const NodeView& v = g_snap.node;
    std::snprintf(buf, sizeof(buf), "%s at %.0f s", type_name(v.type), v.time_s);
    cde_ui_text(buf);
    std::snprintf(buf, sizeof(buf), "x %+.6f  y %+.6f  z %+.6f",
                  v.burn[0], v.burn[1], v.burn[2]);
    cde_ui_text_disabled(buf);

    float step = static_cast<float>(g_step);
    const float step_default = static_cast<float>(g_step_default);
    if (cde_ui_slider_row("step (km/s)", &step, 1e-9f, 10.0f, "%.9f", &step_default,
                          /*logarithmic=*/1)) {
        g_step = step;
        cde::ConfigSetFloat("step_kms", step);
    }
    cde_ui_help("how much one click changes the burn; the slider spreads the small "
                "values out");

    if (g_enabled) {
        const float colours[3][3] = {{0.65f, 0.18f, 0.18f},
                                     {0.18f, 0.55f, 0.22f},
                                     {0.18f, 0.32f, 0.72f}};
        // x runs along the trajectory, y away from the primary, z along the orbit normal.
        static const char* axis_names[3] = {"x", "y", "z"};
        cde_ui_push_button_repeat(1);
        for (int axis = 0; axis < 3; ++axis) {
            cde_ui_push_button_color(colours[axis][0], colours[axis][1], colours[axis][2]);
            std::snprintf(buf, sizeof(buf), "-%s", axis_names[axis]);
            if (cde_ui_button(buf)) {
                g_pending.queued = true;
                g_pending.axis = axis;
                g_pending.delta = -g_step;
            }
            cde_ui_same_line();
            std::snprintf(buf, sizeof(buf), "+%s", axis_names[axis]);
            if (cde_ui_button(buf)) {
                g_pending.queued = true;
                g_pending.axis = axis;
                g_pending.delta = g_step;
            }
            cde_ui_pop_button_color();
            cde_ui_same_line();
            std::snprintf(buf, sizeof(buf), "%+.6f", v.burn[axis]);
            cde_ui_text(buf);
        }
        cde_ui_pop_button_repeat();
        if (cde_ui_button("reset burn")) {
            g_pending.queued = true;
            g_pending.axis = -1;
            g_pending.delta = 0.0;
        }
        cde_ui_help("sets x, y and z back to zero through the same burn path");
    } else {
        cde_ui_text_disabled("edits are off");
    }
    cde_ui_window_end();
}

}  // namespace

void ne_init(void) {
    if (!cde::Host()) {
        CDE_ERROR("node_editor: no host API");
        return;
    }
    g_runtime = &cde::game::Runtime::Get();
    if (!g_runtime) {
        CDE_ERROR("node_editor: the game runtime is not available");
        return;
    }
    g_enabled = cde::ConfigBool("enabled", 1);
    g_log = cde::ConfigBool("log", 0);
    g_step = static_cast<double>(cde::ConfigFloat("step_kms", 0.001f));
    if (!(g_step > 0.0)) g_step = 0.001;
    g_step_default = g_step;

    g_get_itinerary = reinterpret_cast<GetItineraryFn>(
        cde::Resolve("Fleet::GetMissionItinerary"));
    const uintptr_t tick = cde::Resolve("StrategicManager::Tick");
    if (g_get_itinerary && tick) {
        g_tick_orig = reinterpret_cast<TickFn>(cde::Hook(
            "node_editor.tick", reinterpret_cast<void*>(tick),
            reinterpret_cast<void*>(&hook_tick)));
    }
    const int ok = (g_get_itinerary ? 1 : 0) + (g_tick_orig ? 1 : 0);
    if (ok != 2) {
        CDE_WARN("node_editor: %d of 2 hooks resolved; staying read-only", ok);
        g_enabled = 0;
    }
    cde_ui_panel("Manoeuvre Node Editor (F3)", ne_panel);
    cde_ui_hud("node_editor.window", ne_hud);
    cde_ui_hotkey(0x72 /* VK_F3 */, toggle_window);
    CDE_INFO("node_editor: hooks=%d enabled=%d step=%.9f", ok, g_enabled, g_step);
}


CDE_MOD("org.cdere.node-editor", "Manoeuvre Node Editor", "0.9.1", "coade-re")
