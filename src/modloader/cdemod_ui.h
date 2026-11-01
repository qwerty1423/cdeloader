/* cdemod_ui.h — in-game UI (Dear ImGui overlay) for CDE mod plugins.
 *
 * The overlay lives in cdeloader.dll; a plugin links against the loader and
 * calls these functions.  A panel is drawn by the loader inside its ImGui menu
 * (toggle with F2 in game; Insert also works).  All calls must happen on the render thread, i.e.
 * from a registered panel draw function — do NOT call them from on_init/on_frame.
 *
 * If the overlay is unavailable (no OpenGL swap hook), cde_ui_available() is 0
 * and registering a panel is a harmless no-op.
 */
#ifndef CDEMOD_UI_H
#define CDEMOD_UI_H

#include "cdemod_api.h"

#if defined(_WIN32)
#  if defined(CDEMOD_UI_BUILD)
#    define CDEMOD_UI_API __declspec(dllexport)
#  else
#    define CDEMOD_UI_API __declspec(dllimport)
#  endif
#else
#  define CDEMOD_UI_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (CDEMOD_CALL *CdeUiDrawFn)(void);

/* Restart the game: start a second instance and exit this one. Safe to call with a staged update
 * pending - the successor's proxy waits for this process to disappear before it replaces any DLL.
 * Call it only when the game is idle: unsaved progress is lost. */
CDEMOD_UI_API int CDEMOD_CALL cde_game_relaunch(void);

/* Non-zero once the overlay's ImGui context is up. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_available(void);

/* Register (or replace) a named panel.  Safe to call from on_init. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_panel(const char* name, CdeUiDrawFn draw);

/* Register (or replace) a hotkey: `vk` is a Win32 virtual-key code (e.g. 0x75
 * = VK_F6).  The callback runs on the window thread when the key is pressed and
 * the loader's menu is closed; keep it to flags/state changes.  Registering the
 * same vk twice replaces the callback.  Safe to call from on_init. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_hotkey(int vk, CdeUiDrawFn fn);

/* Register (or replace) an always-on HUD callback: it is drawn every frame,
 * whether or not the F10 menu is open, and is the right place for markers and
 * other overlays.  Widgets are not meaningful there; use the cde_ui_overlay_*
 * primitives.  Safe to call from on_init. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_hud(const char* name, CdeUiDrawFn draw);

/* Widgets, valid only inside a panel draw callback.  Each returns non-zero when
 * the value changed / the button was pressed. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_button(const char* label);
/* Small button, for the rows of a value column. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_button_small(const char* label);
/* Tint the buttons drawn until the matching pop: 0..1 per channel. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_push_button_color(float r, float g, float b);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_pop_button_color(void);
CDEMOD_UI_API int CDEMOD_CALL cde_ui_checkbox(const char* label, int* value);
CDEMOD_UI_API int CDEMOD_CALL cde_ui_slider_int(const char* label, int* value, int lo, int hi);
CDEMOD_UI_API int CDEMOD_CALL cde_ui_slider_float(const char* label, float* value, float lo, float hi);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_text(const char* text);
/* Greyed text: keep a row in place when its value is unavailable, instead of
 * hiding it and making the window jump around. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_text_disabled(const char* text);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_separator(void);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_same_line(void);
/* Continue on the same line at an absolute x offset, so a column of values
 * lines up under a label the way the ImGui demo window does it. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_same_line_at(float x);

/* Slider plus a numeric entry box on the same line, for people who want a round
 * number or a value the slider cannot hit.  Returns non-zero when the value
 * changed. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_slider_float_edit(const char* label,
                                                       float* value, float lo,
                                                       float hi);
/* The same, with a printf format choosing the precision of the entry box, and a
 * small "reset" button that puts `reset_to` back.  Pass NULL to omit the button. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_slider_float_edit_fmt(const char* label,
                                                           float* value, float lo,
                                                           float hi, const char* fmt,
                                                           const float* reset_to);
/* Slider on a line of its own, so it can use the whole window width, with its
 * label and entry box on the line below and an optional reset button.  `logarithmic`
 * spreads the low end of the range out, which is what a step size wants. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_slider_row(const char* label, float* value,
                                                float lo, float hi, const char* fmt,
                                                const float* reset_to, int logarithmic);
/* While this is on, a button held down fires repeatedly (ImGui's repeat).  Useful
 * for +/- controls that a user taps many times. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_push_button_repeat(int on);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_pop_button_repeat(void);

/* Entry box alone, for a value that wants typing rather than dragging. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_input_float(const char* label, float* value,
                                                 float lo, float hi, const char* fmt);

/* The game's TacticalManager instance, captured by the loader from the game's
 * own call to TacticalManager::GetPlayerSpacecraftComponents.  NULL until a
 * tactical view has been entered at least once.  Never guess this pointer: a
 * tactical method called on another manager crashes. */
CDEMOD_UI_API void* CDEMOD_CALL cde_game_tactical_manager(void);

/* Tooltip for the item drawn immediately before this call (ImGui's
 * SetTooltip).  Use it instead of writing explanations into the interface. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_tooltip(const char* text);

/* Draws a muted "(?)" after the last item and shows `text` while it is
 * hovered.  The ReShade-style way to keep a label short. */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_help(const char* text);

/* Screen-space drawing for markers/HUDs.  Zero argument errors are ignored.
 * Colour is ImGui's packed format: 0xAABBGGRR (alpha in the high byte).
 * Valid only inside a panel draw callback (the overlay is composited over the
 * game, so these draw above the 3D view). */
CDEMOD_UI_API void CDEMOD_CALL cde_ui_screen_size(int* width, int* height);
/* Draggable ImGui window for a plugin HUD.  Call begin/end once per frame from a
 * cde_ui_hud callback (or a panel).  begin returns 1 when content is visible but
 * end must always be called.  `open` may be NULL; `flags` is passed to
 * ImGui::Begin.  cond: 0 = none, 1 = always, 2 = once, 4 = first use ever
 * (ImGuiCond values).  Widgets (cde_ui_text/button/...) and the window_* draw
 * helpers are valid between begin and end. */
CDEMOD_UI_API int CDEMOD_CALL cde_ui_window_begin(const char* name, int* open, int flags);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_window_end(void);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_window_pos(float x, float y, int cond);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_window_size(float w, float h, int cond);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_cursor_screen_pos(float* x, float* y);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_dummy(float w, float h);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_window_line(float x1, float y1, float x2,
                                                  float y2, unsigned rgba,
                                                  float thickness);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_window_rect(float x, float y, float w,
                                                  float h, unsigned rgba,
                                                  float thickness);

CDEMOD_UI_API void CDEMOD_CALL cde_ui_overlay_text(float x, float y, unsigned rgba,
                                                   const char* text);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_overlay_line(float x1, float y1, float x2,
                                                   float y2, unsigned rgba,
                                                   float thickness);
CDEMOD_UI_API void CDEMOD_CALL cde_ui_overlay_rect(float x, float y, float w,
                                                   float h, unsigned rgba,
                                                   float thickness);

#ifdef __cplusplus
}
#endif

#endif /* CDEMOD_UI_H */
