#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <string>

#include "config.h"
#include "menu.h"
#include "render_snapshot.h"

extern std::atomic<int> g_ScreenWidth;
extern std::atomic<int> g_ScreenHeight;

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------
namespace OverlayStyle {
    // Color-keyed window: this exact color is see-through, so never draw with it.
    constexpr COLORREF Transparent = RGB(0, 0, 0);

    constexpr COLORREF Enemy       = RGB(255, 50, 50);
    constexpr COLORREF Target      = RGB(255, 220, 0);
    constexpr COLORREF Snapline    = RGB(40, 220, 255);
    constexpr COLORREF HpHigh      = RGB(50, 255, 50);
    constexpr COLORREF HpMid       = RGB(255, 230, 50);
    constexpr COLORREF HpLow       = RGB(255, 50, 50);
    constexpr COLORREF FovCircle   = RGB(180, 180, 180);
    constexpr COLORREF Accent      = RGB(0, 255, 140);
    constexpr COLORREF TextWhite   = RGB(255, 255, 255);
    constexpr COLORREF TextDim     = RGB(200, 200, 200);
    constexpr COLORREF TextFooter  = RGB(140, 160, 180);
    constexpr COLORREF MenuBg      = RGB(20, 24, 32);
    constexpr COLORREF MenuHeader  = RGB(32, 42, 58);
    constexpr COLORREF MenuSelect  = RGB(28, 64, 52);
    constexpr COLORREF ValueOn     = RGB(60, 255, 60);
    constexpr COLORREF ValueOff    = RGB(255, 60, 60);
    constexpr COLORREF ValueFloat  = RGB(255, 230, 80);
    constexpr COLORREF ValueKey    = RGB(200, 220, 255);

    constexpr float UnitsPerMeter  = 50.0f;

    constexpr int MenuX = 30, MenuY = 50, MenuW = 390;
    constexpr int MenuHeaderH = 28, MenuRowH = 24, MenuValueColX = MenuW - 120;
}

struct AimModeStyle { const wchar_t* Name; COLORREF Color; };

inline AimModeStyle GetAimModeStyle(int mode) {
    using namespace OverlayStyle;
    switch (mode) {
        case AIM_MODE_HARD_LOCK: return { L"HARD LOCK", Target };
        case AIM_MODE_SMOOTH:    return { L"SMOOTH",    Snapline };
        case AIM_MODE_SILENT:    return { L"SILENT",    ValueOn };
        default:                 return { L"OFF",       ValueOff };
    }
}

inline const wchar_t* GetAimKeyName(int keyType) {
    static constexpr const wchar_t* kNames[] = { L"RMB", L"LMB", L"SHIFT", L"ALT" };
    return (keyType >= 0 && keyType < 4) ? kNames[keyType] : L"?";
}

// ---------------------------------------------------------------------------
// InProcessOverlay
// ---------------------------------------------------------------------------
class InProcessOverlay {
public:
    InProcessOverlay() = default;
    ~InProcessOverlay() { Cleanup(); }
    InProcessOverlay(const InProcessOverlay&) = delete;
    InProcessOverlay& operator=(const InProcessOverlay&) = delete;

    bool Create(HINSTANCE hInst) {
        m_hInstance = hInst;
        m_hGameWnd  = FindTargetWindow();

        if (!RegisterWindowClass()) return false;

        m_hOverlay = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kClassName, L"", WS_POPUP,
            0, 0, m_Width, m_Height,
            nullptr, nullptr, hInst, nullptr);
        if (!m_hOverlay) return false;

        SetLayeredWindowAttributes(m_hOverlay, OverlayStyle::Transparent, 0, LWA_COLORKEY);

        // Grayscale AA only: ClearType's colored subpixel fringes look bad on a color-keyed window.
        m_hFont     = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH, L"Arial");
        m_hMenuFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH, L"Consolas");

        if (!m_Pal.Create()) return false;

        SyncToGameWindow();
        return true;
    }

    void Cleanup() {
        DestroyBackbuffer();
        m_Pal.Destroy();
        if (m_hFont)     { DeleteObject(m_hFont);     m_hFont = nullptr; }
        if (m_hMenuFont) { DeleteObject(m_hMenuFont); m_hMenuFont = nullptr; }
        if (m_hOverlay)  { DestroyWindow(m_hOverlay); m_hOverlay = nullptr; }
        if (m_ClassRegistered) {
            UnregisterClassW(kClassName, m_hInstance);
            m_ClassRegistered = false;
        }
        m_IsShown = false;
    }

    void Render() {
        if (!m_hOverlay) return;

        const bool visible = SyncToGameWindow();
        SetShown(visible);
        if (!visible) return;

        const RECT full{ 0, 0, m_Width, m_Height };
        FillRect(m_hMemDC, &full, m_Pal.ClearBrush);

        // Holds the snapshot alive for the whole draw, even if a new one is published.
        const SnapshotChannel::Ptr snap = g_Snapshots.Acquire();

        SelectObject(m_hMemDC, m_hFont);
        DrawStatusLine(snap ? snap->Entities.size() : 0);

        if (snap) {
            if (g_Config.AimModeSelect != AIM_MODE_OFF) DrawFovCircle(snap->View.FOV);
            if (g_Config.EspEnabled)
                for (const auto& ent : snap->Entities) DrawEntity(ent);
        }

        if (g_MenuOpen) DrawMenu();

        Present();
    }

private:
    static constexpr const wchar_t* kClassName = L"BL_Internal_Overlay_Host_V3";

    // -- GDI objects created once, not every frame -------------------------
    struct Palette {
        HPEN   Enemy = nullptr, Target = nullptr, Snapline = nullptr, Fov = nullptr, MenuBorder = nullptr;
        HPEN   HpHigh = nullptr, HpMid = nullptr, HpLow = nullptr;
        HBRUSH ClearBrush = nullptr, MenuBg = nullptr, MenuHeader = nullptr, MenuSelect = nullptr;
        HBRUSH Null = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));

        bool Create() {
            using namespace OverlayStyle;
            Enemy      = CreatePen(PS_SOLID, 2, OverlayStyle::Enemy);
            Target     = CreatePen(PS_SOLID, 2, OverlayStyle::Target);
            Snapline   = CreatePen(PS_SOLID, 1, OverlayStyle::Snapline);
            Fov        = CreatePen(PS_SOLID, 1, FovCircle);
            MenuBorder = CreatePen(PS_SOLID, 2, Accent);
            HpHigh     = CreatePen(PS_SOLID, 2, OverlayStyle::HpHigh);
            HpMid      = CreatePen(PS_SOLID, 2, OverlayStyle::HpMid);
            HpLow      = CreatePen(PS_SOLID, 2, OverlayStyle::HpLow);
            ClearBrush = CreateSolidBrush(Transparent);
            MenuBg     = CreateSolidBrush(OverlayStyle::MenuBg);
            MenuHeader = CreateSolidBrush(OverlayStyle::MenuHeader);
            MenuSelect = CreateSolidBrush(OverlayStyle::MenuSelect);
            return Enemy && Target && Snapline && Fov && MenuBorder && HpHigh && HpMid && HpLow &&
                   ClearBrush && MenuBg && MenuHeader && MenuSelect;
        }

        void Destroy() {
            auto del = [](auto& h) { if (h) { DeleteObject(h); h = nullptr; } };
            del(Enemy); del(Target); del(Snapline); del(Fov); del(MenuBorder);
            del(HpHigh); del(HpMid); del(HpLow);
            del(ClearBrush); del(MenuBg); del(MenuHeader); del(MenuSelect);
        }

        HPEN HealthPen(float frac) const { return frac > 0.5f ? HpHigh : (frac > 0.25f ? HpMid : HpLow); }
    };

    // -- Window plumbing ---------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        // No PostQuitMessage here: this window lives inside the game's process,
        // and posting WM_QUIT could end the game's own message loop.
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool RegisterWindowClass() {
        if (m_ClassRegistered) return true;
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = m_hInstance;
        wc.lpszClassName = kClassName;
        m_ClassRegistered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        return m_ClassRegistered;
    }

    // Only ever matches windows owned by *this* process. The old version's
    // EnumWindows fallback compared against the HWND (not the PID), so it never matched;
    // and FindWindow by title alone could grab e.g. an Explorer folder named "Borderlands".
    HWND FindTargetWindow() const {
        const DWORD pid = GetCurrentProcessId();
        auto ownedByUs = [&](HWND h) {
            DWORD p = 0;
            GetWindowThreadProcessId(h, &p);
            return h && p == pid && h != m_hOverlay;
        };

        HWND h = FindWindowW(nullptr, L"Borderlands");
        if (ownedByUs(h)) return h;
        h = FindWindowW(L"LaunchUnrealUWindowsClient", nullptr);
        if (ownedByUs(h)) return h;

        struct Ctx { DWORD Pid; HWND Exclude; HWND Found; } ctx{ pid, m_hOverlay, nullptr };
        EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(hw, &p);
            if (p != c.Pid || hw == c.Exclude || !IsWindowVisible(hw)) return TRUE;
            RECT r{};
            GetClientRect(hw, &r);
            if (r.right - r.left > 300) { c.Found = hw; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.Found;
    }

    // Tracks the game's client area; returns false when the overlay should be hidden
    // (game minimized, alt-tabbed away, or window gone) so it doesn't draw over other apps.
    bool SyncToGameWindow() {
        if (!m_hGameWnd || !IsWindow(m_hGameWnd)) {
            m_hGameWnd = FindTargetWindow();          // e.g. fullscreen toggle recreated it
            if (!m_hGameWnd) return false;
        }
        if (IsIconic(m_hGameWnd) || GetForegroundWindow() != m_hGameWnd) return false;

        RECT rc{};
        if (!GetClientRect(m_hGameWnd, &rc)) return false;
        POINT pt{ 0, 0 };
        ClientToScreen(m_hGameWnd, &pt);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        if (pt.x != m_PosX || pt.y != m_PosY || w != m_Width || h != m_Height) {
            m_PosX = pt.x; m_PosY = pt.y; m_Width = w; m_Height = h;
            SetWindowPos(m_hOverlay, HWND_TOPMOST, m_PosX, m_PosY, m_Width, m_Height, SWP_NOACTIVATE);
        }

        g_ScreenWidth.store(m_Width, std::memory_order_relaxed);
        g_ScreenHeight.store(m_Height, std::memory_order_relaxed);

        return EnsureBackbuffer(m_Width, m_Height);
    }

    void SetShown(bool shown) {
        if (shown == m_IsShown) return;
        ShowWindow(m_hOverlay, shown ? SW_SHOWNA : SW_HIDE);
        m_IsShown = shown;
    }

    // Grows with the window. The old fixed max(…, 2560×1440) buffer silently
    // clipped on 4K / ultrawide.
    bool EnsureBackbuffer(int w, int h) {
        if (m_hMemDC && w <= m_BufW && h <= m_BufH) return true;
        DestroyBackbuffer();

        HDC screen = GetDC(m_hOverlay);
        m_hMemDC     = CreateCompatibleDC(screen);
        m_hMemBitmap = m_hMemDC ? CreateCompatibleBitmap(screen, w, h) : nullptr;
        ReleaseDC(m_hOverlay, screen);

        if (!m_hMemBitmap) { DestroyBackbuffer(); return false; }

        m_hOldBitmap = static_cast<HBITMAP>(SelectObject(m_hMemDC, m_hMemBitmap));
        m_BufW = w; m_BufH = h;
        SetBkMode(m_hMemDC, TRANSPARENT);   // DC state persists; set once
        return true;
    }

    // Deleting the DC first releases every selected pen/brush/font, so the
    // palette and fonts can then be deleted safely. (The old code deleted pens
    // that were still selected — DeleteObject fails on those, leaking a GDI
    // handle every frame until the 10k-handle limit crashed the game.)
    void DestroyBackbuffer() {
        if (m_hMemDC) {
            if (m_hOldBitmap) SelectObject(m_hMemDC, m_hOldBitmap);
            DeleteDC(m_hMemDC);
        }
        if (m_hMemBitmap) DeleteObject(m_hMemBitmap);
        m_hMemDC = nullptr; m_hMemBitmap = nullptr; m_hOldBitmap = nullptr;
        m_BufW = m_BufH = 0;
    }

    void Present() {
        HDC hdc = GetDC(m_hOverlay);
        BitBlt(hdc, 0, 0, m_Width, m_Height, m_hMemDC, 0, 0, SRCCOPY);
        ReleaseDC(m_hOverlay, hdc);
    }

    // -- Drawing primitives ------------------------------------------------
    void Text(int x, int y, const wchar_t* s, int len, COLORREF color) {
        SetTextColor(m_hMemDC, color);
        TextOutW(m_hMemDC, x, y, s, len);
    }
    void Text(int x, int y, const std::wstring& s, COLORREF color) {
        Text(x, y, s.c_str(), static_cast<int>(s.size()), color);
    }
    void Text(int x, int y, const wchar_t* s, COLORREF color) {
        Text(x, y, s, static_cast<int>(std::wcslen(s)), color);
    }

    void Line(HPEN pen, int x1, int y1, int x2, int y2) {
        SelectObject(m_hMemDC, pen);
        MoveToEx(m_hMemDC, x1, y1, nullptr);
        LineTo(m_hMemDC, x2, y2);
    }

    void OutlineRect(HPEN pen, int l, int t, int r, int b) {
        SelectObject(m_hMemDC, pen);
        SelectObject(m_hMemDC, m_Pal.Null);
        Rectangle(m_hMemDC, l, t, r, b);
    }

    // -- HUD pieces --------------------------------------------------------
    void DrawStatusLine(size_t targetCount) {
        wchar_t buf[160];
        const int len = swprintf(buf, 160, L"[BL:GOTY Active] Menu: [INSERT] | ESP: %s | AIM: [%s] | Targets: %zu",
                                 g_Config.EspEnabled ? L"ON" : L"OFF",
                                 GetAimModeStyle(g_Config.AimModeSelect).Name,
                                 targetCount);
        if (len > 0) Text(20, 15, buf, len, OverlayStyle::Accent);
    }

    // Projects the aim cone with the same math as ViewProjector, so the circle
    // matches what the aimbot actually accepts. The old linear formula
    // (AimbotFov / FOV * halfWidth) drew it roughly half the real size.
    void DrawFovCircle(float viewFov) {
        if (!(viewFov > 0.0f && viewFov < 180.0f)) return;
        const float aimFov = (std::min)(g_Config.AimbotFov, 89.0f);
        if (aimFov <= 0.0f) return;

        const float halfW = m_Width * 0.5f;
        const float r = halfW * std::tan(aimFov * UEMath::DegreesToRadians) /
                                std::tan(viewFov * 0.5f * UEMath::DegreesToRadians);

        const int cx = m_Width / 2, cy = m_Height / 2, ri = static_cast<int>(r);
        SelectObject(m_hMemDC, m_Pal.Fov);
        SelectObject(m_hMemDC, m_Pal.Null);
        Ellipse(m_hMemDC, cx - ri, cy - ri, cx + ri, cy + ri);
    }

    void DrawEntity(const RenderEntity& ent) {
        const int top    = static_cast<int>(ent.Head.Y);
        const int bottom = static_cast<int>(ent.Feet.Y);
        const int left   = static_cast<int>(ent.Feet.X - ent.BoxWidth * 0.5f);
        const int right  = static_cast<int>(ent.Feet.X + ent.BoxWidth * 0.5f);

        if (g_Config.EspBox)
            OutlineRect(ent.IsStickyTarget() ? m_Pal.Target : m_Pal.Enemy, left, top, right, bottom);

        if (g_Config.EspSnaplines)
            Line(m_Pal.Snapline, m_Width / 2, m_Height,
                 static_cast<int>(ent.Feet.X), static_cast<int>(ent.Feet.Y));

        if (g_Config.EspHealthBar) {
            const float frac = ent.HealthPct();
            const int barX   = left - 6;
            const int filled = bottom - static_cast<int>((bottom - top) * frac);
            Line(m_Pal.HealthPen(frac), barX, bottom, barX, filled);
        }

        if (g_Config.EspDistance) {
            wchar_t buf[24];
            const int len = swprintf(buf, 24, L"[%dm]", static_cast<int>(ent.Distance / OverlayStyle::UnitsPerMeter));
            if (len > 0) Text(left, bottom + 2, buf, len, OverlayStyle::TextWhite);
        }
    }

    template <typename MenuItem>
    static void FormatMenuValue(const MenuItem& item, wchar_t* out, size_t outLen, COLORREF& color) {
        using namespace OverlayStyle;
        out[0] = L'\0';
        color  = TextWhite;

        if (item.pBoolVal) {
            swprintf(out, outLen, L"%s", *item.pBoolVal ? L"[ ON ]" : L"[ OFF ]");
            color = *item.pBoolVal ? ValueOn : ValueOff;
        } else if (item.pIntVal == &g_Config.AimModeSelect) {
            const AimModeStyle s = GetAimModeStyle(*item.pIntVal);
            swprintf(out, outLen, L"< %s >", s.Name);
            color = s.Color;
        } else if (item.pIntVal == &g_Config.AimbotKeyType) {
            swprintf(out, outLen, L"< %s >", GetAimKeyName(*item.pIntVal));
            color = ValueKey;
        } else if (item.pIntVal) {
            swprintf(out, outLen, L"< %d >", *item.pIntVal);
        } else if (item.pFloatVal) {
            swprintf(out, outLen, L"< %.1f >", *item.pFloatVal);
            color = ValueFloat;
        }
    }

    void DrawMenu() {
        using namespace OverlayStyle;
        SelectObject(m_hMemDC, m_hMenuFont);

        const int itemCount = static_cast<int>(g_MenuItems.size());
        const int menuH     = 35 + itemCount * MenuRowH + 30;
        const int x0 = MenuX, y0 = MenuY, x1 = MenuX + MenuW, y1 = MenuY + menuH;

        const RECT bg{ x0, y0, x1, y1 };
        FillRect(m_hMemDC, &bg, m_Pal.MenuBg);
        const RECT header{ x0, y0, x1, y0 + MenuHeaderH };
        FillRect(m_hMemDC, &header, m_Pal.MenuHeader);
        OutlineRect(m_Pal.MenuBorder, x0, y0, x1, y1);

        Text(x0 + 12, y0 + 6, L"BORDERLANDS GOTY - TACTICAL MENU", TextWhite);

        int y = y0 + 36;
        for (int i = 0; i < itemCount; ++i, y += MenuRowH) {
            const auto& item = g_MenuItems[i];
            const bool selected = (i == g_MenuIndex);

            if (selected) {
                const RECT sel{ x0 + 4, y - 2, x1 - 4, y + 20 };
                FillRect(m_hMemDC, &sel, m_Pal.MenuSelect);
            }

            const std::wstring label(item.Title.begin(), item.Title.end());   // ASCII titles
            Text(x0 + 14, y, label, selected ? Accent : TextDim);

            wchar_t value[48];
            COLORREF valueColor;
            FormatMenuValue(item, value, 48, valueColor);
            Text(x0 + MenuValueColX, y, value, valueColor);
        }

        Text(x0 + 10, y + 6, L"[UP/DN] Select | [LFT/RGT/ENT] Toggle | [INS] Close", TextFooter);
    }

    // -- State -------------------------------------------------------------
    HWND      m_hGameWnd  = nullptr;
    HWND      m_hOverlay  = nullptr;
    HINSTANCE m_hInstance = nullptr;
    bool      m_ClassRegistered = false;
    bool      m_IsShown   = false;

    int m_Width = 1920, m_Height = 1080;
    int m_PosX  = 0,    m_PosY   = 0;

    HDC     m_hMemDC     = nullptr;
    HBITMAP m_hMemBitmap = nullptr;
    HBITMAP m_hOldBitmap = nullptr;
    int     m_BufW = 0,  m_BufH = 0;

    HFONT   m_hFont     = nullptr;
    HFONT   m_hMenuFont = nullptr;
    Palette m_Pal;
};

inline InProcessOverlay g_Overlay;
