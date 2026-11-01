// community_fixes.cpp — a runtime "fix pack" mod that re-implements a subset of
// the CDE Community Patch's crash fixes as loader hooks.
//
// Why this exists
// ---------------
// The community patch (`CDE-Community-Patch-0.1.0-test5`) ships an exe delta
// plus a helper DLL (`cdepatch.dll`) that, at load time, resolves fixed
// addresses in CDE.exe and installs hooks ("replace the function, call the
// original through a trampoline").  This mod demonstrates that the same fixes
// can be expressed as an ordinary mod: every target is resolved by PDB symbol
// name (never a hardcoded address) through the loader's symbol table, and the
// guard is installed with the loader's inline-hook engine.
//
// Scope
// -----
// The community patch makes ~40 changes.  Most of the performance ones
// (broadphase collision, worker threads, the streak pool, several caches) are
// large re-implementations of game internals and are *not* reproduced here;
// they are only resolved and reported so the log shows exactly what a given
// CDE build covers.  The four below are self-contained *crash fixes* whose
// behaviour is fully explained by the community changelog and the PDB
// signature, so they can be re-implemented safely.
//
// Reference: notes/PARITY.md (the full site map) and docs/13-community-patch-parity.md.
//
// Build: scripts/build-modloader.sh

#include <cstdint>

static void cf_init(void);
static void cf_frame(void);
static void cf_selftest(void);

#define CDE_USER_INIT()  cf_init()
#define CDE_USER_FRAME() cf_frame()

#include "cdemod.hpp"

// ---------------------------------------------------------------------------
// 1. Campaign::IsLevelBeaten(DataParser*, Level*, LevelRating::Type) const
//
// Community patch (test2): "Crash when leaving a mission ... Campaigns without
// such missions crashed."  When the campaign has no mission of the requested
// rating, the Level* is null and the game dereferences it.  Fix: null -> false.
//
// ABI (MSVC __thiscall): this = ECX, args on the stack in order.
// ---------------------------------------------------------------------------
typedef bool(__thiscall* IsLevelBeatenFn)(const void*, void*, void*, int);
static IsLevelBeatenFn g_orig_ilb = nullptr;
static unsigned g_hits_ilb = 0;

static bool __thiscall detour_ilb(const void* self, void* parser, void* level, int type) {
    if (!level) {
        ++g_hits_ilb;
        return false;  // campaign has no level of this rating
    }
    return g_orig_ilb(self, parser, level, type);
}

// ---------------------------------------------------------------------------
// 2. SpacecraftComponent::ResetArmorTextureCooling(void)
//
// Community patch (test3): "crashes when leaving a battle ... in the armor heat
// reset (ResetArmorTextureCooling) for craft without armor texture data."
// The craft's armor data pointer at this+0x118 can be null; the original
// dereferences it.  Fix: skip when the pointer is null.
// ---------------------------------------------------------------------------
typedef void(__thiscall* ResetArmorFn)(void*);
static ResetArmorFn g_orig_rac = nullptr;
static unsigned g_hits_rac = 0;

static void __thiscall detour_rac(void* self) {
    if (!self) {
        ++g_hits_rac;
        return;
    }
    void* armor = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + 0x118);
    if (!armor) {
        ++g_hits_rac;
        return;  // craft has no armor texture data
    }
    g_orig_rac(self);
}

// ---------------------------------------------------------------------------
// 3. ControllableComponent::IsMarkedAsSeparatingFromFleet(void) const
//
// Community patch (test3): "when splitting fleets afterwards (GenerateNewFleets)
// for craft without a controllable part."  The call site passes a null
// ControllableComponent*.  Fix: null receiver -> false (not separating).
// ---------------------------------------------------------------------------
typedef bool(__thiscall* IsSeparatingFn)(const void*);
static IsSeparatingFn g_orig_sep = nullptr;
static unsigned g_hits_sep = 0;

static bool __thiscall detour_sep(const void* self) {
    if (!self) {
        ++g_hits_sep;
        return false;
    }
    return g_orig_sep(self);
}

// ---------------------------------------------------------------------------
// 4. Material::GetBareSphereCriticalRadius_m(void) const -> double
//
// Community patch (test2): "A nuclear payload with fuel that cannot go critical
// ... is now reported as unable to go critical."  When the fuel is subcritical
// the closed-form radius is not a finite number and the caller's assert fires.
// Fix: replace a non-finite result with a very large ("infinite") radius, which
// is the physically correct answer for a subcritical mass.
//
// This is the one guard here that is an *approximation* of the community fix
// (we sanitise the result rather than re-deriving it); it is logged as such.
// ---------------------------------------------------------------------------
typedef double(__thiscall* CritRadiusFn)(const void*);
static CritRadiusFn g_orig_cr = nullptr;
static unsigned g_hits_cr = 0;

static double __thiscall detour_cr(const void* self) {
    double r = g_orig_cr(self);
    if (r != r) {  // NaN: fuel cannot go critical
        ++g_hits_cr;
        return 1.0e300;  // effectively infinite critical radius
    }
    return r;
}

// ---------------------------------------------------------------------------
// Sites we resolve + report but do not (yet) re-implement.  These are the
// remaining community-patch crash fixes; most need a `util::Str<char>` (returned
// by value) or a richer re-implementation to reproduce faithfully.
// ---------------------------------------------------------------------------
struct ReportSite {
    const char* label;
    const char* symbol;
};

static const ReportSite kReport[] = {
    {"UIArmorLayerPanel::GetLayerName (stale layer guard)",
     "?GetLayerName@UIArmorLayerPanel@@QBE?AV?$Str@D@util@@XZ"},
    {"CompartmentButton::ShouldDrawCrosshairs (dead ship guard)",
     "?ShouldDrawCrosshairs@CompartmentButton@@ABE_NXZ"},
    {"util::SimpleSerializationManager::GetPointer (missing reference)",
     "?GetPointer@SimpleSerializationManager@util@@UBEPAVReflectable@2@ABV?$Str@D@2@0@Z"},
    {"CelestialObject::GetLPointName (parentless body)",
     "?GetLPointName@CelestialObject@@QBE?AV?$Str@D@util@@H@Z"},
    {"PrintSigFigures (invalid number -> NaN)",
     "?PrintSigFigures@@YA?AV?$Str@D@util@@NH@Z"},
    {"LaserModule::GetRadiusAtDistance_m (stale wavelength)",
     "?GetRadiusAtDistance_m@LaserModule@@UBENN@Z"},
    {"SteamHandler::Initialize (no Steam -> continue)",
     "?Initialize@SteamHandler@@QAE_NXZ"},
    {"TacticalManager::GetSelectedSpacecrafts (drop destroyed craft)",
     "?GetSelectedSpacecrafts@TacticalManager@@QBE?AV?$Darray@V?$WeakPtr@VSpacecraftComponent@@@util@@@util@@XZ"},
    {"Itinerary::RegenerateTrajectoryAfter (fleet map object gone)",
     "?RegenerateTrajectoryAfter@Itinerary@@AAEXABUItineraryNode@@_NNV?$WeakPtr@VFleet@@@util@@@Z"},
    {"BriefingsHandler::GetCustomizableFleet (side without a fleet)",
     "?GetCustomizableFleet@BriefingsHandler@@QBEPAUStartingFleet@@_N@Z"},
    {"util::LogEnsure (redefined data logging)",
     "?LogEnsure@util@@YAXPBDH00@Z"},
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
template <typename T>
static T install(const char* label, const char* symbol, T detour) {
    uintptr_t va = cde::Resolve(symbol);
    if (!va) {
        CDE_WARN("community-fix '%s': symbol not found", label);
        return nullptr;
    }
    T orig = cde::Hook(label, reinterpret_cast<T>(va), detour);
    if (orig) {
        CDE_INFO("hooked %s @ %p -> %p", label, reinterpret_cast<void*>(va),
                 reinterpret_cast<void*>(detour));
    } else {
        CDE_ERROR("HOOK FAILED %s @ %p", label, reinterpret_cast<void*>(va));
    }
    return orig;
}

static void cf_init(void) {
    if (!cde::ConfigBool("community_fixes", 1)) {
        CDE_INFO("community fixes disabled by config");
        return;
    }

    CDE_INFO("community fixes: re-implementing %u crash guards", 4u);

    g_orig_ilb = install(
        "Campaign::IsLevelBeaten",
        "?IsLevelBeaten@Campaign@@QBE_NPAVDataParser@@PAULevel@@W4Type@LevelRating@@@Z",
        &detour_ilb);
    g_orig_rac = install("SpacecraftComponent::ResetArmorTextureCooling",
                         "?ResetArmorTextureCooling@SpacecraftComponent@@AAEXXZ", &detour_rac);
    g_orig_sep = install("ControllableComponent::IsMarkedAsSeparatingFromFleet",
                         "?IsMarkedAsSeparatingFromFleet@ControllableComponent@@QBE_NXZ",
                         &detour_sep);
    g_orig_cr = install("Material::GetBareSphereCriticalRadius_m",
                        "?GetBareSphereCriticalRadius_m@Material@@QBENXZ", &detour_cr);

    if (!cde::ConfigBool("report_sites", 1)) return;

    // Report the community-patch sites we know about so the log shows coverage.
    unsigned resolved = 0;
    for (const ReportSite& s : kReport) {
        uintptr_t va = cde::Resolve(s.symbol);
        if (va) {
            ++resolved;
            CDE_INFO("community-fix site (report only): %s @ %p", s.label,
                     reinterpret_cast<void*>(va));
        } else {
            CDE_DEBUG("community-fix site (report only, unresolved): %s", s.label);
        }
    }
    CDE_INFO("community fixes: %u reported site(s) resolved", resolved);

    if (cde::ConfigBool("selftest", 0)) cf_selftest();
}

// A guard can be proved *live* without any game state: call the hooked entry
// with the null pointer the guard protects against.  If the hook were not
// installed these calls would reach the original and crash; getting the guard's
// safe return value proves interception.  Off by default.
static void cf_selftest(void) {
    typedef bool(__thiscall * IlbEntry)(const void*, void*, void*, int);
    typedef bool(__thiscall * SepEntry)(const void*);
    typedef void(__thiscall * RacEntry)(void*);

    uintptr_t ilb = cde::Resolve(
        "?IsLevelBeaten@Campaign@@QBE_NPAVDataParser@@PAULevel@@W4Type@LevelRating@@@Z");
    uintptr_t sep =
        cde::Resolve("?IsMarkedAsSeparatingFromFleet@ControllableComponent@@QBE_NXZ");
    uintptr_t rac = cde::Resolve("?ResetArmorTextureCooling@SpacecraftComponent@@AAEXXZ");

    if (ilb) {
        bool r = reinterpret_cast<IlbEntry>(ilb)(nullptr, nullptr, nullptr, 0);
        CDE_INFO("selftest: IsLevelBeaten(null level) = %d (expect 0)", r ? 1 : 0);
    }
    if (sep) {
        bool r = reinterpret_cast<SepEntry>(sep)(nullptr);
        CDE_INFO("selftest: IsMarkedAsSeparatingFromFleet(null) = %d (expect 0)", r ? 1 : 0);
    }
    if (rac) {
        reinterpret_cast<RacEntry>(rac)(nullptr);
        CDE_INFO("selftest: ResetArmorTextureCooling(null) returned without crashing");
    }
}

static void cf_frame(void) {
    static unsigned last_total = 0;
    unsigned total = g_hits_ilb + g_hits_rac + g_hits_sep + g_hits_cr;
    if (total != last_total) {
        last_total = total;
        CDE_INFO("community fixes fired: IsLevelBeaten=%u ResetArmorTextureCooling=%u "
                 "IsMarkedAsSeparatingFromFleet=%u GetBareSphereCriticalRadius_m=%u",
                 g_hits_ilb, g_hits_rac, g_hits_sep, g_hits_cr);
    }
}

CDE_MOD("org.cdere.community-fixes", "CDE Community Fixes (parity mod)", "0.1.0", "coade-re")
