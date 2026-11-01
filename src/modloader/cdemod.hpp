/* cdemod.hpp — the C++ plugin SDK for the CDE mod loader.
 *
 * Authors include this header, optionally define CDE_USER_INIT / _FRAME /
 * _SHUTDOWN to point at their own functions, and use CDE_MOD(...) once.  The
 * loader then calls into the plugin through the C ABI in cdemod_api.h; the rest
 * of this header is inline sugar so a plugin needs no import library.
 *
 *   #include <cdemod.hpp>
 *
 *   static void my_init()  { CDE_INFO("hello from %s", "my mod"); }
 *   static void my_frame() { }
 *   #define CDE_USER_INIT()    my_init()
 *   #define CDE_USER_FRAME()   my_frame()
 *
 *   static int (CDEMOD_CALL *orig)(int) = nullptr;
 *   static int CDEMOD_CALL detour(int x) { return orig(x) + 1; }
 *
 *   CDE_MOD("org.example.mymod", "My Mod", "1.0.0", "me")
 *
 * (Define the user macros *before* including this header, or forward-declare the
 * functions they name first.)
 */
#ifndef CDEMOD_HPP
#define CDEMOD_HPP

#include "cdemod_api.h"
#include "cdemod_ui.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#ifndef CDE_USER_INIT
#  define CDE_USER_INIT()
#endif
#ifndef CDE_USER_FRAME
#  define CDE_USER_FRAME()
#endif
#ifndef CDE_USER_SHUTDOWN
#  define CDE_USER_SHUTDOWN()
#endif

namespace cde {

/* The loader stores its host table pointer here for the current plugin. */
inline const CdeHostApi*& _host() {
    static const CdeHostApi* h = nullptr;
    return h;
}

inline const CdeHostApi* Host() { return _host(); }

/* ---------------------------------------------------------------- logging */
inline void Log(int level, const char* message) {
    if (_host() && _host()->log) _host()->log(level, message);
}

inline void Logf(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log(level, buf);
}

/* ------------------------------------------------------------ config ---- */
inline int ConfigInt(const char* key, int def = 0) {
    return (_host() && _host()->config_get_int) ? _host()->config_get_int(key, def) : def;
}
inline int ConfigBool(const char* key, int def = 0) {
    return (_host() && _host()->config_get_bool) ? _host()->config_get_bool(key, def) : def;
}
inline float ConfigFloat(const char* key, float def = 0.0f) {
    return (_host() && _host()->config_get_float) ? _host()->config_get_float(key, def) : def;
}
inline int ConfigStr(const char* key, char* out, int cap) {
    return (_host() && _host()->config_get_str) ? _host()->config_get_str(key, out, cap) : 0;
}
inline bool ConfigSetInt(const char* key, int value) {
    return _host() && _host()->config_set_int &&
           _host()->config_set_int(key, value) != 0;
}
inline bool ConfigSetBool(const char* key, int value) {
    return _host() && _host()->config_set_bool &&
           _host()->config_set_bool(key, value ? 1 : 0) != 0;
}
inline bool ConfigSetFloat(const char* key, float value) {
    return _host() && _host()->config_set_float &&
           _host()->config_set_float(key, value) != 0;
}
inline bool ConfigSetStr(const char* key, const char* value) {
    return _host() && _host()->config_set_str &&
           _host()->config_set_str(key, value) != 0;
}
/* Writes each key this mod read back to the default it first supplied. */
inline int ConfigReset(void) {
    return (_host() && _host()->config_reset) ? _host()->config_reset() : 0;
}
inline bool ConfigFlush(void) {
    return _host() && _host()->config_flush && _host()->config_flush() != 0;
}

/* ------------------------------------------------------------ memory ---- */
inline bool ReadMem(uintptr_t address, void* out, unsigned size) {
    return _host() && _host()->read_mem && _host()->read_mem(address, out, size) != 0;
}
inline bool WriteMem(uintptr_t address, const void* data, unsigned size) {
    return _host() && _host()->write_mem && _host()->write_mem(address, data, size) != 0;
}
inline uintptr_t Resolve(const char* name) {
    return (_host() && _host()->resolve_symbol) ? _host()->resolve_symbol(name) : 0;
}
inline uintptr_t Pattern(const char* signature) {
    return (_host() && _host()->find_pattern) ? _host()->find_pattern(signature) : 0;
}
inline bool Patch(const char* name, uintptr_t address, const void* expect,
                  const void* replacement, unsigned size) {
    return _host() && _host()->patch_bytes &&
           _host()->patch_bytes(name, address, expect, replacement, size) != 0;
}

/* ------------------------------------------------------------- hooks ---- */
inline void* Hook(const char* name, void* target, void* detour) {
    return (_host() && _host()->create_hook) ? _host()->create_hook(name, target, detour) : nullptr;
}
template <typename T>
inline T Hook(const char* name, T target, T detour) {
    return reinterpret_cast<T>(Hook(name, reinterpret_cast<void*>(target),
                                    reinterpret_cast<void*>(detour)));
}
inline bool EnableHook(void* handle) {
    return _host() && _host()->enable_hook && _host()->enable_hook(handle) != 0;
}
inline bool DisableHook(void* handle) {
    return _host() && _host()->disable_hook && _host()->disable_hook(handle) != 0;
}

inline bool OnFrame(void (CDEMOD_CALL* callback)(void)) {
    return _host() && _host()->on_frame && _host()->on_frame(callback) != 0;
}

/* ------------------------------------------------------------------ UI ----
 * The in-game overlay (phase C).  Register a panel from on_init; its draw
 * callback runs on the render thread and uses the cde_ui_* widgets.  Using any
 * of these requires linking against cdeloader (the import library built by
 * scripts/build-modloader.sh), so only reference them if you use them. */
/* Restart the game. The loader routes this through cde-relaunch.exe when it is present, so the
 * successor starts only after this process has released its DLLs; that is what makes a staged
 * update apply cleanly. Any plugin may call it. */
inline int RelaunchGame(void) { return cde_game_relaunch(); }

inline void UI(const char* name, CdeUiDrawFn draw) { cde_ui_panel(name, draw); }
inline bool UIAvailable() { return cde_ui_available() != 0; }

namespace detail {
inline int CDEMOD_CALL on_init(const CdeHostApi* host) {
    _host() = host;
    CDE_USER_INIT();
    return 0;
}
inline void CDEMOD_CALL on_frame() { CDE_USER_FRAME(); }
inline void CDEMOD_CALL on_shutdown() { CDE_USER_SHUTDOWN(); }
}  // namespace detail

}  // namespace cde

/* Convenience logging macros. */
#define CDE_ERROR(...) ::cde::Logf(CDE_LOG_ERROR, __VA_ARGS__)
#define CDE_WARN(...)  ::cde::Logf(CDE_LOG_WARN, __VA_ARGS__)
#define CDE_INFO(...)  ::cde::Logf(CDE_LOG_INFO, __VA_ARGS__)
#define CDE_DEBUG(...) ::cde::Logf(CDE_LOG_DEBUG, __VA_ARGS__)

/* Declare the plugin to the loader.  Use exactly once. */
#define CDE_MOD(ID, NAME, VERSION, AUTHOR)                                     \
    extern "C" CDEMOD_EXPORT const CdeModInfo* CDEMOD_CALL CdeMod_GetInfo(void) { \
        static CdeModInfo info;                                                \
        info.abi_version = CDEMOD_ABI_VERSION;                                 \
        info.struct_size = static_cast<uint32_t>(sizeof(CdeModInfo));          \
        info.id = (ID);                                                        \
        info.name = (NAME);                                                    \
        info.version = (VERSION);                                              \
        info.author = (AUTHOR);                                                \
        info.on_init = &::cde::detail::on_init;                                \
        info.on_frame = &::cde::detail::on_frame;                              \
        info.on_shutdown = &::cde::detail::on_shutdown;                        \
        return &info;                                                          \
    }

#endif /* CDEMOD_HPP */
