@echo off
cd src\project\msvc2017\output
echo Starting Quake3e Debug...
quake3e-debug.x64.exe +set developer 1 +set logfile 2 +set com_hunkMegs 256 +set com_zoneMegs 128
pause