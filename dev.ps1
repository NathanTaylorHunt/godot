#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('doctor', 'build', 'build-templates', 'stage-satori')]
    [string]$Command = 'doctor'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$EngineRoot = $PSScriptRoot
$WorkspaceRoot = (Resolve-Path (Join-Path $EngineRoot '..\..')).Path
$DotNetInstallDir = Join-Path $HOME '.dotnet'
$DotNetCacheRoot = Join-Path $WorkspaceRoot '.cache\dotnet'
$SatoriVersion = '2026.824.0'
$SatoriArchiveHash = '8e5c59d363c58330a81fec8131aa0440a9363a4449019225578061fbed89eb87'
$SatoriCacheRoot = Join-Path $WorkspaceRoot ".cache\satori\$SatoriVersion\win-x64"
$SatoriArchive = Join-Path $SatoriCacheRoot 'win-x64.zip'
$SatoriRuntimeSource = Join-Path $SatoriCacheRoot 'contents\win-x64'

function Assert-Command {
    param([string]$Name)

    $commandInfo = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $commandInfo) {
        throw "Required command '$Name' was not found on PATH."
    }

    Write-Host "[OK] ${Name}: $($commandInfo.Source)"
}

function Get-VsWherePath {
    $programFilesX86 = ${env:ProgramFiles(x86)}
    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer (vswhere.exe) was not found.'
    }

    return $vswhere
}

function Assert-SatoriRuntime {
    if (-not (Test-Path -LiteralPath $SatoriArchive -PathType Leaf)) {
        throw "Missing Satori archive '$SatoriArchive'. Download and verify the pinned Satori runtime before building."
    }

    $actualHash = (Get-FileHash -LiteralPath $SatoriArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $SatoriArchiveHash) {
        throw "Satori archive hash mismatch. Expected $SatoriArchiveHash, got $actualHash."
    }

    foreach ($fileName in 'coreclr.dll', 'clrjit.dll', 'System.Private.CoreLib.dll') {
        $path = Join-Path $SatoriRuntimeSource $fileName
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Verified Satori archive has not been extracted correctly; missing '$path'."
        }
    }

    Write-Host "[OK] Satori runtime: $SatoriVersion (win-x64)"
}

function Assert-DotNetSdk {
    $dotnetExecutable = Join-Path $DotNetInstallDir 'dotnet.exe'
    if (-not (Test-Path -LiteralPath $dotnetExecutable -PathType Leaf)) {
        throw "Required .NET 10 SDK host was not found at '$dotnetExecutable'."
    }

    $version = & $dotnetExecutable --version
    if ([version]$version -lt [version]'10.0.0' -or -not $version.StartsWith('10.')) {
        throw "This fork targets net10.0; a .NET 10 SDK is required. Found '$version' at '$dotnetExecutable'."
    }

    $env:PATH = "$DotNetInstallDir;$env:PATH"
    $env:DOTNET_ROOT = $DotNetInstallDir
    $env:GODOT_DOTNET_CLI = $dotnetExecutable
    $env:DOTNET_CLI_HOME = Join-Path $DotNetCacheRoot 'cli-home'
    $env:NUGET_PACKAGES = Join-Path $DotNetCacheRoot 'nuget-packages'
    $env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
    $env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
    $env:MSBuildEnableWorkloadResolver = 'false'
    New-Item -ItemType Directory -Force -Path $env:DOTNET_CLI_HOME, $env:NUGET_PACKAGES | Out-Null
    Write-Host "[OK] dotnet: $dotnetExecutable ($version)"
}

function Import-VsDevEnvironment {
    $vswhere = Get-VsWherePath
    $installationPath = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($installationPath)) {
        throw 'Visual Studio C++ build tools were not found.'
    }

    $devCommand = Join-Path $installationPath 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCommand -PathType Leaf)) {
        throw "Visual Studio developer environment script was not found at '$devCommand'."
    }

    $environment = & cmd.exe /d /s /c "`"$devCommand`" -no_logo -arch=x64 && set"
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($matches[1])" -Value $matches[2]
        }
    }

    Write-Host "[OK] Visual Studio environment: $installationPath"
}

function Invoke-Doctor {
    if ($PSVersionTable.PSVersion -lt [version]'7.4') {
        throw "PowerShell 7.4 or newer is required; found $($PSVersionTable.PSVersion)."
    }

    Write-Host "[OK] PowerShell: $($PSVersionTable.PSVersion)"
    Assert-Command -Name 'git'
    Assert-Command -Name 'python'
    Assert-Command -Name 'scons'
    Assert-SatoriRuntime
    Import-VsDevEnvironment
    Assert-DotNetSdk
}

function Stage-SatoriRuntime {
    $destination = Join-Path $EngineRoot 'bin\GodotSharp\Tools\Satori\win-x64'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null

    foreach ($fileName in 'coreclr.dll', 'clrjit.dll', 'System.Private.CoreLib.dll') {
        Copy-Item -LiteralPath (Join-Path $SatoriRuntimeSource $fileName) -Destination (Join-Path $destination $fileName) -Force
    }

    Set-Content -LiteralPath (Join-Path $destination 'version.txt') -Value $SatoriVersion -NoNewline
    Write-Host "[OK] Staged Satori runtime: $destination"
}

function Stage-SatoriPlayRuntime {
    # A private dotnet root for play-from-editor (see GDMono::push_play_runtime_environment):
    # the newest installed 10.0.x shared framework and hostfxr, with the Satori overlay applied
    # to the framework directory, so the editor's play child can run Satori while the editor
    # itself keeps the stock runtime. Exports are unaffected; they use the Satori\ overlay.
    $dotnetRoot = Join-Path $env:ProgramFiles 'dotnet'
    $frameworks = Get-ChildItem -LiteralPath (Join-Path $dotnetRoot 'shared\Microsoft.NETCore.App') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like '10.0.*' } | Sort-Object { [version]$_.Name } -Descending
    if (-not $frameworks) {
        Write-Host "[WARN] No installed Microsoft.NETCore.App 10.0.x under '$dotnetRoot'; skipping Satori play runtime staging."
        return
    }
    $framework = $frameworks[0]
    $fxrs = Get-ChildItem -LiteralPath (Join-Path $dotnetRoot 'host\fxr') -Directory | Sort-Object { [version]$_.Name } -Descending
    $fxr = $fxrs[0]

    $destination = Join-Path $EngineRoot 'bin\GodotSharp\Tools\SatoriPlay\win-x64\dotnet'
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    $fxrDestination = Join-Path $destination ("host\fxr\" + $fxr.Name)
    $fwDestination = Join-Path $destination ("shared\Microsoft.NETCore.App\" + $framework.Name)
    New-Item -ItemType Directory -Path $fxrDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $fxr.FullName '*') -Destination $fxrDestination -Recurse -Force
    Copy-Item -Path $framework.FullName -Destination $fwDestination -Recurse -Force

    foreach ($fileName in 'coreclr.dll', 'clrjit.dll', 'System.Private.CoreLib.dll') {
        Copy-Item -LiteralPath (Join-Path $SatoriRuntimeSource $fileName) -Destination (Join-Path $fwDestination $fileName) -Force
    }

    Set-Content -LiteralPath (Join-Path $destination 'version.txt') -Value "$SatoriVersion over Microsoft.NETCore.App $($framework.Name)" -NoNewline
    Write-Host "[OK] Staged Satori play runtime: $destination (framework $($framework.Name), hostfxr $($fxr.Name))"
}

function Invoke-Build {
    Invoke-Doctor

    Push-Location $EngineRoot
    try {
        & scons platform=windows target=editor arch=x86_64 dev_build=yes module_mono_enabled=yes
        if ($LASTEXITCODE -ne 0) {
            throw "Godot build failed with exit code $LASTEXITCODE."
        }

        $editor = Join-Path $EngineRoot 'bin\godot.windows.editor.dev.x86_64.mono.console.exe'
        if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) {
            throw "Expected built editor was not found at '$editor'."
        }

        & $editor --headless --generate-mono-glue .\modules\mono\glue
        if ($LASTEXITCODE -ne 0) {
            throw "Godot C# glue generation failed with exit code $LASTEXITCODE."
        }

        & python .\modules\mono\build_scripts\build_assemblies.py --godot-output-dir=bin --godot-platform=windows --dev-debug
        if ($LASTEXITCODE -ne 0) {
            throw "Godot managed assembly build failed with exit code $LASTEXITCODE."
        }
    }
    finally {
        Pop-Location
    }

    Stage-SatoriRuntime
    Stage-SatoriPlayRuntime
}

function Invoke-TemplateBuild {
    Invoke-Doctor

    Push-Location $EngineRoot
    try {
        # debug_symbols=yes makes MSVC emit a PDB beside each template (/Zi /FS, /DEBUG:FULL).
        # /OPT:REF is still applied at optimize=speed, so codegen is unchanged. The PDB never
        # ships: the Dorifto export archives it under export\symbols\<template-sha>\ and the
        # release manifest records the template and PDB hashes so a dump can be symbolized.
        foreach ($target in 'template_debug', 'template_release') {
            & scons platform=windows target=$target arch=x86_64 module_mono_enabled=yes debug_symbols=yes
            if ($LASTEXITCODE -ne 0) {
                throw "Godot $target build failed with exit code $LASTEXITCODE."
            }
        }
    }
    finally {
        Pop-Location
    }
}

switch ($Command) {
    'doctor' { Invoke-Doctor }
    'build' { Invoke-Build }
    'build-templates' { Invoke-TemplateBuild }
    'stage-satori' { Assert-SatoriRuntime; Stage-SatoriRuntime; Stage-SatoriPlayRuntime }
}
