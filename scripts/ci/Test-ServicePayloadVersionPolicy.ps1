[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$builder = Get-Content -LiteralPath (Join-Path $root '.github\scripts\Build-ServiceRuntime.ps1') -Raw
$workflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\build.yml') -Raw

foreach ($token in @(
    "-split '[+-]', 2",
    '$packageBaseVersion',
    'runtime package base version',
    '(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?'
)) {
    if (-not $builder.Contains($token)) {
        throw "Service runtime builder does not preserve immutable SemVer generations safely: $token"
    }
}

foreach ($token in @(
    '${{ github.run_id }}',
    '${{ github.sha }}',
    '0.2.0+ci.',
    'artifacts/service-runtime-baseline',
    'artifacts/service-runtime-failing-upgrade',
    'artifacts/service-runtime-current',
    '$baselineRuntimeVersion',
    '$failingRuntimeVersion',
    '$currentRuntimeVersion'
)) {
    if (-not $workflow.Contains($token)) {
        throw "Main build does not bind service payload generations to run and commit identity: $token"
    }
}

if ($workflow -notmatch '(?s)Build-ServiceRuntime\.ps1[^\r\n]*service-runtime-baseline[^\r\n]*baselineRuntimeVersion' -or
    $workflow -notmatch '(?s)Build-ServiceRuntime\.ps1[^\r\n]*service-runtime-current[^\r\n]*currentRuntimeVersion') {
    throw 'Installer baseline and current payloads are not built as distinct immutable generations.'
}

Write-Host 'Service payload immutable-version policy passed.'
