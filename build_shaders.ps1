[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$Clean,
    [switch]$VerboseMode
)

if ($VerboseMode) {
    $VerbosePreference = 'Continue'
}

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSCommandPath

$stageMap = @{
    '.comp'  = 'comp'
    '.frag'  = 'frag'
    '.geom'  = 'geom'
    '.mesh'  = 'mesh'
    '.rchit' = 'rchit'
    '.rahit' = 'rahit'
    '.rgen'  = 'rgen'
    '.rint'  = 'rint'
    '.rmiss' = 'rmiss'
    '.rcall' = 'rcall'
    '.task'  = 'task'
    '.tesc'  = 'tesc'
    '.tese'  = 'tese'
    '.vert'  = 'vert'
}

$shaderJobs = @(
    @{
        Name   = 'RayTracing'
        Source = Join-Path $root 'src\engine\renderer\shaders\rtx'
        Output = Join-Path $root 'baseq3\shaders\rtx'
        Filter = '*.rgen','*.rchit','*.rahit','*.rmiss','*.rint','*.rcall'
        RayTracing = $true
    },
    @{
        Name   = 'Compute'
        Source = Join-Path $root 'src\engine\renderer\shaders\compute'
        Output = Join-Path $root 'baseq3\shaders\compute'
        Filter = '*.comp'
        RayTracing = $false
    },
    @{
        Name   = 'GLSL'
        Source = Join-Path $root 'src\engine\renderer\shaders\glsl'
        Output = Join-Path $root 'baseq3\shaders\glsl'
        Filter = '*.vert','*.frag','*.geom','*.tesc','*.tese','*.mesh','*.task'
        RayTracing = $false
    },
    @{
        Name   = 'PostProcess'
        Source = Join-Path $root 'src\engine\renderer\shaders\postprocess'
        Output = Join-Path $root 'baseq3\shaders\postprocess'
        Filter = '*.vert','*.frag'
        RayTracing = $false
    }
)

function Get-ValidatorPath {
    if ($env:GLSLANG_VALIDATOR -and (Test-Path $env:GLSLANG_VALIDATOR)) {
        return (Resolve-Path $env:GLSLANG_VALIDATOR).Path
    }

    if ($env:VULKAN_SDK) {
        $candidate = Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe'
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $pathCandidate = Get-Command glslangValidator -ErrorAction SilentlyContinue
    if ($pathCandidate) {
        return $pathCandidate.Path
    }

    return $null
}

if ($Clean) {
    foreach ($job in $shaderJobs) {
        if (-not (Test-Path $job.Output)) {
            continue
        }

        Write-Verbose "Cleaning $($job.Output)"
        Get-ChildItem -LiteralPath $job.Output -Filter '*.spv' -File -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }

    if (-not $Force) {
        Write-Host 'Shader outputs cleaned.'
        return
    }
}

$validator = Get-ValidatorPath
if (-not $validator) {
    Write-Warning 'glslangValidator not found. Skipping shader compilation.'
    return
}

$compiled = @()
$skipped = @()

foreach ($job in $shaderJobs) {
    if (-not (Test-Path $job.Source)) {
        Write-Verbose "Skipping $($job.Name) because source directory is missing."
        continue
    }

    if (-not (Test-Path $job.Output)) {
        New-Item -ItemType Directory -Path $job.Output | Out-Null
    }

    $files = Get-ChildItem -LiteralPath $job.Source -Include $job.Filter -File -ErrorAction SilentlyContinue
    foreach ($file in $files) {
        $ext = $file.Extension.ToLowerInvariant()
        if (-not $stageMap.ContainsKey($ext)) {
            Write-Verbose "Skipping $($file.FullName) because the stage is unknown."
            continue
        }

        $targetName = [System.IO.Path]::GetFileNameWithoutExtension($file.Name) + '.spv'
        $targetPath = Join-Path $job.Output $targetName

        if ((-not $Force) -and (Test-Path $targetPath)) {
            $targetItem = Get-Item $targetPath
            if ($targetItem.LastWriteTimeUtc -ge $file.LastWriteTimeUtc) {
                $skipped += $targetPath
                Write-Verbose "Skipping unchanged shader $targetName."
                continue
            }
        }

        $args = @('-V', '-S', $stageMap[$ext], '--target-env', 'vulkan1.2', '-o', $targetPath, $file.FullName)

        Write-Verbose "Compiling $($file.FullName) -> $targetPath"
        & $validator @args | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "glslangValidator failed for $($file.FullName) with exit code $LASTEXITCODE."
        }

        $compiled += $targetPath
    }
}

Write-Host ("Shader compilation complete. Compiled {0}, skipped {1}." -f $compiled.Count, $skipped.Count)
