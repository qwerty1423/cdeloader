/* overlay.h — loader-internal interface to the ImGui overlay (phase C).
 *
 * The public, plugin-facing API is cdemod_ui.h; this header is what
 * modloader.cpp needs to drive the overlay (install the swap detour, run a
 * frame, share the logger).
 */
#ifndef CDEMOD_OVERLAY_H
#define CDEMOD_OVERLAY_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CdeOverlayLogFn)(int level, const char* message);

/* Route overlay messages through the loader's logger. */
void overlay_set_logger(CdeOverlayLogFn fn);

/* Start with the menu open (for tests / screenshots). */
void overlay_set_start_open(int open);
void overlay_set_panels_open(int open);

/* Run one overlay frame against the current OpenGL context (from the
 * wglSwapBuffers detour).  Lazily creates the ImGui context on first use. */
void overlay_frame(HDC hdc);

/* Non-zero once ImGui is initialised. */
int overlay_available(void);

/* Run ``fn`` under the loader's per-call fault guard (implemented in
 * modloader.cpp); returns 0 if it faulted. */
int cde_run_guarded(const char* id, void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* CDEMOD_OVERLAY_H */
