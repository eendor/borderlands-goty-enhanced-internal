#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "config.h"

// ---------------------------------------------------------------------------
// MenuItem
// ---------------------------------------------------------------------------
struct MenuItem {
    std::string Title;
    bool*  pBoolVal  = nullptr;
    float* pFloatVal = nullptr;
    int*   pIntVal   = nullptr;
    float  MinVal    = 0.0f;
    float  MaxVal    = 0.0f;
    float  Step      = 1.0f;

    static MenuItem Toggle(std::string title, bool* v) {
        MenuItem m; m.Title = std::move(title); m.pBoolVal = v; return m;
    }
    // Integer choice in [0, count), wraps both directions.
    static MenuItem Choice(std::string title, int* v, int count) {
        MenuItem m; m.Title = std::move(title); m.pIntVal = v;
        m.MinVal = 0.0f; m.MaxVal = static_cast<float>(count - 1); return m;
    }
    static MenuItem Slider(std::string title, float* v, float minV, float maxV, float step) {
        MenuItem m; m.Title = std::move(title); m.pFloatVal = v;
        m.MinVal = minV; m.MaxVal = maxV; m.Step = step; return m;
    }

    // LEFT = -1, RIGHT = +1
    void Adjust(int dir) {
        if (pBoolVal) {
            *pBoolVal = dir > 0;                                   // left = OFF, right = ON
        } else if (pIntVal) {
            const int lo = static_cast<int>(MinVal), hi = static_cast<int>(MaxVal);
            const int span = hi - lo + 1;
            if (span > 0) *pIntVal = lo + (((*pIntVal - lo + dir) % span) + span) % span;
        } else if (pFloatVal) {
            *pFloatVal = SnapToStep(*pFloatVal + dir * Step);
        }
    }

    // ENTER
    void Activate() {
        if (pBoolVal)     *pBoolVal = !*pBoolVal;
        else if (pIntVal) Adjust(+1);
    }

private:
    // Keeps repeated 0.05 steps from drifting to 0.35000002 etc.
    float SnapToStep(float v) const {
        if (Step > 0.0f) v = MinVal + std::round((v - MinVal) / Step) * Step;
        return (std::clamp)(v, MinVal, MaxVal);
    }
};

// ---------------------------------------------------------------------------
// Menu state
// ---------------------------------------------------------------------------
inline bool g_MenuOpen  = true;   // open on inject so it's visible immediately
inline int  g_MenuIndex = 0;

inline std::vector<MenuItem> g_MenuItems = {
    MenuItem::Toggle("[ESP] Master Switch",   &g_Config.EspEnabled),
    MenuItem::Toggle("  - Bounding Boxes",    &g_Config.EspBox),
    MenuItem::Toggle("  - Snaplines",         &g_Config.EspSnaplines),
    MenuItem::Toggle("  - Health Bars",       &g_Config.EspHealthBar),
    MenuItem::Toggle("  - Distance Labels",   &g_Config.EspDistance),
    MenuItem::Choice("[AIM] Operating Mode",  &g_Config.AimModeSelect, AIM_MODE_COUNT),
    MenuItem::Choice("  - Aim Key Trigger",   &g_Config.AimbotKeyType, AIM_KEY_COUNT),
    MenuItem::Slider("  - Smooth Factor",     &g_Config.AimbotSmooth,    1.5f, 15.0f, 0.5f),
    MenuItem::Slider("  - Lock FOV Radius",   &g_Config.AimbotFov,       5.0f, 90.0f, 5.0f),
    MenuItem::Slider("  - Target Crit Ratio", &g_Config.AimTargetZRatio, 0.1f,  0.8f, 0.05f),
};

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
inline bool IsGameForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Edge-triggered keys with optional hold-to-repeat. Replaces the single
// 130 ms global cooldown, under which a held INSERT flickered the menu
// open/closed and a quick tap could be missed entirely.
class KeyInput {
public:
    bool Pressed(int vk, ULONGLONG now) { return Poll(vk, now, false); }   // once per press
    bool Repeat (int vk, ULONGLONG now) { return Poll(vk, now, true);  }   // press + auto-repeat when held

    void Reset() { for (auto& s : m_Keys) s = {}; }

private:
    static constexpr ULONGLONG kInitialDelayMs = 350;
    static constexpr ULONGLONG kRepeatMs       = 90;

    struct State { bool Down = false; ULONGLONG NextRepeat = 0; };
    State m_Keys[256];

    bool Poll(int vk, ULONGLONG now, bool repeat) {
        State& s = m_Keys[vk & 0xFF];
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool fire = false;

        if (down && !s.Down) {
            fire = true;
            s.NextRepeat = now + kInitialDelayMs;
        } else if (down && repeat && now >= s.NextRepeat) {
            fire = true;
            s.NextRepeat = now + kRepeatMs;
        }
        s.Down = down;
        return fire;
    }
};

inline void HandleMenuInput() {
    static KeyInput input;

    // Don't react to keys typed into other apps.
    if (!IsGameForeground()) { input.Reset(); return; }

    const ULONGLONG now = GetTickCount64();

    if (input.Pressed(VK_INSERT, now)) g_MenuOpen = !g_MenuOpen;
    if (!g_MenuOpen || g_MenuItems.empty()) return;

    const int count = static_cast<int>(g_MenuItems.size());
    g_MenuIndex = (std::clamp)(g_MenuIndex, 0, count - 1);

    if (input.Repeat(VK_UP, now))   g_MenuIndex = (g_MenuIndex - 1 + count) % count;
    if (input.Repeat(VK_DOWN, now)) g_MenuIndex = (g_MenuIndex + 1) % count;

    MenuItem& item = g_MenuItems[g_MenuIndex];
    if (input.Repeat(VK_LEFT, now))    item.Adjust(-1);
    if (input.Repeat(VK_RIGHT, now))   item.Adjust(+1);
    if (input.Pressed(VK_RETURN, now)) item.Activate();
}
