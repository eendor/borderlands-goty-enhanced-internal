#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>

#include "config.h"
#include "menu.h"
#include "object_resolver.h"
#include "overlay.h"

void EngineTick();
void EngineShutdown();

namespace {

constexpr DWORD kTickIntervalMs      = 8;       // ~125 Hz
constexpr DWORD kEngineWaitTimeoutMs = 30000;   // give up if globals never appear
constexpr DWORD kEnginePollMs        = 100;

bool ShouldStop() {
    if (IsGameForeground() && (GetAsyncKeyState(VK_END) & 0x8000)) g_UnloadRequested.store(true);
    return g_UnloadRequested.load();
}

// The overlay window belongs to this thread, so this thread must pump its
// messages. Without it, Windows marks the window "Not Responding" after ~5 s
// and DWM may ghost it.
void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// Replaces the blind Sleep(800): wait until GNames / GObjects are actually
// readable, which also covers slow loads and injecting at the splash screen.
bool WaitForEngine() {
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const ULONGLONG deadline = GetTickCount64() + kEngineWaitTimeoutMs;

    while (GetTickCount64() < deadline) {
        if (ShouldStop()) return false;

        TArray64 names, objects;
        if (ReadGlobalArray(base, g_Config.RVA_GNames, names) &&
            ReadGlobalArray(base, g_Config.RVA_GObjects, objects))
            return true;

        Sleep(kEnginePollMs);
    }
    return false;
}

// Default Windows timer resolution is ~15.6 ms, so Sleep(8) really slept
// ~15 ms and the loop ran at ~64 Hz, not 125. Raise it for our lifetime only.
struct TimerResolutionScope {
    TimerResolutionScope()  { timeBeginPeriod(1); }
    ~TimerResolutionScope() { timeEndPeriod(1); }
    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;
};

// Fixed-rate loop: sleeps only the time left in each tick, so a slow
// EngineTick doesn't push the rate down further.
void RunLoop() {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);

    const LONGLONG interval = freq.QuadPart * kTickIntervalMs / 1000;
    LONGLONG next = now.QuadPart + interval;

    while (!ShouldStop()) {
        PumpMessages();
        HandleMenuInput();
        EngineTick();
        g_Overlay.Render();

        QueryPerformanceCounter(&now);
        const LONGLONG remaining = next - now.QuadPart;
        if (remaining > 0)
            Sleep(static_cast<DWORD>(remaining * 1000 / freq.QuadPart));

        next += interval;
        if (now.QuadPart - next > interval * 4)   // fell far behind (breakpoint, hitch): resync
            next = now.QuadPart + interval;
    }
}

} // namespace

DWORD WINAPI MainThread(LPVOID) {
    {
        TimerResolutionScope timer;

        // Register the window class under the DLL's own HINSTANCE, not the
        // game's. WndProc lives in this DLL; a class left registered under the
        // exe instance would point at freed code after an unload/reinject.
        if (WaitForEngine() && g_Overlay.Create(g_hModule))
            RunLoop();

        // Same thread that created the window, so DestroyWindow actually works.
        EngineShutdown();
        g_Overlay.Cleanup();
        PumpMessages();   // flush anything DestroyWindow queued
    }   // timer resolution restored before unloading

    FreeLibraryAndExitThread(g_hModule, 0);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);
            g_hModule = hinstDLL;
            HANDLE hThread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
            if (!hThread) return FALSE;
            CloseHandle(hThread);
            break;
        }
        case DLL_PROCESS_DETACH:
            // No cleanup here. We're under the loader lock, DestroyWindow from a
            // foreign thread fails, and on process exit (lpvReserved != nullptr)
            // other threads are already dead. MainThread does all teardown
            // before calling FreeLibraryAndExitThread.
            if (lpvReserved == nullptr) g_UnloadRequested.store(true);
            break;
    }
    return TRUE;
}
