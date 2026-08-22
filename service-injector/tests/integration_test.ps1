[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Injector,

    [Parameter(Mandatory)]
    [string] $Target,

    [Parameter(Mandatory)]
    [string] $Module,

    [Parameter(Mandatory)]
    [string] $QuietModule,

    [Parameter(Mandatory)]
    [string] $SlowModule,

    [Parameter(Mandatory)]
    [string] $DecoyModule,

    [Parameter(Mandatory)]
    [string] $TimeoutInjector,

    [Parameter(Mandatory)]
    [string] $InheritedHandleLauncher,

    [Parameter(Mandatory)]
    [ValidateSet('x86', 'x64')]
    [string] $Architecture
)

$ErrorActionPreference = 'Stop'
$generation = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
$profileDigest = "sha256:$generation"
$testRoot = Join-Path $env:TEMP ("mactype-injector-test-" + [Guid]::NewGuid().ToString('N'))
$processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
$fixedModule = Join-Path (Split-Path -Parent $Injector) ([IO.Path]::GetFileName($Module))
$moduleBackup = "$fixedModule.normal"
$moduleReplaced = $false

function Wait-ForFile([string] $Path, [System.Diagnostics.Process] $Process) {
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not (Test-Path -LiteralPath $Path) -and
           -not $Process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 20
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Process $($Process.Id) did not publish $Path."
    }
}

function Start-Marker([string] $Name, [string] $PreloadModule = '') {
    $metadataPath = Join-Path $testRoot "$Name-metadata.json"
    $resultPath = Join-Path $testRoot "$Name-result.json"
    $arguments = @(
        '--metadata', $metadataPath,
        '--result', $resultPath,
        '--wait-ms', '5000',
        '--expected-module', $fixedModule
    )
    if ($PreloadModule) {
        $arguments += @('--preload', $PreloadModule)
    }
    $process = Start-Process -FilePath $Target -ArgumentList $arguments -PassThru
    $processes.Add($process)
    Wait-ForFile -Path $metadataPath -Process $process
    return [pscustomobject]@{
        Process = $process
        Identity = (Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json)
        ResultPath = $resultPath
    }
}

function Invoke-Broker(
    [string] $Executable,
    $Identity,
    [UInt32] $HandlePid = 0,
    [string[]] $ExtraArguments = @()
) {
    if ($HandlePid -eq 0) { $HandlePid = [UInt32]$Identity.pid }
    if ($HandlePid -eq 0) { throw 'Inherited-handle launcher requires a nonzero target PID.' }
    $handlePidText = $HandlePid.ToString([Globalization.CultureInfo]::InvariantCulture)
    $arguments = @(
        $Executable, $handlePidText,
        '--pid', [string]$Identity.pid,
        '--creation-time', [string]$Identity.creationTime,
        '--session-id', [string]$Identity.sessionId,
        '--generation-id', $generation,
        '--profile-digest', $profileDigest
    ) + $ExtraArguments
    $json = & $InheritedHandleLauncher $arguments
    $exitCode = $LASTEXITCODE
    if ([string]::IsNullOrWhiteSpace([string]$json)) {
        throw "Inherited-handle launcher returned no JSON for PID $handlePidText (exit $exitCode)."
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Json = [string]$json
        Response = ([string]$json | ConvertFrom-Json)
    }
}

function Assert-Response($Invocation, [int] $ExitCode, [string] $Status, [string] $Code) {
    if ($Invocation.ExitCode -ne $ExitCode -or
        $Invocation.Response.schemaVersion -ne 2 -or
        $Invocation.Response.status -ne $Status -or
        $Invocation.Response.code -ne $Code) {
        throw "Unexpected injector response: $($Invocation.Json) (exit $($Invocation.ExitCode))"
    }
    if ($Invocation.Json.Length -gt 1536) {
        throw 'Injector response exceeded the 1536-byte public bound.'
    }
}

function Assert-EvidenceModuleLoad($Invocation, [int] $Expected) {
    $encoded = [string]$Invocation.Response.rendererEvidence
    if ($encoded -notmatch '^[0-9a-f]{624}$') {
        throw "Injector response omitted canonical renderer evidence: $($Invocation.Json)"
    }
    $actual = [Convert]::ToInt32($encoded.Substring(18, 2), 16)
    if ($actual -ne $Expected) {
        throw "Unexpected renderer module-load origin $actual (expected $Expected): $($Invocation.Json)"
    }
}

New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    foreach ($path in @($Injector, $Target, $Module, $QuietModule, $SlowModule, $DecoyModule, $TimeoutInjector, $InheritedHandleLauncher)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required test artifact does not exist: $path"
        }
    }

    $valid = Start-Marker -Name 'valid'
    $loaded = Invoke-Broker -Executable $Injector -Identity $valid.Identity
    Assert-Response $loaded 0 'injected' 'renderer-active'
    if (-not $loaded.Response.cleanupComplete) {
        throw 'Successful injection did not report complete cleanup.'
    }
    if ([string]$loaded.Response.rendererEvidence -notmatch '^[0-9a-f]{624}$') {
        throw 'Successful injection omitted canonical renderer activation evidence.'
    }
    Assert-EvidenceModuleLoad $loaded 2

    $duplicate = Invoke-Broker -Executable $Injector -Identity $valid.Identity
    Assert-Response $duplicate 0 'injected' 'renderer-active'
    if (-not $duplicate.Response.cleanupComplete) {
        throw 'Already-loaded renderer verification did not release its helper-owned reference.'
    }
    Assert-EvidenceModuleLoad $duplicate 3

    $valid.Process.WaitForExit(5000)
    if (-not $valid.Process.HasExited -or $valid.Process.ExitCode -ne 0) {
        throw 'Marker target did not observe the fixed adjacent module.'
    }

    Copy-Item -LiteralPath $fixedModule -Destination $moduleBackup -Force
    Copy-Item -LiteralPath $QuietModule -Destination $fixedModule -Force
    $moduleReplaced = $true
    $quiet = Start-Marker -Name 'renderer-quiet-skip'
    $quietResponse = Invoke-Broker -Executable $Injector -Identity $quiet.Identity
    Assert-Response $quietResponse 0 'skipped' 'process-excluded'
    if (-not $quietResponse.Response.cleanupComplete -or
        [string]$quietResponse.Response.rendererEvidence -notmatch '^[0-9a-f]{624}$') {
        throw 'Renderer quiet skip did not return verified bounded evidence and cleanup.'
    }
    Assert-EvidenceModuleLoad $quietResponse 2
    $quiet.Process.WaitForExit(7000)
    if (-not $quiet.Process.HasExited -or $quiet.Process.ExitCode -ne 7) {
        throw 'Explicit renderer skip left the fixed renderer resident in the target.'
    }

    $quietPreloaded = Start-Marker -Name 'renderer-quiet-skip-preloaded' -PreloadModule $fixedModule
    $quietPreloadedResponse = Invoke-Broker -Executable $Injector -Identity $quietPreloaded.Identity
    Assert-Response $quietPreloadedResponse 0 'skipped' 'process-excluded'
    if (-not $quietPreloadedResponse.Response.cleanupComplete) {
        throw 'Preloaded quiet renderer verification did not release its helper-owned reference.'
    }
    Assert-EvidenceModuleLoad $quietPreloadedResponse 3
    $quietPreloaded.Process.WaitForExit(5000)
    if (-not $quietPreloaded.Process.HasExited -or $quietPreloaded.Process.ExitCode -ne 0) {
        throw 'Preloaded quiet renderer lost the target-owned module reference.'
    }
    Copy-Item -LiteralPath $moduleBackup -Destination $fixedModule -Force
    Remove-Item -LiteralPath $moduleBackup -Force
    $moduleReplaced = $false

    if ([IO.Path]::GetFileName($DecoyModule) -ne [IO.Path]::GetFileName($fixedModule) -or
        [IO.Path]::GetFullPath($DecoyModule) -eq [IO.Path]::GetFullPath($fixedModule)) {
        throw 'Decoy fixture must use the fixed basename from a different directory.'
    }
    $decoy = Start-Marker -Name 'same-basename-decoy' -PreloadModule $DecoyModule
    Start-Sleep -Milliseconds 1800
    if (Test-Path -LiteralPath $decoy.ResultPath) {
        $prematureResult = Get-Content -LiteralPath $decoy.ResultPath -Raw | ConvertFrom-Json
        if ($prematureResult.loaded) {
            throw 'Marker target accepted a same-basename module from the wrong full path.'
        }
    }
    $decoyResponse = Invoke-Broker -Executable $Injector -Identity $decoy.Identity
    Assert-Response $decoyResponse 2 'rejected' 'conflicting-mactype-module-loaded'
    $decoy.Process.Refresh()
    if ($decoy.Process.HasExited) {
        throw 'Injector damaged the target after detecting a same-basename conflict.'
    }
    $decoy.Process.WaitForExit(7000)
    if (-not $decoy.Process.HasExited -or $decoy.Process.ExitCode -ne 7) {
        throw 'Conflict target did not remain healthy with the expected module absent.'
    }

    $afterConflict = Start-Marker -Name 'after-same-basename-conflict'
    $afterConflictResponse = Invoke-Broker -Executable $Injector -Identity $afterConflict.Identity
    Assert-Response $afterConflictResponse 0 'injected' 'renderer-active'
    $afterConflict.Process.WaitForExit(5000)
    if (-not $afterConflict.Process.HasExited -or $afterConflict.Process.ExitCode -ne 0) {
        throw 'A clean process did not recover to normal injection after a conflict.'
    }

    $wrongCreation = Start-Marker -Name 'wrong-creation'
    $wrongCreationIdentity = $wrongCreation.Identity.PSObject.Copy()
    $wrongCreationIdentity.creationTime = [Int64]$wrongCreationIdentity.creationTime + 1
    $creationResponse = Invoke-Broker -Executable $Injector -Identity $wrongCreationIdentity
    Assert-Response $creationResponse 2 'rejected' 'creation-time-mismatch'

    $wrongHandle = Start-Marker -Name 'wrong-handle'
    $handleResponse = Invoke-Broker -Executable $Injector -Identity $wrongHandle.Identity `
        -HandlePid $PID
    Assert-Response $handleResponse 2 'rejected' 'process-handle-pid-mismatch'

    $invalidHandleArguments = @(
        '--process-handle', '4',
        '--pid', [string]$wrongHandle.Identity.pid,
        '--creation-time', [string]$wrongHandle.Identity.creationTime,
        '--session-id', [string]$wrongHandle.Identity.sessionId,
        '--generation-id', $generation,
        '--profile-digest', $profileDigest
    )
    $invalidHandleJson = & $Injector $invalidHandleArguments
    $invalidHandleResponse = [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Json = [string]$invalidHandleJson
        Response = ([string]$invalidHandleJson | ConvertFrom-Json)
    }
    Assert-Response $invalidHandleResponse 2 'rejected' 'process-handle-invalid'

    $wrongSession = Start-Marker -Name 'wrong-session'
    $wrongSessionIdentity = $wrongSession.Identity.PSObject.Copy()
    $wrongSessionIdentity.sessionId = [UInt32]$wrongSessionIdentity.sessionId + 1
    $sessionResponse = Invoke-Broker -Executable $Injector -Identity $wrongSessionIdentity
    Assert-Response $sessionResponse 2 'rejected' 'session-mismatch'

    $pathResponse = Invoke-Broker -Executable $Injector -Identity $wrongSession.Identity `
        -ExtraArguments @('--dll', 'C:\untrusted\evil.dll')
    Assert-Response $pathResponse 2 'rejected' 'invalid-request'

    if (-not [Environment]::Is64BitOperatingSystem) {
        throw 'The architecture-mismatch integration contract requires 64-bit Windows.'
    }
    $oppositeExecutable = if ($Architecture -eq 'x64') {
        Join-Path $env:WINDIR 'SysWOW64\cmd.exe'
    } else {
        Join-Path $env:WINDIR 'System32\cmd.exe'
    }
    $opposite = Start-Process -FilePath $oppositeExecutable `
        -ArgumentList '/d /c ping -n 10 127.0.0.1 >nul' -PassThru
    $processes.Add($opposite)
    $opposite.Refresh()
    $oppositeIdentity = [pscustomobject]@{
        pid = $opposite.Id
        creationTime = $opposite.StartTime.ToUniversalTime().ToFileTimeUtc()
        sessionId = $opposite.SessionId
    }
    $architectureResponse = Invoke-Broker -Executable $Injector -Identity $oppositeIdentity
    Assert-Response $architectureResponse 0 'skipped' 'architecture-mismatch'

    Copy-Item -LiteralPath $fixedModule -Destination $moduleBackup -Force
    Copy-Item -LiteralPath $SlowModule -Destination $fixedModule -Force
    $moduleReplaced = $true
    $slow = Start-Marker -Name 'late-load'
    $timeoutResponse = Invoke-Broker -Executable $TimeoutInjector -Identity $slow.Identity
    Assert-Response $timeoutResponse 0 'injected' 'renderer-active'
    if (-not $timeoutResponse.Response.cleanupComplete) {
        throw 'Verified late injection did not release remote memory and handles.'
    }
    $slow.Process.WaitForExit(5000)
    if (-not $slow.Process.HasExited -or $slow.Process.ExitCode -ne 0) {
        throw 'Marker target did not observe the verified late module load.'
    }
} finally {
    foreach ($process in $processes) {
        if ($process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            [void]$process.WaitForExit(5000)
        }
    }
    if ($moduleReplaced -and (Test-Path -LiteralPath $moduleBackup)) {
        Copy-Item -LiteralPath $moduleBackup -Destination $fixedModule -Force
    }
    Remove-Item -LiteralPath $moduleBackup -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
