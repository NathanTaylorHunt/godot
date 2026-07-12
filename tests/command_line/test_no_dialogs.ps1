#requires -Version 7.4
[CmdletBinding()]
param(
    [string]$GodotPath = (Join-Path $PSScriptRoot '..\..\bin\godot.windows.editor.dev.x86_64.mono.console.exe')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$FixtureRoot = Join-Path $PSScriptRoot 'fixtures'
$ArtifactRoot = Join-Path $ProjectRoot 'logs\headless-no-dialogs'

if (-not (Test-Path -LiteralPath $GodotPath -PathType Leaf)) {
    throw "Pinned Godot executable was not found: $GodotPath"
}

New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null

function Invoke-GodotCase {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [int]$TimeoutMilliseconds = 10000,
        [int]$ExpectedExitCode = -1,
        [string]$RequiredText = ''
    )

    $caseRoot = Join-Path $ArtifactRoot $Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $logPath = Join-Path $caseRoot 'godot.log'
    $stdoutPath = Join-Path $caseRoot 'stdout.log'
    $stderrPath = Join-Path $caseRoot 'stderr.log'
    $process = $null

    try {
        $process = Start-Process `
            -FilePath $GodotPath `
            -ArgumentList (@($Arguments) + @('--log-file', $logPath)) `
            -WorkingDirectory $ProjectRoot `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath `
            -WindowStyle Hidden `
            -PassThru

        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            $process.Kill($true)
            throw "Case '$Name' timed out; a modal dialog may still be blocking the process. Inspect $caseRoot"
        }

        if ($ExpectedExitCode -ge 0 -and $process.ExitCode -ne $ExpectedExitCode) {
            throw "Case '$Name' exited with $($process.ExitCode), expected $ExpectedExitCode. Inspect $caseRoot"
        }
        if ($ExpectedExitCode -lt 0 -and $process.ExitCode -eq 0) {
            throw "Case '$Name' unexpectedly succeeded. Inspect $caseRoot"
        }

        $output = @(
            Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue
            Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
            Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
        ) -join "`n"
        if ($RequiredText -and $output -notlike "*$RequiredText*") {
            throw "Case '$Name' did not contain required text '$RequiredText'. Inspect $caseRoot"
        }

        Write-Host "[OK] $Name exit=$($process.ExitCode)"
    }
    finally {
        if ($null -ne $process) {
            if (-not $process.HasExited) {
                $process.Kill($true)
            }
            $process.Dispose()
        }
    }
}

$cleanProject = Join-Path $FixtureRoot 'clean'
Invoke-GodotCase `
    -Name 'help' `
    -Arguments @('--headless', '--help') `
    -ExpectedExitCode 0 `
    -RequiredText '--no-dialogs'

Invoke-GodotCase `
    -Name 'headless-startup-failure' `
    -Arguments @('--headless', '--path', $cleanProject, '--main-loop', 'DefinitelyMissingMainLoop') `
    -RequiredText "Couldn't detect whether to run the editor"

Invoke-GodotCase `
    -Name 'explicit-no-dialogs-startup-failure' `
    -Arguments @('--no-dialogs', '--path', $cleanProject, '--main-loop', 'DefinitelyMissingMainLoop') `
    -RequiredText "Couldn't detect whether to run the editor"

Invoke-GodotCase `
    -Name 'script-alert' `
    -Arguments @('--headless', '--path', (Join-Path $FixtureRoot 'no-dialogs'), '--script', 'res://main.gd') `
    -ExpectedExitCode 23 `
    -RequiredText 'headless alert fixture'

Invoke-GodotCase `
    -Name 'successful-headless-run' `
    -Arguments @('--headless', '--path', (Join-Path $FixtureRoot 'clean'), '--script', 'res://main.gd') `
    -ExpectedExitCode 0 `
    -RequiredText 'clean headless fixture'

Write-Host "[OK] headless no-dialogs command-line regression suite artifacts=$ArtifactRoot"
