#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <cstdint>

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum AimMode : int {
    AIM_MODE_OFF       = 0,
    AIM_MODE_HARD_LOCK = 1,
    AIM_MODE_SMOOTH    = 2,
    AIM_MODE_SILENT    = 3,
    AIM_MODE_COUNT
};

enum AimKeyType : int {
    AIM_KEY_RMB   = 0,
    AIM_KEY_LMB   = 1,
    AIM_KEY_SHIFT = 2,
    AIM_KEY_ALT   = 3,
    AIM_KEY_COUNT
};

constexpr int AimKeyTypeToVK(int type) {
    switch (type) {
        case AIM_KEY_LMB:   return VK_LBUTTON;
        case AIM_KEY_SHIFT: return VK_LSHIFT;
        case AIM_KEY_ALT:   return VK_LMENU;
        default:            return VK_RBUTTON;
    }
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
struct InternalConfig {
    // --- Global RVAs (WillowGame UE3 x64) ---------------------------------
    uintptr_t RVA_GObjects                      = 0x025D95D0; // TArray<UObject*> GObjects
    uintptr_t RVA_GNames                        = 0x025D9740; // TArray<FNameEntry*> GNames
    uintptr_t RVA_GEngine                       = 0x025EA030; // UEngine* GEngine (unused)

    // --- WillowPlayerController -------------------------------------------
    uintptr_t Offset_Controller_Level           = 0x40;   // ULevel* PersistentLevel
    uintptr_t Offset_Controller_Location        = 0x90;   // FVector Location
    uintptr_t Offset_Controller_Rotation        = 0x9C;   // FRotator ControlRotation
    uintptr_t Offset_Controller_Pawn            = 0x260;  // WillowPlayerPawn* Pawn
    uintptr_t Offset_Controller_ShotTarget      = 0x3EC;  // AActor* ShotTarget
    uintptr_t Offset_Controller_Camera          = 0x6DC;  // Camera* PlayerCamera (unused)
    uintptr_t Offset_Controller_AimingHelp      = 0x704;  // bool bAimingHelp, bit 1 (unused)
    uintptr_t Offset_Controller_GearboxPawnList = 0xCF0;  // GearboxPawnList*

    // --- GearboxPawnList ----------------------------------------------------
    uintptr_t Offset_PawnList_Pawns             = 0x60;   // TArray<APawn*> Pawns

    // --- AActor / APawn / WillowPawn -----------------------------------------
    uintptr_t Offset_Pawn_Location              = 0x90;   // FVector Location (cylinder centre)
    uintptr_t Offset_Pawn_Rotation              = 0x9C;   // FRotator Rotation (unused)
    uintptr_t Offset_Pawn_CollisionComponent    = 0x1D8;  // UCylinderComponent*
    uintptr_t Offset_Pawn_BaseEyeHeight         = 0x424;  // float BaseEyeHeight
    uintptr_t Offset_Pawn_EyeHeight             = 0x428;  // float EyeHeight (unused)
    uintptr_t Offset_Pawn_Health                = 0x448;  // float HealthVar
    uintptr_t Offset_Pawn_MaxHealth             = 0x44C;  // int32 HealthMaxVar
    uintptr_t Offset_Pawn_AimAtActor            = 0xA20;  // AActor* AimAtActor
    uintptr_t Offset_Pawn_AimAtVector           = 0xA28;  // FVector AimAtVector

    // --- UCylinderComponent ---------------------------------------------------
    uintptr_t Offset_Cylinder_CollisionHeight   = 0x29C;  // float CollisionHeight
    uintptr_t Offset_Cylinder_CollisionRadius   = 0x2A0;  // float CollisionRadius

    // --- ULevel -------------------------------------------------------------
    uintptr_t Offset_Level_Actors               = 0x60;   // TArray<AActor*> Actors

    // --- ESP (menu-editable) ------------------------------------------------
    bool  EspEnabled      = true;
    bool  EspBox          = true;
    bool  EspSnaplines    = true;
    bool  EspHealthBar    = true;
    bool  EspDistance     = true;
    float EspMaxDistance  = 25000.0f;          // UE units (~500 m)

    // --- Aim (menu-editable) ------------------------------------------------
    int   AimModeSelect   = AIM_MODE_SILENT;
    int   AimbotKeyType   = AIM_KEY_RMB;
    float AimbotFov       = 30.0f;             // cone radius, degrees
    float AimbotSmooth    = 4.5f;              // smooth-mode divisor
    float AimTargetZRatio = 0.35f;             // 0 = centre, 1 = top of cylinder

    // Derived from AimbotKeyType on demand, so it can never fall out of sync
    // (previously it was only re-synced while the menu was open).
    int AimbotVirtualKey() const { return AimKeyTypeToVK(AimbotKeyType); }
};

// ---------------------------------------------------------------------------
// Globals — C++17 inline variables, so any header can include this without
// depending on which .cpp happens to define them.
// ---------------------------------------------------------------------------
inline InternalConfig     g_Config;
inline HMODULE            g_hModule                = nullptr;
inline std::atomic<bool>  g_UnloadRequested{ false };

inline uintptr_t          g_CachedPlayerController = 0;
inline uintptr_t          g_StickyTargetActor      = 0;
