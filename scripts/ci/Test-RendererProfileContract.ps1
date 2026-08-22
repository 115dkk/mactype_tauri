[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Win32', 'x64')]
    [string] $Architecture,

    [Parameter(Mandatory)]
    [string] $BuildRoot,

    [Parameter(Mandatory)]
    [string] $OpenCoreRoot
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resolvedBuildRoot = [IO.Path]::GetFullPath((Join-Path $root $BuildRoot))
$resolvedOpenCore = (Resolve-Path -LiteralPath $OpenCoreRoot).Path
$suffix = if ($Architecture -eq 'x64') { '64' } else { '32' }
$coreName = if ($Architecture -eq 'x64') { 'MacType64.dll' } else { 'MacType.dll' }
$probe = Join-Path $resolvedBuildRoot "$Architecture\Release\renderer-profile-probe$suffix.exe"
$sourceCore = Join-Path $resolvedOpenCore $coreName
foreach ($required in @($probe, $sourceCore)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Renderer profile contract input is missing: $required"
    }
}

$testRoot = Join-Path $resolvedBuildRoot "renderer-profile-contract-$Architecture"
$runtimeCore = Join-Path $testRoot $coreName
$runtimeProfile = Join-Path $testRoot 'MacType.ini'
$knownNames = @($coreName, 'MacType.ini')

function Remove-TestRoot {
    if (-not (Test-Path -LiteralPath $testRoot)) { return }
    $unexpected = @(Get-ChildItem -LiteralPath $testRoot -Force | Where-Object {
        $_.Name -notin $knownNames
    })
    if ($unexpected.Count -ne 0) {
        throw "Refusing to clean renderer profile test directory with unexpected contents: $($unexpected.FullName -join ', ')"
    }
    foreach ($path in @($runtimeProfile, $runtimeCore)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    Remove-Item -LiteralPath $testRoot -Force
}

function Assert-Rejected([string] $Case) {
    & $probe $runtimeCore reject
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer accepted invalid profile case '$Case' (probe exit $LASTEXITCODE)."
    }
}

function Assert-Loaded([string] $Case) {
    & $probe $runtimeCore load
    if ($LASTEXITCODE -ne 0) {
        throw "Renderer rejected valid profile case '$Case' (probe exit $LASTEXITCODE)."
    }
}

Remove-TestRoot
New-Item -ItemType Directory -Path $testRoot | Out-Null
try {
    Copy-Item -LiteralPath $sourceCore -Destination $runtimeCore

    Assert-Rejected 'missing'

    [IO.File]::WriteAllText(
        $runtimeProfile,
        "[General]`r`nDirectWrite=0`r`nFontSubstitutes=0`r`nHookChildProcesses=0`r`n"
    )
    Assert-Loaded 'minimal General profile'

    [IO.File]::WriteAllBytes($runtimeProfile, [byte[]]::new(0))
    Assert-Rejected 'empty'

    [IO.File]::WriteAllText($runtimeProfile, "[Broken]`r`nValue=1`r`n")
    Assert-Rejected 'missing General section'

    [IO.File]::WriteAllText(
        $runtimeProfile,
        "[General]`r`nAlternativeFile=missing-profile.ini`r`n"
    )
    Assert-Rejected 'missing selected profile'

    $oversized = [IO.File]::Open($runtimeProfile, [IO.FileMode]::Create, [IO.FileAccess]::Write)
    try { $oversized.SetLength(4MB + 1) } finally { $oversized.Dispose() }
    Assert-Rejected 'oversized'
} finally {
    Remove-TestRoot
}

Write-Host "Renderer profile fail-closed contract passed for $Architecture."
