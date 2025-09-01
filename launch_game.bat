@echo off
echo Launching Quake3e-HD with Ultra Quality Settings and RTX
echo =========================================================
cd src\project\msvc2017\output
start quake3e-debug.x64.exe +set fs_basepath "." +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1920 +set r_customheight 1080 +set developer 1 +set r_displayRefresh 144 +map q3dm17
echo.
echo Game launched. Use these console commands:
echo   /demo four         - Play demo
echo   /timedemo 1        - Enable benchmark mode
echo   /rtx_status        - Check RTX status
echo   /r_speeds 1        - Show rendering stats
echo   /vid_restart       - Restart renderer
echo.
pause