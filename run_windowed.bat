@echo off
cd src\project\msvc2017\output
echo Starting Quake3e in windowed mode...
quake3e-debug.x64.exe +set r_mode 6 +set r_fullscreen 0 +set com_hunkMegs 256 +set com_zoneMegs 128 +set developer 1
pause