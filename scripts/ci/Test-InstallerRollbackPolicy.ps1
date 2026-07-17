[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\build.yml') -Raw
$installerTest = Get-Content -LiteralPath (Join-Path $root 'scripts\ci\Test-InstallerWindows.ps1') -Raw
$installerHelperPath = Join-Path $root 'scripts\ci\lib\InstallerWindowsAssertions.ps1'
$fixturePath = Join-Path $root '.github\scripts\Build-FailingServiceRuntimeFixture.ps1'

foreach ($token in @(
    '$failingRuntimeVersion',
    'artifacts/service-runtime-failing-upgrade',
    'Build-FailingServiceRuntimeFixture.ps1',
    'artifacts/installer-failing-upgrade',
    '-FailingUpgradeInstaller'
)) {
    if (-not $workflow.Contains($token)) {
        throw "Hosted installer CI omits the rollback fixture contract: $token"
    }
}

if (-not (Test-Path -LiteralPath $installerHelperPath -PathType Leaf)) {
    throw 'Installer E2E bounded-process helper is missing.'
}
$installerHelper = Get-Content -LiteralPath $installerHelperPath -Raw
foreach ($token in @(
    'BoundedProcessRunner',
    'InstallerProcessTimeoutMilliseconds',
    'StandardOutput',
    'StandardError'
)) {
    if (-not $installerHelper.Contains($token)) {
        throw "Installer E2E process execution is not bounded with diagnostic capture: $token"
    }
}
if ($installerHelper -match '(?is)Start-Process\b.*?-Wait\b') {
    throw 'Installer E2E must not wait indefinitely with Start-Process -Wait.'
}

if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw 'The deliberate failing-upgrade payload builder is missing.'
}
$fixture = Get-Content -LiteralPath $fixturePath -Raw
foreach ($token in @('test-only', 'mactype-service-setup.exe', 'mactype-service.exe', 'Get-FileHash')) {
    if (-not $fixture.Contains($token)) {
        throw "Failing-upgrade fixture is not a valid manifest-preserving start failure: $token"
    }
}

foreach ($token in @(
    '[string] $FailingUpgradeInstaller',
    'Deliberately failing protected upgrade',
    'Assert-BaselineRestoredAfterFailedUpgrade',
    '$baselineApplicationSnapshot',
    '$baselineServiceSnapshot'
)) {
    if (-not $installerTest.Contains($token)) {
        throw "Installer E2E does not prove automatic rollback at the installer boundary: $token"
    }
}

$uploadSteps = [regex]::Matches(
    $workflow,
    '(?ms)^\s+- uses: actions/upload-artifact@[^\r\n]*\r?\n.*?(?=^\s+- (?:uses:|name:)|^  [a-zA-Z0-9_-]+:|\z)'
)
foreach ($step in $uploadSteps) {
    if ($step.Value.Contains('failing-upgrade')) {
        throw 'The deliberate failing-upgrade fixture must never be uploaded as a release artifact.'
    }
}

Write-Host 'Installer failed-upgrade rollback policy passed.'
