[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Read-RepositoryFile {
    param([Parameter(Mandatory)][string] $RelativePath)

    $path = Join-Path $root $RelativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required alpha policy file is missing: $RelativePath"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-Contains {
    param(
        [Parameter(Mandatory)][string] $Content,
        [Parameter(Mandatory)][string] $Token,
        [Parameter(Mandatory)][string] $Failure
    )

    if (-not $Content.Contains($Token)) {
        throw $Failure
    }
}

$buildWorkflow = Read-RepositoryFile '.github\workflows\build.yml'
$lintWorkflow = Read-RepositoryFile '.github\workflows\lint.yml'
$cppcheckBuild = Read-RepositoryFile '.github\scripts\Build-Cppcheck.ps1'
$cppcheckGate = Read-RepositoryFile 'scripts\ci\Test-OpenCoreCppcheck.ps1'
$project = Read-RepositoryFile 'gdipp.vcxproj'
$charter = Read-RepositoryFile 'CLAUDE.md'
$lintPolicy = Read-RepositoryFile 'docs\lint-policy.md'

foreach ($workflow in @($buildWorkflow, $lintWorkflow)) {
    Assert-Contains $workflow 'branches: [directwrite, main, codex/alpha-plus-dll]' `
        'A required workflow no longer runs on alpha-plus-dll pushes.'
}

foreach ($token in @(
    'open-core-cppcheck:',
    "if: github.ref == 'refs/heads/codex/alpha-plus-dll' || github.base_ref == 'codex/alpha-plus-dll'",
    '.github/scripts/Build-Cppcheck.ps1',
    'scripts/ci/Test-OpenCoreCppcheck.ps1',
    'alpha-open-core-cppcheck'
)) {
    Assert-Contains $buildWorkflow $token "Alpha Cppcheck workflow contract is missing: $token"
}

$pinnedCommit = '502c802a69c78f3d8cfd9973aa2108ae169c73b5'
Assert-Contains $cppcheckBuild $pinnedCommit 'Cppcheck is no longer pinned to the reviewed source commit.'
Assert-Contains $cppcheckBuild "`$version -ne 'Cppcheck 2.20.0'" 'Cppcheck build no longer verifies version 2.20.0.'
Assert-Contains $cppcheckGate "`$version -ne 'Cppcheck 2.20.0'" 'Cppcheck gate no longer verifies version 2.20.0.'

foreach ($token in @(
    "Project = 'Release|Win32'",
    "Project = 'Release|x64'",
    '--enable=warning,performance,portability',
    '--error-exitcode=1',
    '--inline-suppr',
    '--include=$salHeader'
)) {
    Assert-Contains $cppcheckGate $token "Alpha Cppcheck gate is missing: $token"
}

if ($cppcheckGate.Contains('--suppress=unknownMacro')) {
    throw 'unknownMacro must be modeled explicitly; suppressing it can abort translation-unit analysis.'
}

$legacyDebtMatch = [regex]::Match(
    $cppcheckGate,
    '(?ms)\$legacyDebtIds\s*=\s*@\((?<body>.*?)\)'
)
if (-not $legacyDebtMatch.Success) {
    throw 'The named legacy Cppcheck debt list is missing.'
}
$legacyDebt = $legacyDebtMatch.Groups['body'].Value
$forbiddenSafetySuppressions = @(
    'nullPointer',
    'nullPointerRedundantCheck',
    'nullPointerOutOfMemory',
    'nullPointerOutOfResources',
    'memleak',
    'resourceLeak',
    'uninitvar',
    'uninitMemberVar',
    'uninitStructMember',
    'danglingLifetime',
    'useAfterFree',
    'invalidLifetime',
    'uselessAssignmentPtrArg'
)
foreach ($id in $forbiddenSafetySuppressions) {
    if ($legacyDebt -match "(?m)'$([regex]::Escape($id))'") {
        throw "Safety diagnostic cannot become named legacy debt: $id"
    }
}

if ($project.Contains('<ExcludedFromBuild')) {
    throw 'Rendering translation units may not be hidden from a configuration with ExcludedFromBuild.'
}

$removedFirstPartyFiles = @(
    'VersionHelper.cpp',
    'VersionHelper.h',
    'ft - non-ref.cpp',
    'wow64layer.h'
)
foreach ($relativePath in $removedFirstPartyFiles) {
    if (Test-Path -LiteralPath (Join-Path $root $relativePath)) {
        throw "Unused first-party file was restored instead of being analyzed or deleted: $relativePath"
    }
}

foreach ($token in @(
    'hide obsolete first-party source files behind lint exclusions',
    'Every first-party translation unit in `gdipp.vcxproj` stays in the project'
)) {
    $document = if ($token.StartsWith('Every')) { $lintPolicy } else { $charter }
    Assert-Contains $document $token "Alpha first-party lint policy is missing: $token"
}

Write-Host 'Alpha rendering-core lint policy passed.'
