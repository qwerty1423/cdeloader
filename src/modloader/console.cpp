#include "console.h"

#include <windows.h>

static int g_allocated = 0;

static void ensure_console(void) {
    if (g_allocated) return;
    if (!GetConsoleWindow()) {
        if (!AllocConsole()) return;
        SetConsoleTitleA("CDE Mod Loader - log");
        SetConsoleOutputCP(CP_UTF8);
    }
    g_allocated = 1;
}

void console_start(int visible) {
    if (!visible) return;
    ensure_console();
    HWND hwnd = GetConsoleWindow();
    /* show without stealing focus from the game window */
    if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

void console_log_line(int level, const char* text) {
    if (!g_allocated || !text) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || !out) return;
    WORD attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    if (level == 0) attr = FOREGROUND_RED | FOREGROUND_INTENSITY;        /* error */
    else if (level == 1) attr = FOREGROUND_RED | FOREGROUND_GREEN |
                               FOREGROUND_INTENSITY;                     /* warn */
    SetConsoleTextAttribute(out, attr);
    DWORD written = 0;
    WriteConsoleA(out, text, (DWORD)lstrlenA(text), &written, 0);
    WriteConsoleA(out, "\r\n", 2, &written, 0);
    SetConsoleTextAttribute(out, FOREGROUND_RED | FOREGROUND_GREEN |
                                   FOREGROUND_BLUE);
}

void console_toggle(void) {
    ensure_console();
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOWNOACTIVATE);
}

void* console_window(void) {
    return (void*)GetConsoleWindow();
}

int console_is_visible(void) {
    HWND hwnd = GetConsoleWindow();
    return hwnd && IsWindowVisible(hwnd) ? 1 : 0;
}
