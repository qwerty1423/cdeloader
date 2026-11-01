// CDE patch helper DLL — template.
//
// The injected `.cdei` import table makes the game load this DLL at startup
// (see docs/09-modding-and-patching.md).  DllMain then applies runtime fixes:
// byte patches verified against the expected original bytes, and optional
// inline hooks.  Keep this file dependency-free: only Win32 and a static CRT,
// so it can be loaded into the MSVC-built game.
//
// Build: scripts/build-patchdll.sh  (i686-w64-mingw32-g++)
//
// Everything here is intentionally defensive: if an address does not hold the
// bytes we expect (a different game build), the fix is skipped and logged
// instead of crashing the host.

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define HELPER_VERSION "0.1.0"

static HMODULE g_self = NULL;
static char    g_log_path[MAX_PATH];
static char    g_dir[MAX_PATH];
static BOOL    g_enabled = TRUE;

// ---------------------------------------------------------------------------
// logging

static void log_line(const char* fmt, ...) {
    if (!g_log_path[0]) return;
    FILE* f = fopen(g_log_path, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
            st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
    fprintf(stderr, "cdepatch: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void discover_dir(void) {
    // The exe's directory, so the log lands next to CDE.exe.
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    _snprintf(g_log_path, MAX_PATH, "%s\\cdepatch.log", g_dir);
    g_log_path[MAX_PATH - 1] = 0;
}

// ---------------------------------------------------------------------------
// memory helpers

static void* va_to_ptr(uintptr_t va) {
    // The game is preferred-base 0x400000.  If ASLR moved it, relocate.
    HMODULE base = GetModuleHandleA(NULL);
    uintptr_t image = (uintptr_t)base;
    if (va >= 0x400000 && image != 0x400000) {
        return (void*)(image + (va - 0x400000));
    }
    return (void*)va;
}

static BOOL write_protected(void* addr, const void* data, SIZE_T size) {
    DWORD old;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(addr, data, size);
    VirtualProtect(addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    return TRUE;
}

// Patch `size` bytes at `va`, but only if they currently equal `expected`.
static BOOL patch_bytes(const char* name, uintptr_t va,
                        const unsigned char* expected,
                        const unsigned char* replacement, SIZE_T size) {
    void* addr = va_to_ptr(va);
    if (memcmp(addr, expected, size) != 0) {
        log_line("PATCH SKIPPED %s @ 0x%08lx: original bytes differ", name,
                 (unsigned long)va);
        return FALSE;
    }
    if (!write_protected(addr, replacement, size)) {
        log_line("PATCH FAILED %s @ 0x%08lx: VirtualProtect error %lu", name,
                 (unsigned long)va, (unsigned long)GetLastError());
        return FALSE;
    }
    log_line("patched %s @ 0x%08lx", name, (unsigned long)va);
    return TRUE;
}

// ---------------------------------------------------------------------------
// inline hook: 5-byte E9 rel32 jump + trampoline (x86)

#pragma pack(push, 1)
struct Trampoline {
    unsigned char code[16 + 5];  // copied prologue + jmp back
};
#pragma pack(pop)

static void* install_hook(const char* name, void* target, void* detour) {
    // Copy the first 5 bytes to a trampoline near the target, then append a
    // rel32 jump back to target+5.  Only whole-instruction-safe targets should
    // be hooked; we require the caller to know the prologue is >= 5 bytes.
    unsigned char* t = (unsigned char*)target;
    void* mem = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
    if (!mem) {
        log_line("HOOK FAILED %s: no trampoline memory", name);
        return NULL;
    }
    unsigned char* tr = (unsigned char*)mem;
    memcpy(tr, t, 5);
    tr[5] = 0xE9;
    *(int32_t*)(tr + 6) = (int32_t)((t + 5) - (tr + 10));

    unsigned char jump[5];
    jump[0] = 0xE9;
    *(int32_t*)(jump + 1) = (int32_t)((unsigned char*)detour - (t + 5));
    if (!write_protected(t, jump, 5)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        log_line("HOOK FAILED %s @ %p: VirtualProtect error %lu", name, target,
                 (unsigned long)GetLastError());
        return NULL;
    }
    log_line("hooked %s @ %p -> %p (trampoline %p)", name, target, detour, tr);
    return tr;
}

// ---------------------------------------------------------------------------
// settings (cdepatch.ini next to the exe)

static void read_settings(void) {
    char ini[MAX_PATH];
    _snprintf(ini, MAX_PATH, "%s\\cdepatch.ini", g_dir);
    ini[MAX_PATH - 1] = 0;
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        log_line("no cdepatch.ini, defaults");
        return;
    }
    int enabled = GetPrivateProfileIntA("patch", "enabled", 1, ini);
    g_enabled = enabled ? TRUE : FALSE;
    log_line("settings: enabled=%d (cdepatch.ini)", enabled);
}

// ---------------------------------------------------------------------------
// fixes
//
// Add one entry per bug fix.  Keep the expected bytes so a mismatched build is
// skipped rather than corrupted.  example (do NOT ship): disable a `ret` guard.
//
//   static const unsigned char orig[] = {0xFF, 0xD6};
//   static const unsigned char repl[] = {0x66, 0x90};
//   patch_bytes("SomeCheck", 0x00504DD6, orig, repl, sizeof(orig));

static void apply_fixes(void) {
    if (!g_enabled) {
        log_line("patch disabled");
        return;
    }
    // No fixes in the template; reference the API so it stays available.
    (void)&patch_bytes;
    (void)&install_hook;
    log_line("no fixes configured (template build)");
}

// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        discover_dir();
        log_line("cdepatch %s loaded, host at %p", HELPER_VERSION,
                 GetModuleHandleA(NULL));
        read_settings();
        apply_fixes();
    }
    return TRUE;
}

// The injected import resolves this symbol; the loader only needs it to exist.
// We do the work in DllMain, so this is informational.
extern "C" __declspec(dllexport) int CdeInit(void) {
    log_line("CdeInit() called");
    return 0;
}
