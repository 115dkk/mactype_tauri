[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string] $Architecture,

    [Parameter(Mandatory = $true)]
    [string] $BuildRoot,

    [Parameter(Mandatory = $true)]
    [string] $OpenCoreRoot,

    [Parameter(Mandatory = $true)]
    [string] $Profile,

    [Parameter(Mandatory = $true)]
    [string] $OutputRoot
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$resolvedBuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
$resolvedCoreRoot = (Resolve-Path -LiteralPath $OpenCoreRoot).Path
$resolvedProfile = (Resolve-Path -LiteralPath $Profile).Path
$fullOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $fullOutputRoot) {
    throw "DWriteCore evidence root already exists: $fullOutputRoot"
}

$suffix = if ($Architecture -eq 'x64') { '64' } else { '32' }
$coreName = if ($Architecture -eq 'x64') { 'MacType64.dll' } else { 'MacType.dll' }
$buildOutput = Join-Path $resolvedBuildRoot "$Architecture\Release"
$probe = Join-Path $buildOutput "dwritecore-contract-probe$suffix.exe"
$proxy = Join-Path $buildOutput 'DWriteCore.dll'
$sourceCore = Join-Path $resolvedCoreRoot $coreName
foreach ($required in @($probe, $proxy, $sourceCore)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required DWriteCore contract input is missing: $required"
    }
}

$runtime = Join-Path $fullOutputRoot 'runtime'
New-Item -ItemType Directory -Path $runtime | Out-Null
$stagedCore = Join-Path $runtime $coreName
$stagedProxy = Join-Path $runtime 'DWriteCore.dll'
Copy-Item -LiteralPath $sourceCore -Destination $stagedCore
Copy-Item -LiteralPath $proxy -Destination $stagedProxy
Copy-Item -LiteralPath $resolvedProfile -Destination (Join-Path $runtime 'MacType.ini')

& $probe $stagedCore $stagedProxy
if ($LASTEXITCODE -ne 0) {
    throw "DWriteCore hook contract failed for $Architecture with exit code $LASTEXITCODE"
}

[ordered]@{
    schemaVersion = 1
    architecture = if ($Architecture -eq 'x64') { 'x64' } else { 'x86' }
    dynamicFactoryLookupIntercepted = $true
    factoryCreated = $true
    core = $coreName
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $fullOutputRoot 'result.json') -Encoding utf8

Write-Host "DWriteCore dynamic factory lookup and creation passed for $Architecture."
