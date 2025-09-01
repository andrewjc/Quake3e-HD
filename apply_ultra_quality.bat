@echo off
echo ============================================
echo Applying Ultra Quality Settings to Quake3e-HD
echo ============================================
echo.

REM Copy ultra quality config to the output directory
copy /Y "baseq3\ultra_quality.cfg" "src\project\msvc2017\output\baseq3\ultra_quality.cfg"

REM Launch the game with ultra quality settings
echo Launching Quake3e with Ultra Quality settings...
echo.
echo Commands to execute in console:
echo   /exec ultra_quality
echo   /vid_restart
echo.

cd /d "F:\Development\Quake3e-HD"
"src\project\msvc2017\output\quake3e-debug.x64.exe" +exec ultra_quality +set r_customwidth 2560 +set r_customheight 1440 +vid_restart

echo.
echo ============================================
echo Ultra Quality Settings Applied!
echo ============================================
pause