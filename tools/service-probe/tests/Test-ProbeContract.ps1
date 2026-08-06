[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64')]
    [string] $Architecture = 'x64',

    [string] $BuildRoot = (Join-Path ([System.IO.Path]::GetTempPath()) 'mactype-service-probe-contract')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Equal {
    param(
        [Parameter(Mandatory)] $Actual,
        [Parameter(Mandatory)] $Expected,
        [Parameter(Mandatory)] [string] $Message
    )

    if ($Actual -ne $Expected) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

$sourceRoot = Split-Path -Parent $PSScriptRoot
$buildDirectory = Join-Path $BuildRoot $Architecture
$resultDirectory = Join-Path $buildDirectory 'contract-results'
$suffix = if ($Architecture -eq 'x64') { '64' } else { '32' }

& cmake -S $sourceRoot -B $buildDirectory -A $Architecture
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

& cmake --build $buildDirectory --config Release --target probe-console probe-window probe-spawn-tree probe-timeout-fixture renderer-raii-tests browser-launch-gate
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

$raiiTestPath = Join-Path $buildDirectory 'Release\renderer-raii-tests.exe'
& $raiiTestPath
if ($LASTEXITCODE -ne 0) {
    throw "Renderer RAII fault-injection tests failed with exit code $LASTEXITCODE"
}

$gatePath = Join-Path $buildDirectory "Release\browser-launch-gate$suffix.exe"
$gatePidPath = Join-Path $resultDirectory 'browser-launch-gate.pid'
$gateNamespace = "probe-contract-$PID"
New-Item -ItemType Directory -Force $resultDirectory | Out-Null
Remove-Item -LiteralPath $gatePidPath -Force -ErrorAction SilentlyContinue
$env:MACTYPE_BROWSER_GATE_TARGET = $env:ComSpec
$env:MACTYPE_BROWSER_GATE_PID_FILE = $gatePidPath
$env:MACTYPE_BROWSER_GATE_TIMEOUT_MS = '5000'
$env:MACTYPE_DIRECTWRITE_DIAGNOSTICS = $gateNamespace
$gate = $null
$gateEvent = $null
try {
    $gate = Start-Process -FilePath $gatePath -ArgumentList '/d', '/c', 'exit 0' -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not (Test-Path -LiteralPath $gatePidPath -PathType Leaf)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Browser launch gate did not publish its child PID.' }
        Start-Sleep -Milliseconds 25
    }
    $gatedPid = [int](Get-Content -LiteralPath $gatePidPath -Raw)
    $gateEvent = [System.Threading.EventWaitHandle]::new(
        $false,
        [System.Threading.EventResetMode]::ManualReset,
        "Local\MacType.$gateNamespace.pid-$gatedPid.hook-ready"
    )
    $gateEvent.Set() | Out-Null
    if (-not $gate.WaitForExit(5000)) { throw 'Browser launch gate did not resume and reap its child.' }
    if ($gate.ExitCode -ne 0) { throw "Browser launch gate failed with exit code $($gate.ExitCode)." }
} finally {
    if ($gateEvent) { $gateEvent.Dispose() }
    if ($gate -and -not $gate.HasExited) { Stop-Process -Id $gate.Id -Force }
    Remove-Item Env:MACTYPE_BROWSER_GATE_TARGET -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_BROWSER_GATE_PID_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_BROWSER_GATE_TIMEOUT_MS -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_DIRECTWRITE_DIAGNOSTICS -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $gatePidPath -Force -ErrorAction SilentlyContinue
}

$resultPath = Join-Path $resultDirectory 'console.json'
$probePath = Join-Path $buildDirectory "Release\probe-console$suffix.exe"
& $probePath --out $resultPath --wait-ms 25 `
    --font-source 'Arial' --font-replacement 'Courier New'
if ($LASTEXITCODE -ne 0) {
    throw "Console probe failed with exit code $LASTEXITCODE"
}

$result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
Assert-Equal $result.schemaVersion 1 'Probe JSON schema version changed unexpectedly'
Assert-Equal $result.probeKind 'console' 'Console probe reported the wrong kind'
Assert-Equal $result.architecture $(if ($Architecture -eq 'x64') { 'x64' } else { 'x86' }) 'Probe reported the wrong architecture'
if ($result.reportedWindowsMajorVersion -lt 10) {
    throw "Probe compatibility manifest is missing or stale: GetVersionEx reported $($result.reportedWindowsMajorVersion).$($result.reportedWindowsMinorVersion)"
}
if ($result.sessionId -is [string] -or $null -eq $result.sessionId) {
    throw 'Session ID must be a machine-readable JSON number'
}

if ($result.pid -le 0 -or $result.parentPid -le 0) {
    throw 'Probe must report positive process and parent process IDs'
}

if ($result.renderFingerprint -notmatch '^sha256:[0-9a-f]{64}$') {
    throw "Render fingerprint is not a SHA-256 value: $($result.renderFingerprint)"
}

if ($result.fontSubstitution.sourceFamily -ne 'Arial' -or
    $result.fontSubstitution.replacementFamily -ne 'Courier New') {
    throw 'Probe did not preserve the requested font-substitution pair.'
}
foreach ($property in @(
    'disabledSourceFingerprint', 'activeSourceFingerprint',
    'disabledReplacementFingerprint'
)) {
    if ($result.fontSubstitution.gdi.$property -notmatch '^sha256:[0-9a-f]{64}$') {
        throw "GDI font-substitution observation omitted $property."
    }
}
if ($result.fontSubstitution.gdi.disabledSourceFingerprint -eq
    $result.fontSubstitution.gdi.disabledReplacementFingerprint) {
    throw 'The stock Arial and Courier New controls must render differently.'
}
if (-not $result.fontSubstitution.gdi.controlsStable) {
    throw 'Repeated disabled GDI control renders must be byte-for-byte stable.'
}
if ($result.fontSubstitution.directWrite.disabledSourceFamily -ne 'Arial' -or
    $result.fontSubstitution.directWrite.disabledReplacementFamily -ne 'Courier New') {
    throw 'DirectWrite controls did not resolve the requested font families.'
}
if (-not $result.fontSubstitution.directWrite.controlsStable) {
    throw 'Repeated DirectWrite family observations must be stable.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.disabledSourceFamily -ne 'Arial' -or
    $result.fontSubstitution.directWriteIndexedCollection.disabledReplacementFamily -ne 'Courier New') {
    throw 'Indexed DirectWrite collection controls did not resolve the requested font families.'
}
if (-not $result.fontSubstitution.directWriteIndexedCollection.controlsStable) {
    throw 'Indexed DirectWrite collection controls must remain distinct.'
}
if (-not $result.fontSubstitution.directWriteIndexedCollection.retainedMetadataStable) {
    throw 'A retained indexed DirectWrite font must preserve its source metadata identity.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.disabledSourcePostScriptName -eq
    $result.fontSubstitution.directWriteIndexedCollection.disabledReplacementPostScriptName) {
    throw 'Indexed DirectWrite PostScript-name controls must remain distinct.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.activePinnedSourceFamily -ne 'Arial') {
    throw 'A retained stock DirectWrite font object must continue to resolve to Arial.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.activePinnedSourceDescriptorFamily -ne 'Arial') {
    throw 'A retained stock DirectWrite face descriptor must continue to resolve to Arial.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.retainedObjectReplacementObserved) {
    throw 'A retained stock DirectWrite font object must not report a substitution.'
}
if ($result.fontSubstitution.directWriteIndexedCollection.retainedDescriptorReplacementObserved) {
    throw 'A retained stock DirectWrite face descriptor must not report a substitution.'
}
if ($result.fontSubstitution.directWriteCustomCollection.disabledSourceFamily -ne 'Arial' -or
    $result.fontSubstitution.directWriteCustomCollection.disabledReplacementFamily -ne 'Courier New') {
    throw 'Custom DirectWrite collection controls did not resolve the requested font families.'
}
if (-not $result.fontSubstitution.directWriteCustomCollection.controlsStable) {
    throw 'Repeated custom DirectWrite collection observations must be stable.'
}
if (-not $result.mactypeModuleLoaded) {
    if ($result.fontSubstitution.gdi.activeSourceFingerprint -ne
        $result.fontSubstitution.gdi.disabledSourceFingerprint) {
        throw 'A stock Windows probe changed when only the MacType diagnostic environment switch changed.'
    }
    if ($result.fontSubstitution.gdi.replacementObserved) {
        throw 'A stock Windows probe must not report a MacType font substitution.'
    }
    if ($result.fontSubstitution.directWrite.activeSourceFamily -ne 'Arial' -or
        $result.fontSubstitution.directWrite.replacementObserved) {
        throw 'A stock Windows probe must not report a DirectWrite font substitution.'
    }
    if ($result.fontSubstitution.directWriteIndexedCollection.activeSourceFamily -ne 'Arial' -or
        $result.fontSubstitution.directWriteIndexedCollection.replacementObserved) {
        throw 'A stock Windows probe must not report an indexed DirectWrite collection substitution.'
    }
    if ($result.fontSubstitution.directWriteIndexedCollection.activeSourcePostScriptName -ne
        $result.fontSubstitution.directWriteIndexedCollection.disabledSourcePostScriptName) {
        throw 'A stock Windows probe must preserve indexed DirectWrite PostScript metadata.'
    }
    if ($result.fontSubstitution.directWriteCustomCollection.activeSourceFamily -ne 'Arial' -or
        $result.fontSubstitution.directWriteCustomCollection.replacementObserved) {
        throw 'A stock Windows probe must not report a custom DirectWrite collection substitution.'
    }
}

if ($null -eq $result.modules -or $result.modules.GetType().Name -ne 'Object[]') {
    throw 'Probe must report modules as a JSON array'
}

$requiredProperties = @(
    'mactypeModuleLoaded', 'mactypeModulePath', 'mactypeVersion',
    'versionSource', 'loadObservedAt'
)
foreach ($property in $requiredProperties) {
    if ($property -notin $result.PSObject.Properties.Name) {
        throw "Probe JSON omitted required property '$property'"
    }
}
if ($result.mactypeModuleLoaded -and [string]::IsNullOrWhiteSpace($result.mactypeModulePath)) {
    throw 'A loaded MacType module must report its path'
}

if ([string]::IsNullOrWhiteSpace($result.startedAt) -or [string]::IsNullOrWhiteSpace($result.observedAt)) {
    throw 'Probe timestamps must be populated'
}

Write-Host "Service probe contract passed for $Architecture."

$precreatedFactoryResultPath = Join-Path $resultDirectory 'precreated-directwrite.json'
& $probePath --out $precreatedFactoryResultPath --wait-ms 0 `
    --precreate-directwrite-factory
Assert-Equal $LASTEXITCODE 0 'A stock precreated DirectWrite factory probe must run'
$precreatedFactoryResult = Get-Content -LiteralPath $precreatedFactoryResultPath -Raw | ConvertFrom-Json
Assert-Equal $precreatedFactoryResult.directWriteFactoryPrecreated $true `
    'Probe did not preserve the browser-like precreated DirectWrite factory contract'
Assert-Equal $precreatedFactoryResult.directWriteFactoryType 'shared' `
    'Probe did not identify the precreated shared DirectWrite factory'
Assert-Equal $precreatedFactoryResult.fontSubstitution.directWrite.activeSourceFamily 'Arial' `
    'A stock precreated DirectWrite factory unexpectedly substituted Arial'

$isolatedFactoryResultPath = Join-Path $resultDirectory 'precreated-isolated-directwrite.json'
& $probePath --out $isolatedFactoryResultPath --wait-ms 0 `
    --precreate-isolated-directwrite-factory
Assert-Equal $LASTEXITCODE 0 'A stock precreated isolated DirectWrite factory probe must run'
$isolatedFactoryResult = Get-Content -LiteralPath $isolatedFactoryResultPath -Raw | ConvertFrom-Json
Assert-Equal $isolatedFactoryResult.directWriteFactoryPrecreated $true `
    'Probe did not preserve the Chromium-like isolated DirectWrite factory contract'
Assert-Equal $isolatedFactoryResult.directWriteFactoryType 'isolated' `
    'Probe did not identify the precreated isolated DirectWrite factory'
Assert-Equal $isolatedFactoryResult.fontSubstitution.directWrite.activeSourceFamily 'Arial' `
    'A stock precreated isolated DirectWrite factory unexpectedly substituted Arial'

$missingPreloadResult = Join-Path $resultDirectory 'missing-preload.json'
& $probePath --out $missingPreloadResult --wait-ms 0 `
    --preload-mactype (Join-Path $resultDirectory 'missing-MacType.dll')
Assert-Equal $LASTEXITCODE 4 'A missing explicit MacType preload must fail'
if (Test-Path -LiteralPath $missingPreloadResult) {
    throw 'A failed explicit MacType preload must not write a successful observation.'
}

$windowResultPath = Join-Path $resultDirectory 'window.json'
$windowProbePath = Join-Path $buildDirectory "Release\probe-window$suffix.exe"
$windowProcess = Start-Process -FilePath $windowProbePath -ArgumentList @(
    '--out', "`"$windowResultPath`"", '--wait-ms', '25'
) -Wait -PassThru
Assert-Equal $windowProcess.ExitCode 0 'Window probe failed'
$windowResult = Get-Content -LiteralPath $windowResultPath -Raw | ConvertFrom-Json
Assert-Equal $windowResult.probeKind 'window' 'Window probe reported the wrong kind'
Assert-Equal $windowResult.architecture $result.architecture 'Window and console probes disagree on architecture'

$treeResultPath = Join-Path $resultDirectory 'tree.json'
$treeProbePath = Join-Path $buildDirectory "Release\probe-spawn-tree$suffix.exe"
& $treeProbePath --out $treeResultPath --wait-ms 25
if ($LASTEXITCODE -ne 0) {
    throw "Spawn-tree probe failed with exit code $LASTEXITCODE"
}
$treeResult = Get-Content -LiteralPath $treeResultPath -Raw | ConvertFrom-Json
Assert-Equal $treeResult.probeKind 'spawn-tree' 'Spawn-tree manifest reported the wrong kind'
Assert-Equal $treeResult.nodes.Count 3 'Spawn-tree manifest must contain parent, child, and grandchild artifacts'
foreach ($node in $treeResult.nodes) {
    if (-not $node.present) {
        throw "Spawn-tree node artifact was not written: $($node.role)"
    }
    $nodeResult = Get-Content -LiteralPath $node.artifact -Raw | ConvertFrom-Json
    Assert-Equal $nodeResult.probeKind 'spawn-tree-node' "Spawn-tree $($node.role) reported the wrong kind"
    Assert-Equal $nodeResult.role $node.role "Spawn-tree $($node.role) reported the wrong role"
    Assert-Equal $nodeResult.treeLevel $node.level "Spawn-tree $($node.role) reported the wrong level"
}

Write-Host "Window and spawn-tree contracts passed for $Architecture."

$timeoutManifest = Join-Path $resultDirectory 'timeout-tree.json'
$timeoutFixture = Join-Path $buildDirectory "Release\probe-timeout-fixture$suffix.exe"
$timeoutProcess = Start-Process -FilePath $treeProbePath -ArgumentList @(
    '--out', "`"$timeoutManifest`"", '--wait-ms', '0',
    '--child-exe', "`"$timeoutFixture`""
) -PassThru
if (-not $timeoutProcess.WaitForExit(30000)) {
    $timeoutProcess.Kill($true)
    throw 'Spawn-tree launcher exceeded its finite timeout contract.'
}
Assert-Equal $timeoutProcess.ExitCode 4 'A timed-out spawn tree must fail its contract'
$timeoutResult = Get-Content -LiteralPath $timeoutManifest -Raw | ConvertFrom-Json
Assert-Equal $timeoutResult.childLaunched $true 'Timeout fixture was not launched'
Assert-Equal $timeoutResult.childExitCode 1460 'A child timeout must be explicit ERROR_TIMEOUT'

$fixturePidPaths = @(
    "$timeoutManifest.fixture-child.pid",
    "$timeoutManifest.fixture-descendant.pid"
)
$liveFixtureProcesses = [System.Collections.Generic.List[object]]::new()
foreach ($pidPath in $fixturePidPaths) {
    if (-not (Test-Path -LiteralPath $pidPath -PathType Leaf)) {
        throw "Timeout fixture did not write its process ID: $pidPath"
    }
    $fixturePid = [int] (Get-Content -LiteralPath $pidPath -Raw)
    $fixtureProcess = Get-Process -Id $fixturePid -ErrorAction SilentlyContinue
    if ($null -ne $fixtureProcess) {
        $liveFixtureProcesses.Add($fixtureProcess)
    }
}
foreach ($fixtureProcess in $liveFixtureProcesses) {
    Stop-Process -InputObject $fixtureProcess -Force -ErrorAction SilentlyContinue
}
if ($liveFixtureProcesses.Count -ne 0) {
    throw 'Timed-out spawn-tree descendants survived their launcher.'
}

Write-Host "Spawn-tree timeout cleanup contract passed for $Architecture."
