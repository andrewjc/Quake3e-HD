@echo off
echo Starting Quake3e with material export...
cd /d "F:\Development\Quake3e-HD"
"src\project\msvc2017\output\quake3e-debug.x64.exe" +set developer 2 +set r_materialExport 1 +set r_materialAutoGen 1 +set r_materialDebug 1 +set r_materialOverride 1 +set com_hunkMegs 256 +condump startup.txt +map q3dm1 +wait 30 +quit > console_output.txt 2>&1
echo Done. Check console_output.txt
type console_output.txt
pause