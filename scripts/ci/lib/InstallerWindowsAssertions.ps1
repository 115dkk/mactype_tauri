Import-Module (Join-Path $PSScriptRoot 'OpenServiceTestSupport.psm1')

$script:InstallerProcessTimeoutMilliseconds = 10 * 60 * 1000
$script:InstallerProcessMaximumOutputBytes = 64 * 1024
$script:InstallerProcessTerminationTimeoutMilliseconds = 5000

function Get-FixedService {
    param([Parameter(Mandatory)] [string] $Name)

    Get-CimInstance Win32_Service -Filter "Name='$Name'" -ErrorAction SilentlyContinue
}

function Get-ServiceSnapshot {
    param([Parameter(Mandatory)] [string] $Name)

    $service = Get-FixedService -Name $Name
    if (-not $service) { return $null }
    @($service.PathName, $service.StartMode, $service.StartName, $service.DisplayName, $service.State) -join "`n"
}

function Get-ServiceExecutablePath {
    param([Parameter(Mandatory)] [string] $ImagePath)

    if ($ImagePath -match '^"([^"]+)"(?:\s.*)?$') { return $Matches[1] }
    ($ImagePath -split '\s+', 2)[0]
}

function Invoke-ProcessExit {
    param(
        [Parameter(Mandatory)] [string] $File,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [ValidateRange(1, 600000)]
        [int] $TimeoutMilliseconds = $script:InstallerProcessTimeoutMilliseconds
    )

    $resolvedFile = (Resolve-Path -LiteralPath $File).Path
    [MacType.ControlCenter.Ci.BoundedProcessRunner]::RunArguments(
        $resolvedFile,
        $Arguments,
        $null,
        $TimeoutMilliseconds,
        $script:InstallerProcessMaximumOutputBytes,
        $script:InstallerProcessTerminationTimeoutMilliseconds
    )
}

function Invoke-ExpectedSuccess {
    param(
        [Parameter(Mandatory)] [string] $File,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [Parameter(Mandatory)] [string] $Label
    )

    $result = Invoke-ProcessExit -File $File -Arguments $Arguments
    if ($result.ExitCode -ne 0) {
        throw "$Label exited with code $($result.ExitCode). " +
            "stdout=$($result.StandardOutput) stderr=$($result.StandardError)"
    }
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory)] [string] $File,
        [Parameter(Mandatory)] [string[]] $Arguments,
        [Parameter(Mandatory)] [string] $Label
    )

    $result = Invoke-ProcessExit -File $File -Arguments $Arguments
    if ($result.ExitCode -eq 0) {
        throw "$Label unexpectedly succeeded. " +
            "stdout=$($result.StandardOutput) stderr=$($result.StandardError)"
    }
}

function Initialize-UserMarkers {
    param(
        [Parameter(Mandatory)] [string] $UserMarkerRoot,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $UserMarkers
    )

    foreach ($entry in $UserMarkers.GetEnumerator()) {
        $path = Join-Path $UserMarkerRoot $entry.Key
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, $entry.Value, [Text.UTF8Encoding]::new($false))
    }
}

function Assert-UserMarkers {
    param(
        [Parameter(Mandatory)] [string] $UserMarkerRoot,
        [Parameter(Mandatory)] [System.Collections.IDictionary] $UserMarkers
    )

    foreach ($entry in $UserMarkers.GetEnumerator()) {
        $path = Join-Path $UserMarkerRoot $entry.Key
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Installer removed LocalAppData user state: $($entry.Key)"
        }
        if ([IO.File]::ReadAllText($path) -cne $entry.Value) {
            throw "Installer changed LocalAppData user state: $($entry.Key)"
        }
    }
}

function Get-TreeSnapshot {
    param([Parameter(Mandatory)] [string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Cannot snapshot missing directory: $Path"
    }
    @(
        Get-ChildItem -LiteralPath $Path -Recurse -File | Sort-Object FullName | ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($Path, $_.FullName)
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$relative|$($_.Length)|$hash"
        }
    ) -join "`n"
}

function Get-ApplicationUsabilitySnapshot {
    param([Parameter(Mandatory)] [string] $ApplicationRoot)

    @(
        'MacType Control Center.exe',
        'service-runtime\mactype-service-setup.exe'
    ) | ForEach-Object {
        $path = Join-Path $ApplicationRoot $_
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Required application entry point is missing: $_"
        }
        "$_|$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant())"
    } | Sort-Object | Join-String -Separator "`n"
}

function Assert-ServicePayload {
    param([Parameter(Mandatory)] [string] $ApplicationRoot)

    $runtimeRoot = Join-Path $ApplicationRoot 'service-runtime'
    $manifestPath = Join-Path $runtimeRoot 'payload\manifest.json'
    $payloadFiles = Join-Path $runtimeRoot 'payload\files'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
    if ($manifest.Count -ne 3 -or $manifest.schema -ne 1 -or $manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+') {
        throw 'Installed open-service manifest violates the fixed schema/version contract.'
    }
    $expectedNames = @('mactype-service.exe', 'mactype-injector32.exe', 'mactype-injector64.exe', 'MacType.dll', 'MacType64.dll')
    if (Compare-Object ($expectedNames | Sort-Object) ($manifest.files.Keys | Sort-Object)) {
        throw 'Installed open-service manifest contains a missing or unapproved payload filename.'
    }
    $installedNames = @(Get-ChildItem -LiteralPath $payloadFiles -File | Select-Object -ExpandProperty Name)
    if (Compare-Object ($expectedNames | Sort-Object) ($installedNames | Sort-Object)) {
        throw 'Installed open-service payload directory contains a missing or unapproved file.'
    }
    foreach ($name in $expectedNames) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $payloadFiles $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($manifest.files[$name] -cne "sha256:$hash") {
            throw "Installed open-service payload hash does not match manifest: $name"
        }
    }
    $manifest
}

function Assert-RequiredApplicationFiles {
    param([Parameter(Mandatory)] [string] $ApplicationRoot)

    $expected = @(
        'MacType Control Center.exe',
        'mactype-preview32.exe',
        'MacType.dll',
        'MacType64.dll',
        'MacType.Core.dll',
        'MacType64.Core.dll',
        'MacLoader.exe',
        'MacLoader64.exe',
        'MacType.ini',
        'ini\Default.ini',
        'languages\en.json',
        'languages\ko.json',
        'THIRD_PARTY_NOTICES.md',
        'LICENSE.txt',
        'service-runtime\mactype-service-setup.exe',
        'service-runtime\payload\manifest.json',
        'service-runtime\payload\files\mactype-service.exe',
        'service-runtime\payload\files\mactype-injector32.exe',
        'service-runtime\payload\files\mactype-injector64.exe',
        'service-runtime\payload\files\MacType.dll',
        'service-runtime\payload\files\MacType64.dll'
    )
    foreach ($relative in $expected) {
        if (-not (Test-Path -LiteralPath (Join-Path $ApplicationRoot $relative) -PathType Leaf)) {
            throw "Installer omitted required file: $relative"
        }
    }
}

function Assert-CommonDesktopShortcut {
    param(
        [Parameter(Mandatory)] [string] $CommonDesktopShortcut,
        [Parameter(Mandatory)] [string] $ApplicationRoot
    )

    if (-not (Test-Path -LiteralPath $CommonDesktopShortcut -PathType Leaf)) {
        throw 'Admin installer did not create the requested common desktop shortcut.'
    }
    $shell = New-Object -ComObject WScript.Shell
    try {
        $shortcut = $shell.CreateShortcut($CommonDesktopShortcut)
        $expected = [IO.Path]::GetFullPath((Join-Path $ApplicationRoot 'MacType Control Center.exe'))
        $actual = [IO.Path]::GetFullPath($shortcut.TargetPath)
        if (-not $expected.Equals($actual, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Common desktop shortcut targets '$actual' instead of '$expected'."
        }
    }
    finally {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
    }
}

function Assert-ReadyOpenService {
    param(
        [Parameter(Mandatory)] [hashtable] $PayloadManifest,
        [Parameter(Mandatory)] [string] $OpenServiceName,
        [Parameter(Mandatory)] [string] $ServiceRoot,
        [Parameter(Mandatory)] [string] $ProfileRoot,
        [Parameter(Mandatory)] [string] $DistributionDefaultProfilePath
    )

    $service = Get-FixedService -Name $OpenServiceName
    if (-not $service) { throw 'Installer did not register the fixed open service.' }
    $current = Get-Content -LiteralPath (Join-Path $ServiceRoot 'current.json') -Raw | ConvertFrom-Json
    if ($current.schema -ne 1 -or $current.version -cne $PayloadManifest.version) {
        throw 'Active runtime pointer does not select the bundled runtime version.'
    }
    $generationRoot = Join-Path $ServiceRoot ("bin\" + $current.version)
    $expectedImage = [IO.Path]::GetFullPath((Join-Path $generationRoot 'mactype-service.exe'))
    $actualImage = Get-ServiceExecutablePath -ImagePath $service.PathName
    if (-not $expectedImage.Equals([IO.Path]::GetFullPath($actualImage), [StringComparison]::OrdinalIgnoreCase)) {
        throw "Open service image '$actualImage' is not the active protected runtime '$expectedImage'."
    }
    if ($service.StartMode -ne 'Auto' -or $service.StartName -ne 'LocalSystem' -or $service.State -ne 'Running') {
        throw "Open service is not Auto/LocalSystem/Running: $($service.StartMode)/$($service.StartName)/$($service.State)"
    }

    $activePointerPath = Join-Path $ProfileRoot 'active.json'
    $activeBytes = [IO.File]::ReadAllBytes($activePointerPath)
    $active = [Text.Encoding]::UTF8.GetString($activeBytes) | ConvertFrom-Json
    if ($active.schema -ne 1 -or $active.generation -notmatch '^sha256:[0-9a-f]{64}$') {
        throw 'Protected active profile pointer is invalid.'
    }
    $profileGeneration = Join-Path $ProfileRoot ("generations\" + $active.generation.Substring(7))
    $profilePath = Join-Path $profileGeneration 'profile.ini'
    if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        throw 'Protected active profile generation is missing.'
    }
    $expectedDefaultHash = (Get-FileHash -LiteralPath $DistributionDefaultProfilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $activeProfileHash = (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ("sha256:$activeProfileHash" -cne $active.generation -or $activeProfileHash -cne $expectedDefaultHash) {
        throw 'Fresh bootstrap did not publish the exact bundled default profile.'
    }
    if (-not [Linq.Enumerable]::SequenceEqual(
        [IO.File]::ReadAllBytes((Join-Path $generationRoot 'MacType.ini')),
        [IO.File]::ReadAllBytes($profilePath)
    )) {
        throw 'Runtime-adjacent profile does not exactly match the protected active profile.'
    }

    $receipt = Get-Content -LiteralPath (Join-Path $ServiceRoot ("runtime-receipts\" + $current.version + '.json')) -Raw | ConvertFrom-Json -AsHashtable
    if ($receipt.schema -ne 1 -or $receipt.version -cne $current.version) {
        throw 'Installed runtime receipt is invalid.'
    }
    foreach ($entry in $receipt.files.GetEnumerator()) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $generationRoot $entry.Key) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($entry.Value -cne "sha256:$hash") { throw "Installed runtime differs from receipt: $($entry.Key)" }
    }

    $health = Get-Content -LiteralPath (Join-Path $ServiceRoot 'health.json') -Raw | ConvertFrom-Json
    if ($health.protocolVersion -ne 1 -or $health.serviceVersion -cne $current.version -or $health.health -cne 'ready' -or $health.activeProfileDigest -cne $active.generation -or $null -ne $health.lastError) {
        throw 'Installer returned before strict Ready health for the exact active profile.'
    }
    foreach ($component in @('profile', 'observer', 'injector32', 'injector64')) {
        if ($health.readiness.$component -cne 'ready') {
            throw "Required Ready component was not ready: $component"
        }
    }
    [pscustomobject]@{
        ActivePointerBytes = [Convert]::ToBase64String($activeBytes)
        ActiveGeneration = $active.generation
        ProfileGenerationRoot = $profileGeneration
        ProfileGenerationSnapshot = Get-TreeSnapshot -Path $profileGeneration
        RuntimeVersion = $current.version
    }
}

function Assert-BaselineRestoredAfterFailedUpgrade {
    param(
        [Parameter(Mandatory)] [pscustomobject] $Baseline,
        [Parameter(Mandatory)] [string] $BaselineApplicationSnapshot,
        [Parameter(Mandatory)] [string] $BaselineServiceSnapshot,
        [Parameter(Mandatory)] [string] $BaselineRuntimeSnapshot,
        [Parameter(Mandatory)] [string] $ApplicationRoot,
        [Parameter(Mandatory)] [string] $OpenServiceName,
        [Parameter(Mandatory)] [string] $ServiceRoot,
        [Parameter(Mandatory)] [string] $ProfileRoot
    )

    Assert-RequiredApplicationFiles -ApplicationRoot $ApplicationRoot
    if ((Get-ApplicationUsabilitySnapshot -ApplicationRoot $ApplicationRoot) -cne $BaselineApplicationSnapshot) {
        throw 'Failed upgrade did not preserve the existing app and protected broker entry points.'
    }
    if ((Get-ServiceSnapshot -Name $OpenServiceName) -cne $BaselineServiceSnapshot) {
        throw 'Failed upgrade did not restore the exact baseline service configuration and state.'
    }
    $current = Get-Content -LiteralPath (Join-Path $ServiceRoot 'current.json') -Raw | ConvertFrom-Json
    if ($current.schema -ne 1 -or $current.version -cne $Baseline.RuntimeVersion) {
        throw 'Failed upgrade did not restore the baseline active runtime pointer.'
    }
    $baselineRuntimeRoot = Join-Path $ServiceRoot ("bin\" + $Baseline.RuntimeVersion)
    if ((Get-TreeSnapshot -Path $baselineRuntimeRoot) -cne $BaselineRuntimeSnapshot) {
        throw 'Failed upgrade changed the immutable baseline runtime generation.'
    }
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $ProfileRoot 'active.json'))) -cne $Baseline.ActivePointerBytes -or
        (Get-TreeSnapshot -Path $Baseline.ProfileGenerationRoot) -cne $Baseline.ProfileGenerationSnapshot) {
        throw 'Failed upgrade did not preserve the exact protected profile generation.'
    }
    $health = Get-Content -LiteralPath (Join-Path $ServiceRoot 'health.json') -Raw | ConvertFrom-Json
    if ($health.protocolVersion -ne 1 -or $health.serviceVersion -cne $Baseline.RuntimeVersion -or
        $health.health -cne 'ready' -or $health.activeProfileDigest -cne $Baseline.ActiveGeneration -or
        $null -ne $health.lastError) {
        throw 'Failed upgrade did not restore strict Ready health for the baseline runtime/profile.'
    }
    foreach ($component in @('profile', 'observer', 'injector32', 'injector64')) {
        if ($health.readiness.$component -cne 'ready') {
            throw "Failed upgrade left baseline readiness incomplete: $component"
        }
    }
}

function New-ForeignService {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $DisplayName
    )

    $image = '"' + (Join-Path $env:SystemRoot 'System32\cmd.exe') + '" /c exit 0'
    & sc.exe create $Name 'binPath=' $image 'start=' 'demand' 'DisplayName=' $DisplayName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Could not create test service $Name." }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Get-FixedService -Name $Name) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Get-FixedService -Name $Name)) { throw "Test service $Name did not become observable." }
}

function Remove-TestService {
    param([Parameter(Mandatory)] [string] $Name)

    if (-not (Get-FixedService -Name $Name)) { return }
    & sc.exe stop $Name | Out-Null
    & sc.exe delete $Name | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Get-FixedService -Name $Name) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (Get-FixedService -Name $Name) { throw "Test service $Name could not be removed." }
}
