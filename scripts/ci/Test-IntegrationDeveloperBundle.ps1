[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$fixtureRoot = [IO.Path]::GetFullPath((Join-Path $root 'artifacts\integration-bundle-contract-fixture'))
$artifactPrefix = [IO.Path]::GetFullPath((Join-Path $root 'artifacts')).TrimEnd('\') + '\'
if (-not $fixtureRoot.StartsWith($artifactPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Fixture root escaped the artifacts directory: $fixtureRoot"
}

try {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
    $input = Join-Path $fixtureRoot 'input'
    $core = Join-Path $input 'core'
    $runtimeFiles = Join-Path $input 'service-runtime\payload\files'
    New-Item -ItemType Directory -Path $core, $runtimeFiles -Force | Out-Null

    $app = Join-Path $input 'MacType Control Center.exe'
    $preview = Join-Path $input 'mactype-preview32.exe'
    Set-Content -LiteralPath $app -Value 'fixture app'
    Set-Content -LiteralPath $preview -Value 'fixture preview'
    foreach ($name in @(
        'MacType.dll',
        'MacType64.dll',
        'MacType.Core.dll',
        'MacType64.Core.dll',
        'MacLoader.exe',
        'MacLoader64.exe'
    )) {
        Set-Content -LiteralPath (Join-Path $core $name) -Value "fixture $name"
    }
    $runtime = Join-Path $input 'service-runtime'
    Set-Content -LiteralPath (Join-Path $runtime 'mactype-service-setup.exe') -Value 'fixture setup'
    $payloadNames = @(
        'mactype-service.exe',
        'mactype-injector32.exe',
        'mactype-injector64.exe',
        'MacType.dll',
        'MacType64.dll'
    )
    foreach ($name in $payloadNames) {
        Set-Content -LiteralPath (Join-Path $runtimeFiles $name) -Value "fixture $name"
    }
    $manifestFiles = [ordered]@{}
    foreach ($name in $payloadNames) {
        $manifestFiles[$name] = 'sha256:fixture'
    }
    [ordered]@{ schema = 1; version = 'fixture'; files = $manifestFiles } |
        ConvertTo-Json -Depth 4 -Compress |
        Set-Content -LiteralPath (Join-Path $runtime 'payload\manifest.json') -Encoding utf8NoBOM

    $outputRelative = 'artifacts\integration-bundle-contract-fixture\output'
    & (Join-Path $root '.github\scripts\Build-IntegrationDeveloperBundle.ps1') `
        -ApplicationExe $app `
        -PreviewHelper $preview `
        -CoreRoot $core `
        -ServiceRuntimeRoot $runtime `
        -OutputRoot $outputRelative

    $output = Join-Path $root $outputRelative
    foreach ($relative in @(
        'README.md',
        'installation-tree\MacType Control Center.exe',
        'installation-tree\mactype-preview32.exe',
        'installation-tree\MacType.dll',
        'installation-tree\MacType64.dll',
        'installation-tree\MacType.Core.dll',
        'installation-tree\MacType64.Core.dll',
        'installation-tree\MacLoader.exe',
        'installation-tree\MacLoader64.exe',
        'installation-tree\MacType.ini',
        'installation-tree\ini\Default.ini',
        'installation-tree\languages\en.json',
        'installation-tree\LICENSE.txt',
        'installation-tree\THIRD_PARTY_NOTICES.md',
        'installation-tree\service-runtime\mactype-service-setup.exe',
        'installation-tree\service-runtime\payload\manifest.json',
        'installation-tree\service-runtime\payload\files\mactype-service.exe',
        'installation-tree\service-runtime\payload\files\mactype-injector32.exe',
        'installation-tree\service-runtime\payload\files\mactype-injector64.exe',
        'installation-tree\service-runtime\payload\files\MacType.dll',
        'installation-tree\service-runtime\payload\files\MacType64.dll'
    )) {
        if (-not (Test-Path -LiteralPath (Join-Path $output $relative) -PathType Leaf)) {
            throw "Integration/Developer bundle fixture is missing: $relative"
        }
    }
    $readme = Get-Content -LiteralPath (Join-Path $output 'README.md') -Raw
    foreach ($token in @(
        'not a portable or independently installed product',
        'HKLM64\SOFTWARE\MacType\ControlCenter',
        'does not request UAC or attempt rollback'
    )) {
        if (-not $readme.Contains($token)) {
            throw "Integration/Developer README is missing its boundary warning: $token"
        }
    }

    Write-Host 'Integration/Developer bundle layout contract passed.'
}
finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}
