@echo off
echo ============================================================================
echo Quake3e Debug Test
echo ============================================================================
echo.

cd src\project\msvc2017\output

REM Kill any running instance
taskkill /IM quake3e-debug.x64.exe /F 2>nul

echo Starting game with debug mode and test map...
echo.

start quake3e-debug.x64.exe ^
    +set fs_game baseq3 ^
    +set developer 1 ^
    +set sv_cheats 1 ^
    +set r_fullscreen 0 ^
    +set r_mode 4 ^
    +devmap q3dm17

echo.
echo ============================================================================
echo IMPORTANT: The game needs to load a map for debug keys to work!
echo ============================================================================
echo.
echo The game will try to load q3dm17 (The Longest Yard)
echo.
echo If the map doesn't load:
echo 1. Press ~ to open console
echo 2. Type: map q3dm1  (or any map you have)
echo 3. Press ~ to close console
echo.
echo Once in a map, test these keys:
echo - F5: Wireframe mode (r_showtris)
echo - F10: Performance stats (r_speeds)
echo - KP_DOWNARROW: God mode
echo.
echo If keys still don't work in-game:
echo 1. Open console (~)
echo 2. Type: bind F5 "toggle r_showtris"
echo 3. Type: r_showtris 1  (to turn on wireframe directly)
echo.
pause