/* cdemod_api.h — the C ABI shared by the CDE mod loader and its plugins.
 *
 * This header is the contract in docs/10-mod-framework.md §4.  It must be
 * compiler-agnostic: the loader is built with MinGW-w64 (i686) while plugins may
 * be built with MSVC, so everything here is plain C with fixed-width types and
 * no C++ names.  The ABI is versioned; a plugin reports the version it was built
 * against and the loader rejects a mismatch instead of crashing.
 */
#ifndef CDEMOD_API_H
#define CDEMOD_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDEMOD_ABI_VERSION 1u

/* Calling convention / export helpers (x86 cdecl on both toolchains). */
#if defined(_MSC_VER)
#  define CDEMOD_CALL __cdecl
#  define CDEMOD_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && defined(_WIN32)
#  define CDEMOD_CALL __attribute__((cdecl))
#  define CDEMOD_EXPORT __attribute__((dllexport))
#else
#  define CDEMOD_CALL
#  define CDEMOD_EXPORT
#endif

/* A plugin that exports this symbol is recognised by the loader. */
#define CDEMOD_GETINFO_SYMBOL "CdeMod_GetInfo"

/* Log levels accepted by CdeHostApi::log. */
enum {
    CDE_LOG_ERROR = 0,
    CDE_LOG_WARN  = 1,
    CDE_LOG_INFO  = 2,
    CDE_LOG_DEBUG = 3
};

typedef struct CdeHostApi CdeHostApi;

/* Services the loader offers to plugins.  All addresses are the game's virtual
 * addresses (image base 0x400000); the loader relocates them to the live image
 * base.  reserved[] is zeroed and reserved for future growth (overlay, data
 * redirect, …) so the ABI does not break when features are added.
 */
struct CdeHostApi {
    uint32_t abi_version;
    uint32_t struct_size;

    /* logging */
    void  (CDEMOD_CALL *log)(int level, const char* message);

    /* per-mod configuration (mods/config/<mod id>.ini) */
    int   (CDEMOD_CALL *config_get_int)(const char* key, int def);
    int   (CDEMOD_CALL *config_get_bool)(const char* key, int def);
    float (CDEMOD_CALL *config_get_float)(const char* key, float def);
    int   (CDEMOD_CALL *config_get_str)(const char* key, char* out, int cap);

    /* memory access */
    int   (CDEMOD_CALL *read_mem)(uintptr_t address, void* out, unsigned size);
    int   (CDEMOD_CALL *write_mem)(uintptr_t address, const void* data, unsigned size);

    /* addressing: resolve a PDB name, or find a byte pattern in the main module */
    uintptr_t (CDEMOD_CALL *resolve_symbol)(const char* name);
    uintptr_t (CDEMOD_CALL *find_pattern)(const char* pattern);

    /* verified byte patch: writes replacement only if the current bytes equal
     * expect (pass expect = 0 to skip the guard); returns 1 on success. */
    int   (CDEMOD_CALL *patch_bytes)(const char* name, uintptr_t address,
                                     const void* expect, const void* replacement,
                                     unsigned size);

    /* inline hooks (x86 5-byte jmp + trampoline).  create returns a trampoline
     * handle to call the original, or 0 on failure. */
    void* (CDEMOD_CALL *create_hook)(const char* name, void* target, void* detour);
    int   (CDEMOD_CALL *enable_hook)(void* handle);
    int   (CDEMOD_CALL *disable_hook)(void* handle);

    /* per-frame callbacks */
    int   (CDEMOD_CALL *on_frame)(void (CDEMOD_CALL *callback)(void));

    /* configuration write-back (added in place of reserved slots, so the
     * struct layout and abi_version stay compatible).  Values are written to
     * mods/config/<mod id>.ini immediately; reset writes each key that this mod
     * has read back to the default it supplied on its first read. */
    int   (CDEMOD_CALL *config_set_int)(const char* key, int value);
    int   (CDEMOD_CALL *config_set_bool)(const char* key, int value);
    int   (CDEMOD_CALL *config_set_float)(const char* key, float value);
    int   (CDEMOD_CALL *config_set_str)(const char* key, const char* value);
    int   (CDEMOD_CALL *config_reset)(void);
    int   (CDEMOD_CALL *config_flush)(void);

    void* reserved[2];
};

/* A plugin's descriptor, returned by its exported CdeMod_GetInfo. */
typedef struct CdeModInfo {
    uint32_t    abi_version;   /* CDEMOD_ABI_VERSION at plugin build time */
    uint32_t    struct_size;   /* sizeof(CdeModInfo) */
    const char* id;            /* reverse-DNS, unique */
    const char* name;
    const char* version;
    const char* author;
    int  (CDEMOD_CALL *on_init)(const CdeHostApi* host);  /* 0 = ok */
    void (CDEMOD_CALL *on_frame)(void);                   /* optional */
    void (CDEMOD_CALL *on_shutdown)(void);                /* optional */
} CdeModInfo;

typedef const CdeModInfo* (CDEMOD_CALL *CdeModGetInfoFn)(void);

/* The loader's own export, referenced by the injected .cdei import table. */
typedef int (CDEMOD_CALL *CdeModInitFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* CDEMOD_API_H */
