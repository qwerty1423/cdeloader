// Deliberately broken plugin, used by scripts/test-modloader.sh.
//
// It faults inside on_init (null dereference) to prove per-call isolation: the
// loader must log the fault, disable this mod, and keep the healthy plugins
// running.  Built as a 32-bit DLL exporting CdeMod_GetInfo.
//
// Build: scripts/build-modloader.sh

#include "cdemod_api.h"

static int CDEMOD_CALL faulty_init(const CdeHostApi* host) {
    host->log(CDE_LOG_INFO, "faulty plugin: about to dereference null");
    volatile int* p = nullptr;
    *p = 1;  // access violation, recovered by the loader's guard
    return 0;  // unreachable
}

static const CdeModInfo faulty_info = {
    CDEMOD_ABI_VERSION,
    sizeof(CdeModInfo),
    "org.example.faulty",
    "Faulty Mod (isolation test)",
    "1.0.0",
    "coade-re",
    faulty_init,
    nullptr,
    nullptr
};

extern "C" CDEMOD_EXPORT const CdeModInfo* CDEMOD_CALL CdeMod_GetInfo(void) {
    return &faulty_info;
}
