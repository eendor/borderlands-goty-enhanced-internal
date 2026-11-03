#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "config.h"
#include "ue_math.h"
#include "safe_memory.h"
#include "object_resolver.h"
#include "world_to_screen.h"
#include "render_snapshot.h"

// Set these from the render thread (swapchain / Present hook) whenever the
// backbuffer size changes. Previously the size was read back out of the
// snapshot, which nothing ever updated.
std::atomic<int> g_ScreenWidth{ 1920 };
std::atomic<int> g_ScreenHeight{ 1080 };

namespace {

using SafeMem::IsValidPtr;

// ---------------------------------------------------------------------------
// Tunables / fallbacks
// ---------------------------------------------------------------------------
constexpr float   kFallbackFov       = 90.0f;
constexpr float   kDefaultEyeHeight  = 55.0f;
constexpr float   kMinEyeHeight      = 10.0f;
constexpr float   kMaxEyeHeight      = 120.0f;
constexpr float   kDefaultColHeight  = 45.0f;
constexpr float   kDefaultColRadius  = 30.0f;
constexpr float   kMinAimDistance    = 10.0f;
constexpr float   kMinBoxHeight      = 4.0f;
constexpr int32_t kMaxPawnListActors = 256;
constexpr int32_t kMaxLevelActors    = 1024;

// ---------------------------------------------------------------------------
// Angle helpers (degrees)
// ---------------------------------------------------------------------------
struct AimAngles { float Pitch = 0.0f, Yaw = 0.0f; };

// Wraps into [-180, 180] in one step (replaces the while-loops).
inline float NormalizeDegrees(float a) { return std::remainder(a, 360.0f); }

inline AimAngles AnglesTo(const FVector& from, const FVector& to) {
    const FVector d = to - from;
    return { std::atan2(d.Z, d.Size2D()) * UEMath::RadiansToDegrees,
             std::atan2(d.Y, d.X)        * UEMath::RadiansToDegrees };
}

// PitchDeg()/YawDeg() normalize first. The old code converted raw Pitch, so
// looking below the horizon (Pitch stored as ~65000) produced a ~360° error.
inline AimAngles AnglesOf(const FRotator& r) { return { r.PitchDeg(), r.YawDeg() }; }

inline AimAngles DeltaAngles(const AimAngles& target, const AimAngles& current) {
    return { NormalizeDegrees(target.Pitch - current.Pitch),
             NormalizeDegrees(target.Yaw   - current.Yaw) };
}

inline float AngularDistance(const AimAngles& d) { return std::hypot(d.Pitch, d.Yaw); }

// ---------------------------------------------------------------------------
// Data gathering
// ---------------------------------------------------------------------------
inline uintptr_t ModuleBase() {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return base;
}

MinimalViewInfo BuildView(uintptr_t pController, uintptr_t pLocalPawn, FRotator& outControlRot) {
    MinimalViewInfo view;
    outControlRot = SafeMem::Read<FRotator>(pController + g_Config.Offset_Controller_Rotation);
    view.Rotation = outControlRot;
    view.FOV      = kFallbackFov;

    FVector pawnLoc;
    if (pLocalPawn && SafeMem::TryRead(pLocalPawn + g_Config.Offset_Pawn_Location, pawnLoc)) {
        float eye = SafeMem::Read<float>(pLocalPawn + g_Config.Offset_Pawn_BaseEyeHeight, kDefaultEyeHeight);
        if (!(eye >= kMinEyeHeight && eye <= kMaxEyeHeight)) eye = kDefaultEyeHeight;   // also catches NaN
        view.Location = pawnLoc + FVector(0.0f, 0.0f, eye);
    } else {
        view.Location = SafeMem::Read<FVector>(pController + g_Config.Offset_Controller_Location);
    }
    return view;
}

void AppendActorArray(uintptr_t arrayAddr, int32_t maxCount, uintptr_t exclude, std::vector<uintptr_t>& out) {
    TArray64 arr;
    if (!SafeMem::TryRead(arrayAddr, arr) || !arr.IsValid() || !IsValidPtr(arr.Data)) return;

    const int32_t n = (std::min)(arr.Num(), maxCount);
    for (int32_t i = 0; i < n; ++i) {
        const uintptr_t p = ReadArrayPtr(arr, i);
        if (IsValidPtr(p) && p != exclude) out.push_back(p);
    }
}

void GatherCandidates(uintptr_t pController, uintptr_t pLocalPawn, std::vector<uintptr_t>& out) {
    out.clear();

    // Primary: GearboxPawnList
    const uintptr_t pawnList = SafeMem::Read<uintptr_t>(pController + g_Config.Offset_Controller_GearboxPawnList);
    if (IsValidPtr(pawnList))
        AppendActorArray(pawnList + g_Config.Offset_PawnList_Pawns, kMaxPawnListActors, pLocalPawn, out);
    if (!out.empty()) return;

    // Fallback: PersistentLevel->Actors (health filtering happens in ReadActor)
    const uintptr_t level = SafeMem::Read<uintptr_t>(pController + g_Config.Offset_Controller_Level);
    if (IsValidPtr(level))
        AppendActorArray(level + g_Config.Offset_Level_Actors, kMaxLevelActors, pLocalPawn, out);
}

struct ActorInfo {
    uintptr_t Ptr       = 0;
    FVector   Center;
    float     Distance  = 0.0f;
    float     Health    = 0.0f;
    float     MaxHealth = 100.0f;
    float     ColHeight = kDefaultColHeight;
    float     ColRadius = kDefaultColRadius;
};

bool ReadActor(uintptr_t actor, const FVector& viewLoc, ActorInfo& out) {
    out = {};
    out.Ptr = actor;

    if (!SafeMem::TryRead(actor + g_Config.Offset_Pawn_Location, out.Center)) return false;

    out.Distance = viewLoc.Distance(out.Center);
    if (out.Distance <= 1.0f || out.Distance > g_Config.EspMaxDistance) return false;

    // A failed health read is now "skip", not "assume 100 HP and draw it".
    if (!SafeMem::TryRead(actor + g_Config.Offset_Pawn_Health, out.Health) || !(out.Health > 0.0f))
        return false;

    const int32_t maxHp = SafeMem::Read<int32_t>(actor + g_Config.Offset_Pawn_MaxHealth, 0);
    if (maxHp > 0) out.MaxHealth = static_cast<float>(maxHp);

    // UCylinderComponent: Location is the cylinder centre in UE3.
    const uintptr_t cyl = SafeMem::Read<uintptr_t>(actor + g_Config.Offset_Pawn_CollisionComponent);
    if (IsValidPtr(cyl)) {
        const float h = SafeMem::Read<float>(cyl + g_Config.Offset_Cylinder_CollisionHeight);
        const float r = SafeMem::Read<float>(cyl + g_Config.Offset_Cylinder_CollisionRadius);
        if (h > 10.0f && h < 300.0f) out.ColHeight = h;
        if (r > 10.0f && r < 200.0f) out.ColRadius = r;
    }
    return true;
}

bool BuildRenderEntity(const ActorInfo& a, const ViewProjector& proj, RenderEntity& out) {
    FVector feet = a.Center; feet.Z -= a.ColHeight;
    FVector head = a.Center; head.Z += a.ColHeight;

    FVector2D sFeet, sHead;
    if (!proj.ProjectVisible(feet, sFeet) || !proj.ProjectVisible(head, sHead)) return false;

    const float boxH = (std::max)(std::abs(sFeet.Y - sHead.Y), kMinBoxHeight);
    const float boxW = (std::max)(boxH * (a.ColRadius / a.ColHeight * 1.05f), boxH * 0.4f);

    out = {};
    out.Feet      = sFeet;
    out.Head      = sHead;
    out.BoxWidth  = boxW;
    out.Health    = a.Health;
    out.MaxHealth = a.MaxHealth;
    out.Distance  = a.Distance;
    return true;
}

// ---------------------------------------------------------------------------
// Aim actuation
// ---------------------------------------------------------------------------
struct TargetChoice {
    uintptr_t Actor = 0;
    FVector   AimPos;
};

inline FVector AimPointOf(const ActorInfo& a) {
    FVector p = a.Center;
    p.Z += a.ColHeight * g_Config.AimTargetZRatio;
    return p;
}

bool IsAimTriggerHeld() {
    // Silent aim triggers on fire (LMB) only, so ADS/zoom never moves the camera.
    // Hard-lock / smooth trigger on the configured aim key.
    const int key = (g_Config.AimModeSelect == AIM_MODE_SILENT) ? VK_LBUTTON : g_Config.AimbotVirtualKey();
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void ClearAimState(uintptr_t pController, uintptr_t pLocalPawn) {
    SafeMem::Write<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, 0);
    if (pLocalPawn) SafeMem::Write<uintptr_t>(pLocalPawn + g_Config.Offset_Pawn_AimAtActor, 0);
}

void WriteAimTarget(uintptr_t pController, uintptr_t pLocalPawn, const TargetChoice& t) {
    // Never set AimAtActor / bAimingHelp here — it twists weapon IK and drags the camera on zoom.
    if (pLocalPawn) SafeMem::Write<FVector>(pLocalPawn + g_Config.Offset_Pawn_AimAtVector, t.AimPos);
    SafeMem::Write<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, t.Actor);
}

// Rotates relative to the current (raw, possibly accumulated) rotation, so
// yaw never jumps across the ±32768 wrap. divisor = 1 is an instant snap.
void RotateToward(uintptr_t pController, const FRotator& current, const AimAngles& delta, float divisor) {
    struct PitchYaw { int32_t Pitch, Yaw; };   // Roll left untouched
    const PitchYaw py{
        current.Pitch + static_cast<int32_t>(std::lround(delta.Pitch / divisor * UEMath::DegreesToURotation)),
        current.Yaw   + static_cast<int32_t>(std::lround(delta.Yaw   / divisor * UEMath::DegreesToURotation))
    };
    SafeMem::Write(pController + g_Config.Offset_Controller_Rotation, py);
}

void ApplyAim(uintptr_t pController, uintptr_t pLocalPawn, const MinimalViewInfo& view,
              const FRotator& controlRot, const TargetChoice& target) {
    const AimAngles delta = DeltaAngles(AnglesTo(view.Location, target.AimPos), AnglesOf(controlRot));

    switch (g_Config.AimModeSelect) {
        case AIM_MODE_SILENT:    break;                                       // no camera movement
        case AIM_MODE_HARD_LOCK: RotateToward(pController, controlRot, delta, 1.0f); break;
        case AIM_MODE_SMOOTH:    RotateToward(pController, controlRot, delta, (std::max)(g_Config.AimbotSmooth, 1.5f)); break;
        default:                 return;
    }
    WriteAimTarget(pController, pLocalPawn, target);
}

} // namespace

// ---------------------------------------------------------------------------
// EngineTick
// ---------------------------------------------------------------------------
void EngineTick() {
    const uintptr_t moduleBase = ModuleBase();
    if (!moduleBase) return;

    // 1. Controller / pawn
    const uintptr_t pController = ResolvePlayerController(moduleBase);
    if (!IsValidPtr(pController)) return;

    uintptr_t pLocalPawn = SafeMem::Read<uintptr_t>(pController + g_Config.Offset_Controller_Pawn);
    if (!IsValidPtr(pLocalPawn)) pLocalPawn = 0;

    // 2. Camera
    FRotator controlRot;
    const MinimalViewInfo view = BuildView(pController, pLocalPawn, controlRot);

    const int screenW = g_ScreenWidth.load(std::memory_order_relaxed);
    const int screenH = g_ScreenHeight.load(std::memory_order_relaxed);
    const ViewProjector proj(view, screenW, screenH);   // axes + FOV computed once per tick

    // 3. Candidates (buffers reused across ticks — no per-frame allocation)
    static std::vector<uintptr_t> candidates;
    static std::vector<uintptr_t> entityActors;
    GatherCandidates(pController, pLocalPawn, candidates);
    entityActors.clear();

    // Aim trigger state; clear engine aim pointers once on release instead of
    // every inactive tick (which overwrote the game's own aim assist constantly).
    const bool aimActive = (g_Config.AimModeSelect != AIM_MODE_OFF) && IsAimTriggerHeld();
    static bool s_WasAimActive = false;
    if (!aimActive) {
        if (s_WasAimActive) ClearAimState(pController, pLocalPawn);
        g_StickyTargetActor = 0;
    }
    s_WasAimActive = aimActive;

    // 4. Read, project, select
    FrameSnapshot snap = g_Snapshots.BeginFrame();
    snap.View         = view;
    snap.ScreenWidth  = screenW;
    snap.ScreenHeight = screenH;

    const AimAngles viewAngles = AnglesOf(controlRot);
    TargetChoice best, sticky;
    float bestAngle = g_Config.AimbotFov;

    for (const uintptr_t pActor : candidates) {
        ActorInfo actor;
        if (!ReadActor(pActor, view.Location, actor)) continue;

        RenderEntity ent;
        if (proj.IsValid() && BuildRenderEntity(actor, proj, ent)) {
            snap.Entities.push_back(ent);
            entityActors.push_back(pActor);
        }

        if (!aimActive) continue;

        const FVector aimPos = AimPointOf(actor);
        if (view.Location.Distance(aimPos) <= kMinAimDistance) continue;

        // Sticky lock: remember it, but keep looping so every entity still
        // reaches the ESP (the old `break` dropped the rest while locked).
        if (pActor == g_StickyTargetActor) {
            sticky = { pActor, aimPos };
            continue;
        }

        const float angle = AngularDistance(DeltaAngles(AnglesTo(view.Location, aimPos), viewAngles));
        if (angle < bestAngle) {
            bestAngle = angle;
            best = { pActor, aimPos };
        }
    }

    const TargetChoice target = sticky.Actor ? sticky : best;
    if (aimActive) g_StickyTargetActor = target.Actor;

    // Flag this tick's target (not last tick's) before sorting.
    if (g_StickyTargetActor) {
        for (size_t i = 0; i < snap.Entities.size(); ++i)
            if (entityActors[i] == g_StickyTargetActor)
                snap.Entities[i].SetFlag(RenderEntity::StickyTarget);
    }

    snap.SortByDistanceDesc();
    g_Snapshots.Publish(std::move(snap));

    // 5. Actuate
    if (aimActive && target.Actor)
        ApplyAim(pController, pLocalPawn, view, controlRot, target);
}

// ---------------------------------------------------------------------------
// EngineShutdown — call once before unloading. Releases any aim pointers we
// wrote so the game isn't left targeting an actor after the DLL is gone.
// ---------------------------------------------------------------------------
void EngineShutdown() {
    const uintptr_t pController = g_CachedPlayerController;
    if (IsValidPtr(pController)) {
        uintptr_t pLocalPawn = SafeMem::Read<uintptr_t>(pController + g_Config.Offset_Controller_Pawn);
        if (!IsValidPtr(pLocalPawn)) pLocalPawn = 0;
        ClearAimState(pController, pLocalPawn);
    }
    g_StickyTargetActor = 0;
    ResetObjectResolverCache();
    g_Snapshots.Reset();
}
