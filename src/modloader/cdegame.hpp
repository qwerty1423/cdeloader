/* cdegame.hpp — small shared helpers for mods (no game calls, header-only).
 *
 * Deliberately dependency-free math plus UI glue that several mods need:
 * projecting a world position to screen pixels, a panel helper, config string
 * reads, and the game version the symbol table targets.
 *
 *   #include "cdegame.hpp"
 *   cde::game::CameraView cam = ...;              // from game accessors
 *   float x, y, depth;
 *   if (cde::game::Project(cam, wx, wy, wz, sw, sh, &x, &y, &depth)) { ... }
 */
#ifndef CDEGAME_HPP
#define CDEGAME_HPP

#include "cdemod.hpp"
#include "cdemod_ui.h"

#include <cmath>
#include <cstdio>

namespace cde {
namespace game {

/* Version of the retail build the shipped symbol table was generated from. */
inline const char* Version() { return "1.2.1.3"; }

struct Vec3 {
    double x, y, z;
};

inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(const Vec3& v) {
    double n = std::sqrt(Dot(v, v));
    return n > 1e-12 ? Vec3{v.x / n, v.y / n, v.z / n} : Vec3{0, 0, 0};
}
inline Vec3 Scale(const Vec3& v, double s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

/* Camera basis as returned by CameraManager::GetCameraPos/Forward/Right (any
 * consistent length unit works; world and camera must use the same unit). */
struct CameraView {
    Vec3 pos;
    Vec3 forward;
    Vec3 right;      // unit, pointing to screen +X
    Vec3 up;         // unit, pointing to screen +Y (may be derived)
    double fov_y_rad;
    double aspect;   // width / height
};

/* Project a world point to pixels.  Returns false when behind the camera or
 * closer than `near`.  `depth` receives the forward distance. */
inline bool Project(const CameraView& c, double wx, double wy, double wz,
                    int screen_w, int screen_h, float* out_x, float* out_y,
                    float* out_depth, double near = 0.0) {
    const Vec3 rel = Sub(Vec3{wx, wy, wz}, c.pos);
    const double depth = Dot(rel, c.forward);
    if (depth <= near || depth <= 1e-12) return false;
    const double tan_half = std::tan(c.fov_y_rad * 0.5);
    if (tan_half <= 1e-12 || c.aspect <= 1e-12) return false;
    const double ndc_x = (Dot(rel, c.right) / (depth * tan_half * c.aspect));
    const double ndc_y = (Dot(rel, c.up) / (depth * tan_half));
    if (out_x) *out_x = (float)((ndc_x * 0.5 + 0.5) * screen_w);
    if (out_y) *out_y = (float)((1.0 - (ndc_y * 0.5 + 0.5)) * screen_h);
    if (out_depth) *out_depth = (float)depth;
    return true;
}

/* Derive the up vector from forward/right (right-handed, Y-up in screen terms). */
inline CameraView MakeView(const Vec3& pos, const Vec3& forward, const Vec3& right,
                           double fov_y_rad, double aspect) {
    CameraView c;
    c.pos = pos;
    c.forward = Normalize(forward);
    c.right = Normalize(right);
    c.up = Normalize(Cross(c.right, c.forward));
    c.fov_y_rad = fov_y_rad;
    c.aspect = aspect;
    return c;
}

/* Screen size, valid only inside a panel draw callback. */
inline void ScreenSize(int* w, int* h) { cde_ui_screen_size(w, h); }

/* String config value (writes "" on failure). */
inline void ConfigString(const char* key, char* out, int cap, const char* def = "") {
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!cde::ConfigStr(key, out, cap) && def) {
        std::snprintf(out, (size_t)cap, "%s", def);
    }
}

/* Small helper for a per-mod panel: call Begin() from on_init once. */
struct Panel {
    const char* name;
    explicit Panel(const char* panel_name) : name(panel_name) {}
    void Register(CdeUiDrawFn draw) { cde_ui_panel(name, draw); }
    int Available() const { return cde_ui_available(); }
};

/* Colour helper: 0xAABBGGRR, the format cde_ui_overlay_* expects. */
inline unsigned Rgba(int r, int g, int b, int a = 255) {
    return ((unsigned)(a & 255) << 24) | ((unsigned)(b & 255) << 16) |
           ((unsigned)(g & 255) << 8) | (unsigned)(r & 255);
}


/* ------------------------------------------------------------ runtime ---- *
 * Lazy access to the live game objects.  This is the F8 shared runtime: the
 * ABI details below were established against the retail build (see
 * docs/10-mod-framework.md and the F4 dumps in the session notes).
 *
 *   cde::game::Runtime& rt = cde::game::Runtime::Get();
 *   cde::game::CameraView cam;
 *   if (rt.Camera(&cam)) { ...Project(cam, ...); }
 *
 * All calls are safe before the game is ready (they return false).  Do not call
 * these from a plugin's on_init; use the first frames/HUD instead.
 */
namespace detail {

typedef void* (__cdecl* FnGetGameManager)(void);
typedef void* (__thiscall* FnGetManager)(void* game_manager);
typedef void (__thiscall* FnGetVec3)(const void* self, Vec3* out);
typedef double (__thiscall* FnGetDouble)(const void* self);
typedef bool (__thiscall* FnGetBool)(const void* self);

struct DarrayRaw {
    void* vtable;
    void* data;
    int size;
    int cap;
};
struct WeakPtrRaw {
    void* ptr;
    void* ctrl;
};
typedef void (__thiscall* FnGetDarray)(const void* self, DarrayRaw* out);
typedef void (__thiscall* FnDarrayDtor)(DarrayRaw* self);

template <typename T>
inline T Sym(const char* name) {
    return reinterpret_cast<T>(cde::Resolve(name));
}

}  // namespace detail

struct FleetInfo {
    void* object;
    Vec3 pos;      // Fleet::GetPosition_km (campaign frame)
    bool player;
};

struct BodyInfo {
    void* object;
    Vec3 pos;                       // CelestialObject::GetPosition_km (absolute km)
    double radius_km;               // CelestialObject::GetVisualRadius_km
    double mu_km3_s2;               // CelestialObject::GetStandardGravitationalParameter_km3__s2 (0 if unresolved)
    bool luminous;
};

// Convert the game's km/s^2 to m/s^2 for display.
inline double Kms2ToMs2(double a) { return a * 1000.0; }

/* One of the player's spacecraft components (a top-level craft, as the game
 * lists it): the SpacecraftComponent pointer plus the CraftInstance it drives.
 * Both are game pointers: read what you need, do not keep them across frames
 * without checking they are still alive (a destroyed craft's WeakPtr goes null
 * and is skipped here). */
struct ComponentInfo {
    void* component;  // SpacecraftComponent*
    void* craft;      // SpacecraftComponent::Craft (CraftInstance*), may be null
};

class Runtime {
public:
    static Runtime& Get() {
        static Runtime instance;
        return instance;
    }

    bool Init() {
        if (ready_) return true;
        if (!get_gm_) {
            get_gm_ = detail::Sym<detail::FnGetGameManager>(
                "?GetGameManager@@YAPAVGameManager@@XZ");
            get_manager_ = detail::Sym<detail::FnGetManager>(
                "GameManager::GetManager<class StrategicManager>");
            cam_pos_ = detail::Sym<detail::FnGetVec3>("CameraManager::GetCameraPos");
            cam_forward_ =
                detail::Sym<detail::FnGetVec3>("CameraManager::GetCameraForward");
            cam_up_ = detail::Sym<detail::FnGetVec3>("CameraManager::GetCameraUp");
            cam_right_ =
                detail::Sym<detail::FnGetVec3>("CameraManager::GetCameraRight");
            cam_fovy_ = detail::Sym<detail::FnGetDouble>(
                "CameraManager::GetCameraFieldOfViewY_radians");
            cam_aspect_ =
                detail::Sym<detail::FnGetDouble>("CameraManager::GetAspectRatio");
            cam_target_ =
                detail::Sym<detail::FnGetVec3>("CameraManager::GetCameraTargetCenter");
            obj_pos_ = detail::Sym<detail::FnGetVec3>(
                "CelestialObject::GetPosition_km");
            get_fleets_ = detail::Sym<detail::FnGetDarray>("StrategicManager::GetFleets");
            fleet_pos_ = detail::Sym<detail::FnGetVec3>("Fleet::GetPosition_km");
            fleet_player_ = detail::Sym<detail::FnGetBool>("Fleet::IsPlayerFleet");
            // No public GetCelestialObjects exists; the private memoized
            // functions are the real producers (verified against the PDB).
            get_bodies_ = detail::Sym<detail::FnGetDarray>(
                "StrategicManager::GetGameplayRelevantCelestialObjects_MemoizedFunction");
            if (!get_bodies_)
                get_bodies_ = detail::Sym<detail::FnGetDarray>(
                    "StrategicManager::GetCelestialObjects_MemoizedFunction");
            if (!get_bodies_)
                get_bodies_ = detail::Sym<detail::FnGetDarray>(
                    "StrategicManager::GetGravitationallyRelevantCelestialObjects_MemoizedFunction");
            body_pos_ = detail::Sym<detail::FnGetVec3>(
                "CelestialObject::GetPosition_km");
            body_radius_ =
                detail::Sym<detail::FnGetDouble>("CelestialObject::GetVisualRadius_km");
            body_luminous_ =
                detail::Sym<detail::FnGetBool>("CelestialObject::IsLuminousBody");
            body_mu_ = detail::Sym<detail::FnGetDouble>(
                "CelestialObject::GetStandardGravitationalParameter_km3__s2");
            // These destructors are ICF-folded: the table carries one alias per
            // instantiated type and the shipped spelling is the *virtual* form
            // (UAE), not QAE.  Asking for QAE resolved to null, so Fleets() leaked
            // one array per call (F77).
            darray_dtor_ = detail::Sym<detail::FnDarrayDtor>(
                "??1?$Darray@V?$WeakPtr@VFleet@@@util@@@util@@UAE@XZ");
            // F8: the player's own craft (tactical layer).  Same by-value
            // Darray<WeakPtr<SpacecraftComponent>> shape as GetFleets.
            get_player_components_ = detail::Sym<detail::FnGetDarray>(
                "TacticalManager::GetPlayerSpacecraftComponents");
            parts_dtor_ = detail::Sym<detail::FnDarrayDtor>(
                "??1?$Darray@V?$WeakPtr@VSpacecraftComponent@@@util@@@util@@UAE@XZ");
        }
        // A missing destructor would leak one array per call (F77), and nothing
        // else would report it: say so once, loudly, instead of leaking quietly.
        if (!darray_dtor_ || !parts_dtor_)
            CDE_WARN("cde::game: Darray destructor unresolved; array returns will leak");
        if (!get_gm_ || !cam_pos_ || !cam_forward_ || !cam_right_ || !cam_fovy_ ||
            !cam_aspect_)
            return false;
        void* gm = get_gm_();
        if (!gm) return false;
        void* cam = nullptr;
        void* active = nullptr;
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(gm) + 0x24, &cam, sizeof cam) ||
            !cam)
            return false;
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(gm) + 0x38, &active,
                          sizeof active) ||
            !active)
            return false;
        gm_ = gm;
        camera_ = cam;
        active_ = active;
        if (get_manager_) strategic_ = get_manager_(gm);
        ready_ = true;
        return true;
    }

    bool Ready() const { return ready_; }
    void* GameManager() { return Init() ? gm_ : nullptr; }
    void* StrategicManager() { return Init() ? strategic_ : nullptr; }
    void* ActiveManager() { return Init() ? active_ : nullptr; }
    void* CameraManager() { return Init() ? camera_ : nullptr; }
    bool ActiveIsStrategic() {
        return Init() && strategic_ && active_ == strategic_;
    }

    /* Camera in the *view* convention Project expects (forward points where the
     * camera looks).  The game's forward is either the view or its negation;
     * the target center disambiguates, as verified from an F4 dump. */
    bool Camera(CameraView* out) {
        if (!out || !Init()) return false;
        Vec3 pos = {0, 0, 0}, fwd = {0, 0, 0}, up = {0, 0, 1}, right = {1, 0, 0};
        cam_pos_(camera_, &pos);
        cam_forward_(camera_, &fwd);
        cam_up_(camera_, &up);
        cam_right_(camera_, &right);
        const double fovy = cam_fovy_(camera_);
        const double aspect = cam_aspect_(camera_);
        if (!Finite(pos) || !Finite(fwd) || !Finite(up) || !Finite(right) ||
            !std::isfinite(fovy) || !std::isfinite(aspect) || fovy <= 1e-4 ||
            fovy > 3.0 || aspect <= 1e-3)
            return false;
        int sign = -1;
        if (cam_target_) {
            Vec3 target = {0, 0, 0};
            cam_target_(camera_, &target);
            const Vec3 to_target = Sub(target, pos);
            if (Finite(to_target)) {
                const double dt = Dot(to_target, fwd);
                if (dt > 1e-9) sign = 1;
                else if (dt < -1e-9) sign = -1;
            }
        }
        out->pos = pos;
        out->forward = sign > 0 ? fwd : Scale(fwd, -1.0);
        out->up = up;
        out->right = right;
        out->fov_y_rad = fovy;
        out->aspect = aspect;
        return true;
    }

    /* Position of the render-reference celestial object (StrategicManager+0x250)
     * in the same kilometres the camera uses; false when there is none. */
    bool ReferencePosition(Vec3* out) {
        if (!out || !Init() || !strategic_ || !obj_pos_) return false;
        void* object = nullptr;
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(strategic_) + 0x250, &object,
                          sizeof object) ||
            !object)
            return false;
        Vec3 pos = {0, 0, 0};
        if (!cde::ReadMem(reinterpret_cast<uintptr_t>(object), &pos, sizeof pos))
            return false;
        obj_pos_(object, &pos);
        if (!Finite(pos)) return false;
        *out = pos;
        return true;
    }

    /* Snapshot the strategic fleets.  Returns the number written (<= max).
     * Positions are the campaign-frame values the game itself reports; bodies
     * are absolute, so subtract ReferencePosition() when mixing them. */
    int Fleets(FleetInfo* out, int max) {
        if (!out || max <= 0 || !Init() || !strategic_ || !get_fleets_ ||
            !fleet_pos_)
            return 0;
        detail::DarrayRaw arr = {nullptr, nullptr, 0, 0};
        get_fleets_(strategic_, &arr);
        int written = 0;
        if (arr.size > 0 && arr.size < 8192 && arr.data) {
            const detail::WeakPtrRaw* items = static_cast<const detail::WeakPtrRaw*>(arr.data);
            for (int i = 0; i < arr.size && written < max; ++i) {
                void* fleet = items[i].ptr;
                if (!fleet) continue;
                FleetInfo info = {fleet, {0, 0, 0}, false};
                fleet_pos_(fleet, &info.pos);
                if (!Finite(info.pos)) continue;
                if (fleet_player_) info.player = fleet_player_(fleet);
                out[written++] = info;
            }
        }
        // The getter returns the array by value: the caller owns it.
        if (darray_dtor_) darray_dtor_(&arr);
        return written;
    }

    /* The loader captures the game's TacticalManager from its own call to
     * TacticalManager::GetPlayerSpacecraftComponents; never guess it, a tactical
     * method on the wrong manager crashes.  NULL until a tactical view exists. */
    void* TacticalManager() { return cde_game_tactical_manager(); }

    /* The player's spacecraft components in the tactical view.  Returns the
     * number written (<= max).  Empty until a tactical view has been entered at
     * least once (that is when the game itself calls the getter and we capture
     * the manager). */
    // SpacecraftComponent offsets verified against GetCraft (0x007d0480); see
    // notes/PDB-OFFSET-AUDIT.md.  Do not take these from the PDB without a code read.
    static const uintptr_t kOffCraft = 0x118;

    int PlayerComponents(ComponentInfo* out, int max) {
        if (!out || max <= 0 || !Init() || !get_player_components_)
            return 0;
        void* tactical = TacticalManager();
        if (!tactical) return 0;
        detail::DarrayRaw arr = {nullptr, nullptr, 0, 0};
        get_player_components_(tactical, &arr);
        int written = 0;
        if (arr.size > 0 && arr.size < 4096 && arr.data) {
            const detail::WeakPtrRaw* items =
                static_cast<const detail::WeakPtrRaw*>(arr.data);
            for (int i = 0; i < arr.size && written < max; ++i) {
                void* component = items[i].ptr;
                if (!component) continue;
                ComponentInfo info = {component, nullptr};
                // SpacecraftComponent::GetCraft (0x007d0480) reads the WeakPtr at
                // +0x118/+0x11C (data + controller).  The PDB's 0x138 is 0x20 too
                // high in this build (notes/PDB-OFFSET-AUDIT.md, F73), and reading
                // it handed callers a garbage pointer: calling CraftInstance
                // methods on it faulted (0xc0000005 in GetDeltaVRemaining_km__s).
                detail::WeakPtrRaw craft = {nullptr, nullptr};
                cde::ReadMem(reinterpret_cast<uintptr_t>(component) + kOffCraft, &craft,
                             sizeof craft);
                info.craft = craft.ctrl ? craft.ptr : nullptr;
                out[written++] = info;
            }
        }
        if (parts_dtor_) parts_dtor_(&arr);
        return written;
    }

    int Bodies(BodyInfo* out, int max) {
        if (!out || max <= 0 || !Init() || !strategic_ || !get_bodies_ ||
            !body_pos_)
            return 0;
        detail::DarrayRaw arr = {nullptr, nullptr, 0, 0};
        get_bodies_(strategic_, &arr);
        int written = 0;
        if (arr.size > 0 && arr.size < 8192 && arr.data) {
            const detail::WeakPtrRaw* items = static_cast<const detail::WeakPtrRaw*>(arr.data);
            for (int i = 0; i < arr.size && written < max; ++i) {
                void* body = items[i].ptr;
                if (!body) continue;
                BodyInfo info = {body, {0, 0, 0}, 0.0, 0.0, false};
                body_pos_(body, &info.pos);
                if (!Finite(info.pos)) continue;
                if (body_radius_) info.radius_km = body_radius_(body);
                if (body_mu_) info.mu_km3_s2 = body_mu_(body);
                if (body_luminous_) info.luminous = body_luminous_(body);
                out[written++] = info;
            }
        }
        if (darray_dtor_) darray_dtor_(&arr);
        return written;
    }

private:
    Runtime() = default;
    static bool Finite(const Vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    bool ready_ = false;
    void* gm_ = nullptr;
    void* strategic_ = nullptr;
    void* active_ = nullptr;
    void* camera_ = nullptr;
    detail::FnGetGameManager get_gm_ = nullptr;
    detail::FnGetManager get_manager_ = nullptr;
    detail::FnGetVec3 cam_pos_ = nullptr;
    detail::FnGetVec3 cam_forward_ = nullptr;
    detail::FnGetVec3 cam_up_ = nullptr;
    detail::FnGetVec3 cam_right_ = nullptr;
    detail::FnGetDouble cam_fovy_ = nullptr;
    detail::FnGetDouble cam_aspect_ = nullptr;
    detail::FnGetVec3 cam_target_ = nullptr;
    detail::FnGetVec3 obj_pos_ = nullptr;
    detail::FnGetDarray get_fleets_ = nullptr;
    detail::FnGetDarray get_player_components_ = nullptr;
    detail::FnDarrayDtor parts_dtor_ = nullptr;
    void* tactical_ = nullptr;
    detail::FnGetVec3 fleet_pos_ = nullptr;
    detail::FnGetBool fleet_player_ = nullptr;
    detail::FnGetDarray get_bodies_ = nullptr;
    detail::FnGetVec3 body_pos_ = nullptr;
    detail::FnGetDouble body_radius_ = nullptr;
    detail::FnGetBool body_luminous_ = nullptr;
    detail::FnGetDouble body_mu_ = nullptr;
    detail::FnDarrayDtor darray_dtor_ = nullptr;
};

}  // namespace game
}  // namespace cde

#endif /* CDEGAME_HPP */
