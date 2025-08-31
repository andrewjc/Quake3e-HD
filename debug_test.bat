@echo off
echo Starting Quake3e with debug test configuration...
echo.

cd src\project\msvc2017\output

REM Kill any existing process
taskkill /IM quake3e-debug.x64.exe /F 2>nul

REM Start with explicit config execution in console
start quake3e-debug.x64.exe ^
    +set developer 1 ^
    +set fs_game baseq3 ^
    +set r_fullscreen 0 ^
    +set r_mode 3 ^
    +set com_hunkMegs 256 ^
    +set in_mouse 1 ^
    +set in_nograb 0 ^
    +wait ^
    +exec test_binds.cfg ^
    +wait ^
    +echo "Test binds should be loaded" ^
    +bind F12 "quit" ^
    +echo "F12 bound to quit" ^
    +echo "Type 'exec test_binds' in console if binds don't work"

echo.
echo Game launched. 
echo.
echo Once in game:
echo 1. Press ~ to open console
echo 2. Type: exec test_binds
echo 3. Press ~ to close console  
echo 4. Test F1-F4 keys
echo.
echo If no response, try:
echo - Type in console: /bind F1 "echo test"
echo - Then press F1
echo.
pause