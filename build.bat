@echo off
setlocal
echo [i] Compiling Borderlands GOTY Enhanced Internal DLL...

where g++ >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set GXX=g++
) else if exist "C:\Tools\mingw64\bin\g++.exe" (
    set GXX=C:\Tools\mingw64\bin\g++.exe
) else (
    echo [-] Error: MinGW g++ compiler not found in PATH or C:\Tools\mingw64\bin\
    exit /b 1
)

if not exist "build" mkdir "build"

%GXX% -shared -O2 -std=c++17 -static -static-libgcc -static-libstdc++ src\main.cpp src\engine_tick.cpp -o build\bl_internal_v3.dll -lwinmm -lgdi32 -luser32 -lkernel32

if %ERRORLEVEL% equ 0 (
    echo [+] Success: build\bl_internal_v3.dll generated.
) else (
    echo [-] Compilation failed with error %ERRORLEVEL%.
)
