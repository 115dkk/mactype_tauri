[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$required = @(
    'distribution\MacType.ini',
    'distribution\ini\Default.ini',
    'distribution\languages\en.json',
    'distribution\languages\ko.json',
    'distribution\THIRD_PARTY_NOTICES.md',
    'LICENSE'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
        throw "Distribution source file is missing: $relative"
    }
}

$binary = Get-ChildItem -LiteralPath (Join-Path $root 'distribution') -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') }
if ($binary) { throw "Prebuilt binary is forbidden in distribution/: $($binary.FullName)" }

$english = Get-Content -LiteralPath (Join-Path $root 'distribution\languages\en.json') -Raw | ConvertFrom-Json -AsHashtable
$korean = Get-Content -LiteralPath (Join-Path $root 'distribution\languages\ko.json') -Raw | ConvertFrom-Json -AsHashtable
if (Compare-Object ($english.Keys | Sort-Object) ($korean.Keys | Sort-Object)) {
    throw 'English and Korean distribution translation keys differ.'
}

$profile = Get-Content -LiteralPath (Join-Path $root 'distribution\ini\Default.ini') -Raw
foreach ($section in @('[General]', '[DirectWrite]', '[Individual]', '[Exclude]', '[ExcludeModule]')) {
    if (-not $profile.Contains($section)) { throw "Default profile is missing section $section" }
}

$buildScript = Get-Content -LiteralPath (Join-Path $root '.github\scripts\Build-LegacyCore.ps1') -Raw
foreach ($commit in @(
    'ef771574d04721baf45a1b66bfb4692193603088',
    'a457397ffa9d20e8df43e2c143c60da78c16c059',
    'd644ce94e8c7f7f5a31591577c78134ea3ac1fae',
    '667359c7967249dd9d28d8f8cef65b60e7e2d963'
)) {
    if (-not $buildScript.Contains($commit)) { throw "Core dependency is not pinned: $commit" }
}

$installer = Get-Content -LiteralPath (Join-Path $root 'installer\mactype-control-center.iss') -Raw
foreach ($legacy in @('MacTray.exe', 'MacTuner.exe', 'MacWiz.exe', 'VisTuner.exe', 'EasyHK32.dll', 'EasyHK64.dll')) {
    if ($installer.Contains($legacy)) { throw "Installer references forbidden legacy binary: $legacy" }
}
if (-not $installer.Contains('MacType64.dll') -or -not $installer.Contains('MacLoader64.exe')) {
    throw 'Installer does not contain the independent x86/x64 core set.'
}

Write-Host 'Independent distribution policy passed.'
