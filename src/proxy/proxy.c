/* proxy.c — load the CDE mod loader without changing CDE.exe.
 *
 * Installed as <game>/vcruntime140.dll; every export is a forwarder to the
 * renamed original (see vcruntime140.def), so the game keeps its normal
 * runtime.  PROXY_LABEL only affects the log line.
 *
 * DllMain loads cdeloader.dll and calls CdeModInit.  If the exe already imports
 * cdeloader.dll (delta mode), LoadLibrary returns the loaded module and Windows
 * does not run its DllMain again, so init happens exactly once either way.
 *
 * Before loading the loader, DllMain waits for any previous CDE.exe to exit and then applies a staged
 * update from mods/.pending.
 * That is the only moment at which it is possible: Windows locks a loaded DLL, so
 * cdeloader.dll and every plugin are unreplaceable once CdeModInit has run, and at
 * process attach none of them is loaded yet.  The updater mod stages files there
 * and asks for a restart; the replacement happens here on the way back up.
 *
 * Build: scripts/build-proxy.sh
 */
#include <windows.h>
#include <tlhelp32.h>

#ifndef PROXY_LABEL
#define PROXY_LABEL "vcruntime140"
#endif

static void proxy_log(const char* text) {
    HANDLE f = CreateFileA("cdeloader.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, text, (DWORD)lstrlenA(text), &written, 0);
        CloseHandle(f);
    }
}

/* Apply anything the updater mod staged in mods\.pending.
 *
 * Layout: mods\.pending\<relative path> mirrors the game folder, so a staged file
 * mods\.pending\mods\laser-cap\laser-cap.dll replaces mods\laser-cap\laser-cap.dll.
 * A sibling "ready" file marks the staging complete; without it the directory is
 * treated as a partial download and left alone, which is what keeps a half-finished
 * transfer from being applied.
 *
 * Replacement is copy-then-rename rather than a direct copy: the destination may be
 * read-only, and MoveFileEx over an existing file is atomic enough for this. No
 * failure aborts startup - a broken update must never stop the game from running.
 */
/* Wait for a previous CDE.exe to exit before touching its files.
 *
 * This is what makes a separate relaunch helper unnecessary: the game starts its successor the simple
 * way (CreateProcess, then exit), and the successor's proxy does the waiting here, at process attach,
 * before anything is loaded or replaced. A normal launch finds no other instance and pays nothing.
 *
 * 15 seconds is longer than any real shutdown and short enough that a hung predecessor cannot hold the
 * new process hostage; when it expires the update is applied anyway and the log says so.
 */
static void proxy_wait_for_previous_instance(void) {
    DWORD self = GetCurrentProcessId();
    for (int round = 0; round < 30; round++) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32 entry;
        entry.dwSize = sizeof(entry);
        DWORD other = 0;
        if (Process32First(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == self) continue;
                if (lstrcmpiA(entry.szExeFile, "CDE.exe") != 0) continue;
                other = entry.th32ProcessID;
                break;
            } while (Process32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
        if (!other) {
            if (round > 0) proxy_log("[proxy] previous instance gone\r\n");
            return;
        }
        if (round == 0) proxy_log("[proxy] waiting for the previous instance to exit\r\n");
        Sleep(500);
    }
    proxy_log("[proxy] previous instance still running after 15s; continuing\r\n");
}

static void proxy_apply_pending(void) {
    char ready[MAX_PATH];
    lstrcpyA(ready, "mods\\.pending\\ready");
    if (GetFileAttributesA(ready) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    char pattern[MAX_PATH];
    lstrcpyA(pattern, "mods\\.pending\\*");
    WIN32_FIND_DATAA find;
    HANDLE it = FindFirstFileA(pattern, &find);
    if (it == INVALID_HANDLE_VALUE) {
        return;
    }
    int applied = 0;
    int skipped = 0;
    do {
        if (lstrcmpA(find.cFileName, "ready") == 0) {
            continue;                       /* the marker, handled separately */
        }
        if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            /* A top-level staged file is a game-root file: cdeloader.dll, its symbols. They were being
             * skipped here, so a loader update staged eight files and applied six - and the leftover
             * copy then got scanned as a plugin folder, initialising the loader twice. */
            char target[MAX_PATH];
            wsprintfA(target, "%s", find.cFileName);
            SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);
            char source[MAX_PATH];
            wsprintfA(source, "mods\\.pending\\%s", find.cFileName);
            if (CopyFileA(source, target, FALSE)) {
                applied++;
            } else {
                skipped++;
            }
            continue;
        }
        if (find.cFileName[0] == '.') {
            continue;
        }
        /* Walk this staged subtree and move every file into place. */
        char sub[MAX_PATH];
        wsprintfA(sub, "mods\\.pending\\%s\\*", find.cFileName);
        WIN32_FIND_DATAA item;
        HANDLE sub_it = FindFirstFileA(sub, &item);
        if (sub_it == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            char source[MAX_PATH];
            char target[MAX_PATH];
            wsprintfA(source, "mods\\.pending\\%s\\%s", find.cFileName, item.cFileName);
            wsprintfA(target, "mods\\%s\\%s", find.cFileName, item.cFileName);
            SetFileAttributesA(target, FILE_ATTRIBUTE_NORMAL);
            if (CopyFileA(source, target, FALSE)) {
                applied++;
            } else {
                skipped++;
            }
        } while (FindNextFileA(sub_it, &item));
        FindClose(sub_it);
    } while (FindNextFileA(it, &find));
    FindClose(it);

    char msg[MAX_PATH];
    wsprintfA(msg, "[proxy] staged update applied: %d file(s), %d failed\r\n", applied, skipped);
    proxy_log(msg);

    /* Consume the marker so the same staging is not reapplied on every launch, and remove the staged
     * copies: a leftover mods\.pending\cdeloader.dll is a full second copy of the loader inside the
     * plugin search path. */
    DeleteFileA(ready);

    char stale[MAX_PATH];
    lstrcpyA(stale, "mods\\.pending\\*");
    WIN32_FIND_DATAA leftover;
    HANDLE walk = FindFirstFileA(stale, &leftover);
    if (walk != INVALID_HANDLE_VALUE) {
        do {
            if (lstrcmpA(leftover.cFileName, ".") == 0 || lstrcmpA(leftover.cFileName, "..") == 0) {
                continue;
            }
            char path[MAX_PATH];
            wsprintfA(path, "mods\\.pending\\%s", leftover.cFileName);
            if (leftover.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                char inner[MAX_PATH];
                wsprintfA(inner, "%s\\*", path);
                WIN32_FIND_DATAA item;
                HANDLE sub = FindFirstFileA(inner, &item);
                if (sub != INVALID_HANDLE_VALUE) {
                    do {
                        if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        char file[MAX_PATH];
                        wsprintfA(file, "%s\\%s", path, item.cFileName);
                        SetFileAttributesA(file, FILE_ATTRIBUTE_NORMAL);
                        DeleteFileA(file);
                    } while (FindNextFileA(sub, &item));
                    FindClose(sub);
                }
                RemoveDirectoryA(path);
            } else {
                DeleteFileA(path);
            }
        } while (FindNextFileA(walk, &leftover));
        FindClose(walk);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        proxy_wait_for_previous_instance();
        proxy_apply_pending();
        HMODULE loader = LoadLibraryA("cdeloader.dll");
        if (!loader) {
            proxy_log("[proxy] cdeloader.dll not found; proxy idle\r\n");
            return TRUE;
        }
        typedef int(__cdecl* init_fn)(void);
        init_fn init = (init_fn)GetProcAddress(loader, "CdeModInit");
        if (init) {
            char msg[128];
            int rc = init();
            wsprintfA(msg, "[proxy] %s proxy loaded; CdeModInit=%d\r\n",
                      PROXY_LABEL, rc);
            proxy_log(msg);
        } else {
            proxy_log("[proxy] CdeModInit missing; proxy idle\r\n");
        }
    }
    return TRUE;
}
