@echo off
echo ============================================================================
echo Quake3e-HD Ultra Graphics Launcher
echo ============================================================================
echo.
echo Starting Quake3e with maximum graphics settings...
echo.

cd src\project\msvc2017\output

REM Launch with ultra settings and load q3dm1 for testing
start quake3e-debug.x64.exe ^
    +set fs_game baseq3 ^
    +set r_backend vulkan ^
    +set r_allowExtensions 1 ^
    +set r_ext_compressed_textures 1 ^
    +set r_ext_multitexture 1 ^
    +set r_ext_compiled_vertex_array 1 ^
    +set r_ext_texture_env_add 1 ^
    +set r_ext_texture_filter_anisotropic 1 ^
    +set com_hunkMegs 512 ^
    +set com_maxfps 300 ^
    +set developer 1 ^
    +exec autoexec.cfg ^
    +devmap q3dm1

echo.
echo Game launched! The window should appear shortly.
echo.
echo If the game doesn't start, ensure you have:
echo 1. Copied your Quake 3 baseq3 folder with pak0.pk3 to this directory
echo 2. Vulkan drivers installed
echo 3. Visual C++ Redistributables installed
echo.
echo Press any key to exit this launcher...
pause >nul