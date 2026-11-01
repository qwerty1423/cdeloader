// cdeloader.dll — the CDE mod loader (docs/10-mod-framework.md).
//
// Loaded into CDE.exe by the injected .cdei import of CdeModInit.  From an init
// thread (never under the loader lock) it discovers plugins in mods/*/plugin.dll,
// validates their C ABI, and initialises them with a versioned host API table
// (log / config / memory / symbol / pattern / patch / hook / on_frame).  A tick
// thread drives per-frame callbacks.
//
// Build: scripts/build-modloader.sh  (i686-w64-mingw32-g++)
//
// Everything is defensive: a bad plugin, a missing symbol table or a mismatched
// game build is logged and skipped, never fatal.

#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>

#include <string>
#include <vector>
#include <map>
#include <unordered_map>

#include "console.h"
#include <unordered_set>
#include <algorithm>
#include <set>

#include "cdemod_api.h"

// Target build this symbol table was generated from (docs/01).
#define CDEMOD_GAME_VERSION "1.2.1.3"
#include "x86len.h"
#include "overlay.h"

#define LOADER_VERSION "0.1.0"
#define GAME_IMAGE_BASE 0x400000u
#define HOOK_MAX_STOLEN 32u

// ---------------------------------------------------------------------------
// state

static HMODULE     g_self = NULL;
static std::string g_dir;
static std::string g_log_path;
static int         g_log_level = CDE_LOG_INFO;
static int         g_console_start = 0;
static bool        g_enabled = true;

static CdeHostApi  g_api;
static std::string g_current_mod_id;
static CRITICAL_SECTION g_lock;

struct SymbolEntry { uintptr_t va; std::string name; std::string mangled; };
static std::vector<SymbolEntry> g_symbols;
static std::unordered_map<std::string, uintptr_t> g_by_name;
static std::unordered_map<std::string, uintptr_t> g_by_mangled;
static std::string g_symbols_sha;

struct LoadedMod {
    std::string id;
    std::string name;
    std::string version;
    std::string dir;
    std::vector<std::string> depends;
    std::vector<std::string> load_after;
    const CdeModInfo* info;
};
static std::vector<LoadedMod> g_mods;

// Plugins whose folder sits under mods/disabled at startup.
struct DisabledMod {
    std::string folder;
    std::string id;
    std::string version;
};
static std::vector<DisabledMod> g_disabled;
static int g_mods_pending = 0;
static std::unordered_set<std::string> g_faulted;  // mods disabled after a fault

struct HookState {
    std::string name;
    std::string owner;   // mod that installed it, for conflict diagnostics
    unsigned char* target;
    void* detour;
    void* tramp;
    unsigned char original[HOOK_MAX_STOLEN];
    size_t stolen;
    bool enabled;
};
static std::vector<HookState*> g_hooks;

static std::vector<void (*)(void)> g_frame_callbacks;

// Data-file redirect (Phase E): mods/<mod>/Data/<rel> shadows the game's files.
static std::unordered_map<std::string, std::string> g_redirect;  // normalized rel -> abs path
static bool g_redirect_enabled = false;
static bool g_overlay_enabled = true;
static bool g_overlay_open = false;
static bool g_panels_open = false;

// ---------------------------------------------------------------------------
// logging

static const char* level_name(int level) {
    switch (level) {
        case CDE_LOG_ERROR: return "ERROR";
        case CDE_LOG_WARN:  return "WARN";
        case CDE_LOG_INFO:  return "INFO";
        case CDE_LOG_DEBUG: return "DEBUG";
        default:            return "INFO";
    }
}

static void log_write(int level, const char* msg) {
    if (level > g_log_level) return;
    const char* mod = g_current_mod_id.empty() ? "loader" : g_current_mod_id.c_str();
    if (!g_log_path.empty()) {
        FILE* f = fopen(g_log_path.c_str(), "a");
        if (f) {
            // UTC, so a log line means the same thing wherever it was written and can
            // be lined up with other machines' logs without converting time zones.
            SYSTEMTIME st;
            GetSystemTime(&st);
            fprintf(f, "[%04d-%02d-%02dT%02d:%02d:%02dZ][%-5s][%s] %s\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    level_name(level), mod, msg);
            fclose(f);
        }
    }
    char dbg[1200];
    _snprintf(dbg, sizeof(dbg), "[cdeloader][%-5s][%s] %s\n", level_name(level), mod, msg);
    dbg[sizeof(dbg) - 1] = 0;
    OutputDebugStringA(dbg);
    fprintf(stderr, "%s", dbg);
    console_log_line(level, dbg);
}

static void logf(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    log_write(level, buf);
}

// ---------------------------------------------------------------------------
// crash handler: a faulting plugin must leave a diagnosable dump, not just die

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    unsigned long code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;
    logf(CDE_LOG_ERROR, "UNHANDLED EXCEPTION 0x%08lx at %p (writing dump)", code, addr);
    if (!g_dir.empty()) {
        std::string dmp = g_dir + "\\cdeloader_crash.dmp";
        HANDLE f = CreateFileA(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = info;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                              MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(f);
            logf(CDE_LOG_ERROR, "wrote %s", dmp.c_str());
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// per-call plugin isolation
//
// i686 GCC has no __try/__except, so we guard plugin calls with a vectored
// exception handler plus setjmp/longjmp: a hardware fault inside on_init/on_frame
// unwinds back to the guard and the offending mod is disabled, instead of taking
// the whole process down.  (Stack overflow is deliberately not recovered — there
// is no stack to longjmp on.)

struct Guard {
    jmp_buf jb;
    volatile bool armed;
    volatile unsigned long code;
    void* volatile addr;
};
static Guard* volatile t_guard = nullptr;

static LONG CALLBACK vec_handler(PEXCEPTION_POINTERS info) {
    Guard* g = t_guard;
    if (!g || !g->armed) return EXCEPTION_CONTINUE_SEARCH;
    unsigned long code = info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
            g->code = code;
            g->addr = info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
            longjmp(g->jb, 1);
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

template <typename F>
static bool guarded_call(const std::string& id, const char* what, F&& fn) {
    Guard g;
    g.armed = false;
    Guard* prev = t_guard;
    if (setjmp(g.jb) == 0) {
        t_guard = &g;
        g.armed = true;
        fn();
        g.armed = false;
        t_guard = prev;
        return true;
    }
    t_guard = prev;
    EnterCriticalSection(&g_lock);
    g_faulted.insert(id);
    LeaveCriticalSection(&g_lock);
    logf(CDE_LOG_ERROR, "plugin %s faulted in %s (0x%08lx at %p) — disabled",
         id.c_str(), what, g.code, g.addr);
    return false;
}

static bool mod_live(const std::string& id) {
    EnterCriticalSection(&g_lock);
    bool live = g_faulted.find(id) == g_faulted.end();
    LeaveCriticalSection(&g_lock);
    return live;
}

// ---------------------------------------------------------------------------
// directory discovery

static void discover_dir(void) {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string path(exe);
    size_t slash = path.find_last_of("\\/");
    g_dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
    g_log_path = g_dir + "\\cdeloader.log";
}

// ---------------------------------------------------------------------------
// SHA-256 (to verify the host exe against the symbol table's build id)

namespace {

struct Sha256 {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;

    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void init() {
        static const uint32_t k[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        for (int i = 0; i < 8; i++) state[i] = k[i];
        bitlen = 0;
        datalen = 0;
    }

    void transform(const uint8_t* chunk) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t m[64];
        for (uint32_t i = 0, j = 0; i < 16; i++, j += 4)
            m[i] = (uint32_t(chunk[j]) << 24) | (uint32_t(chunk[j+1]) << 16) |
                   (uint32_t(chunk[j+2]) << 8) | uint32_t(chunk[j+3]);
        for (uint32_t i = 16; i < 64; i++) {
            uint32_t s0 = rotr(m[i-15],7) ^ rotr(m[i-15],18) ^ (m[i-15] >> 3);
            uint32_t s1 = rotr(m[i-2],17) ^ rotr(m[i-2],19) ^ (m[i-2] >> 10);
            m[i] = m[i-16] + s0 + m[i-7] + s1;
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],e=state[4],f=state[5],g=state[6],h=state[7];
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = h + S1 + ch + k[i] + m[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
        state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
    }

    void update(const uint8_t* p, size_t len) {
        for (size_t i = 0; i < len; i++) {
            data[datalen++] = p[i];
            if (datalen == 64) {
                transform(data);
                bitlen += 512;
                datalen = 0;
            }
        }
    }

    void final(uint8_t out[32]) {
        uint32_t i = datalen;
        if (datalen < 56) {
            data[i++] = 0x80;
            while (i < 56) data[i++] = 0x00;
        } else {
            data[i++] = 0x80;
            while (i < 64) data[i++] = 0x00;
            transform(data);
            memset(data, 0, 56);
        }
        bitlen += uint64_t(datalen) * 8;
        for (int j = 0; j < 8; j++) data[63 - j] = uint8_t(bitlen >> (8 * j));
        transform(data);
        for (i = 0; i < 4; i++)
            for (int j = 0; j < 8; j++)
                out[i + 4 * j] = uint8_t(state[j] >> (24 - i * 8));
    }
};

// Hashes of the plugin files as the installer left them, mods/.coade-hashes.
// Used to notice a plugin that was replaced or corrupted after installation.
static std::map<std::string, std::string> g_mod_hashes;

static void load_mod_hashes(void) {
    g_mod_hashes.clear();
    const std::string path = g_dir + "\\mods\\.coade-hashes";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        const size_t space = text.find("  ");
        if (space == std::string::npos || text.size() < space + 3) continue;
        g_mod_hashes[text.substr(space + 2)] = text.substr(0, space);
    }
    fclose(f);
}

std::string file_sha256(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    Sha256 sha;
    sha.init();
    std::vector<uint8_t> buf(65536);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) sha.update(buf.data(), n);
    fclose(f);
    uint8_t digest[32];
    sha.final(digest);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    hex[64] = 0;
    return std::string(hex);
}

}  // namespace

// ---------------------------------------------------------------------------
// address helpers

static void* va_to_ptr(uintptr_t va) {
    HMODULE base = GetModuleHandleA(NULL);
    uintptr_t image = reinterpret_cast<uintptr_t>(base);
    if (va >= GAME_IMAGE_BASE && image) return reinterpret_cast<void*>(image + (va - GAME_IMAGE_BASE));
    return reinterpret_cast<void*>(va);
}

static bool write_protected(void* addr, const void* data, size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, data, size);
    VirtualProtect(addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    return true;
}

// ---------------------------------------------------------------------------
// symbol table

static void load_symbols(void) {
    std::string path = g_dir + "\\cdeloader.symbols";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        logf(CDE_LOG_WARN, "no cdeloader.symbols next to the exe; resolve_symbol disabled");
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            char* s = strstr(line, "sha256=");
            if (s) {
                s += 7;
                char* e = s;
                while (*e && *e != ' ' && *e != '\r' && *e != '\n') e++;
                g_symbols_sha.assign(s, e - s);
            }
            continue;
        }
        char* p = line;
        unsigned long va = strtoul(p, &p, 16);
        if (*p == '\t') p++; else continue;
        char* name = p;
        char* tab = strchr(p, '\t');
        if (!tab) continue;
        *tab = 0;
        char* man = tab + 1;
        char* nl = strchr(man, '\n');
        if (nl) *nl = 0;
        nl = strchr(man, '\r');
        if (nl) *nl = 0;
        if (!name[0]) continue;
        SymbolEntry e;
        e.va = static_cast<uintptr_t>(va);
        e.name = name;
        e.mangled = man;
        g_symbols.push_back(e);
        if (g_by_name.find(e.name) == g_by_name.end()) g_by_name[e.name] = e.va;
        if (e.mangled[0] && g_by_mangled.find(e.mangled) == g_by_mangled.end())
            g_by_mangled[e.mangled] = e.va;
    }
    fclose(f);
    logf(CDE_LOG_INFO, "loaded %u symbols (declared source %s)",
         static_cast<unsigned>(g_symbols.size()),
         g_symbols_sha.empty() ? "?" : g_symbols_sha.c_str());
}


static void verify_host_hash(void) {
    if (g_symbols_sha.empty()) return;
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string exe_str(exe);
    std::string actual = file_sha256(exe_str);
    if (actual.empty()) return;
    if (actual == g_symbols_sha) {
        logf(CDE_LOG_INFO, "host exe matches the symbol table build");
        return;
    }
    // We are normally launched from the *patched* exe, whose hash necessarily
    // differs from the stock build the symbol table was made from.  The patch
    // only adds an import, so the game's own VAs are unchanged; the installer
    // leaves the stock exe next to us as CDE.exe.original.  Accept that as proof.
    std::string backup = exe_str + ".original";
    std::string backup_sha = file_sha256(backup);
    if (!backup_sha.empty() && backup_sha == g_symbols_sha) {
        logf(CDE_LOG_INFO,
             "patched exe; symbol table matches %s (VAs valid)", backup.c_str());
        return;
    }
    // Builds of the game that a known installer produces in place, so there is no
    // stock exe left next to the patched one.  The patch only adds an import, so the
    // code addresses the symbol table describes are unchanged.
    static const char* patched_builds[] = {
        "26c70b79fcac69d61ffb23dfe5c2fc4c8611a2a4924a53b4fe4a46f863292465",
    };
    for (const char* known : patched_builds) {
        if (actual == known) {
            logf(CDE_LOG_INFO,
                 "host exe is the community-patched build (%s); VAs unchanged",
                 actual.c_str());
            return;
        }
    }
    logf(CDE_LOG_WARN, "host exe hash %s != symbol table %s; VAs may be stale",
         actual.c_str(), g_symbols_sha.c_str());
}

// ---------------------------------------------------------------------------
// host API implementations

static void CDEMOD_CALL api_log(int level, const char* message) {
    log_write(level, message ? message : "");
}

static const char* mod_config_path() {
    static std::string path;
    path = g_dir + "\\mods\\config\\" + (g_current_mod_id.empty() ? "loader" : g_current_mod_id) + ".ini";
    return path.c_str();
}

// Defaults remembered for config_reset(): the value a mod passed on its first
// read of each key.  Keyed by mod id so panels from different mods cannot
// interfere.
static std::map<std::string, std::string> g_config_defaults;

static std::string config_default_key(const char* key) {
    return (g_current_mod_id.empty() ? "loader" : g_current_mod_id) + "|" +
           (key ? key : "");
}

static void remember_default(const char* key, const char* value) {
    if (!key || !key[0]) return;
    const std::string k = config_default_key(key);
    if (g_config_defaults.find(k) == g_config_defaults.end())
        g_config_defaults[k] = value ? value : "";
}

extern "C" const char* cde_current_mod_id(void) {
    return g_current_mod_id.c_str();
}
extern "C" void cde_set_current_mod(const char* id) {
    g_current_mod_id = id ? id : "";
}

static int CDEMOD_CALL api_config_get_int(const char* key, int def) {
    char defs[32];
    wsprintfA(defs, "%d", def);
    remember_default(key, defs);
    return static_cast<int>(GetPrivateProfileIntA("mod", key, def, mod_config_path()));
}
static int CDEMOD_CALL api_config_get_bool(const char* key, int def) {
    remember_default(key, def ? "1" : "0");
    return GetPrivateProfileIntA("mod", key, def ? 1 : 0, mod_config_path()) ? 1 : 0;
}
static float CDEMOD_CALL api_config_get_float(const char* key, float def) {
    char defs[64];
    _snprintf(defs, sizeof(defs), "%g", def);
    remember_default(key, defs);
    char buf[64] = {0};
    GetPrivateProfileStringA("mod", key, defs, buf, sizeof(buf), mod_config_path());
    return static_cast<float>(atof(buf));
}
static int CDEMOD_CALL api_config_get_str(const char* key, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    GetPrivateProfileStringA("mod", key, "", out, cap, mod_config_path());
    return out[0] ? 1 : 0;
}static int CDEMOD_CALL api_config_set_int(const char* key, int value) {
    if (!key || !key[0]) return 0;
    char text[32];
    wsprintfA(text, "%d", value);
    return WritePrivateProfileStringA("mod", key, text, mod_config_path()) ? 1 : 0;
}
static int CDEMOD_CALL api_config_set_bool(const char* key, int value) {
    return api_config_set_int(key, value ? 1 : 0);
}
static int CDEMOD_CALL api_config_set_float(const char* key, float value) {
    if (!key || !key[0]) return 0;
    char text[64];
    snprintf(text, sizeof text, "%g", value);
    return WritePrivateProfileStringA("mod", key, text, mod_config_path()) ? 1 : 0;
}
static int CDEMOD_CALL api_config_set_str(const char* key, const char* value) {
    if (!key || !key[0]) return 0;
    return WritePrivateProfileStringA("mod", key, value ? value : "",
                                      mod_config_path()) ? 1 : 0;
}
static int CDEMOD_CALL api_config_reset(void) {
    const std::string prefix =
        (g_current_mod_id.empty() ? "loader" : g_current_mod_id) + "|";
    int count = 0;
    for (const auto& kv : g_config_defaults) {
        if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string cfg_key = kv.first.substr(prefix.size());
        if (WritePrivateProfileStringA("mod", cfg_key.c_str(), kv.second.c_str(),
                                       mod_config_path()))
            ++count;
    }
    return count;
}
static int CDEMOD_CALL api_config_flush(void) {
    return WritePrivateProfileStringA(nullptr, nullptr, nullptr,
                                      mod_config_path()) ? 1 : 0;
}


static int CDEMOD_CALL api_read_mem(uintptr_t address, void* out, unsigned size) {
    if (!out || !size) return 0;
    memcpy(out, va_to_ptr(address), size);
    return 1;
}
static int CDEMOD_CALL api_write_mem(uintptr_t address, const void* data, unsigned size) {
    if (!data || !size) return 0;
    return write_protected(va_to_ptr(address), data, size) ? 1 : 0;
}

// The game's TacticalManager, captured from its own call to
// TacticalManager::GetPlayerSpacecraftComponents.  Calling a tactical method on
// another manager (the strategic one, the main-menu one) dereferences garbage, so
// the instance is never guessed: the loader hooks the getter once, remembers the
// `this` the game passes, and hands it to mods through
// cde_game_tactical_manager().
static void* g_tactical_manager = nullptr;
static void* g_tactical_capture_orig = nullptr;

static void* __fastcall tactical_capture(void* self, void* edx, void* out) {
    if (self) g_tactical_manager = self;
    typedef void* (__fastcall * Fn)(void*, void*, void*);
    if (g_tactical_capture_orig)
        return reinterpret_cast<Fn>(g_tactical_capture_orig)(self, edx, out);
    return out;
}

extern "C" __declspec(dllexport) void* CDEMOD_CALL cde_game_tactical_manager(void) {
    return g_tactical_manager;
}


static uintptr_t CDEMOD_CALL api_resolve_symbol(const char* name) {
    if (!name || !name[0]) return 0;
    std::string n(name);
    uintptr_t va = 0;
    auto it = g_by_name.find(n);
    if (it != g_by_name.end()) va = it->second;
    if (!va) {
        auto im = g_by_mangled.find(n);
        if (im != g_by_mangled.end()) va = im->second;
    }
    if (!va) {
        std::string suffix = "::" + n;
        int hits = 0;
        for (auto& s : g_symbols) {
            const std::string& d = s.name;
            if (d == n || (d.size() > suffix.size() &&
                           d.compare(d.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                va = s.va;
                if (++hits > 1) return 0;  // ambiguous
            }
        }
    }
    return va ? reinterpret_cast<uintptr_t>(va_to_ptr(va)) : 0;
}


// --- pattern scanning -------------------------------------------------------

struct Range { const unsigned char* start; size_t len; };

static std::vector<Range> main_module_code_ranges(void) {
    std::vector<Range> ranges;
    HMODULE base = GetModuleHandleA(NULL);
    if (!base) return ranges;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(base);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(p);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return ranges;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(p + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return ranges;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            Range r;
            r.start = p + sec[i].VirtualAddress;
            r.len = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (r.len) ranges.push_back(r);
        }
    }
    return ranges;
}

static uintptr_t CDEMOD_CALL api_find_pattern(const char* pattern) {
    if (!pattern || !pattern[0]) return 0;
    std::vector<unsigned char> bytes;
    std::vector<unsigned char> mask;
    const char* p = pattern;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            p++;
        } else {
            char hex[3] = {p[0], p[1], 0};
            if (!isxdigit((unsigned char)hex[0]) || (p[1] && !isxdigit((unsigned char)p[1])))
                return 0;
            bytes.push_back(static_cast<unsigned char>(strtoul(hex, nullptr, 16)));
            mask.push_back(0xFF);
            p++;
            if (*p) p++;
        }
    }
    if (bytes.empty()) return 0;

    for (const Range& r : main_module_code_ranges()) {
        if (r.len < bytes.size()) continue;
        for (size_t i = 0; i + bytes.size() <= r.len; i++) {
            bool ok = true;
            for (size_t j = 0; j < bytes.size(); j++) {
                if (mask[j] && r.start[i + j] != bytes[j]) { ok = false; break; }
            }
            if (ok) return reinterpret_cast<uintptr_t>(r.start + i);
        }
    }
    return 0;
}

static int CDEMOD_CALL api_patch_bytes(const char* name, uintptr_t address,
                                       const void* expect, const void* replacement,
                                       unsigned size) {
    if (!replacement || !size) return 0;
    void* addr = va_to_ptr(address);
    if (expect && memcmp(addr, expect, size) != 0) {
        logf(CDE_LOG_WARN, "patch skipped %s @ 0x%08lx: original bytes differ",
             name ? name : "?", static_cast<unsigned long>(address));
        return 0;
    }
    if (!write_protected(addr, replacement, size)) {
        logf(CDE_LOG_ERROR, "patch failed %s @ 0x%08lx", name ? name : "?",
             static_cast<unsigned long>(address));
        return 0;
    }
    logf(CDE_LOG_INFO, "patched %s @ 0x%08lx", name ? name : "?",
         static_cast<unsigned long>(address));
    return 1;
}

// --- hooks ------------------------------------------------------------------

// Length of whole instructions starting at ``target`` needed to cover >= ``min``
// bytes.  Returns 0 if an instruction cannot be decoded or the region is too
// large; sets ``*has_rel`` if any stolen instruction is position-dependent.
static size_t hook_stolen_size(const unsigned char* target, size_t min, bool* has_rel) {
    size_t total = 0;
    *has_rel = false;
    while (total < min) {
        int rel = 0;
        size_t len = x86_instruction_length(target + total, HOOK_MAX_STOLEN - total, &rel);
        if (len == 0) return 0;
        if (rel) *has_rel = true;
        total += len;
        if (total > HOOK_MAX_STOLEN) return 0;
    }
    return total;
}

// Emit a 5-byte E9 jmp into ``buf``; ``at_addr`` is where ``buf`` will actually
// execute (for the target patch the buffer is a stack copy, so its address
// differs from where the bytes end up).
static void write_jmp_at(unsigned char* buf, uintptr_t at_addr, const void* dest) {
    buf[0] = 0xE9;
    *reinterpret_cast<int32_t*>(buf + 1) =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(dest) - (at_addr + 5));
}

// Install a hook without logging success.  Logging calls the file API, so a
// caller that hooks CreateFile must set its "real function" pointer before any
// format write happens — otherwise the just-installed detour re-enters with a
// null pointer.  Only failures are logged here (no hook is live yet).
static void* install_hook(const char* name, void* target, void* detour, size_t* stolen_out) {
    if (!target || !detour) return nullptr;
    const char* nm = name ? name : "hook";
    unsigned char* t = reinterpret_cast<unsigned char*>(target);

    bool rel = false;
    size_t stolen = hook_stolen_size(t, 5, &rel);
    if (stolen == 0) {
        logf(CDE_LOG_ERROR, "hook refused %s @ %p: undecodable prologue", nm, target);
        return nullptr;
    }
    if (rel) {
        logf(CDE_LOG_ERROR, "hook refused %s @ %p: relative branch in prologue", nm, target);
        return nullptr;
    }

    void* mem = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) {
        logf(CDE_LOG_ERROR, "hook failed %s: no trampoline memory", nm);
        return nullptr;
    }
    unsigned char* tr = reinterpret_cast<unsigned char*>(mem);
    memcpy(tr, t, stolen);
    write_jmp_at(tr + stolen, reinterpret_cast<uintptr_t>(tr + stolen), t + stolen);

    HookState* h = new HookState();
    h->name = nm;
    h->owner = g_current_mod_id;
    h->target = t;
    h->detour = detour;
    h->tramp = tr;
    h->stolen = stolen;
    memcpy(h->original, t, stolen);
    h->enabled = false;

    unsigned char patch[HOOK_MAX_STOLEN];
    write_jmp_at(patch, reinterpret_cast<uintptr_t>(t), detour);
    for (size_t k = 5; k < stolen; k++) patch[k] = 0x90;
    if (!write_protected(t, patch, stolen)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        delete h;
        logf(CDE_LOG_ERROR, "hook failed %s @ %p", nm, target);
        return nullptr;
    }
    h->enabled = true;
    EnterCriticalSection(&g_lock);
    g_hooks.push_back(h);
    LeaveCriticalSection(&g_lock);
    if (stolen_out) *stolen_out = stolen;
    return tr;
}

static void install_tactical_capture(void) {
    uintptr_t fn = api_resolve_symbol("TacticalManager::GetPlayerSpacecraftComponents");
    if (!fn) return;
    size_t stolen = 0;
    void* tr = install_hook("loader.tactical_capture", reinterpret_cast<void*>(fn),
                            reinterpret_cast<void*>(&tactical_capture), &stolen);
    if (tr) {
        g_tactical_capture_orig = tr;
        logf(CDE_LOG_INFO, "tactical manager capture hooked @ %p (%u bytes stolen)",
             reinterpret_cast<void*>(fn), static_cast<unsigned>(stolen));
    }
}

static void* CDEMOD_CALL api_create_hook(const char* name, void* target, void* detour) {
    // Two mods hooking the same prologue would stack trampolines and corrupt
    // each other.  Refuse the second instead, naming both owners.
    for (HookState* h : g_hooks) {
        if (h->target == target) {
            logf(CDE_LOG_ERROR,
                 "hook conflict: %s @ %p already hooked by '%s'; refusing '%s'",
                 name ? name : "hook", target,
                 h->owner.empty() ? "?" : h->owner.c_str(),
                 g_current_mod_id.empty() ? "?" : g_current_mod_id.c_str());
            return nullptr;
        }
    }
    size_t stolen = 0;
    void* tr = install_hook(name, target, detour, &stolen);
    if (tr) {
        logf(CDE_LOG_INFO, "hooked %s @ %p -> %p (%u bytes stolen, trampoline %p)",
             name ? name : "hook", target, detour, static_cast<unsigned>(stolen), tr);
    }
    return tr;
}

static HookState* find_hook(void* tramp) {
    for (HookState* h : g_hooks) if (h->tramp == tramp) return h;
    return nullptr;
}

static int CDEMOD_CALL api_enable_hook(void* handle) {
    HookState* h = find_hook(handle);
    if (!h) return 0;
    if (h->enabled) return 1;
    unsigned char patch[HOOK_MAX_STOLEN];
    write_jmp_at(patch, reinterpret_cast<uintptr_t>(h->target), h->detour);
    for (size_t k = 5; k < h->stolen; k++) patch[k] = 0x90;
    if (!write_protected(h->target, patch, h->stolen)) return 0;
    h->enabled = true;
    return 1;
}
static int CDEMOD_CALL api_disable_hook(void* handle) {
    HookState* h = find_hook(handle);
    if (!h) return 0;
    if (!h->enabled) return 1;
    if (!write_protected(h->target, h->original, h->stolen)) return 0;
    h->enabled = false;
    return 1;
}

static int CDEMOD_CALL api_on_frame(void (CDEMOD_CALL* callback)(void)) {
    if (!callback) return 0;
    EnterCriticalSection(&g_lock);
    g_frame_callbacks.push_back(callback);
    LeaveCriticalSection(&g_lock);
    return 1;
}

// ---------------------------------------------------------------------------
// data-file redirect (Phase E)
//
// Each mod may ship a ``Data/`` folder whose layout mirrors the game's data
// tree.  We index every file by its normalized path (lower-case, ``/``) and hook
// ``CreateFileA/W``; a request whose normalized path ends with an indexed path at
// a segment boundary is served from the mod file instead.  Conflicts (two mods
// providing the same path) keep the first by mod name and are logged.

static std::string norm_path(const std::string& in) {
    std::string s = in;
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static void index_walk(const std::string& base, const std::string& cur) {
    std::string pattern = cur + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = cur + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_walk(base, full);
            continue;
        }
        std::string rel = full.substr(base.size());
        if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel = rel.substr(1);
        std::string key = norm_path(rel);
        if (key.empty()) continue;
        auto it = g_redirect.find(key);
        if (it == g_redirect.end()) {
            g_redirect[key] = full;
        } else if (it->second != full) {
            logf(CDE_LOG_WARN, "data conflict '%s': keeping %s, ignoring %s",
                 key.c_str(), it->second.c_str(), full.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Longest indexed path that is a segment-boundary suffix of ``requested``.
static const std::string* lookup_redirect(const std::string& requested) {
    std::string req = norm_path(requested);
    const std::string* best = nullptr;
    size_t best_len = 0;
    for (const auto& kv : g_redirect) {
        const std::string& k = kv.first;
        if (k.empty() || k.size() > req.size()) continue;
        if (req.compare(req.size() - k.size(), k.size(), k) != 0) continue;
        if (req.size() > k.size()) {
            char before = req[req.size() - k.size() - 1];
            if (before != '/') continue;  // must align on a path segment
        }
        if (k.size() > best_len) { best_len = k.size(); best = &kv.second; }
    }
    return best;
}

typedef HANDLE (WINAPI *CreateFileWFn)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
typedef HANDLE (WINAPI *CreateFileAFn)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                       DWORD, DWORD, HANDLE);
static CreateFileWFn g_real_CreateFileW = nullptr;
static CreateFileAFn g_real_CreateFileA = nullptr;

static bool wide_to_narrow(LPCWSTR w, std::string& out) {
    int len = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return false;
    out.resize(len - 1);
    WideCharToMultiByte(CP_ACP, 0, w, -1, &out[0], len, nullptr, nullptr);
    return true;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE tmpl) {
    std::string narrow;
    if (name && wide_to_narrow(name, narrow)) {
        if (const std::string* rep = lookup_redirect(narrow)) {
            int wl = MultiByteToWideChar(CP_ACP, 0, rep->c_str(), -1, nullptr, 0);
            if (wl > 1) {
                std::wstring w(wl - 1, L'\0');
                MultiByteToWideChar(CP_ACP, 0, rep->c_str(), -1, &w[0], wl);
                logf(CDE_LOG_INFO, "data redirect %s -> %s", narrow.c_str(), rep->c_str());
                return g_real_CreateFileW(w.c_str(), access, share, sa, disposition, flags, tmpl);
            }
        }
    }
    return g_real_CreateFileW(name, access, share, sa, disposition, flags, tmpl);
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                      DWORD flags, HANDLE tmpl) {
    if (name) {
        if (const std::string* rep = lookup_redirect(name)) {
            logf(CDE_LOG_INFO, "data redirect %s -> %s", name, rep->c_str());
            return g_real_CreateFileA(rep->c_str(), access, share, sa, disposition, flags, tmpl);
        }
    }
    return g_real_CreateFileA(name, access, share, sa, disposition, flags, tmpl);
}

static void setup_redirect(void) {
    if (!g_redirect_enabled) return;
    std::string mods_dir = g_dir + "\\mods";
    std::vector<std::string> dirs;
    std::string pattern = mods_dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                // A dot-prefixed folder is bookkeeping, not a plugin: mods\.pending holds a staged
                // update, and loading the cdeloader.dll copy inside it starts a second initialisation
                // of the loader in the same process - which then fails every hook it re-installs.
                if (fd.cFileName[0] == '.') {
                    logf(CDE_LOG_INFO, "mods: ignoring %s (staging, not a plugin folder)",
                         fd.cFileName);
                    continue;
                }
                dirs.push_back(fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(dirs.begin(), dirs.end());
    for (const std::string& d : dirs) {
        std::string data = mods_dir + "\\" + d + "\\Data";
        if (GetFileAttributesA(data.c_str()) != INVALID_FILE_ATTRIBUTES)
            index_walk(data, data);
    }
    if (g_redirect.empty()) {
        logf(CDE_LOG_INFO, "data redirect enabled but no mod Data/ files found");
        return;
    }

    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return;
    FARPROC cfw = GetProcAddress(k, "CreateFileW");
    FARPROC cfa = GetProcAddress(k, "CreateFileA");
    if (cfw) {
        void* tr = install_hook("CreateFileW", reinterpret_cast<void*>(cfw),
                                reinterpret_cast<void*>(&hook_CreateFileW), nullptr);
        if (tr) g_real_CreateFileW = reinterpret_cast<CreateFileWFn>(tr);
    }
    if (cfa) {
        void* tr = install_hook("CreateFileA", reinterpret_cast<void*>(cfa),
                                reinterpret_cast<void*>(&hook_CreateFileA), nullptr);
        if (tr) g_real_CreateFileA = reinterpret_cast<CreateFileAFn>(tr);
    }
    logf(CDE_LOG_INFO, "data redirect active: %u path(s), CreateFileW=%s CreateFileA=%s",
         static_cast<unsigned>(g_redirect.size()),
         g_real_CreateFileW ? "hooked" : "off", g_real_CreateFileA ? "hooked" : "off");
}

// ---------------------------------------------------------------------------
// in-game ImGui overlay (phase C)

static void overlay_log_bridge(int level, const char* message) {
    logf(level, "%s", message);
}

extern "C" int cde_run_guarded(const char* id, void (*fn)(void)) {
    return guarded_call(std::string(id ? id : "overlay"), "ui", fn) ? 1 : 0;
}

typedef BOOL (WINAPI *WglSwapBuffersFn)(HDC);
static WglSwapBuffersFn g_real_wglSwap = nullptr;
static bool g_in_overlay_swap = false;

static BOOL WINAPI hook_wglSwapBuffers(HDC hdc) {
    if (!g_in_overlay_swap) {
        g_in_overlay_swap = true;
        overlay_frame(hdc);
        g_in_overlay_swap = false;
    }
    return g_real_wglSwap(hdc);
}

static void setup_overlay(void) {
    if (!g_overlay_enabled) return;
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) {
        logf(CDE_LOG_WARN, "overlay: opengl32.dll not loaded; overlay inactive");
        return;
    }
    FARPROC swap = GetProcAddress(gl, "wglSwapBuffers");
    if (!swap) {
        logf(CDE_LOG_WARN, "overlay: wglSwapBuffers not found; overlay inactive");
        return;
    }
    overlay_set_logger(&overlay_log_bridge);
    overlay_set_start_open(g_overlay_open ? 1 : 0);
    overlay_set_panels_open(g_panels_open ? 1 : 0);
    void* tr = install_hook("wglSwapBuffers", reinterpret_cast<void*>(swap),
                            reinterpret_cast<void*>(&hook_wglSwapBuffers), nullptr);
    if (!tr) {
        logf(CDE_LOG_WARN, "overlay: could not hook wglSwapBuffers; overlay inactive");
        return;
    }
    g_real_wglSwap = reinterpret_cast<WglSwapBuffersFn>(tr);
    logf(CDE_LOG_INFO, "overlay: wglSwapBuffers hooked (F2 or Insert toggles the menu)");
}

// ---------------------------------------------------------------------------
// frame tick

static DWORD WINAPI tick_thread(LPVOID param) {
    (void)param;
    for (;;) {
        std::vector<void (*)(void)> cbs;
        std::vector<LoadedMod> mods;
        EnterCriticalSection(&g_lock);
        cbs = g_frame_callbacks;
        mods = g_mods;
        LeaveCriticalSection(&g_lock);
        for (const LoadedMod& m : mods) {
            if (!m.info->on_frame) continue;
            if (!mod_live(m.id)) continue;
            g_current_mod_id = m.id;
            guarded_call(m.id, "on_frame", [&] { m.info->on_frame(); });
        }
        for (void (*cb)(void) : cbs) cb();
        g_current_mod_id.clear();
        Sleep(16);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// settings + plugin discovery

static void read_settings(void) {
    std::string ini = g_dir + "\\cdeloader.ini";
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logf(CDE_LOG_INFO, "no cdeloader.ini; defaults");
        return;
    }
    g_enabled = GetPrivateProfileIntA("loader", "enabled", 1, ini.c_str()) ? true : false;
    g_log_level = GetPrivateProfileIntA("loader", "log_level", CDE_LOG_INFO, ini.c_str());
    g_console_start = GetPrivateProfileIntA("loader", "console", 1, ini.c_str()) ? 1 : 0;
    console_start(g_console_start);
    logf(CDE_LOG_INFO, "console: %s (hwnd=%p)",
         g_console_start ? "started visible" : "shown from the F2 menu",
         console_window());
    g_redirect_enabled = GetPrivateProfileIntA("loader", "redirect", 0, ini.c_str()) ? true : false;
    g_overlay_enabled = GetPrivateProfileIntA("loader", "overlay", 1, ini.c_str()) ? true : false;
    g_overlay_open = GetPrivateProfileIntA("loader", "overlay_open", 0, ini.c_str()) ? true : false;
    g_panels_open = GetPrivateProfileIntA("loader", "panels_open", 0, ini.c_str()) ? true : false;
    logf(CDE_LOG_INFO, "settings: enabled=%d log_level=%d redirect=%d overlay=%d",
         g_enabled ? 1 : 0, g_log_level, g_redirect_enabled ? 1 : 0,
         g_overlay_enabled ? 1 : 0);
}

static bool mod_disabled(const char* id, const std::string& ini) {
    if (!id || !id[0]) return false;
    return GetPrivateProfileIntA("disabled", id, 0, ini.c_str()) ? true : false;
}

static std::string ini_get(const std::string& path, const char* key, const char* def) {
    char buf[512];
    DWORD n = GetPrivateProfileStringA("mod", key, def, buf, sizeof(buf), path.c_str());
    return std::string(buf, n);
}

static std::vector<std::string> ini_list(const std::string& path, const char* key) {
    std::vector<std::string> out;
    std::string v = ini_get(path, key, "");
    size_t i = 0;
    while (i < v.size()) {
        size_t j = v.find(',', i);
        std::string tok = v.substr(i, j == std::string::npos ? std::string::npos : j - i);
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(tok.substr(a, b - a + 1));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}

// "1.2.1.3", "1.2.1.3,1.2.1.4" or "*" (any). Unknown specs deny.
static bool game_version_allowed(const std::string& spec) {
    if (spec.empty() || spec == "*") return true;
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        std::string tok = spec.substr(i, j == std::string::npos ? std::string::npos : j - i);
        size_t a = tok.find_first_not_of(" \t");
        size_t b = tok.find_last_not_of(" \t");
        if (a != std::string::npos) {
            tok = tok.substr(a, b - a + 1);
            if (tok == CDEMOD_GAME_VERSION || tok == "*") return true;
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return false;
}

static void order_mods(void) {
    // Drop mods with an unsatisfied dependency.
    std::set<std::string> present;
    for (const LoadedMod& m : g_mods) present.insert(m.id);

    std::vector<LoadedMod> kept;
    for (const LoadedMod& m : g_mods) {
        bool ok = true;
        for (const std::string& dep : m.depends) {
            if (!present.count(dep)) {
                logf(CDE_LOG_ERROR, "%s requires missing mod '%s'; skipping",
                     m.id.c_str(), dep.c_str());
                ok = false;
                break;
            }
        }
        if (ok) kept.push_back(m);
    }

    // Stable topological order honouring depends + load_after, tie-broken by id
    // (kept is already name-sorted).
    std::vector<LoadedMod> ordered;
    std::set<std::string> done;
    while (ordered.size() < kept.size()) {
        bool progress = false;
        for (const LoadedMod& m : kept) {
            if (done.count(m.id)) continue;
            bool ready = true;
            for (const std::string& p : m.depends)
                if (!done.count(p)) { ready = false; break; }
            if (ready)
                for (const std::string& p : m.load_after)
                    if (!done.count(p)) { ready = false; break; }
            if (ready) {
                ordered.push_back(m);
                done.insert(m.id);
                progress = true;
            }
        }
        if (!progress) {
            for (const LoadedMod& m : kept)
                if (!done.count(m.id)) {
                    logf(CDE_LOG_WARN, "load-order cycle involving %s", m.id.c_str());
                    ordered.push_back(m);
                    done.insert(m.id);
                }
            break;
        }
    }
    g_mods = ordered;
}

static void load_plugins(void) {
    std::string disabled_ini = g_dir + "\\cdeloader.ini";
    std::set<std::string> seen_ids;
    std::string mods_dir = g_dir + "\\mods";
    std::vector<std::string> dirs;

    std::string pattern = mods_dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
                // A dot-prefixed folder is bookkeeping, not a plugin: mods\.pending holds a staged
                // update, and loading the cdeloader.dll copy inside it starts a second initialisation
                // of the loader in the same process - which then fails every hook it re-installs.
                if (fd.cFileName[0] == '.') {
                    logf(CDE_LOG_INFO, "mods: ignoring %s (staging, not a plugin folder)",
                         fd.cFileName);
                    continue;
                }
                dirs.push_back(fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(dirs.begin(), dirs.end());

    // Every DLL in a mod folder is a candidate: the release packages name the file
    // after the plugin (mods/node-editor/node-editor.dll), and plugin.dll from older
    // packages still loads because the scan does not care about the file name.
    std::vector<std::string> dlls;
    for (const std::string& d : dirs) {
        const std::string folder = mods_dir + "\\" + d;
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((folder + "\\*.dll").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::string> names;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) dlls.push_back(folder + "\\" + name);
    }

    for (const std::string& dll : dlls) {
        // .../mods/<folder>/<name>.dll
        const std::string file_name = dll.substr(dll.find_last_of("\\") + 1);
        std::string folder = dll.substr(0, dll.find_last_of("\\"));
        folder = folder.substr(folder.find_last_of("\\") + 1);

        // Confirm the file is the one the installer recorded, when it recorded any.
        {
            const std::string key = folder + "/" + file_name;
            std::map<std::string, std::string>::const_iterator it = g_mod_hashes.find(key);
            if (it != g_mod_hashes.end()) {
                const std::string actual = file_sha256(dll);
                if (!actual.empty() && actual != it->second) {
                    logf(CDE_LOG_WARN,
                         "%s differs from the installed release (%s, expected %s)",
                         dll.c_str(), actual.c_str(), it->second.c_str());
                } else if (!actual.empty()) {
                    logf(CDE_LOG_INFO, "%s verified (%s)", dll.c_str(), actual.c_str());
                }
            }
        }

        HMODULE m = LoadLibraryA(dll.c_str());
        if (!m) {
            logf(CDE_LOG_ERROR, "failed to load %s (err %lu)", dll.c_str(),
                 static_cast<unsigned long>(GetLastError()));
            continue;
        }
        CdeModGetInfoFn get_info = reinterpret_cast<CdeModGetInfoFn>(
            reinterpret_cast<void*>(GetProcAddress(m, CDEMOD_GETINFO_SYMBOL)));
        if (!get_info) {
            logf(CDE_LOG_ERROR, "%s has no %s export", dll.c_str(), CDEMOD_GETINFO_SYMBOL);
            continue;
        }
        const CdeModInfo* info = get_info();
        if (!info) {
            logf(CDE_LOG_ERROR, "%s returned null info", dll.c_str());
            continue;
        }
        if (info->abi_version != CDEMOD_ABI_VERSION) {
            logf(CDE_LOG_ERROR, "%s abi %u != %u; skipping", dll.c_str(),
                 info->abi_version, CDEMOD_ABI_VERSION);
            continue;
        }
        if (info->struct_size < sizeof(CdeModInfo)) {
            logf(CDE_LOG_ERROR, "%s descriptor too small (%u); skipping", dll.c_str(),
                 info->struct_size);
            continue;
        }
        std::string mod_id = info->id ? info->id : folder;
        if (!seen_ids.insert(mod_id).second) {
            logf(CDE_LOG_ERROR, "duplicate mod id '%s' in %s; skipping the second copy",
                 mod_id.c_str(), dll.c_str());
            continue;
        }
        std::string marker = mods_dir + "\\disabled\\" + mod_id;
        if (GetFileAttributesA(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
            logf(CDE_LOG_INFO, "%s disabled by mods/disabled/%s", mod_id.c_str(),
                 mod_id.c_str());
            continue;
        }
        if (mod_disabled(info->id, disabled_ini)) {
            logf(CDE_LOG_INFO, "%s disabled by cdeloader.ini", mod_id.c_str());
            continue;
        }
        std::string mdir = mods_dir + "\\" + folder;
        std::string mini = mdir + "\\mod.ini";
        bool has_ini = GetFileAttributesA(mini.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (has_ini && !GetPrivateProfileIntA("mod", "enabled", 1, mini.c_str())) {
            logf(CDE_LOG_INFO, "%s disabled by mod.ini", mod_id.c_str());
            continue;
        }
        if (has_ini) {
            std::string api = ini_get(mini, "api", "");
            if (!api.empty() && atoi(api.c_str()) != (int)CDEMOD_ABI_VERSION) {
                logf(CDE_LOG_ERROR, "%s needs mod API %s, loader has %u; skipping",
                     mod_id.c_str(), api.c_str(), (unsigned)CDEMOD_ABI_VERSION);
                continue;
            }
            std::string gv = ini_get(mini, "game_versions", "");
            if (!gv.empty() && !game_version_allowed(gv)) {
                logf(CDE_LOG_WARN,
                     "%s declares game_versions '%s' but this is %s; loading anyway",
                     mod_id.c_str(), gv.c_str(), CDEMOD_GAME_VERSION);
            }
        }
        LoadedMod lm;
        lm.id = info->id ? info->id : folder;
        lm.name = info->name ? info->name : lm.id;
        lm.version = info->version ? info->version : "?";
        lm.dir = mdir;
        if (has_ini) {
            lm.depends = ini_list(mini, "depends");
            lm.load_after = ini_list(mini, "load_after");
        }
        lm.info = info;
        g_mods.push_back(lm);
        logf(CDE_LOG_INFO, "discovered %s (%s %s)", lm.id.c_str(), lm.name.c_str(), lm.version.c_str());
    }

    order_mods();
    std::string order;
    for (const LoadedMod& m : g_mods) {
        if (!order.empty()) order += ", ";
        order += m.id;
    }
    logf(CDE_LOG_INFO, "load order: %s", order.empty() ? "(none)" : order.c_str());
    logf(CDE_LOG_INFO, "chainloader: %u active of %u plugin folder(s)",
         (unsigned)g_mods.size(), (unsigned)dirs.size());
    for (const LoadedMod& m : g_mods) {
        std::string deps, after;
        for (const std::string& d : m.depends) deps += (deps.empty() ? "" : ",") + d;
        for (const std::string& d : m.load_after) after += (after.empty() ? "" : ",") + d;
        logf(CDE_LOG_INFO, "  %s %s depends=[%s] load_after=[%s] dir=%s",
             m.id.c_str(), m.version.c_str(), deps.c_str(), after.c_str(),
             m.dir.c_str());
        if (!deps.empty()) {
            std::set<std::string> present;
            for (const LoadedMod& p : g_mods) present.insert(p.id);
            for (const std::string& d : m.depends)
                if (!present.count(d))
                    logf(CDE_LOG_ERROR, "  %s: dependency '%s' is not active",
                         m.id.c_str(), d.c_str());
        }
    }
    // Warn about load_after targets that are not active (usually a typo).
    for (const LoadedMod& m : g_mods) {
        for (const std::string& d : m.load_after) {
            bool found = false;
            for (const LoadedMod& p : g_mods) if (p.id == d) found = true;
            if (!found)
                logf(CDE_LOG_WARN, "  %s: load_after '%s' is not active",
                     m.id.c_str(), d.c_str());
        }
    }
}

// Read-only accessors for the overlay's mod list (F2 menu).  g_mods and
// g_disabled are fixed after startup, so the render thread can read them without a
// lock.
extern "C" int cde_mods_count(void) { return (int)g_mods.size(); }
extern "C" const char* cde_mods_id(int i) {
    return i >= 0 && i < (int)g_mods.size() ? g_mods[(size_t)i].id.c_str() : "";
}
extern "C" const char* cde_mods_version(int i) {
    return i >= 0 && i < (int)g_mods.size() ? g_mods[(size_t)i].version.c_str() : "";
}
extern "C" const char* cde_mods_dir(int i) {
    return i >= 0 && i < (int)g_mods.size() ? g_mods[(size_t)i].dir.c_str() : "";
}

extern "C" int cde_mods_disabled_count(void) { return (int)g_disabled.size(); }
extern "C" const char* cde_mods_disabled_id(int i) {
    return i >= 0 && i < (int)g_disabled.size() ? g_disabled[(size_t)i].id.c_str() : "";
}
extern "C" const char* cde_mods_disabled_version(int i) {
    return i >= 0 && i < (int)g_disabled.size() ? g_disabled[(size_t)i].version.c_str() : "";
}

extern "C" int cde_mods_pending_changes(void) { return g_mods_pending; }

// The plugin's own folder name, which is what gets moved.
static std::string mod_folder(const char* id) {
    if (!id || !*id) return std::string();
    for (const LoadedMod& m : g_mods) {
        if (m.id != id) continue;
        size_t slash = m.dir.find_last_of("\\/");
        return slash == std::string::npos ? m.dir : m.dir.substr(slash + 1);
    }
    return std::string();
}

static std::string disabled_root(void) { return g_dir + "\\mods\\disabled"; }

static int write_text_file(const std::string& path, const std::string& text) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD wrote = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(f);
    return wrote == text.size();
}

static int file_exists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Ask for a plugin to be disabled (or re-enabled) at the next start.  The plugin's
// DLL is loaded in this process, so its folder cannot be moved now; the intent file
// is consumed by apply_mod_intents() before the next scan.
extern "C" int cde_mods_set_disabled(const char* id, int disabled) {
    if (!id || !*id) return 0;
    const std::string root = disabled_root();
    CreateDirectoryA((g_dir + "\\mods").c_str(), nullptr);
    CreateDirectoryA(root.c_str(), nullptr);
    std::string folder = mod_folder(id);
    std::string version;
    for (const LoadedMod& m : g_mods)
        if (m.id == id) version = m.version;
    if (!folder.empty()) {
        const std::string intent = root + "\\." +
                                   (disabled ? "disable-" : "enable-") + folder;
        if (disabled) {
            write_text_file(intent, std::string(id) + " " + version + "\n");
            DeleteFileA((root + "\\.enable-" + folder).c_str());
        } else {
            write_text_file(intent, folder + "\n");
            DeleteFileA((root + "\\.disable-" + folder).c_str());
        }
        g_mods_pending = 1;
        logf(CDE_LOG_INFO, "%s will be %s at the next start", id,
             disabled ? "disabled" : "enabled");
        return 1;
    }
    // A plugin that is currently disabled: its folder sits under mods/disabled.
    for (size_t i = 0; i < g_disabled.size(); ++i) {
        if (g_disabled[i].id != id) continue;
        const std::string folder2 = g_disabled[i].folder;
        write_text_file(root + "\\.enable-" + folder2, folder2 + "\n");
        DeleteFileA((root + "\\.disable-" + folder2).c_str());
        g_mods_pending = 1;
        logf(CDE_LOG_INFO, "%s will be enabled at the next start", id);
        return 1;
    }
    return 0;
}

// Restart the game: start a second instance and leave this one.
//
// No helper is needed for a staged update to apply: the successor's proxy waits, at process attach,
// for this instance to disappear before it touches any file (proxy.c, proxy_wait_for_previous_instance).
// Keeping the launch simple means one fewer program to ship, update and explain.
extern "C" __declspec(dllexport) int __cdecl cde_game_relaunch(void) {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    logf(CDE_LOG_INFO, "relaunching %s", exe);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                        g_dir.c_str(), &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // TerminateProcess rather than ExitProcess: exit runs DLL detach handlers, and the crash seen on
    // a relaunch (UNHANDLED EXCEPTION 0x80000100) comes from there. A restart wants the process gone,
    // not tidied - the successor reloads everything anyway.
    TerminateProcess(GetCurrentProcess(), 0);
    return 1;
}

// Move plugin folders according to the intent files written by the F2 menu, then
// list what is disabled.  Runs before the plugin scan.
static void apply_mod_intents(void) {
    const std::string root = disabled_root();
    CreateDirectoryA((g_dir + "\\mods").c_str(), nullptr);
    CreateDirectoryA(root.c_str(), nullptr);

    // Makes the moves in a stable order: "enable" first, then "disable".
    const char* prefixes[2] = {"enable-", "disable-"};
    for (int pass = 0; pass < 2; ++pass) {
        WIN32_FIND_DATAA fd;
        const std::string pattern = root + "\\." + prefixes[pass] + "*";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string name = fd.cFileName;
            const std::string folder = name.substr(1 + strlen(prefixes[pass]));
            if (folder.empty()) continue;
            const std::string from = (pass == 0 ? root + "\\" + folder
                                               : g_dir + "\\mods\\" + folder);
            const std::string to = (pass == 0 ? g_dir + "\\mods\\" + folder
                                              : root + "\\" + folder);
            if (file_exists(from) && !file_exists(to)) {
                if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_COPY_ALLOWED)) {
                    logf(CDE_LOG_INFO, "mods: moved %s -> %s", from.c_str(), to.c_str());
                    if (pass == 1) {
                        // Keep the identity with the folder so a later session can
                        // list it without loading the plugin.
                        char info[256] = {0};
                        HANDLE f = CreateFileA((root + "\\." + "disable-" + folder).c_str(),
                                               GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (f != INVALID_HANDLE_VALUE) {
                            DWORD got = 0;
                            ReadFile(f, info, sizeof info - 1, &got, nullptr);
                            CloseHandle(f);
                            write_text_file(to + "\\.mod-info", std::string(info, got));
                        }
                    }
                } else {
                    logf(CDE_LOG_WARN, "mods: could not move %s (still running?)",
                         from.c_str());
                }
            }
            DeleteFileA((root + "\\" + name).c_str());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // Whatever is left under mods/disabled is a disabled plugin.
    g_disabled.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::string folder = fd.cFileName;
            if (folder == "." || folder == "..") continue;
            DisabledMod d;
            d.folder = folder;
            d.id = folder;
            std::string info;
            HANDLE f = CreateFileA((root + "\\" + folder + "\\.mod-info").c_str(),
                                   GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[256] = {0};
                DWORD got = 0;
                ReadFile(f, buf, sizeof buf - 1, &got, nullptr);
                CloseHandle(f);
                info.assign(buf, got);
            }
            size_t space = info.find(' ');
            if (space != std::string::npos) {
                d.id = info.substr(0, space);
                size_t nl = info.find('\n', space);
                d.version = info.substr(space + 1,
                                        (nl == std::string::npos ? info.size() : nl) - space - 1);
            }
            g_disabled.push_back(d);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!g_disabled.empty())
        logf(CDE_LOG_INFO, "mods: %u disabled plugin folder(s) under mods/disabled",
             (unsigned)g_disabled.size());
}

static void init_plugins(void) {
    for (const LoadedMod& m : g_mods) {
        g_current_mod_id = m.id;
        if (!m.info->on_init) continue;
        int rc = 0;
        if (guarded_call(m.id, "on_init", [&] { rc = m.info->on_init(&g_api); }) && rc != 0)
            logf(CDE_LOG_ERROR, "%s on_init returned %d", m.id.c_str(), rc);
    }
    g_current_mod_id.clear();
}

// ---------------------------------------------------------------------------
// startup

static void setup_api(void) {
    memset(&g_api, 0, sizeof(g_api));
    g_api.abi_version = CDEMOD_ABI_VERSION;
    g_api.struct_size = sizeof(CdeHostApi);
    g_api.log = api_log;
    g_api.config_get_int = api_config_get_int;
    g_api.config_get_bool = api_config_get_bool;
    g_api.config_get_float = api_config_get_float;
    g_api.config_get_str = api_config_get_str;
    g_api.read_mem = api_read_mem;
    g_api.write_mem = api_write_mem;
    g_api.resolve_symbol = api_resolve_symbol;
    g_api.find_pattern = api_find_pattern;
    g_api.patch_bytes = api_patch_bytes;
    g_api.create_hook = api_create_hook;
    g_api.enable_hook = api_enable_hook;
    g_api.disable_hook = api_disable_hook;
    g_api.on_frame = api_on_frame;
    g_api.config_set_int = api_config_set_int;
    g_api.config_set_bool = api_config_set_bool;
    g_api.config_set_float = api_config_set_float;
    g_api.config_set_str = api_config_set_str;
    g_api.config_reset = api_config_reset;
    g_api.config_flush = api_config_flush;
}

static DWORD WINAPI init_thread(LPVOID param) {
    (void)param;
    discover_dir();
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, vec_handler);
    logf(CDE_LOG_INFO, "cdeloader %s starting (host %p)", LOADER_VERSION, GetModuleHandleA(NULL));
    if (GetModuleHandleA("cdepatch.dll")) {
        logf(CDE_LOG_INFO,
             "community patch detected (cdepatch.dll loaded); running side by side");
    }
    read_settings();
    if (!g_enabled) {
        logf(CDE_LOG_INFO, "cdeloader disabled by cdeloader.ini");
        return 0;
    }
    setup_api();
    load_symbols();
    verify_host_hash();
    install_tactical_capture();
    apply_mod_intents();
    load_mod_hashes();
    load_plugins();
    init_plugins();
    setup_redirect();
    setup_overlay();
    HANDLE t = CreateThread(nullptr, 0, tick_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    logf(CDE_LOG_INFO, "cdeloader ready: %u plugin(s)", static_cast<unsigned>(g_mods.size()));
    return 0;
}

// ---------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        InitializeCriticalSection(&g_lock);
        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

// The injected .cdei import resolves this; work happens on the init thread.
extern "C" CDEMOD_EXPORT int CDEMOD_CALL CdeModInit(void) {
    logf(CDE_LOG_INFO, "CdeModInit() called");
    return 0;
}

// Marker so a payload can tell whether the loader is present.
extern "C" CDEMOD_EXPORT int CDEMOD_CALL CdeLoader_IsCdeLoader(void) {
    return 1;
}
