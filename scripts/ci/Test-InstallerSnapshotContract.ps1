[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\InstallerWindowsAssertions.ps1')

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "mactype-installer-snapshot-$PID-$([Guid]::NewGuid().ToString('N'))"
$applicationRoot = Join-Path $temporaryRoot 'MacType Control Center'
$serviceRoot = Join-Path $applicationRoot 'Service'
$applicationPath = Join-Path $applicationRoot 'MacType Control Center.exe'
$brokerPath = Join-Path (Join-Path $applicationRoot 'service-runtime') 'mactype-service-setup.exe'
$healthPath = Join-Path $serviceRoot 'health.json'

try {
    New-Item -ItemType Directory -Path (Split-Path -Parent $brokerPath), $serviceRoot -Force | Out-Null
    [IO.File]::WriteAllText($applicationPath, 'baseline-app', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($brokerPath, 'baseline-broker', [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($healthPath, 'baseline-health', [Text.UTF8Encoding]::new($false))

    $baseline = Get-TreeSnapshot -Path $applicationRoot -ExcludedRoot $serviceRoot

    [IO.File]::WriteAllText($healthPath, 'restarted-health', [Text.UTF8Encoding]::new($false))
    $candidatePath = Join-Path (Join-Path (Join-Path $serviceRoot 'bin') '0.3.0') `
        'mactype-service.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $candidatePath) -Force | Out-Null
    [IO.File]::WriteAllText($candidatePath, 'failed-candidate', [Text.UTF8Encoding]::new($false))

    $afterServiceMutation = Get-TreeSnapshot -Path $applicationRoot -ExcludedRoot $serviceRoot
    if ($afterServiceMutation -cne $baseline) {
        throw 'Protected service state leaked into the immutable app-side snapshot.'
    }

    $expectedHash = (Get-FileHash -LiteralPath $applicationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText($applicationPath, 'changed-app', [Text.UTF8Encoding]::new($false))
    $actualHash = (Get-FileHash -LiteralPath $applicationPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $changed = Get-TreeSnapshot -Path $applicationRoot -ExcludedRoot $serviceRoot
    $difference = Get-TreeSnapshotDifference -ExpectedSnapshot $baseline -ActualSnapshot $changed
    $expectedDifference = "changed|MacType Control Center.exe|expected=12|$expectedHash|actual=11|$actualHash"
    if ($difference -cne $expectedDifference) {
        throw "App-side snapshot diagnostics omitted the exact path or hashes.`nExpected: $expectedDifference`nActual: $difference"
    }

    Write-Host 'Installer immutable app-side snapshot contract passed.'
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
