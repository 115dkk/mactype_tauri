[CmdletBinding()]
param(
    [string] $BuildRoot = 'build/renderer-activation-contract'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$sourceRoot = Join-Path $repositoryRoot 'shared\renderer-activation-contract'
$resolvedBuildRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildRoot))

& node (Join-Path $repositoryRoot 'scripts\generate-renderer-activation-contract.mjs') --check
if ($LASTEXITCODE -ne 0) {
    throw "Renderer activation generated sources are stale (exit $LASTEXITCODE)."
}

foreach ($architecture in @('Win32', 'x64')) {
    $buildDirectory = Join-Path $resolvedBuildRoot $architecture
    & cmake -S $sourceRoot -B $buildDirectory -A $architecture
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer activation $architecture configure failed (exit $LASTEXITCODE)."
    }
    & cmake --build $buildDirectory --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer activation $architecture build failed (exit $LASTEXITCODE)."
    }
    & ctest --test-dir $buildDirectory -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer activation $architecture contract failed (exit $LASTEXITCODE)."
    }
}

Write-Host 'Renderer activation generated sources and x86/x64 C ABI contracts passed.'
