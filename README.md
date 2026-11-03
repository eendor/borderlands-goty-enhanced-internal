# Borderlands: GOTY Enhanced - Internal Tooling & ESP / Aimbot Framework

An internal instrumentation and tactical HUD framework for **Borderlands: Game of the Year Enhanced** (Win64, DirectX 11 / Unreal Engine 3 `WillowGame`).

Built for research, practice, and camera/aimbot mechanics exploration.

---

## Features

- **Dynamic Reflection Resolution:**
  - Runtime object enumeration via `GObjects` (`0x025D95D0`) and `GNames` (`0x025D9740`).
  - Automatically identifies live `WillowPlayerController`, `WillowPlayerPawn`, and actor tables without hardcoded pointer paths that break across game levels.

- **3D Collision Cylinder Bounding:**
  - Directly reads each creature's `UCylinderComponent` (`+0x1D8`) for accurate `CollisionHeight` (`+0x29C`) and `CollisionRadius` (`+0x2A0`).
  - Accurately grounds the bounding boxes to ground contact points (`Center.Z - CollisionHeight`) up to head height (`Center.Z + CollisionHeight`), fully encompassing Skags, Psychos, and humanoids.

- **Multi-Mode Aiming Pipeline:**
  - **`[OFF]`**: All aimbot interventions disabled.
  - **`[HARD LOCK]`**: Instantaneous 1-to-1 camera snap and tracking anchored at the critical head/mouth spot (`Center.Z + (ColHeight * 0.35)`).
  - **`[SMOOTH]`**: Interpolated camera glide using adjustable smoothing factor (`AimbotSmooth`).
  - **`[SILENT AIM]`**: Decoupled shot trace redirection. Zero camera manipulation or view snapping. Routes hitscan and projectile trajectories via engine properties (`ShotTarget` and `AimAtVector`). Completely decouples from Right-Click ADS / Zoom, ensuring iron sights aiming has zero hitching or camera jerking.

- **In-Process Hardware Composition HUD & Menu:**
  - Transparent overlay with double-buffered GDI rendering (`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST`).
  - Integrated interactive configuration menu toggled in-game with `[INSERT]`.

---

## Controls

| Key | Action |
|---|---|
| `INSERT` | Toggle In-Game Configuration Menu |
| `UP / DOWN` | Navigate menu options |
| `LEFT / RIGHT` | Adjust values / Toggle settings |
| `RETURN` | Toggle boolean value or cycle aim mode |
| `END` | Cleanly detach overlay and unload DLL |

---

## Menu Options

- `[ESP] Master Switch`: Global toggle for overlay rendering.
- `  - Bounding Boxes`: 2D projected collision boxes.
- `  - Snaplines`: Screen-bottom targeting lines to target feet.
- `  - Health Bars`: Real-time health fraction bar on target left edge.
- `  - Distance Labels`: Target distance meters.
- `[AIM] Operating Mode`: Switch between `OFF`, `HARD LOCK`, `SMOOTH`, `SILENT AIM`.
- `  - Aim Key Trigger`: Trigger binding (`RMB`, `LMB`, `LSHIFT`, `LALT`).
- `  - Smooth Factor`: Smoothing divisor for Smooth Aim Mode (1.5 - 15.0).
- `  - Lock FOV Radius`: Target selection cone in degrees (5.0° - 90.0°).
- `  - Target Crit Ratio`: Height factor inside collision cylinder for head/mouth target.

---

## Building from Source

### Prerequisites
- MinGW-w64 (`g++.exe` with C++17 support)
- Windows SDK / standard Win32 libraries (`user32`, `gdi32`, `dwmapi`, `d3d9`)

### Build Command
```cmd
g++.exe -shared -O2 -std=c++17 -static -static-libgcc -static-libstdc++ src\internal_esp_aimbot.cpp -o build\bl_internal_v3.dll -ld3d9 -ldwmapi -luser32 -lgdi32
```
Or execute `build.bat`.

---

## Injection
Inject `build/bl_internal_v3.dll` into `BorderlandsGOTY.exe` (Win64) using any standard 64-bit DLL injector (`CreateRemoteThread` + `LoadLibraryW`).
