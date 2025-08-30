@echo off
REM ============================================================================
REM Quake3e Build Script for Windows
REM ============================================================================
REM This script builds Quake3e using MinGW Make
REM Outputs: build/release/quake3e.x64.exe (client)
REM          build/release/quake3e.ded.x64.exe (dedicated server)
REM ============================================================================

setlocal enabledelayedexpansion

REM ============================================================================
REM Configuration
REM ============================================================================
set BUILD_DIR=build
set BUILD_TYPE=release
set JOBS=4
set ARCH=x86_64

REM Build options
set USE_SDL=0
set USE_CURL=0
set USE_VULKAN=1
set USE_RENDERER_DLOPEN=0

REM ============================================================================
REM Parse command line arguments
REM ============================================================================
set CLEAN=0
set DEBUG=0
set VERBOSE=0

:parse_args
if "%1"=="" goto end_parse
if /i "%1"=="clean" set CLEAN=1
if /i "%1"=="debug" set BUILD_TYPE=debug
if /i "%1"=="release" set BUILD_TYPE=release
if /i "%1"=="verbose" set VERBOSE=1
if /i "%1"=="-j" (
    shift
    set JOBS=%1
)
if /i "%1"=="help" goto show_help
if /i "%1"=="-h" goto show_help
if /i "%1"=="--help" goto show_help
shift
goto parse_args
:end_parse

REM ============================================================================
REM Display build configuration
REM ============================================================================
echo.
echo ============================================================================
echo Quake3e Build System
echo ============================================================================
echo Build Type:    %BUILD_TYPE%
echo Architecture:  %ARCH%
echo Jobs:          %JOBS%
echo Output Dir:    %BUILD_DIR%/%BUILD_TYPE%
echo.
echo Features:
echo   Vulkan:      %USE_VULKAN%
echo   SDL:         %USE_SDL%
echo   CURL:        %USE_CURL%
echo   Renderer DLL: %USE_RENDERER_DLOPEN%
echo ============================================================================
echo.

REM ============================================================================
REM Check for MinGW
REM ============================================================================
where mingw32-make >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set MAKE=mingw32-make
    goto found_make
)

where make >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set MAKE=make
    goto found_make
)

echo ERROR: Make not found. Please install MinGW or MSYS2 and add to PATH.
echo.
echo You can install MinGW from:
echo   https://www.mingw-w64.org/
echo   https://www.msys2.org/
echo.
exit /b 1

:found_make
echo Found make: %MAKE%

REM ============================================================================
REM Clean if requested
REM ============================================================================
if %CLEAN%==1 (
    echo.
    echo Cleaning previous build...
    echo ============================================================================
    %MAKE% clean
    if exist %BUILD_DIR% (
        echo Removing %BUILD_DIR% directory...
        rmdir /s /q %BUILD_DIR% 2>nul
    )
    echo Clean complete.
    echo.
)

REM ============================================================================
REM Create build directory structure
REM ============================================================================
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
if not exist %BUILD_DIR%\%BUILD_TYPE% mkdir %BUILD_DIR%\%BUILD_TYPE%

REM ============================================================================
REM Build the project
REM ============================================================================
echo Building Quake3e...
echo ============================================================================

REM Construct make arguments
set MAKE_ARGS=-j%JOBS%
set MAKE_ARGS=%MAKE_ARGS% %BUILD_TYPE%
set MAKE_ARGS=%MAKE_ARGS% USE_SDL=%USE_SDL%
set MAKE_ARGS=%MAKE_ARGS% USE_CURL=%USE_CURL%
set MAKE_ARGS=%MAKE_ARGS% USE_VULKAN=%USE_VULKAN%
set MAKE_ARGS=%MAKE_ARGS% USE_RENDERER_DLOPEN=%USE_RENDERER_DLOPEN%
set MAKE_ARGS=%MAKE_ARGS% USE_VULKAN_API=%USE_VULKAN%
set MAKE_ARGS=%MAKE_ARGS% ARCH=%ARCH%

if %VERBOSE%==1 (
    set MAKE_ARGS=%MAKE_ARGS% V=1
)

echo Running: %MAKE% %MAKE_ARGS%
echo.

%MAKE% %MAKE_ARGS%

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ============================================================================
    echo BUILD FAILED
    echo ============================================================================
    echo.
    echo Troubleshooting:
    echo   1. Make sure MinGW/MSYS2 is properly installed
    echo   2. Check that all required libraries are available
    echo   3. Run "build.bat clean" to start fresh
    echo   4. Run "build.bat verbose" for detailed output
    echo.
    exit /b %ERRORLEVEL%
)

REM ============================================================================
REM Copy executables to build directory
REM ============================================================================
echo.
echo Copying executables to %BUILD_DIR%\%BUILD_TYPE%...

set SRC_DIR=build\%BUILD_TYPE%-mingw64-%ARCH%

if exist %SRC_DIR%\quake3e.x64.exe (
    copy /y %SRC_DIR%\quake3e.x64.exe %BUILD_DIR%\%BUILD_TYPE%\ >nul
    echo   - quake3e.x64.exe (client)
)

if exist %SRC_DIR%\quake3e.%ARCH%.exe (
    copy /y %SRC_DIR%\quake3e.%ARCH%.exe %BUILD_DIR%\%BUILD_TYPE%\ >nul
    echo   - quake3e.%ARCH%.exe (client)
)

if exist %SRC_DIR%\quake3e.ded.x64.exe (
    copy /y %SRC_DIR%\quake3e.ded.x64.exe %BUILD_DIR%\%BUILD_TYPE%\ >nul
    echo   - quake3e.ded.x64.exe (dedicated server)
)

if exist %SRC_DIR%\quake3e.ded.%ARCH%.exe (
    copy /y %SRC_DIR%\quake3e.ded.%ARCH%.exe %BUILD_DIR%\%BUILD_TYPE%\ >nul
    echo   - quake3e.ded.%ARCH%.exe (dedicated server)
)

REM ============================================================================
REM Success
REM ============================================================================
echo.
echo ============================================================================
echo BUILD SUCCESSFUL
echo ============================================================================
echo.
echo Executables built to: %BUILD_DIR%\%BUILD_TYPE%\
echo.
dir /b %BUILD_DIR%\%BUILD_TYPE%\*.exe 2>nul
echo.
echo To run the game:
echo   %BUILD_DIR%\%BUILD_TYPE%\quake3e.x64.exe
echo.
echo To run dedicated server:
echo   %BUILD_DIR%\%BUILD_TYPE%\quake3e.ded.x64.exe
echo.

exit /b 0

REM ============================================================================
REM Help
REM ============================================================================
:show_help
echo.
echo Quake3e Build Script
echo.
echo Usage: build.bat [options]
echo.
echo Options:
echo   clean       - Clean before building
echo   debug       - Build debug version (default: release)
echo   release     - Build release version (default)
echo   verbose     - Show detailed build output
echo   -j N        - Use N parallel jobs (default: 4)
echo   help        - Show this help message
echo.
echo Examples:
echo   build.bat                    - Build release version
echo   build.bat clean              - Clean and build release version
echo   build.bat clean debug        - Clean and build debug version
echo   build.bat -j 8               - Build with 8 parallel jobs
echo   build.bat clean release -j 2 - Clean and build release with 2 jobs
echo.
exit /b 0