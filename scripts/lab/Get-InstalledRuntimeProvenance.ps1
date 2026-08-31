[CmdletBinding()]
param(
    [string] $ServiceRoot = (Join-Path $env:ProgramFiles 'MacType Control Center\Service'),

    [string] $ProfileRoot = (Join-Path $env:ProgramData 'MacType\ControlCenter'),

    [string] $ExpectedManifest,

    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
$maximumJsonBytes = 64KB
$requiredRuntimeFiles = @(
    'mactype-service.exe',
    'mactype-injector32.exe',
    'mactype-injector64.exe',
    'MacType.dll',
    'MacType64.dll'
)
$issues = [Collections.Generic.List[string]]::new()

function Read-BoundedJson([string] $Path, [string] $Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description is missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -le 0 -or $item.Length -gt $maximumJsonBytes) {
        throw "$Description has an invalid size: $Path"
    }
    return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
}

function Get-LowerSha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-SafeVersion([string] $Version) {
    return -not [string]::IsNullOrWhiteSpace($Version) -and
        $Version.Length -le 64 -and
        $Version -notin @('.', '..') -and
        $Version -cmatch '^[A-Za-z0-9.+-]+$'
}

function Get-ManifestHashes($Manifest, [string] $Description) {
    if ($Manifest.schema -ne 1 -or -not (Test-SafeVersion ([string] $Manifest.version))) {
        throw "$Description has an invalid schema or version."
    }
    $hashes = [ordered]@{}
    foreach ($name in $requiredRuntimeFiles) {
        $property = $Manifest.files.PSObject.Properties[$name]
        if (-not $property -or
            [string]$property.Value -cnotmatch '^sha256:[0-9a-f]{64}$') {
            throw "$Description is missing a valid hash for $name."
        }
        $hashes[$name] = ([string] $property.Value).Substring(7)
    }
    return $hashes
}

function Get-GeneralSetting([string] $Path, [string] $Key) {
    $section = ''
    foreach ($rawLine in [IO.File]::ReadAllLines($Path)) {
        $line = $rawLine.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith(';') -or $line.StartsWith('#')) {
            continue
        }
        if ($line.StartsWith('[') -and $line.EndsWith(']')) {
            $section = $line.Substring(1, $line.Length - 2).Trim()
            continue
        }
        if (-not $section.Equals('General', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $separator = $line.IndexOf('=')
        if ($separator -le 0) {
            continue
        }
        if ($line.Substring(0, $separator).Trim().Equals(
            $Key,
            [StringComparison]::OrdinalIgnoreCase
        )) {
            return $line.Substring($separator + 1).Trim()
        }
    }
    return $null
}

$serviceRootPath = [IO.Path]::GetFullPath($ServiceRoot)
$profileRootPath = [IO.Path]::GetFullPath($ProfileRoot)
$current = Read-BoundedJson (
    Join-Path $serviceRootPath 'current.json'
) 'protected runtime pointer'
if ($current.schema -ne 1 -or -not (Test-SafeVersion ([string] $current.version))) {
    throw 'The protected runtime pointer has an invalid schema or version.'
}
$version = [string] $current.version
$runtimeRoot = Join-Path (Join-Path $serviceRootPath 'bin') $version
$receiptPath = Join-Path (
    Join-Path $serviceRootPath 'runtime-receipts'
) "$version.json"
$receipt = Read-BoundedJson $receiptPath 'protected runtime receipt'
$receiptHashes = Get-ManifestHashes $receipt 'protected runtime receipt'
if ([string] $receipt.version -cne $version) {
    $issues.Add('runtime-receipt-version-mismatch')
}

$runtimeFiles = [ordered]@{}
$receiptVerified = $receipt.version -ceq $version
foreach ($name in $requiredRuntimeFiles) {
    $path = Join-Path $runtimeRoot $name
    $exists = Test-Path -LiteralPath $path -PathType Leaf
    $actualHash = if ($exists) { Get-LowerSha256 $path } else { $null }
    $matchesReceipt = $exists -and $actualHash -ceq $receiptHashes[$name]
    if (-not $matchesReceipt) {
        $receiptVerified = $false
        $issues.Add("runtime-receipt-mismatch:$name")
    }
    $runtimeFiles[$name] = [ordered]@{
        path = $path
        sha256 = $actualHash
        receiptSha256 = $receiptHashes[$name]
        matchesReceipt = $matchesReceipt
    }
}

$releaseRequested = -not [string]::IsNullOrWhiteSpace($ExpectedManifest)
$releaseVersion = $null
$releaseMatches = $null
if ($releaseRequested) {
    $expectedPath = [IO.Path]::GetFullPath($ExpectedManifest)
    $expected = Read-BoundedJson $expectedPath 'expected release manifest'
    $expectedHashes = Get-ManifestHashes $expected 'expected release manifest'
    $releaseVersion = [string] $expected.version
    $releaseMatches = $releaseVersion -ceq $version
    foreach ($name in $requiredRuntimeFiles) {
        if ($runtimeFiles[$name].sha256 -cne $expectedHashes[$name]) {
            $releaseMatches = $false
        }
    }
    if (-not $releaseMatches) {
        $issues.Add('installed-runtime-does-not-match-expected-release')
    }
}

$activePointer = Read-BoundedJson (
    Join-Path $profileRootPath 'active.json'
) 'active profile pointer'
$generation = [string] $activePointer.generation
if ($activePointer.schema -ne 1 -or
    $generation -cnotmatch '^sha256:[0-9a-f]{64}$') {
    throw 'The active profile pointer has an invalid schema or generation digest.'
}
$profileDigest = $generation.Substring(7)
$profilePath = Join-Path (
    Join-Path (Join-Path $profileRootPath 'generations') $profileDigest
) 'profile.ini'
if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    throw "The active profile generation is missing: $profilePath"
}
$profileItem = Get-Item -LiteralPath $profilePath
if ($profileItem.Length -le 0 -or $profileItem.Length -gt 1MB) {
    throw "The active profile generation has an invalid size: $profilePath"
}
$actualProfileDigest = Get-LowerSha256 $profilePath
$profileDigestVerified = $actualProfileDigest -ceq $profileDigest
if (-not $profileDigestVerified) {
    $issues.Add('active-profile-digest-mismatch')
}
$unityValue = Get-GeneralSetting $profilePath 'UnityFontHook'
$unityMode = 0
if ($null -ne $unityValue -and
    (-not [int]::TryParse($unityValue, [ref] $unityMode) -or
        $unityMode -lt 0 -or $unityMode -gt 3)) {
    $unityMode = 0
    $issues.Add('active-profile-invalid-unity-mode')
}
$childValue = Get-GeneralSetting $profilePath 'HookChildProcesses'
$hookChildProcesses = $childValue -eq '1'
$privateFreeTypeValue = Get-GeneralSetting $profilePath 'SkipPrivateFreeType'
$skipPrivateFreeType = $privateFreeTypeValue -eq '1'

$serviceState = $null
$serviceImagePath = $null
try {
    $service = Get-CimInstance Win32_Service -Filter "Name='MacTypeControlCenter'" -ErrorAction Stop
    $serviceState = [string] $service.State
    $serviceImagePath = [string] $service.PathName
} catch {
    $serviceState = 'unavailable'
}

$evidence = [ordered]@{
    schema = 1
    kind = 'mactype-installed-runtime-provenance'
    capturedAt = (Get-Date).ToUniversalTime().ToString('O')
    service = [ordered]@{
        state = $serviceState
        imagePath = $serviceImagePath
        root = $serviceRootPath
        version = $version
        runtimeRoot = $runtimeRoot
        receiptPath = $receiptPath
        receiptVerified = $receiptVerified
        files = $runtimeFiles
    }
    release = [ordered]@{
        requested = $releaseRequested
        expectedManifest = if ($releaseRequested) {
            [IO.Path]::GetFullPath($ExpectedManifest)
        } else {
            $null
        }
        expectedVersion = $releaseVersion
        matches = $releaseMatches
    }
    profile = [ordered]@{
        root = $profileRootPath
        generation = $generation
        path = $profilePath
        sha256 = $actualProfileDigest
        digestVerified = $profileDigestVerified
        unityFontHook = $unityMode
        hookChildProcesses = $hookChildProcesses
        skipPrivateFreeType = $skipPrivateFreeType
    }
    issues = $issues.ToArray()
}
$json = $evidence | ConvertTo-Json -Depth 8

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $output = [IO.Path]::GetFullPath($OutputPath)
    if (Test-Path -LiteralPath $output) {
        throw "Runtime provenance evidence already exists: $output"
    }
    $outputRoot = Split-Path -Parent $output
    if (-not (Test-Path -LiteralPath $outputRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $outputRoot | Out-Null
    }
    [IO.File]::WriteAllText($output, $json, [Text.UTF8Encoding]::new($false))
}

$json
