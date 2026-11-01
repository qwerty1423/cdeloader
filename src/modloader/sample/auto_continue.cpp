// auto_continue.cpp — keep the strategic layer running turns (R1).
//
// The strategic layer is turn-based: the Continue/Run Turn button advances one
// turn by the step the player selected (1 min … 30 days).  Directly calling
// `StrategicManager::RunTurn` from a render hook crashes the game (wrong phase),
// so the default mode here is the user's original idea: **click the real
// button**.  That goes through the game's own input path, so orders, modal
// checks and UI state behave exactly as if the player clicked it.
//
// Modes:
//   mode = click    (default) posts WM_LBUTTONDOWN/UP at the button position
//   mode = direct   calls RunTurn from the strategic RenderUI hook (experimental,
//                   known to crash; kept for experiments only)
//
// Config (mods/config/org.cdere.autocontinue.ini):
//   enabled = 0|1        master switch (default 0)
//   mode = click|direct  default click
//   interval_ms = 700    minimum gap between clicks/turns
//   x_pct = 0.913       button position as a fraction of the client area
//   y_pct = 0.942
//   method = post|input  post = PostMessage (default), input = SendInput
//   max_turns = 0        stop after N (0 = unlimited)
//   log_every = 20
//
// Build: scripts/build-modloader.sh

static void ac_init(void);
static void ac_frame(void);
static void ac_shutdown(void);

#define CDE_USER_INIT()  ac_init()
#define CDE_USER_FRAME() ac_frame()
#define CDE_USER_SHUTDOWN() ac_shutdown()

#include "cdemod.hpp"

#include <windows.h>

// --- direct mode (experimental) ---------------------------------------------
typedef bool(__thiscall* IsTurnRunningFn)(const void* self);
typedef void(__thiscall* RunTurnFn)(void* self);
typedef void(__thiscall* RenderUIFn)(void* sm);

static uintptr_t g_va_is_running = 0;
static uintptr_t g_va_run_turn = 0;
static RenderUIFn g_orig_render_ui = nullptr;
static void* g_sm = nullptr;

// --- config ------------------------------------------------------------------
static int g_enabled = 0;
static int g_direct = 0;
static int g_interval_ms = 700;
static double g_x_pct = 0.913;
static double g_y_pct = 0.942;
static int g_use_input = 0;
static int g_max_turns = 0;
static int g_log_every = 20;

static HWND g_hwnd = nullptr;
static DWORD g_last_tick = 0;
static unsigned long g_turns = 0;
static bool g_logged_first = false;

static DWORD g_enum_pid = 0;
static HWND g_enum_best = nullptr;
static LONG g_enum_area = 0;

static BOOL CALLBACK enum_game_window(HWND w, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != g_enum_pid || !IsWindowVisible(w)) return TRUE;
    RECT r;
    if (!GetClientRect(w, &r)) return TRUE;
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > g_enum_area) { g_enum_best = w; g_enum_area = area; }
    return TRUE;
}

static HWND find_game_window(void) {
    g_enum_pid = GetCurrentProcessId();
    g_enum_best = nullptr;
    g_enum_area = 0;
    EnumWindows(enum_game_window, 0);
    return g_enum_best;
}

static void click_run_turn(void) {
    if (!g_hwnd || !IsWindow(g_hwnd)) g_hwnd = find_game_window();
    if (!g_hwnd) return;
    RECT r;
    if (!GetClientRect(g_hwnd, &r)) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    int x = (int)(w * g_x_pct);
    int y = (int)(h * g_y_pct);
    LPARAM pos = MAKELPARAM(x, y);

    if (g_use_input) {
        // Move the real cursor: only for experiments; post is the default.
        POINT p = {x, y};
        ClientToScreen(g_hwnd, &p);
        SetCursorPos(p.x, p.y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    } else {
        PostMessageA(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
        PostMessageA(g_hwnd, WM_LBUTTONUP, 0, pos);
    }
}

static void ac_frame(void) {
    if (!g_enabled) return;
    if (g_max_turns > 0 && (int)g_turns >= g_max_turns) return;

    DWORD now = GetTickCount();
    if (g_last_tick != 0 && now - g_last_tick < (DWORD)g_interval_ms) return;

    if (g_direct && g_sm) {
        if (reinterpret_cast<IsTurnRunningFn>(g_va_is_running)(g_sm)) return;
        reinterpret_cast<RunTurnFn>(g_va_run_turn)(g_sm);
    } else {
        click_run_turn();
    }

    g_last_tick = now;
    ++g_turns;
    if (!g_logged_first) {
        g_logged_first = true;
        cde::Logf(CDE_LOG_INFO, "auto_continue: first %s (%s)",
                  g_direct ? "RunTurn" : "button click",
                  g_direct ? "direct" : (g_use_input ? "SendInput" : "PostMessage"));
    }
    if (g_log_every > 0 && g_turns % (unsigned)g_log_every == 0) {
        cde::Logf(CDE_LOG_INFO, "auto_continue: %lu turns requested", g_turns);
    }
}

static void __thiscall ac_render_ui_detour(void* sm) {
    if (sm != g_sm) g_sm = sm;
    if (g_orig_render_ui) g_orig_render_ui(sm);
}

static void ac_init(void) {
    g_enabled = cde::ConfigBool("enabled", 0);
    char mode[32] = {0};
    cde::ConfigStr("mode", mode, sizeof(mode));
    g_direct = (mode[0] && (mode[0] == 'd' || mode[0] == 'D')) ? 1 : 0;
    g_interval_ms = cde::ConfigInt("interval_ms", 700);
    g_x_pct = cde::ConfigFloat("x_pct", 0.913f);
    g_y_pct = cde::ConfigFloat("y_pct", 0.942f);
    char method[16] = {0};
    cde::ConfigStr("method", method, sizeof(method));
    g_use_input = (method[0] && (method[0] == 'i' || method[0] == 'I')) ? 1 : 0;
    g_max_turns = cde::ConfigInt("max_turns", 0);
    g_log_every = cde::ConfigInt("log_every", 20);

    if (g_direct) {
        g_va_is_running = cde::Resolve("StrategicManager::IsTurnRunning");
        g_va_run_turn = cde::Resolve("StrategicManager::RunTurn");
        uintptr_t render = cde::Resolve("StrategicManager::RenderUI");
        if (g_va_is_running && g_va_run_turn && render) {
            g_orig_render_ui = cde::Hook(
                "StrategicManager::RenderUI",
                reinterpret_cast<RenderUIFn>(render), &ac_render_ui_detour);
        } else {
            cde::Log(CDE_LOG_ERROR, "auto_continue: direct mode symbols missing");
        }
    }

    cde::Logf(CDE_LOG_INFO,
              "auto_continue ready: enabled=%d mode=%s interval=%dms at %.3f,%.3f",
              g_enabled, g_direct ? "direct" : "click", g_interval_ms,
              g_x_pct, g_y_pct);
}

static void ac_shutdown(void) {
    cde::Logf(CDE_LOG_INFO, "auto_continue stopped after %lu turns", g_turns);
}

CDE_MOD("org.cdere.autocontinue", "Auto Continue (R1)", "0.1.0", "coade-re")
