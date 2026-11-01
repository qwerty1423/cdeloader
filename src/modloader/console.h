/* console.h — optional separate logging console (BepInEx-style window).
 *
 * Not the ImGui overlay: this is a real Win32 console allocated on demand, with
 * scrollback and copy/paste.  F3 toggles it; [loader] console = 1 starts visible.
 */
#ifndef CDEMOD_CONSOLE_H
#define CDEMOD_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

void console_start(int visible);
void console_log_line(int level, const char* text);
void console_toggle(void);
int console_is_visible(void);
void* console_window(void);

#ifdef __cplusplus
}
#endif

#endif
