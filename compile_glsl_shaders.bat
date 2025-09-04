@echo off
setlocal enabledelayedexpansion

rem GLSL Shader Compilation Script for Quake3e-HD
rem Compiles GLSL vertex and fragment shaders to SPIR-V

rem Get the directory where this script is located (project root)
set SCRIPT_DIR=%~dp0
cd /d "%SCRIPT_DIR%"

set SHADER_DIR=src\engine\renderer\shaders\glsl
set OUTPUT_DIR=baseq3\shaders\glsl
set GLSLANG=glslangValidator

rem Check if glslangValidator exists
where %GLSLANG% >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ERROR: glslangValidator not found in PATH
    echo Please install Vulkan SDK or add glslangValidator to PATH
    exit /b 1
)

rem Create output directory if it doesn't exist
if not exist %OUTPUT_DIR% (
    echo Creating shader output directory: %OUTPUT_DIR%
    mkdir %OUTPUT_DIR%
)

echo ========================================
echo Compiling GLSL Shaders
echo ========================================

rem Compile vertex shaders
for %%f in (%SHADER_DIR%\*.vert) do (
    echo Compiling %%~nxf...
    %GLSLANG% -V "%%f" -o "%OUTPUT_DIR%\%%~nf.vert.spv" --target-env vulkan1.2 --target-env spirv1.4 -g
    if !ERRORLEVEL! neq 0 (
        echo WARNING: Failed to compile %%~nxf
    )
)

rem Compile fragment shaders
for %%f in (%SHADER_DIR%\*.frag) do (
    echo Compiling %%~nxf...
    %GLSLANG% -V "%%f" -o "%OUTPUT_DIR%\%%~nf.frag.spv" --target-env vulkan1.2 --target-env spirv1.4 -g
    if !ERRORLEVEL! neq 0 (
        echo WARNING: Failed to compile %%~nxf
    )
)

echo ========================================
echo GLSL shader compilation complete!
echo Output directory: %OUTPUT_DIR%
echo ========================================

rem List compiled shaders
echo.
echo Compiled shaders:
dir /b %OUTPUT_DIR%\*.spv 2>nul

endlocal
exit /b 0