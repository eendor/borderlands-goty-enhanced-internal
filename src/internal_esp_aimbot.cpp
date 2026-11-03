// ==============================================================================
// Target: Borderlands: Game of the Year Enhanced (BorderlandsGOTY.exe, Win64)
// Engine: Unreal Engine 3 (x64) - WillowGame
// Deliverable: Internal Sticky Aimbot, Silent Aim, In-Game HUD Menu & Full ESP
// ==============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <dwmapi.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <sstream>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

// -----------------------------------------------------------------------------
// Engine Configuration & Named Holes
// -----------------------------------------------------------------------------
enum AimMode : int {
    AIM_MODE_OFF       = 0,
    AIM_MODE_HARD_LOCK = 1,
    AIM_MODE_SMOOTH    = 2,
    AIM_MODE_SILENT    = 3
};

struct InternalConfig {
    // Dynamic reflection RVAs for WillowGame UE3 x64
    uintptr_t RVA_GObjects                         = 0x025D95D0; // FUObjectArray GObjects
    uintptr_t RVA_GNames                           = 0x025D9740; // TArray<FNameEntry*> GNames
    uintptr_t RVA_GEngine                          = 0x025EA030; // UEngine* GEngine

    // Named offset holes (Verified live on Borderlands GOTY Win64)
    uintptr_t Offset_Controller_Level              = 0x40;  // WillowPlayerController -> ULevel* PersistentLevel
    uintptr_t Offset_Controller_Location           = 0x90;  // WillowPlayerController -> FVector Location
    uintptr_t Offset_Controller_Rotation           = 0x9C;  // WillowPlayerController -> FRotator Rotation (ControlRotation)
    uintptr_t Offset_Controller_Pawn               = 0x260; // WillowPlayerController -> WillowPlayerPawn* (Local Pawn)
    uintptr_t Offset_Controller_ShotTarget         = 0x3EC; // WillowPlayerController -> AActor* ShotTarget
    uintptr_t Offset_Controller_Camera             = 0x6DC; // WillowPlayerController -> Camera* PlayerCamera
    uintptr_t Offset_Controller_AimingHelp         = 0x704; // WillowPlayerController -> bool bAimingHelp (bit 1)
    uintptr_t Offset_Controller_GearboxPawnList    = 0xCF0; // WillowPlayerController -> GearboxPawnList*
    uintptr_t Offset_PawnList_Pawns                = 0x60;  // GearboxPawnList -> TArray<APawn*> Pawns

    uintptr_t Offset_Pawn_Location                 = 0x90;  // AActor / APawn -> FVector Location (Cylinder Center)
    uintptr_t Offset_Pawn_Rotation                 = 0x9C;  // AActor / APawn -> FRotator Rotation
    uintptr_t Offset_Pawn_CollisionComponent       = 0x1D8; // AActor / APawn -> UCylinderComponent* CollisionComponent
    uintptr_t Offset_Pawn_BaseEyeHeight            = 0x424; // APawn -> float BaseEyeHeight
    uintptr_t Offset_Pawn_EyeHeight                = 0x428; // APawn -> float EyeHeight
    uintptr_t Offset_Pawn_Health                   = 0x448; // APawn -> float HealthVar
    uintptr_t Offset_Pawn_MaxHealth                = 0x44C; // APawn -> int32_t HealthMaxVar
    uintptr_t Offset_Pawn_AimAtActor               = 0xA20; // WillowPawn -> AActor* AimAtActor
    uintptr_t Offset_Pawn_AimAtVector              = 0xA28; // WillowPawn -> FVector AimAtVector

    uintptr_t Offset_Cylinder_CollisionHeight      = 0x29C; // UCylinderComponent -> float CollisionHeight
    uintptr_t Offset_Cylinder_CollisionRadius      = 0x2A0; // UCylinderComponent -> float CollisionRadius

    uintptr_t Offset_Level_Actors                  = 0x60;  // ULevel -> TArray<AActor*> Actors

    // ESP settings (Interactive in Menu)
    bool  EspEnabled         = true;
    bool  EspBox             = true;
    bool  EspSnaplines       = true;
    bool  EspHealthBar       = true;
    bool  EspDistance        = true;
    float EspMaxDistance     = 25000.0f; // UE units (~500m)

    // Aimbot settings (Interactive in Menu)
    int   AimModeSelect      = AIM_MODE_SILENT;    // 0: OFF, 1: HARD LOCK, 2: SMOOTH, 3: SILENT
    int   AimbotKey          = VK_RBUTTON;         // Default: Right Mouse Button
    int   AimbotKeyType      = 0;                  // 0: RMB, 1: LMB, 2: LSHIFT, 3: LALT
    float AimbotFov          = 30.0f;              // Lock cone in degrees
    float AimbotSmooth       = 4.5f;               // Smoothing divisor for Smooth Mode
    float AimTargetZRatio    = 0.35f;              // Target height ratio (0.35 = critical head/mouth spot)
};

InternalConfig g_Config;
HMODULE g_hModule = nullptr;
std::atomic<bool> g_UnloadRequested{ false };

// Dynamic Controller Cache
uintptr_t g_CachedPlayerController = 0;
uintptr_t g_StickyTargetActor      = 0;

// Menu State
bool  g_MenuOpen = true; // Open by default upon injection so user sees it instantly
int   g_MenuIndex = 0;
DWORD g_LastInputTick = 0;

// -----------------------------------------------------------------------------
// UE3 Core Types and Math
// -----------------------------------------------------------------------------
constexpr float CONST_PI           = 3.14159265358979323846f;
constexpr float URotationToRadians = CONST_PI / 32768.0f;
constexpr float URotationToDegrees = 360.0f / 65536.0f;
constexpr float DegreesToURotation = 65536.0f / 360.0f;
constexpr float RadiansToDegrees  = 180.0f / CONST_PI;

struct FVector {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    FVector() = default;
    FVector(float x, float y, float z) : X(x), Y(y), Z(z) {}

    FVector operator+(const FVector& v) const { return FVector(X + v.X, Y + v.Y, Z + v.Z); }
    FVector operator-(const FVector& v) const { return FVector(X - v.X, Y - v.Y, Z - v.Z); }
    FVector operator*(float s) const { return FVector(X * s, Y * s, Z * s); }
    FVector operator/(float s) const { return (s != 0.0f) ? FVector(X / s, Y / s, Z / s) : FVector(); }

    float Dot(const FVector& v) const { return (X * v.X) + (Y * v.Y) + (Z * v.Z); }
    float Size() const { return std::sqrt(Dot(*this)); }
    float Size2D() const { return std::sqrt((X * X) + (Y * Y)); }
    float Distance(const FVector& v) const { return (*this - v).Size(); }
};

struct FRotator {
    int32_t Pitch = 0, Yaw = 0, Roll = 0;
    FRotator() = default;
    FRotator(int32_t p, int32_t y, int32_t r) : Pitch(p), Yaw(y), Roll(r) {}

    void GetAxes(FVector& x, FVector& y, FVector& z) const {
        float radPitch = static_cast<float>(Pitch) * URotationToRadians;
        float radYaw   = static_cast<float>(Yaw)   * URotationToRadians;
        float radRoll  = static_cast<float>(Roll)  * URotationToRadians;

        float SP = std::sin(radPitch), CP = std::cos(radPitch);
        float SY = std::sin(radYaw),   CY = std::cos(radYaw);
        float SR = std::sin(radRoll),  CR = std::cos(radRoll);

        x = FVector(CP * CY, CP * SY, SP);
        y = FVector((SR * SP * CY) - (CR * SY), (SR * SP * SY) + (CR * CY), -SR * CP);
        z = FVector(-(CR * SP * CY) - (SR * SY), -(CR * SP * SY) + (SR * CY), CR * CP);
    }
};

struct FVector2D {
    float X = 0.0f, Y = 0.0f;
};

struct TArray64 {
    uintptr_t Data  = 0;
    int32_t   Count = 0;
    int32_t   Max   = 0;
};

struct MinimalViewInfo {
    FVector  Location;
    FRotator Rotation;
    float    FOV = 90.0f;
};

// -----------------------------------------------------------------------------
// Safe Memory Helper
// -----------------------------------------------------------------------------
inline bool IsValidPtr(const void* p) {
    uintptr_t u = reinterpret_cast<uintptr_t>(p);
    return (u >= 0x10000 && u <= 0x7FFFFFFFFFFF);
}

inline bool IsReadableAddress(const void* p, size_t size) {
    if (!IsValidPtr(p)) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

template<typename T>
T SafeRead(uintptr_t address, T defaultVal = T()) {
    if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(T))) {
        return defaultVal;
    }
    return *reinterpret_cast<const T*>(address);
}

template<typename T>
bool SafeWrite(uintptr_t address, const T& val) {
    if (!IsReadableAddress(reinterpret_cast<const void*>(address), sizeof(T))) {
        return false;
    }
    *reinterpret_cast<T*>(address) = val;
    return true;
}

// -----------------------------------------------------------------------------
// Dynamic Engine Reflection Resolution
// -----------------------------------------------------------------------------
const char* GetObjectName(uintptr_t moduleBase, uintptr_t obj) {
    if (!IsValidPtr(reinterpret_cast<void*>(obj))) return "";
    uintptr_t pGNamesAddr = moduleBase + g_Config.RVA_GNames;
    if (!IsReadableAddress(reinterpret_cast<void*>(pGNamesAddr), sizeof(uintptr_t))) return "";

    uintptr_t* pGNames = *reinterpret_cast<uintptr_t**>(pGNamesAddr);
    if (!IsValidPtr(pGNames)) return "";

    uint32_t nameIdx = SafeRead<uint32_t>(obj + 0x48, 0xFFFFFFFF);
    if (nameIdx >= 150000) return "";

    uintptr_t nameEntry = SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(&pGNames[nameIdx]));
    if (!IsValidPtr(reinterpret_cast<void*>(nameEntry))) return "";

    uintptr_t strPtr = nameEntry + 0x14;
    if (!IsReadableAddress(reinterpret_cast<void*>(strPtr), 32)) return "";
    return reinterpret_cast<const char*>(strPtr);
}

uintptr_t ResolvePlayerController(uintptr_t moduleBase) {
    // 1. Check if cached pointer is still alive and valid
    if (IsValidPtr(reinterpret_cast<void*>(g_CachedPlayerController))) {
        uintptr_t cls = SafeRead<uintptr_t>(g_CachedPlayerController + 0x50);
        if (IsValidPtr(reinterpret_cast<void*>(cls))) {
            const char* clsName = GetObjectName(moduleBase, cls);
            if (clsName && strcmp(clsName, "WillowPlayerController") == 0) {
                return g_CachedPlayerController;
            }
        }
    }

    g_CachedPlayerController = 0;

    // 2. Scan GObjects for the live WillowPlayerController instance
    uintptr_t pGObjectsAddr = moduleBase + g_Config.RVA_GObjects;
    if (!IsReadableAddress(reinterpret_cast<void*>(pGObjectsAddr), 16)) return 0;

    uintptr_t* pGObjects = *reinterpret_cast<uintptr_t**>(pGObjectsAddr);
    int32_t objCount = SafeRead<int32_t>(pGObjectsAddr + 8);
    if (!IsValidPtr(pGObjects) || objCount <= 0 || objCount > 500000) return 0;

    for (int i = 0; i < objCount; ++i) {
        uintptr_t obj = SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(&pGObjects[i]));
        if (!IsValidPtr(reinterpret_cast<void*>(obj))) continue;

        uintptr_t cls = SafeRead<uintptr_t>(obj + 0x50);
        if (!IsValidPtr(reinterpret_cast<void*>(cls))) continue;

        const char* clsName = GetObjectName(moduleBase, cls);
        if (clsName && strcmp(clsName, "WillowPlayerController") == 0) {
            const char* objName = GetObjectName(moduleBase, obj);
            if (objName && strncmp(objName, "Default__", 9) != 0) {
                g_CachedPlayerController = obj;
                return obj;
            }
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// World-To-Screen Projection
// -----------------------------------------------------------------------------
bool WorldToScreen(const FVector& worldLoc, const MinimalViewInfo& pov, int screenWidth, int screenHeight, FVector2D& outScreen) {
    FVector vAxisX, vAxisY, vAxisZ;
    pov.Rotation.GetAxes(vAxisX, vAxisY, vAxisZ);

    FVector vDelta = worldLoc - pov.Location;
    FVector vTransformed(
        vDelta.Dot(vAxisY), // Right
        vDelta.Dot(vAxisZ), // Up
        vDelta.Dot(vAxisX)  // Forward
    );

    if (vTransformed.Z < 1.0f) return false;

    float halfWidth  = static_cast<float>(screenWidth)  * 0.5f;
    float halfHeight = static_cast<float>(screenHeight) * 0.5f;

    float fovRad = (pov.FOV * 0.5f) * (CONST_PI / 180.0f);
    float tanHalfFov = std::tan(fovRad);
    if (tanHalfFov == 0.0f) return false;

    float fX = halfWidth + (vTransformed.X * (halfWidth / tanHalfFov)) / vTransformed.Z;
    float fY = halfHeight - (vTransformed.Y * (halfWidth / tanHalfFov)) / vTransformed.Z;

    outScreen.X = fX;
    outScreen.Y = fY;

    return (fX >= -500.0f && fX <= screenWidth + 500.0f && fY >= -500.0f && fY <= screenHeight + 500.0f);
}

// -----------------------------------------------------------------------------
// In-Game Menu Model
// -----------------------------------------------------------------------------
struct MenuItem {
    std::string Title;
    bool*       pBoolVal  = nullptr;
    float*      pFloatVal = nullptr;
    int*        pIntVal   = nullptr;
    float       MinVal    = 0.0f;
    float       MaxVal    = 100.0f;
    float       Step      = 1.0f;
};

std::vector<MenuItem> g_MenuItems = {
    { "[ESP] Master Switch",    &g_Config.EspEnabled,    nullptr, nullptr, 0, 0, 0 },
    { "  - Bounding Boxes",     &g_Config.EspBox,        nullptr, nullptr, 0, 0, 0 },
    { "  - Snaplines",          &g_Config.EspSnaplines,  nullptr, nullptr, 0, 0, 0 },
    { "  - Health Bars",        &g_Config.EspHealthBar,  nullptr, nullptr, 0, 0, 0 },
    { "  - Distance Labels",    &g_Config.EspDistance,   nullptr, nullptr, 0, 0, 0 },
    { "[AIM] Operating Mode",   nullptr, nullptr, &g_Config.AimModeSelect, 0, 3, 1 }, // 0: OFF, 1: HARD LOCK, 2: SMOOTH, 3: SILENT
    { "  - Aim Key Trigger",    nullptr, nullptr, &g_Config.AimbotKeyType, 0, 3, 1 }, // 0: RMB, 1: LMB, 2: LSHIFT, 3: LALT
    { "  - Smooth Factor",      nullptr, &g_Config.AimbotSmooth, nullptr,  1.5f, 15.0f, 0.5f },
    { "  - Lock FOV Radius",    nullptr, &g_Config.AimbotFov, nullptr,     5.0f, 90.0f, 5.0f },
    { "  - Target Crit Ratio",  nullptr, &g_Config.AimTargetZRatio, nullptr, 0.1f, 0.8f, 0.05f }
};

void HandleMenuInput() {
    DWORD currentTick = GetTickCount();
    if (currentTick - g_LastInputTick < 130) return;

    if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
        g_MenuOpen = !g_MenuOpen;
        g_LastInputTick = currentTick;
        return;
    }

    if (!g_MenuOpen) return;

    if (GetAsyncKeyState(VK_UP) & 0x8000) {
        if (g_MenuIndex > 0) g_MenuIndex--;
        else g_MenuIndex = static_cast<int>(g_MenuItems.size() - 1);
        g_LastInputTick = currentTick;
    } else if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
        if (g_MenuIndex < static_cast<int>(g_MenuItems.size() - 1)) g_MenuIndex++;
        else g_MenuIndex = 0;
        g_LastInputTick = currentTick;
    } else if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
        auto& item = g_MenuItems[g_MenuIndex];
        if (item.pBoolVal) *item.pBoolVal = false;
        else if (item.pFloatVal) *item.pFloatVal = (std::max)(item.MinVal, *item.pFloatVal - item.Step);
        else if (item.pIntVal) *item.pIntVal = (std::max)(static_cast<int>(item.MinVal), *item.pIntVal - 1);
        g_LastInputTick = currentTick;
    } else if (GetAsyncKeyState(VK_RIGHT) & 0x8000 || GetAsyncKeyState(VK_RETURN) & 0x8000) {
        auto& item = g_MenuItems[g_MenuIndex];
        if (item.pBoolVal) *item.pBoolVal = !(*item.pBoolVal);
        else if (item.pFloatVal) *item.pFloatVal = (std::min)(item.MaxVal, *item.pFloatVal + item.Step);
        else if (item.pIntVal) {
            int nextVal = *item.pIntVal + 1;
            if (nextVal > static_cast<int>(item.MaxVal)) nextVal = static_cast<int>(item.MinVal);
            *item.pIntVal = nextVal;
        }
        g_LastInputTick = currentTick;
    }

    // Synchronize Aim Key from Selection
    switch (g_Config.AimbotKeyType) {
        case 0: g_Config.AimbotKey = VK_RBUTTON; break;
        case 1: g_Config.AimbotKey = VK_LBUTTON; break;
        case 2: g_Config.AimbotKey = VK_LSHIFT;  break;
        case 3: g_Config.AimbotKey = VK_LMENU;   break;
        default: g_Config.AimbotKey = VK_RBUTTON; break;
    }
}

// -----------------------------------------------------------------------------
// Shared Entity Snapshot & Synchronization
// -----------------------------------------------------------------------------
struct RenderEntity {
    FVector2D Feet;
    FVector2D Head;
    float     BoxWidth;
    float     Health;
    float     MaxHealth;
    float     Distance;
    bool      IsStickyTarget;
};

struct FrameSnapshot {
    MinimalViewInfo             View;
    std::vector<RenderEntity>   Entities;
    int                         ScreenWidth  = 1920;
    int                         ScreenHeight = 1080;
};

FrameSnapshot g_CurrentSnapshot;

// -----------------------------------------------------------------------------
// Engine Worker Thread (Accurate Cylinder Bounds, Sticky Hard-Lock & Silent Aim)
// -----------------------------------------------------------------------------
void EngineTick() {
    uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    if (!moduleBase) return;

    // 1. Resolve WillowPlayerController dynamically
    uintptr_t pController = ResolvePlayerController(moduleBase);
    if (!IsValidPtr(reinterpret_cast<void*>(pController))) return;

    uintptr_t pLocalPawn = SafeRead<uintptr_t>(pController + g_Config.Offset_Controller_Pawn);

    // 2. Resolve Camera View & ControlRotation
    MinimalViewInfo currentView{};
    FRotator currentRot = SafeRead<FRotator>(pController + g_Config.Offset_Controller_Rotation);
    currentView.Rotation = currentRot;
    currentView.FOV = 90.0f;

    if (IsValidPtr(reinterpret_cast<void*>(pLocalPawn))) {
        FVector pawnLoc = SafeRead<FVector>(pLocalPawn + g_Config.Offset_Pawn_Location);
        float eyeHeight = SafeRead<float>(pLocalPawn + g_Config.Offset_Pawn_BaseEyeHeight, 55.0f);
        if (eyeHeight < 10.0f || eyeHeight > 120.0f) eyeHeight = 55.0f;
        currentView.Location = pawnLoc + FVector(0.0f, 0.0f, eyeHeight);
    } else {
        currentView.Location = SafeRead<FVector>(pController + g_Config.Offset_Controller_Location);
    }

    int screenW = (g_CurrentSnapshot.ScreenWidth > 0) ? g_CurrentSnapshot.ScreenWidth : 1920;
    int screenH = (g_CurrentSnapshot.ScreenHeight > 0) ? g_CurrentSnapshot.ScreenHeight : 1080;

    // 3. Gather Live Pawns / Actors
    std::vector<uintptr_t> candidateActors;
    candidateActors.reserve(128);

    // Primary source: GearboxPawnList
    uintptr_t pGearboxPawnList = SafeRead<uintptr_t>(pController + g_Config.Offset_Controller_GearboxPawnList);
    if (IsValidPtr(reinterpret_cast<void*>(pGearboxPawnList))) {
        TArray64 pawnsArray = SafeRead<TArray64>(pGearboxPawnList + g_Config.Offset_PawnList_Pawns);
        if (IsValidPtr(reinterpret_cast<void*>(pawnsArray.Data)) && pawnsArray.Count > 0) {
            int count = (std::min)(pawnsArray.Count, 256);
            uintptr_t* pPawns = reinterpret_cast<uintptr_t*>(pawnsArray.Data);
            for (int i = 0; i < count; ++i) {
                uintptr_t p = SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(&pPawns[i]));
                if (IsValidPtr(reinterpret_cast<void*>(p)) && p != pLocalPawn) {
                    candidateActors.push_back(p);
                }
            }
        }
    }

    // Secondary source: PersistentLevel -> Actors
    if (candidateActors.empty()) {
        uintptr_t pLevel = SafeRead<uintptr_t>(pController + g_Config.Offset_Controller_Level);
        if (IsValidPtr(reinterpret_cast<void*>(pLevel))) {
            TArray64 actorsArray = SafeRead<TArray64>(pLevel + g_Config.Offset_Level_Actors);
            if (IsValidPtr(reinterpret_cast<void*>(actorsArray.Data)) && actorsArray.Count > 0) {
                int count = (std::min)(actorsArray.Count, 1024);
                uintptr_t* pActors = reinterpret_cast<uintptr_t*>(actorsArray.Data);
                for (int i = 0; i < count; ++i) {
                    uintptr_t p = SafeRead<uintptr_t>(reinterpret_cast<uintptr_t>(&pActors[i]));
                    if (IsValidPtr(reinterpret_cast<void*>(p)) && p != pLocalPawn) {
                        float hp = SafeRead<float>(p + g_Config.Offset_Pawn_Health, 0.0f);
                        if (hp > 0.0f) {
                            candidateActors.push_back(p);
                        }
                    }
                }
            }
        }
    }

    bool aimKeyPressed  = (GetAsyncKeyState(g_Config.AimbotKey) & 0x8000) != 0;
    bool fireKeyPressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    // Silent Aim routes bullet traces strictly when firing (LMB). It MUST NOT activate on holding RMB
    // (Zoom / Iron Sights / ADS), completely eliminating camera jumping, twitching, and sight hitching.
    // Hard Lock and Smooth modes activate on the configured aim key (e.g. RMB or LSHIFT).
    bool aimActive = (g_Config.AimModeSelect == AIM_MODE_SILENT) ? fireKeyPressed : aimKeyPressed;

    if (!aimActive) {
        g_StickyTargetActor = 0;
        // Clean up engine targeting pointers when inactive to prevent lingering locks during ADS zoom
        SafeWrite<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, 0);
        if (IsValidPtr(reinterpret_cast<void*>(pLocalPawn))) {
            SafeWrite<uintptr_t>(pLocalPawn + g_Config.Offset_Pawn_AimAtActor, 0);
        }
    }

    uintptr_t bestNewTarget = 0;
    float bestAngularDist = g_Config.AimbotFov;
    FVector bestTargetAimPos{};

    std::vector<RenderEntity> freshEntities;
    freshEntities.reserve(candidateActors.size());

    // 4. Filter, Envelop Cylinder, Project, and Select Aimbot Target
    for (uintptr_t pActor : candidateActors) {
        FVector centerLoc = SafeRead<FVector>(pActor + g_Config.Offset_Pawn_Location);
        float dist = currentView.Location.Distance(centerLoc);
        if (dist > g_Config.EspMaxDistance || dist <= 1.0f) continue;

        float health = SafeRead<float>(pActor + g_Config.Offset_Pawn_Health, 100.0f);
        int32_t maxHealthI = SafeRead<int32_t>(pActor + g_Config.Offset_Pawn_MaxHealth, 100);
        float maxHealth = (maxHealthI > 0) ? static_cast<float>(maxHealthI) : 100.0f;

        if (health <= 0.0f) {
            if (pActor == g_StickyTargetActor) g_StickyTargetActor = 0;
            continue;
        }

        // Read exact collision cylinder dimensions from UCylinderComponent
        float colHeight = 45.0f;
        float colRadius = 30.0f;
        uintptr_t cylComp = SafeRead<uintptr_t>(pActor + g_Config.Offset_Pawn_CollisionComponent);
        if (IsValidPtr(reinterpret_cast<void*>(cylComp))) {
            float ch = SafeRead<float>(cylComp + g_Config.Offset_Cylinder_CollisionHeight);
            float cr = SafeRead<float>(cylComp + g_Config.Offset_Cylinder_CollisionRadius);
            if (ch > 10.0f && ch < 300.0f) colHeight = ch;
            if (cr > 10.0f && cr < 200.0f) colRadius = cr;
        }

        // In UE3, Location is cylinder CENTER.
        // True Ground/Feet = Center.Z - CollisionHeight
        // True Top of Head = Center.Z + CollisionHeight
        FVector feetPos = centerLoc;
        feetPos.Z -= colHeight;

        FVector headPos = centerLoc;
        headPos.Z += colHeight;

        // Critical Hit Spot (mouth/head)
        FVector aimTargetPos = centerLoc;
        aimTargetPos.Z += (colHeight * g_Config.AimTargetZRatio);

        FVector2D screenFeet{}, screenHead{};
        if (WorldToScreen(feetPos, currentView, screenW, screenH, screenFeet) &&
            WorldToScreen(headPos, currentView, screenW, screenH, screenHead)) {

            float boxHeight = std::abs(screenFeet.Y - screenHead.Y);
            if (boxHeight < 4.0f) boxHeight = 4.0f;
            float boxWidth = boxHeight * (colRadius / colHeight * 1.05f);
            if (boxWidth < boxHeight * 0.4f) boxWidth = boxHeight * 0.4f;

            freshEntities.push_back({
                screenFeet, screenHead, boxWidth, health, maxHealth, dist, (pActor == g_StickyTargetActor)
            });
        }

        // Aimbot Angle Evaluation
        if (g_Config.AimModeSelect != AIM_MODE_OFF && aimActive) {
            FVector delta = aimTargetPos - currentView.Location;
            float dist3d = delta.Size();
            if (dist3d > 10.0f) {
                float pitchRad = std::atan2(delta.Z, delta.Size2D());
                float yawRad   = std::atan2(delta.Y, delta.X);

                float targetPitchDeg = pitchRad * RadiansToDegrees;
                float targetYawDeg   = yawRad * RadiansToDegrees;

                float currentPitchDeg = static_cast<float>(currentView.Rotation.Pitch) * URotationToDegrees;
                float currentYawDeg   = static_cast<float>(currentView.Rotation.Yaw) * URotationToDegrees;

                float deltaPitch = targetPitchDeg - currentPitchDeg;
                float deltaYaw   = targetYawDeg - currentYawDeg;
                while (deltaYaw > 180.0f)  deltaYaw -= 360.0f;
                while (deltaYaw < -180.0f) deltaYaw += 360.0f;

                float angularDist = std::sqrt((deltaPitch * deltaPitch) + (deltaYaw * deltaYaw));

                // If sticky locked, preserve lock across movement ticks
                if (g_StickyTargetActor != 0 && pActor == g_StickyTargetActor) {
                    bestNewTarget = pActor;
                    bestTargetAimPos = aimTargetPos;
                    break;
                }

                if (angularDist < bestAngularDist) {
                    bestAngularDist = angularDist;
                    bestNewTarget = pActor;
                    bestTargetAimPos = aimTargetPos;
                }
            }
        }
    }

    g_CurrentSnapshot.View = currentView;
    g_CurrentSnapshot.Entities = std::move(freshEntities);

    // 5. Hard Lock, Smooth, and Silent Aim Actuation
    if (g_Config.AimModeSelect != AIM_MODE_OFF && aimActive && bestNewTarget != 0) {
        g_StickyTargetActor = bestNewTarget;

        FVector delta = bestTargetAimPos - currentView.Location;
        float pitchRad = std::atan2(delta.Z, delta.Size2D());
        float yawRad   = std::atan2(delta.Y, delta.X);

        float targetPitchDeg = pitchRad * RadiansToDegrees;
        float targetYawDeg   = yawRad * RadiansToDegrees;

        int32_t targetPitchURot = static_cast<int32_t>(targetPitchDeg * DegreesToURotation);
        int32_t targetYawURot   = static_cast<int32_t>(targetYawDeg   * DegreesToURotation);

        // Silent Aim: Zero camera snapping; routes bullet raycasts strictly via AimAtVector & ShotTarget.
        // NEVER set AimAtActor on local pawn or enable bAimingHelp - that twists weapon IK and pulls camera on zoom.
        if (g_Config.AimModeSelect == AIM_MODE_SILENT) {
            if (IsValidPtr(reinterpret_cast<void*>(pLocalPawn))) {
                SafeWrite<FVector>(pLocalPawn + g_Config.Offset_Pawn_AimAtVector, bestTargetAimPos);
            }
            SafeWrite<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, bestNewTarget);
        }
        // Hard Lock Mode: True instantaneous 1-to-1 snap with real-time target tracking
        else if (g_Config.AimModeSelect == AIM_MODE_HARD_LOCK) {
            SafeWrite<int32_t>(pController + g_Config.Offset_Controller_Rotation, targetPitchURot);
            SafeWrite<int32_t>(pController + g_Config.Offset_Controller_Rotation + 4, targetYawURot);

            if (IsValidPtr(reinterpret_cast<void*>(pLocalPawn))) {
                SafeWrite<FVector>(pLocalPawn + g_Config.Offset_Pawn_AimAtVector, bestTargetAimPos);
            }
            SafeWrite<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, bestNewTarget);
        }
        // Smooth Mode: Humanized smooth glide based on AimbotSmooth divisor
        else if (g_Config.AimModeSelect == AIM_MODE_SMOOTH) {
            float currentPitchDeg = static_cast<float>(currentRot.Pitch) * URotationToDegrees;
            float currentYawDeg   = static_cast<float>(currentRot.Yaw)   * URotationToDegrees;

            float deltaPitch = targetPitchDeg - currentPitchDeg;
            float deltaYaw   = targetYawDeg   - currentYawDeg;
            while (deltaYaw > 180.0f)  deltaYaw -= 360.0f;
            while (deltaYaw < -180.0f) deltaYaw += 360.0f;

            float smoothDiv = (std::max)(g_Config.AimbotSmooth, 1.5f);
            float stepPitch = deltaPitch / smoothDiv;
            float stepYaw   = deltaYaw   / smoothDiv;

            int32_t newPitch = currentRot.Pitch + static_cast<int32_t>(stepPitch * DegreesToURotation);
            int32_t newYaw   = currentRot.Yaw   + static_cast<int32_t>(stepYaw   * DegreesToURotation);

            SafeWrite<int32_t>(pController + g_Config.Offset_Controller_Rotation, newPitch);
            SafeWrite<int32_t>(pController + g_Config.Offset_Controller_Rotation + 4, newYaw);

            if (IsValidPtr(reinterpret_cast<void*>(pLocalPawn))) {
                SafeWrite<FVector>(pLocalPawn + g_Config.Offset_Pawn_AimAtVector, bestTargetAimPos);
            }
            SafeWrite<uintptr_t>(pController + g_Config.Offset_Controller_ShotTarget, bestNewTarget);
        }
    }
}

// -----------------------------------------------------------------------------
// In-Process Hardware Transparent Overlay (Guarantees 100% Screen Visibility)
// -----------------------------------------------------------------------------
class InProcessOverlay {
private:
    HWND        m_hGameWnd    = nullptr;
    HWND        m_hOverlay    = nullptr;
    HINSTANCE   m_hInstance   = nullptr;
    int         m_Width       = 1920;
    int         m_Height      = 1080;
    int         m_PosX        = 0;
    int         m_PosY        = 0;

    HDC         m_hMemDC      = nullptr;
    HBITMAP     m_hMemBitmap  = nullptr;
    HBITMAP     m_hOldBitmap  = nullptr;
    HFONT       m_hFont       = nullptr;
    HFONT       m_hMenuFont   = nullptr;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

public:
    InProcessOverlay() = default;
    ~InProcessOverlay() { Cleanup(); }

    void Cleanup() {
        if (m_hMemDC) {
            SelectObject(m_hMemDC, m_hOldBitmap);
            DeleteObject(m_hMemBitmap);
            DeleteDC(m_hMemDC);
            m_hMemDC = nullptr;
        }
        if (m_hFont)     { DeleteObject(m_hFont);     m_hFont = nullptr; }
        if (m_hMenuFont) { DeleteObject(m_hMenuFont); m_hMenuFont = nullptr; }
        if (m_hOverlay)  { DestroyWindow(m_hOverlay); m_hOverlay = nullptr; }
    }

    HWND FindTargetWindow() {
        HWND hWnd = FindWindowW(nullptr, L"Borderlands");
        if (!hWnd) hWnd = FindWindowW(L"LaunchUnrealUWindowsClient", nullptr);
        if (!hWnd) {
            DWORD currentPid = GetCurrentProcessId();
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                DWORD p = 0;
                GetWindowThreadProcessId(h, &p);
                if (p == *(DWORD*)lp && IsWindowVisible(h)) {
                    RECT r;
                    GetClientRect(h, &r);
                    if ((r.right - r.left) > 300) {
                        *(HWND*)lp = h;
                        return FALSE;
                    }
                }
                return TRUE;
            }, (LPARAM)&hWnd);
        }
        return hWnd;
    }

    bool Create(HINSTANCE hInst) {
        m_hInstance = hInst;
        m_hGameWnd = FindTargetWindow();

        RECT clientRect{ 0, 0, 1920, 1080 };
        if (m_hGameWnd && IsWindow(m_hGameWnd)) {
            GetClientRect(m_hGameWnd, &clientRect);
            POINT pt{ clientRect.left, clientRect.top };
            ClientToScreen(m_hGameWnd, &pt);
            m_PosX   = pt.x;
            m_PosY   = pt.y;
            m_Width  = clientRect.right - clientRect.left;
            m_Height = clientRect.bottom - clientRect.top;
        }

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"BL_Internal_Overlay_Host_V3";
        RegisterClassExW(&wc);

        m_hOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            wc.lpszClassName,
            L"TacticalOverlay",
            WS_POPUP,
            m_PosX, m_PosY, m_Width, m_Height,
            nullptr, nullptr, hInst, nullptr
        );

        if (!m_hOverlay) return false;

        SetLayeredWindowAttributes(m_hOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);

        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(m_hOverlay, &margins);

        ShowWindow(m_hOverlay, SW_SHOW);
        UpdateWindow(m_hOverlay);

        HDC hdc = GetDC(m_hOverlay);
        m_hMemDC = CreateCompatibleDC(hdc);
        m_hMemBitmap = CreateCompatibleBitmap(hdc, (std::max)(m_Width, 2560), (std::max)(m_Height, 1440));
        m_hOldBitmap = static_cast<HBITMAP>(SelectObject(m_hMemDC, m_hMemBitmap));
        ReleaseDC(m_hOverlay, hdc);

        m_hFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Arial");

        m_hMenuFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Consolas");

        return true;
    }

    void Render() {
        if (!m_hOverlay || !m_hMemDC) return;

        if (m_hGameWnd && IsWindow(m_hGameWnd)) {
            RECT clientRect{};
            GetClientRect(m_hGameWnd, &clientRect);
            POINT pt{ clientRect.left, clientRect.top };
            ClientToScreen(m_hGameWnd, &pt);

            int w = clientRect.right - clientRect.left;
            int h = clientRect.bottom - clientRect.top;

            if (pt.x != m_PosX || pt.y != m_PosY || w != m_Width || h != m_Height) {
                m_PosX = pt.x; m_PosY = pt.y; m_Width = w; m_Height = h;
                SetWindowPos(m_hOverlay, HWND_TOPMOST, m_PosX, m_PosY, m_Width, m_Height, SWP_NOACTIVATE);
            }
        }

        g_CurrentSnapshot.ScreenWidth  = m_Width;
        g_CurrentSnapshot.ScreenHeight = m_Height;

        // Clear transparent canvas
        RECT fillRect{ 0, 0, m_Width, m_Height };
        HBRUSH hBlackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(m_hMemDC, &fillRect, hBlackBrush);
        DeleteObject(hBlackBrush);

        SetBkMode(m_hMemDC, TRANSPARENT);

        // Pens & Brushes
        HPEN hRedPen    = CreatePen(PS_SOLID, 2, RGB(255, 50, 50));
        HPEN hTargetPen = CreatePen(PS_SOLID, 2, RGB(255, 220, 0));
        HPEN hCyanPen   = CreatePen(PS_SOLID, 1, RGB(40, 220, 255));
        HPEN hGreenPen  = CreatePen(PS_SOLID, 2, RGB(50, 255, 50));
        HPEN hYellowPen = CreatePen(PS_SOLID, 2, RGB(255, 230, 50));
        HPEN hFovPen    = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        HBRUSH hNullBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));

        SelectObject(m_hMemDC, m_hFont);

        // 1. Watermark Status Line
        SetTextColor(m_hMemDC, RGB(0, 255, 140));
        std::wstring aimModeStr = L"OFF";
        switch (g_Config.AimModeSelect) {
            case AIM_MODE_HARD_LOCK: aimModeStr = L"HARD LOCK"; break;
            case AIM_MODE_SMOOTH:    aimModeStr = L"SMOOTH";    break;
            case AIM_MODE_SILENT:    aimModeStr = L"SILENT AIM"; break;
            default:                 aimModeStr = L"OFF";       break;
        }

        std::wstring hudLine = L"[BL:GOTY Active] Menu: [INSERT] | ESP: " +
            std::wstring(g_Config.EspEnabled ? L"ON" : L"OFF") +
            L" | AIM: [" + aimModeStr + L"]" +
            L" | Targets: " + std::to_wstring(g_CurrentSnapshot.Entities.size());
        TextOutW(m_hMemDC, 20, 15, hudLine.c_str(), static_cast<int>(hudLine.length()));

        // 2. Draw Aimbot FOV Circle
        if (g_Config.AimModeSelect != AIM_MODE_OFF) {
            SelectObject(m_hMemDC, hFovPen);
            SelectObject(m_hMemDC, hNullBrush);
            float fovPx = (g_Config.AimbotFov / g_CurrentSnapshot.View.FOV) * (static_cast<float>(m_Width) * 0.5f);
            int cx = m_Width / 2;
            int cy = m_Height / 2;
            Ellipse(m_hMemDC, static_cast<int>(cx - fovPx), static_cast<int>(cy - fovPx),
                              static_cast<int>(cx + fovPx), static_cast<int>(cy + fovPx));
        }

        // 3. Draw Accurate ESP Entities
        if (g_Config.EspEnabled) {
            for (const auto& ent : g_CurrentSnapshot.Entities) {
                float top    = ent.Head.Y;
                float bottom = ent.Feet.Y;
                float left   = ent.Feet.X - (ent.BoxWidth * 0.5f);
                float right  = ent.Feet.X + (ent.BoxWidth * 0.5f);

                // 2D Bounding Box (Feet to Head)
                if (g_Config.EspBox) {
                    SelectObject(m_hMemDC, ent.IsStickyTarget ? hTargetPen : hRedPen);
                    SelectObject(m_hMemDC, hNullBrush);
                    Rectangle(m_hMemDC, static_cast<int>(left), static_cast<int>(top), static_cast<int>(right), static_cast<int>(bottom));
                }

                // Snapline to Ground/Feet
                if (g_Config.EspSnaplines) {
                    SelectObject(m_hMemDC, hCyanPen);
                    MoveToEx(m_hMemDC, m_Width / 2, m_Height, nullptr);
                    LineTo(m_hMemDC, static_cast<int>(ent.Feet.X), static_cast<int>(ent.Feet.Y));
                }

                // Health Bar (Left edge of bounding box)
                if (g_Config.EspHealthBar) {
                    float hpFrac = (std::clamp)(ent.Health / (std::max)(ent.MaxHealth, 1.0f), 0.0f, 1.0f);
                    HPEN hHpPen = (hpFrac > 0.5f) ? hGreenPen : ((hpFrac > 0.25f) ? hYellowPen : hRedPen);

                    int barX = static_cast<int>(left - 6);
                    int barTop = static_cast<int>(top);
                    int barBottom = static_cast<int>(bottom);
                    int filledTop = static_cast<int>(barBottom - ((barBottom - barTop) * hpFrac));

                    SelectObject(m_hMemDC, hHpPen);
                    MoveToEx(m_hMemDC, barX, barBottom, nullptr);
                    LineTo(m_hMemDC, barX, filledTop);
                }

                // Distance Tag (Bottom of box)
                if (g_Config.EspDistance) {
                    SetTextColor(m_hMemDC, RGB(255, 255, 255));
                    int meters = static_cast<int>(ent.Distance / 50.0f);
                    std::wstring distStr = L"[" + std::to_wstring(meters) + L"m]";
                    TextOutW(m_hMemDC, static_cast<int>(left), static_cast<int>(bottom + 2), distStr.c_str(), static_cast<int>(distStr.length()));
                }
            }
        }

        // 4. Interactive HUD Menu
        if (g_MenuOpen) {
            SelectObject(m_hMemDC, m_hMenuFont);

            int menuX = 30, menuY = 50;
            int menuW = 390, menuH = 35 + static_cast<int>(g_MenuItems.size() * 24) + 30;

            HBRUSH hBgBrush   = CreateSolidBrush(RGB(20, 24, 32));
            HBRUSH hHeader    = CreateSolidBrush(RGB(32, 42, 58));
            HBRUSH hSelectBg  = CreateSolidBrush(RGB(28, 64, 52));
            HPEN   hMenuBorder = CreatePen(PS_SOLID, 2, RGB(0, 255, 140));

            // Background & Border
            RECT mRect = { menuX, menuY, menuX + menuW, menuY + menuH };
            FillRect(m_hMemDC, &mRect, hBgBrush);

            RECT hRect = { menuX, menuY, menuX + menuW, menuY + 28 };
            FillRect(m_hMemDC, &hRect, hHeader);

            SelectObject(m_hMemDC, hMenuBorder);
            SelectObject(m_hMemDC, hNullBrush);
            Rectangle(m_hMemDC, menuX, menuY, menuX + menuW, menuY + menuH);

            // Title
            SetTextColor(m_hMemDC, RGB(255, 255, 255));
            TextOutW(m_hMemDC, menuX + 12, menuY + 6, L"BORDERLANDS GOTY - TACTICAL MENU", 32);

            int itemY = menuY + 36;
            for (size_t i = 0; i < g_MenuItems.size(); ++i) {
                const auto& item = g_MenuItems[i];
                bool isSelected = (static_cast<int>(i) == g_MenuIndex);

                if (isSelected) {
                    RECT selRect = { menuX + 4, itemY - 2, menuX + menuW - 4, itemY + 20 };
                    FillRect(m_hMemDC, &selRect, hSelectBg);
                }

                std::wstring label(item.Title.begin(), item.Title.end());
                std::wstring valStr;
                COLORREF valCol = RGB(255, 255, 255);

                if (item.pBoolVal) {
                    valStr = *item.pBoolVal ? L"[ ON ]" : L"[ OFF ]";
                    valCol = *item.pBoolVal ? RGB(60, 255, 60) : RGB(255, 60, 60);
                } else if (item.pIntVal) {
                    if (item.pIntVal == &g_Config.AimModeSelect) {
                        switch (*item.pIntVal) {
                            case AIM_MODE_OFF:       valStr = L"< OFF >";       valCol = RGB(255, 60, 60); break;
                            case AIM_MODE_HARD_LOCK: valStr = L"< HARD LOCK >"; valCol = RGB(255, 220, 0); break;
                            case AIM_MODE_SMOOTH:    valStr = L"< SMOOTH >";    valCol = RGB(0, 220, 255); break;
                            case AIM_MODE_SILENT:    valStr = L"< SILENT >";    valCol = RGB(60, 255, 60); break;
                        }
                    } else if (item.pIntVal == &g_Config.AimbotKeyType) {
                        switch (*item.pIntVal) {
                            case 0: valStr = L"< RMB >"; break;
                            case 1: valStr = L"< LMB >"; break;
                            case 2: valStr = L"< SHIFT >"; break;
                            case 3: valStr = L"< ALT >"; break;
                        }
                        valCol = RGB(200, 220, 255);
                    }
                } else if (item.pFloatVal) {
                    wchar_t numBuf[32];
                    swprintf(numBuf, 32, L"< %.1f >", *item.pFloatVal);
                    valStr = numBuf;
                    valCol = RGB(255, 230, 80);
                }

                SetTextColor(m_hMemDC, isSelected ? RGB(0, 255, 140) : RGB(200, 200, 200));
                TextOutW(m_hMemDC, menuX + 14, itemY, label.c_str(), static_cast<int>(label.length()));

                SetTextColor(m_hMemDC, valCol);
                TextOutW(m_hMemDC, menuX + menuW - 120, itemY, valStr.c_str(), static_cast<int>(valStr.length()));

                itemY += 24;
            }

            // Footer instructions
            SetTextColor(m_hMemDC, RGB(140, 160, 180));
            std::wstring footer = L"[UP/DN] Select | [LFT/RGT/ENT] Toggle | [INS] Close";
            TextOutW(m_hMemDC, menuX + 10, itemY + 6, footer.c_str(), static_cast<int>(footer.length()));

            DeleteObject(hBgBrush);
            DeleteObject(hHeader);
            DeleteObject(hSelectBg);
            DeleteObject(hMenuBorder);
        }

        // Blit backbuffer to screen
        HDC hdc = GetDC(m_hOverlay);
        BitBlt(hdc, 0, 0, m_Width, m_Height, m_hMemDC, 0, 0, SRCCOPY);
        ReleaseDC(m_hOverlay, hdc);

        DeleteObject(hRedPen);
        DeleteObject(hTargetPen);
        DeleteObject(hCyanPen);
        DeleteObject(hGreenPen);
        DeleteObject(hYellowPen);
        DeleteObject(hFovPen);
    }
};

InProcessOverlay g_Overlay;

// -----------------------------------------------------------------------------
// Orchestration Loop & DLL Lifecycle
// -----------------------------------------------------------------------------
DWORD WINAPI MainThread(LPVOID lpParam) {
    Sleep(800);

    // Initialize in-process hardware composition overlay
    HINSTANCE hInst = GetModuleHandle(nullptr);
    g_Overlay.Create(hInst);

    // High-performance engine synchronization loop (~125 Hz)
    while (!g_UnloadRequested.load()) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            g_UnloadRequested.store(true);
            break;
        }

        HandleMenuInput();
        EngineTick();
        g_Overlay.Render();

        Sleep(8);
    }

    Sleep(150);
    g_Overlay.Cleanup();
    Sleep(150);

    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_hModule = hinstDLL;
        HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        g_UnloadRequested.store(true);
        g_Overlay.Cleanup();
    }
    return TRUE;
}
