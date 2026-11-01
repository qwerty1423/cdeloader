// overlay.cpp — an in-game Dear ImGui menu for cdeloader (phase C).
//
// Dear ImGui is MIT-licensed and vendored under third_party/imgui (see its
// VENDOR.md).  The loader hooks opengl32!wglSwapBuffers; once per frame we build
// an ImGui frame and render it into the game's current OpenGL context, then let
// the real swap proceed.  The game window's WndProc is subclassed so the toggle
// key (F2/Insert) works.  Game input keeps flowing while the menu is open: only
// the mouse is held back while the cursor is over an ImGui window, so you can
// click panels without also clicking the world (same as a plugin window).
//
// Rendering happens on whichever thread calls wglSwapBuffers (the game's render
// thread); the ImGui context is created for that thread.  Panel draw callbacks
// run there and are fault-guarded by the loader.

#include "overlay.h"
#include "console.h"
#include "cdemod_ui.h"

#include <stdio.h>
#include <stdarg.h>

#include <cmath>
#include <map>
#include <string>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_opengl2.h"

// The win32 backend intentionally leaves this declaration out of its header (to
// avoid pulling in <windows.h>); we include windows.h already, so declare it.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

CdeOverlayLogFn g_log = nullptr;

void olog(int level, const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(level, buf);
}

bool g_imgui = false;
bool g_use_gl3 = false;
bool g_open = false;
bool g_start_open = false;
bool g_panels_open = false;   // mod sections start collapsed
bool g_plugin_windows = false;   // a plugin ImGui window was drawn this frame
bool g_want_mouse = false;       // ImGui owned the mouse in the previous frame
bool g_want_keys = false;        // an ImGui text field had the keyboard last frame
bool g_overlay_drag = false;     // the current mouse-down began over the overlay
bool g_show_demo = false;
HWND g_hwnd = nullptr;
WNDPROC g_orig_wndproc = nullptr;

CRITICAL_SECTION g_panels_lock;
bool g_lock_ready = false;

struct Panel {
    std::string name;
    std::string owner;  // mod id, for config scoping while the callback runs
    CdeUiDrawFn draw;
};
extern "C" const char* cde_current_mod_id(void);
extern "C" void cde_set_current_mod(const char* id);
std::vector<Panel> g_panels;
std::vector<Panel> g_hud;
bool g_in_panel = false;          // only true while a plugin's draw runs
struct Hotkey { int vk; CdeUiDrawFn fn; std::string owner; };
std::vector<Hotkey> g_hotkeys;
DWORD g_render_thread = 0;

bool detect_gl3(void) {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    typedef const unsigned char*(APIENTRY * GetStringFn)(unsigned int);
    GetStringFn getstr = reinterpret_cast<GetStringFn>(
        reinterpret_cast<void*>(GetProcAddress(gl, "glGetString")));
    if (!getstr) return false;
    const char* v = reinterpret_cast<const char*>(getstr(0x1F02 /*GL_VERSION*/));
    int major = 0;
    if (v) sscanf(v, "%d", &major);
    return major >= 3;
}

LRESULT CALLBACK overlay_wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // F3: separate logging console (not the ImGui overlay).  F2 used to be the
    // console; the desktop environment owns F1 (help), so the framework moved.
    // F3 is not the loader's any more: plugins register it through cde_ui_hotkey,
    // and the console is switched from the menu.
    if (false && w == VK_F3) return 0;
    // F2/Insert: the menu.  F1 is deliberately not intercepted (XFCE opens help
    // on it); F10/F11 are the game's pause/fullscreen.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        (w == VK_F2 || w == VK_INSERT)) {
        if ((l & (1 << 30)) == 0) g_open = !g_open;
        return 0;
    }
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if ((l & (1 << 30)) == 0) {  // ignore auto-repeat
            for (const Hotkey& hk : g_hotkeys) {
                if (hk.vk == (int)w) {
                    g_in_panel = false;
                    if (hk.fn) {
                    const std::string prev = cde_current_mod_id();
                    cde_set_current_mod(hk.owner.c_str());
                    cde_run_guarded("hotkey", hk.fn);
                    cde_set_current_mod(prev.c_str());
                }
                    return 0;
                }
            }
        }
    }
    // Release of the toggle keys must not leak into the game either.
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) &&
        (w == VK_F2 || w == VK_INSERT)) {
        return 0;
    }
    if (g_open || g_plugin_windows) {
        // Always tell ImGui where the mouse is: it needs the move events to work out
        // whether the cursor is over one of its windows at all.  What broke the game's
        // node drag was the backend taking the mouse capture, and that is compiled out
        // (IMGUI_WIN32_NO_MOUSE_CAPTURE), not the forwarding.
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        switch (msg) {
            // Movement is never swallowed: the game needs it to track anything it
            // is dragging, and a mouse that merely passes over the overlay must not
            // interrupt that.  Only button presses are taken, and a press is only
            // taken when it begins over the overlay; once the game owns a drag it
            // keeps every event until it releases the button.
            case WM_MOUSEMOVE:
                break;
            case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
                g_overlay_drag = g_want_mouse;
                if (g_overlay_drag) return 0;
                break;
            case WM_LBUTTONUP: case WM_RBUTTONUP:
            case WM_MBUTTONUP: case WM_XBUTTONUP:
            {
                const bool ours = g_overlay_drag;
                g_overlay_drag = false;
                if (ours) return 0;
                break;
            }
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                if (g_want_mouse) return 0;
                break;
            case WM_SETCURSOR:
                if (g_want_mouse) return 0;
                break;
            default:
                break;  // keyboard always reaches the game (no text fields here)
        }
    }
    return CallWindowProcA(g_orig_wndproc, h, msg, w, l);
}

bool ensure_imgui(HDC hdc) {
    if (g_imgui) return true;
    HWND hwnd = WindowFromDC(hdc);
    if (!hwnd) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // don't litter the game folder
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    g_use_gl3 = detect_gl3();
    if (g_use_gl3)
        ImGui_ImplOpenGL3_Init(nullptr);  // default GLSL; OK for GL 3.0+
    else
        ImGui_ImplOpenGL2_Init();

    g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&overlay_wndproc)));
    g_hwnd = hwnd;
    g_imgui = true;
    g_render_thread = GetCurrentThreadId();
    g_open = g_start_open;
    olog(2, "overlay ready (OpenGL %s; F2 or Insert toggles the menu)",
         g_use_gl3 ? "3" : "2");
    return true;
}

bool ui_ready(void) {
    return g_imgui && g_in_panel && GetCurrentThreadId() == g_render_thread;
}

void draw_panels(void) {
    std::vector<Panel> panels;
    EnterCriticalSection(&g_panels_lock);
    panels = g_panels;
    LeaveCriticalSection(&g_panels_lock);

    std::vector<std::string> dead;
    for (const Panel& p : panels) {
        if (ImGui::CollapsingHeader(p.name.c_str(),
                                    g_panels_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            // Panels share one window, so give each its own ID scope; otherwise
            // identical widget labels (e.g. "enabled") collide.
            ImGui::PushID(p.name.c_str());
            const std::string prev = cde_current_mod_id();
            cde_set_current_mod(p.owner.c_str());
            g_in_panel = true;
            bool ok = p.draw && cde_run_guarded(p.name.c_str(), p.draw);
            g_in_panel = false;
            cde_set_current_mod(prev.c_str());
            ImGui::PopID();
            if (!ok) dead.push_back(p.name);
        }
    }
    if (!dead.empty()) {
        EnterCriticalSection(&g_panels_lock);
        for (const std::string& d : dead) {
            for (size_t j = 0; j < g_panels.size(); ++j)
                if (g_panels[j].name == d) {
                    olog(0, "UI panel '%s' faulted; removed", d.c_str());
                    g_panels.erase(g_panels.begin() + j);
                    break;
                }
        }
        LeaveCriticalSection(&g_panels_lock);
    }
}

extern "C" int cde_mods_count(void);
extern "C" const char* cde_mods_id(int index);
extern "C" const char* cde_mods_version(int index);
extern "C" const char* cde_mods_dir(int index);
extern "C" int cde_mods_disabled_count(void);
extern "C" const char* cde_mods_disabled_id(int index);
extern "C" const char* cde_mods_disabled_version(int index);
extern "C" int cde_mods_pending_changes(void);
extern "C" int cde_mods_set_disabled(const char* id, int disabled);
extern "C" int cde_game_relaunch(void);

void draw_mod_list(void) {
    const int n = cde_mods_count();
    const int disabled = cde_mods_disabled_count();
    char header[64];
    snprintf(header, sizeof(header), "Plugins (%d)", n + disabled);
    if (!ImGui::CollapsingHeader(header)) return;

    if (n + disabled == 0) ImGui::TextDisabled("No plugins installed.");

    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%s  %s", cde_mods_id(i), cde_mods_version(i));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cde_mods_dir(i));
        ImGui::SameLine();
        if (ImGui::SmallButton("disable")) cde_mods_set_disabled(cde_mods_id(i), 1);
        ImGui::PopID();
    }

    const ImVec4 off = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    for (int i = 0; i < disabled; ++i) {
        ImGui::PushID(n + i);
        ImGui::PushStyleColor(ImGuiCol_Text, off);
        ImGui::Text("%s  %s", cde_mods_disabled_id(i), cde_mods_disabled_version(i));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("enable")) cde_mods_set_disabled(cde_mods_disabled_id(i), 0);
        ImGui::PopID();
    }

    if (cde_mods_pending_changes()) {
        ImGui::TextUnformatted("Restart required.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Relaunch game")) cde_game_relaunch();
    }
}

void draw_hud(void) {
    std::vector<Panel> hud;
    EnterCriticalSection(&g_panels_lock);
    hud = g_hud;
    LeaveCriticalSection(&g_panels_lock);

    std::vector<std::string> dead;
    for (const Panel& p : hud) {
        g_in_panel = true;
        bool ok = p.draw && cde_run_guarded(p.name.c_str(), p.draw);
        g_in_panel = false;
        if (!ok) dead.push_back(p.name);
    }
    if (!dead.empty()) {
        EnterCriticalSection(&g_panels_lock);
        for (const std::string& d : dead) {
            for (size_t j = 0; j < g_hud.size(); ++j)
                if (g_hud[j].name == d) {
                    olog(0, "HUD callback '%s' faulted; removed", d.c_str());
                    g_hud.erase(g_hud.begin() + j);
                    break;
                }
        }
        LeaveCriticalSection(&g_panels_lock);
    }
}


// ---------------------------------------------------------------------------
// window geometry
//
// ImGui sizes a window to its content the first time it is shown, which makes the
// F2 menu tiny on a 4k display.  Every window therefore starts at a sensible size
// and remembers where it was put: positions and sizes are stored in
// cdeloader.ini under [windows], keyed by window name.

struct WindowGeom {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool valid = false;
};

static std::map<std::string, WindowGeom> g_geom;
static bool g_geom_loaded = false;
static bool g_geom_dirty = false;

static std::string geom_key(const char* name) {
    std::string key;
    for (const char* p = name; p && *p; ++p) {
        const char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-')
            key.push_back(c);
        else
            key.push_back('_');
    }
    return key;
}

static std::string g_ini_path;

static std::string ini_path() {
    if (g_ini_path.empty()) {
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        std::string dir(exe);
        const size_t slash = dir.find_last_of("\\");
        if (slash != std::string::npos) dir.resize(slash);
        g_ini_path = dir + "\\cdeloader.ini";
    }
    return g_ini_path;
}

static void read_float(const char* key, float* out, float fallback) {
    char buf[64] = {0};
    GetPrivateProfileStringA("windows", key, "", buf, sizeof buf, ini_path().c_str());
    *out = buf[0] ? static_cast<float>(atof(buf)) : fallback;
}

static void load_geometry(void) {
    if (g_geom_loaded) return;
    g_geom_loaded = true;
    // The menu has a sensible default even when the ini says nothing.
    WindowGeom menu;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    float def_w = display.x > 0 ? display.x * 0.42f : 760.0f;
    float def_h = display.y > 0 ? display.y * 0.55f : 560.0f;
    if (def_w < 760.0f) def_w = 760.0f;
    if (def_h < 540.0f) def_h = 540.0f;
    read_float("menu_x", &menu.x, 80.0f);
    read_float("menu_y", &menu.y, 80.0f);
    read_float("menu_w", &menu.w, def_w);
    read_float("menu_h", &menu.h, def_h);
    menu.valid = true;
    g_geom["CDE Mod Framework"] = menu;
    // Write the defaults once, so the file shows what can be changed.
    char buf[16] = {0};
    GetPrivateProfileStringA("windows", "menu_w", "", buf, sizeof buf, ini_path().c_str());
    if (!buf[0]) g_geom_dirty = true;
}

static void write_float(const char* key, float value) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.0f", value);
    WritePrivateProfileStringA("windows", key, buf, ini_path().c_str());
}

static void flush_geometry(void) {
    if (!g_geom_dirty) return;
    g_geom_dirty = false;
    for (const auto& entry : g_geom) {
        const std::string& name = entry.first;
        const WindowGeom& g = entry.second;
        if (!g.valid) continue;
        const std::string base = geom_key(name.c_str());
        write_float((base + "_x").c_str(), g.x);
        write_float((base + "_y").c_str(), g.y);
        write_float((base + "_w").c_str(), g.w);
        write_float((base + "_h").c_str(), g.h);
    }
}

// Apply a stored position and size before Begin(), and remember what the window
// ended up as afterwards.
static void prepare_window(const char* name, bool default_size) {
    load_geometry();
    if (!name) return;
    auto it = g_geom.find(name);
    if (it == g_geom.end()) {
        if (!default_size) return;
        // A tool window opens at a usable size, offset from the menu and from each
        // other so several can be open without covering one another exactly.
        static int opened = 0;
        WindowGeom fresh;
        fresh.x = 120.0f + 36.0f * static_cast<float>(opened % 8);
        fresh.y = 120.0f + 36.0f * static_cast<float>(opened % 8);
        fresh.w = 460.0f;
        fresh.h = 380.0f;
        fresh.valid = true;
        ++opened;
        g_geom_dirty = true;
        it = g_geom.emplace(name, fresh).first;
    }
    if (!it->second.valid) return;
    ImGui::SetNextWindowPos(ImVec2(it->second.x, it->second.y), ImGuiCond_FirstUseEver);
    if (it->second.w > 0.0f)
        ImGui::SetNextWindowSize(ImVec2(it->second.w, it->second.h),
                                 ImGuiCond_FirstUseEver);
}

static void record_window(const char* name) {
    if (!name) return;
    auto it = g_geom.find(name);
    if (it == g_geom.end()) return;
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    WindowGeom& g = it->second;
    const bool moved = fabsf(g.x - pos.x) > 1.5f || fabsf(g.y - pos.y) > 1.5f ||
                       fabsf(g.w - size.x) > 1.5f || fabsf(g.h - size.y) > 1.5f;
    if (!moved) return;
    g.x = pos.x;
    g.y = pos.y;
    g.w = size.x;
    g.h = size.y;
    g.valid = true;
    g_geom_dirty = true;
}

void draw_menu(void) {
    if (!g_open) return;
    prepare_window("CDE Mod Framework", true);
    const bool menu_open = ImGui::Begin("CDE Mod Framework (F2)", &g_open);
    record_window("CDE Mod Framework");
    if (menu_open) {
        bool console_on = console_is_visible() != 0;
        if (ImGui::Checkbox("log console", &console_on)) console_toggle();
        ImGui::Separator();
        draw_panels();
        if (g_panels.empty()) ImGui::TextDisabled("No mod panels registered.");
        ImGui::Separator();
        draw_mod_list();
        ImGui::Separator();
        ImGui::Checkbox("ImGui demo window", &g_show_demo);
    }
    ImGui::End();
    if (g_show_demo) ImGui::ShowDemoWindow(&g_show_demo);
}

}  // namespace

void overlay_set_logger(CdeOverlayLogFn fn) { g_log = fn; }

void overlay_set_start_open(int open) { g_start_open = open != 0; }
void overlay_set_panels_open(int open) { g_panels_open = open != 0; }

int overlay_available(void) { return g_imgui ? 1 : 0; }

void overlay_frame(HDC hdc) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_panels_lock);
        g_lock_ready = true;
    }
    if (!ensure_imgui(hdc)) return;

    if (g_use_gl3) ImGui_ImplOpenGL3_NewFrame();
    else ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    g_plugin_windows = false;
    draw_menu();
    draw_hud();
    g_want_mouse = ImGui::GetIO().WantCaptureMouse;
    g_want_keys = ImGui::GetIO().WantCaptureKeyboard;
    ImGui::Render();
    // Persist window geometry a couple of times a second at most; the writes are
    // cheap but there is no reason to touch the file every frame.
    static DWORD last_flush = 0;
    const DWORD now_ms = GetTickCount();
    if (g_geom_dirty && now_ms - last_flush > 500) {
        last_flush = now_ms;
        flush_geometry();
    }
    if (g_use_gl3) ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    else ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

// --- public, plugin-facing API (exported from cdeloader.dll) ----------------

extern "C" {

void CDEMOD_CALL cde_ui_hotkey(int vk, CdeUiDrawFn fn) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_panels_lock);
        g_lock_ready = true;
    }
    if (vk <= 0 || !fn) return;
    EnterCriticalSection(&g_panels_lock);
    for (Hotkey& hk : g_hotkeys) {
        if (hk.vk == vk) {
            hk.fn = fn;
            LeaveCriticalSection(&g_panels_lock);
            return;
        }
    }
    g_hotkeys.push_back(Hotkey{vk, fn, cde_current_mod_id()});
    LeaveCriticalSection(&g_panels_lock);
}

void CDEMOD_CALL cde_ui_hud(const char* name, CdeUiDrawFn draw) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_panels_lock);
        g_lock_ready = true;
    }
    if (!draw) return;
    EnterCriticalSection(&g_panels_lock);
    for (Panel& p : g_hud) {
        if (name && p.name == name) {
            p.draw = draw;
            LeaveCriticalSection(&g_panels_lock);
            return;
        }
    }
    g_hud.push_back(Panel{name ? name : "hud", cde_current_mod_id(), draw});
    LeaveCriticalSection(&g_panels_lock);
}

void CDEMOD_CALL cde_ui_panel(const char* name, CdeUiDrawFn draw) {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_panels_lock);
        g_lock_ready = true;
    }
    if (!name) return;
    EnterCriticalSection(&g_panels_lock);
    bool replaced = false;
    for (Panel& p : g_panels)
        if (p.name == name) {
            p.draw = draw;
            p.owner = cde_current_mod_id();
            replaced = true;
            break;
        }
    if (!replaced) g_panels.push_back(Panel{name, cde_current_mod_id(), draw});
    LeaveCriticalSection(&g_panels_lock);
}

int CDEMOD_CALL cde_ui_available(void) { return g_imgui ? 1 : 0; }

int CDEMOD_CALL cde_ui_button(const char* label) {
    if (!ui_ready()) return 0;
    return ImGui::Button(label ? label : "") ? 1 : 0;
}

int CDEMOD_CALL cde_ui_button_small(const char* label) {
    if (!ui_ready()) return 0;
    return ImGui::SmallButton(label ? label : "") ? 1 : 0;
}

void CDEMOD_CALL cde_ui_push_button_color(float r, float g, float b) {
    if (!ui_ready()) return;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(r, g, b, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(r * 1.25f < 1.0f ? r * 1.25f : 1.0f,
                                 g * 1.25f < 1.0f ? g * 1.25f : 1.0f,
                                 b * 1.25f < 1.0f ? b * 1.25f : 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(r * 0.8f, g * 0.8f, b * 0.8f, 1.0f));
}

void CDEMOD_CALL cde_ui_pop_button_color(void) {
    if (!ui_ready()) return;
    ImGui::PopStyleColor(3);
}

void CDEMOD_CALL cde_ui_push_button_repeat(int on) {
    if (!ui_ready()) return;
    ImGui::PushButtonRepeat(on != 0);
}

void CDEMOD_CALL cde_ui_pop_button_repeat(void) {
    if (!ui_ready()) return;
    ImGui::PopButtonRepeat();
}

int CDEMOD_CALL cde_ui_slider_row(const char* label, float* value, float lo, float hi,
                                  const char* fmt, const float* reset_to,
                                  int logarithmic) {
    if (!ui_ready() || !label || !value) return 0;
    int changed = 0;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    const ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp |
                                   (logarithmic ? ImGuiSliderFlags_Logarithmic : 0);
    if (ImGui::SliderFloat("##slider", value, lo, hi, fmt ? fmt : "%.6f", flags))
        changed = 1;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputFloat("##entry", value, 0.0f, 0.0f, fmt ? fmt : "%.6f",
                          ImGuiInputTextFlags_CharsDecimal))
        changed = 1;
    if (reset_to) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            *value = *reset_to;
            changed = 1;
        }
    }
    ImGui::PopID();
    if (*value < lo) { *value = lo; changed = 1; }
    if (*value > hi) { *value = hi; changed = 1; }
    return changed;
}

int CDEMOD_CALL cde_ui_input_float(const char* label, float* value, float lo,
                                   float hi, const char* fmt) {
    if (!ui_ready() || !label || !value) return 0;
    int changed = ImGui::InputFloat(label, value, 0.0f, 0.0f, fmt ? fmt : "%.6f",
                                    ImGuiInputTextFlags_CharsDecimal)
                      ? 1
                      : 0;
    if (*value < lo) { *value = lo; changed = 1; }
    if (*value > hi) { *value = hi; changed = 1; }
    return changed;
}

int CDEMOD_CALL cde_ui_slider_float_edit_fmt(const char* label, float* value,
                                             float lo, float hi, const char* fmt,
                                             const float* reset_to) {
    if (!ui_ready() || !label || !value) return 0;
    int changed = 0;
    if (ImGui::SliderFloat(label, value, lo, hi, fmt ? fmt : "%.6f",
                           ImGuiSliderFlags_AlwaysClamp))
        changed = 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::InputFloat("##entry", value, 0.0f, 0.0f, fmt ? fmt : "%.6f",
                          ImGuiInputTextFlags_CharsDecimal))
        changed = 1;
    if (reset_to) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            *value = *reset_to;
            changed = 1;
        }
    }
    if (*value < lo) { *value = lo; changed = 1; }
    if (*value > hi) { *value = hi; changed = 1; }
    return changed;
}

int CDEMOD_CALL cde_ui_checkbox(const char* label, int* value) {
    if (!ui_ready() || !value) return 0;
    bool v = value && *value;
    bool changed = ImGui::Checkbox(label ? label : "", &v);
    if (value) *value = v ? 1 : 0;
    return changed ? 1 : 0;
}

int CDEMOD_CALL cde_ui_slider_int(const char* label, int* value, int lo, int hi) {
    if (!ui_ready() || !value) return 0;
    return ImGui::SliderInt(label ? label : "", value, lo, hi) ? 1 : 0;
}

int CDEMOD_CALL cde_ui_slider_float(const char* label, float* value, float lo, float hi) {
    if (!ui_ready() || !value) return 0;
    return ImGui::SliderFloat(label ? label : "", value, lo, hi) ? 1 : 0;
}

void CDEMOD_CALL cde_ui_text_disabled(const char* text) {
    ImGui::TextDisabled("%s", text ? text : "");
}

void CDEMOD_CALL cde_ui_text(const char* text) {
    if (!ui_ready()) return;
    ImGui::TextUnformatted(text ? text : "");
}

void CDEMOD_CALL cde_ui_separator(void) { ImGui::Separator(); }

void CDEMOD_CALL cde_ui_same_line_at(float x) { ImGui::SameLine(x); }

// Label in a fixed column, then the slider, then the entry box, so the label is
// always readable and the row does not stretch the window.
int CDEMOD_CALL cde_ui_slider_float_edit(const char* label, float* value, float lo,
                                         float hi) {
    if (!value) return 0;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(150.0f);
    ImGui::SetNextItemWidth(170.0f);
    bool changed = ImGui::SliderFloat("##slider", value, lo, hi);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputFloat("##entry", value, 0.0f, 0.0f, "%.5g")) changed = true;
    if (*value < lo) { *value = lo; changed = true; }
    if (*value > hi) { *value = hi; changed = true; }
    ImGui::PopID();
    return changed ? 1 : 0;
}

// Tooltips wrap at roughly 25 characters per em so they stay square instead of
// stretching across the screen.
static void wrapped_tooltip(const char* text) {
    if (!text || !*text) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void CDEMOD_CALL cde_ui_tooltip(const char* text) {
    if (ImGui::IsItemHovered()) wrapped_tooltip(text);
}

// A "(?)" marker whose tooltip explains the control it follows.
void CDEMOD_CALL cde_ui_help(const char* text) {
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) wrapped_tooltip(text);
}

void CDEMOD_CALL cde_ui_same_line(void) { ImGui::SameLine(); }

void CDEMOD_CALL cde_ui_screen_size(int* width, int* height) {
    ImGuiIO& io = ImGui::GetIO();
    if (width) *width = (int)io.DisplaySize.x;
    if (height) *height = (int)io.DisplaySize.y;
}

int CDEMOD_CALL cde_ui_window_begin(const char* name, int* open, int flags) {
    if (!ui_ready() || !name) return 0;
    g_plugin_windows = true;  // route WndProc input to ImGui while a window shows
    prepare_window(name, true);
    bool open_b = open && *open != 0;
    const bool visible = ImGui::Begin(name, open ? &open_b : nullptr, flags);
    record_window(name);
    if (open) *open = open_b ? 1 : 0;
    return visible ? 1 : 0;
}

void CDEMOD_CALL cde_ui_window_end(void) {
    if (!ui_ready()) return;
    ImGui::End();
}

void CDEMOD_CALL cde_ui_window_pos(float x, float y, int cond) {
    if (!ui_ready()) return;
    ImGui::SetNextWindowPos(ImVec2(x, y), cond);
}

void CDEMOD_CALL cde_ui_window_size(float w, float h, int cond) {
    if (!ui_ready()) return;
    ImGui::SetNextWindowSize(ImVec2(w, h), cond);
}

void CDEMOD_CALL cde_ui_cursor_screen_pos(float* x, float* y) {
    if (!ui_ready()) {
        if (x) *x = 0.0f;
        if (y) *y = 0.0f;
        return;
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (x) *x = p.x;
    if (y) *y = p.y;
}

void CDEMOD_CALL cde_ui_dummy(float w, float h) {
    if (!ui_ready()) return;
    ImGui::Dummy(ImVec2(w, h));
}

void CDEMOD_CALL cde_ui_window_line(float x1, float y1, float x2, float y2,
                                    unsigned rgba, float thickness) {
    if (!ui_ready()) return;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
                                        (ImU32)rgba, thickness > 0 ? thickness : 1.0f);
}

void CDEMOD_CALL cde_ui_window_rect(float x, float y, float w, float h,
                                    unsigned rgba, float thickness) {
    if (!ui_ready()) return;
    ImGui::GetWindowDrawList()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                                        (ImU32)rgba, 0.0f,
                                        thickness > 0 ? thickness : 1.0f);
}

void CDEMOD_CALL cde_ui_overlay_text(float x, float y, unsigned rgba, const char* text) {
    if (!ui_ready() || !text || !*text) return;
    ImGui::GetForegroundDrawList()->AddText(ImVec2(x, y), (ImU32)rgba, text);
}

void CDEMOD_CALL cde_ui_overlay_line(float x1, float y1, float x2, float y2,
                                     unsigned rgba, float thickness) {
    if (!ui_ready()) return;
    ImGui::GetForegroundDrawList()->AddLine(ImVec2(x1, y1), ImVec2(x2, y2),
                                            (ImU32)rgba, thickness > 0 ? thickness : 1.0f);
}

void CDEMOD_CALL cde_ui_overlay_rect(float x, float y, float w, float h,
                                     unsigned rgba, float thickness) {
    if (!ui_ready()) return;
    ImGui::GetForegroundDrawList()->AddRect(ImVec2(x, y), ImVec2(x + w, y + h),
                                            (ImU32)rgba, 0.0f,
                                            thickness > 0 ? thickness : 1.0f);
}

}  // extern "C"
