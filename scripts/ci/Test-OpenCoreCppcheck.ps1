[CmdletBinding()]
param(
    [string] $CppcheckExecutable
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $CppcheckExecutable) {
    $CppcheckExecutable = Join-Path $root 'build\cppcheck-tool\bin\Release\cppcheck.exe'
}
$CppcheckExecutable = [System.IO.Path]::GetFullPath($CppcheckExecutable)
$project = Join-Path $root 'gdipp.vcxproj'
$salHeader = Join-Path $root 'scripts\ci\cppcheck-windows-sal.h'
$evidenceRoot = Join-Path $root 'artifacts\cppcheck-open-core'

foreach ($required in @($CppcheckExecutable, $project, $salHeader)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required cppcheck input is missing: $required"
    }
}

$version = (& $CppcheckExecutable --version).Trim()
if ($version -ne 'Cppcheck 2.20.0') {
    throw "Cppcheck version mismatch: expected Cppcheck 2.20.0, got $version"
}

New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
$configurations = @(
    @{ Name = 'x86'; Project = 'Release|Win32'; Platform = 'win32A'; Architecture = '_X86_' },
    @{ Name = 'x64'; Project = 'Release|x64'; Platform = 'win64'; Architecture = '_AMD64_' }
)

# Phase-one debt is explicit and category-scoped. None of these suppressions
# covers null dereferences, allocation/resource failures, leaks, uninitialized
# state, invalid lifetime, or output-parameter bugs. Those remain RED gates.
$legacyDebtIds = @(
    'dangerousTypeCast',
    'constStatement',
    'ignoredReturnValue',
    'accessMoved',
    'passedByValue',
    'noOperatorEq',
    'noCopyConstructor',
    'returnByReference',
    'UnionZeroInit',
    'useInitializationList'
)

foreach ($configuration in $configurations) {
    $output = Join-Path $evidenceRoot "$($configuration.Name).txt"

    $arguments = @(
        "--project=$project",
        "--project-configuration=$($configuration.Project)",
        "--platform=$($configuration.Platform)",
        "-D$($configuration.Architecture)",
        "--include=$salHeader",
        '--enable=warning,performance,portability',
        '--check-level=normal',
        '--inline-suppr',
        '--suppress=missingIncludeSystem',
        '--suppress=missingInclude',
        '--suppress=missingReturn:*json.hpp',
        '--error-exitcode=1',
        '--quiet',
        '--template={file}:{line}:{column}: {severity}: {message} [{id}]',
        "--output-file=$output"
    )
    foreach ($id in $legacyDebtIds) {
        $arguments += "--suppress=$id"
    }

    & $CppcheckExecutable @arguments
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $output) {
            Get-Content -LiteralPath $output | Write-Host
        }
        throw "Cppcheck rejected the $($configuration.Project) rendering core configuration."
    }
    if ((Get-Item -LiteralPath $output).Length -ne 0) {
        Get-Content -LiteralPath $output | Write-Host
        throw "Cppcheck emitted diagnostics without a failing exit code for $($configuration.Project)."
    }
    Write-Host "Cppcheck $($configuration.Project): clean"
}
