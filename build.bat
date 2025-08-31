@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM Quake3e Build Script for Windows using MSVC
REM ============================================================================
REM This script builds Quake3e using MSBuild and Visual Studio project files
REM Requires Visual Studio Build Tools or Visual Studio to be installed
REM Run from Visual Studio Developer Command Prompt or ensure MSBuild is in PATH
REM ============================================================================

set "PROJECT_DIR=src\project\msvc2017"
set "BUILD_CONFIG=Release"
set "BUILD_PLATFORM=x64"

REM Parse command line arguments
if not "%1"=="" set "BUILD_CONFIG=%1"
if not "%2"=="" set "BUILD_PLATFORM=%2"

echo ========================================
echo Building Quake3e with configuration: %BUILD_CONFIG% %BUILD_PLATFORM%
echo ========================================
echo.

REM Check if MSBuild is available
where msbuild >nul 2>&1
if errorlevel 1 (
    echo ERROR: MSBuild not found in PATH!
    echo Please run this script from a Visual Studio Developer Command Prompt
    echo or ensure MSBuild is in your PATH.
    exit /b 1
)

REM Set platform configuration for MSBuild
if /i "%BUILD_PLATFORM%"=="x64" set "MSBUILD_PLATFORM=x64"
if /i "%BUILD_PLATFORM%"=="Win64" set "MSBUILD_PLATFORM=x64"
if /i "%BUILD_PLATFORM%"=="Win32" set "MSBUILD_PLATFORM=Win32"
if /i "%BUILD_PLATFORM%"=="x86" set "MSBUILD_PLATFORM=Win32"
if /i "%BUILD_PLATFORM%"=="ARM64" set "MSBUILD_PLATFORM=ARM64"

REM Build dependencies first
echo Building libjpeg...
msbuild "%PROJECT_DIR%\libjpeg.vcxproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%MSBUILD_PLATFORM% /m /v:minimal
if errorlevel 1 (
    echo ERROR: Failed to build libjpeg
    exit /b 1
)
echo.

echo Building libogg...
msbuild "%PROJECT_DIR%\libogg.vcxproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%MSBUILD_PLATFORM% /m /v:minimal
if errorlevel 1 (
    echo ERROR: Failed to build libogg
    exit /b 1
)
echo.

echo Building libvorbis...
msbuild "%PROJECT_DIR%\libvorbis.vcxproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%MSBUILD_PLATFORM% /m /v:minimal
if errorlevel 1 (
    echo ERROR: Failed to build libvorbis
    exit /b 1
)
echo.

REM Build main projects
echo Building quake3e...
msbuild "%PROJECT_DIR%\quake3e.vcxproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%MSBUILD_PLATFORM% /m /v:minimal
if errorlevel 1 (
    echo ERROR: Failed to build quake3e
    exit /b 1
)
echo.

echo Building quake3e-ded (dedicated server)...
msbuild "%PROJECT_DIR%\quake3e-ded.vcxproj" /p:Configuration=%BUILD_CONFIG% /p:Platform=%MSBUILD_PLATFORM% /m /v:minimal
if errorlevel 1 (
    echo ERROR: Failed to build quake3e-ded
    exit /b 1
)
echo.

echo ========================================
echo Build completed successfully!
echo Configuration: %BUILD_CONFIG% %MSBUILD_PLATFORM%
echo ========================================

REM Display output location
if /i "%MSBUILD_PLATFORM%"=="x64" (
    echo Output files should be in: %PROJECT_DIR%\%MSBUILD_PLATFORM%\%BUILD_CONFIG%\
) else (
    echo Output files should be in: %PROJECT_DIR%\%BUILD_CONFIG%\
)

endlocal
exit /b 0