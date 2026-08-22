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
    [string] $OutputRoot,

    [switch] $CrossArchitecture
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$resolvedBuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path
$resolvedCoreRoot = (Resolve-Path -LiteralPath $OpenCoreRoot).Path
$resolvedProfile = (Resolve-Path -LiteralPath $Profile).Path
$fullOutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $fullOutputRoot) {
    throw "Early-injection evidence root already exists: $fullOutputRoot"
}

$suffix = if ($Architecture -eq 'x64') { '64' } else { '32' }
$coreName = if ($Architecture -eq 'x64') { 'MacType64.dll' } else { 'MacType.dll' }
$probe = Join-Path $resolvedBuildRoot "$Architecture\Release\probe-spawn-tree$suffix.exe"
$policyProbe = Join-Path $resolvedBuildRoot "$Architecture\Release\relay-policy-probe$suffix.exe"
if ($CrossArchitecture -and $Architecture -ne 'x64') {
    throw 'Cross-architecture relay evidence must start from the x64 probe.'
}
$childProbe = Join-Path $resolvedBuildRoot 'Win32\Release\probe-spawn-tree32.exe'
$packageNames = if ($CrossArchitecture) {
    @('MacType.dll', 'MacType64.dll')
} else {
    @($coreName)
}
$requiredInputs = @($probe, $policyProbe) + @($packageNames | ForEach-Object {
    Join-Path $resolvedCoreRoot $_
})
if ($CrossArchitecture) {
    $requiredInputs += $childProbe
}
foreach ($required in $requiredInputs) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required early-injection input is missing: $required"
    }
}

$runtime = Join-Path $fullOutputRoot 'runtime'
$results = Join-Path $fullOutputRoot 'results'
New-Item -ItemType Directory -Path $runtime, $results | Out-Null
$stagedCore = Join-Path $runtime $coreName
foreach ($packageName in $packageNames) {
    Copy-Item -LiteralPath (Join-Path $resolvedCoreRoot $packageName) `
        -Destination (Join-Path $runtime $packageName)
}
Copy-Item -LiteralPath $resolvedProfile -Destination (Join-Path $runtime 'MacType.ini')

$manifest = Join-Path $results 'tree.json'
$probeArguments = @('--out', $manifest, '--wait-ms', '50', '--preload-mactype', $stagedCore)
if ($CrossArchitecture) {
    $probeArguments += @('--child-exe', $childProbe, '--grandchild-exe', $probe)
}
& $probe @probeArguments
if ($LASTEXITCODE -ne 0) {
    throw "Early-injection spawn tree failed for $Architecture with exit code $LASTEXITCODE"
}

$tree = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if (-not $tree.childLaunched -or $tree.childExitCode -ne 0 -or $tree.nodes.Count -ne 3) {
    throw "Early-injection spawn tree was incomplete for $Architecture"
}

foreach ($node in $tree.nodes) {
    if (-not $node.present) {
        throw "Early-injection node artifact is missing for $Architecture`: $($node.role)"
    }
    $observation = Get-Content -LiteralPath $node.artifact -Raw | ConvertFrom-Json
    $expectedArchitecture = if ($CrossArchitecture -and $node.role -eq 'child') {
        'x86'
    } elseif ($Architecture -eq 'x64') {
        'x64'
    } else {
        'x86'
    }
    $expectedNodeCore = if ($expectedArchitecture -eq 'x64') {
        'MacType64.dll'
    } else {
        'MacType.dll'
    }
    $expectedCorePath = [System.IO.Path]::GetFullPath((Join-Path $runtime $expectedNodeCore))
    if ($observation.architecture -ne $expectedArchitecture) {
        throw "Early-injection node architecture mismatch for $($node.role)"
    }
    if (-not $observation.mactypeModuleLoaded) {
        throw "MacType was not present at observation time in $($node.role)"
    }
    if (-not [System.IO.Path]::GetFullPath($observation.mactypeModulePath).Equals(
        $expectedCorePath, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected MacType generation in $($node.role): $($observation.mactypeModulePath)"
    }
}

$retiredRuntime = Join-Path $fullOutputRoot 'retired-runtime'
New-Item -ItemType Directory -Path $retiredRuntime | Out-Null
foreach ($packageName in $packageNames) {
    Copy-Item -LiteralPath (Join-Path $resolvedCoreRoot $packageName) `
        -Destination (Join-Path $retiredRuntime $packageName)
}
$retiredProfile = Join-Path $retiredRuntime 'MacType.ini'
Copy-Item -LiteralPath $resolvedProfile -Destination $retiredProfile
$retiredCore = Join-Path $retiredRuntime $coreName
$retiredManifest = Join-Path $results 'retired-tree.json'
$retiredArguments = @(
    '--out', $retiredManifest,
    '--wait-ms', '50',
    '--preload-mactype', $retiredCore,
    '--retire-profile-before-child', $retiredProfile
)
if ($CrossArchitecture) {
    $retiredArguments += @(
        '--child-exe', $childProbe,
        '--grandchild-exe', $probe
    )
}
& $probe @retiredArguments
if ($LASTEXITCODE -ne 0) {
    throw "Retired-generation child relay failed for $Architecture with exit code $LASTEXITCODE"
}
if (Test-Path -LiteralPath $retiredProfile) {
    throw "Retired-generation fixture did not remove its profile for $Architecture"
}
$retiredTree = Get-Content -LiteralPath $retiredManifest -Raw | ConvertFrom-Json
if (-not $retiredTree.childLaunched -or $retiredTree.childExitCode -ne 0 -or
    $retiredTree.nodes.Count -ne 3) {
    throw "Retired-generation child tree was incomplete for $Architecture"
}
foreach ($node in $retiredTree.nodes) {
    if (-not $node.present) {
        throw "Retired-generation node artifact is missing for $Architecture`: $($node.role)"
    }
    $observation = Get-Content -LiteralPath $node.artifact -Raw | ConvertFrom-Json
    if ($node.role -eq 'parent') {
        if (-not $observation.mactypeModuleLoaded) {
            throw "Retired-generation parent did not load MacType for $Architecture"
        }
        if (-not [System.IO.Path]::GetFullPath($observation.mactypeModulePath).Equals(
            [System.IO.Path]::GetFullPath($retiredCore),
            [StringComparison]::OrdinalIgnoreCase)) {
            throw "Retired-generation parent loaded the wrong module for $Architecture"
        }
    } elseif ($observation.mactypeModuleLoaded) {
        throw "Retired generation propagated into the $($node.role) process for $Architecture"
    }
}

& $policyProbe $stagedCore
if ($LASTEXITCODE -ne 0) {
    throw "PID-local mitigation relay contract failed for $Architecture with exit code $LASTEXITCODE"
}

$relayKind = if ($CrossArchitecture) { 'x64/x86 mixed' } else { $Architecture }
Write-Host "Early child relay loaded the fixed generation in the $relayKind tree, stopped at a retired profile boundary, and quietly skipped only the explicitly blocked process."
