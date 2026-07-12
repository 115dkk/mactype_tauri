[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [switch] $SkipInstall
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$helperBuild = Join-Path $root 'build\preview-helper'
$helperOutput = Join-Path $helperBuild "$Configuration\mactype-preview32.exe"

cmake -S (Join-Path $root 'preview-helper') -B $helperBuild -A Win32 -DBUILD_TESTING=ON
cmake --build $helperBuild --config $Configuration --parallel
ctest --test-dir $helperBuild -C $Configuration --output-on-failure

Push-Location (Join-Path $root 'control-center')
try {
    if (-not $SkipInstall) { pnpm install --frozen-lockfile }
    pnpm build
    pnpm tauri build --no-bundle
}
finally {
    Pop-Location
}

$app = Join-Path $root 'control-center\src-tauri\target\release\mactype-control-center.exe'
if ($Configuration -eq 'Debug') {
    $app = Join-Path $root 'control-center\src-tauri\target\debug\mactype-control-center.exe'
}
if (-not (Test-Path -LiteralPath $app)) { throw "Tauri executable missing: $app" }
if (-not (Test-Path -LiteralPath $helperOutput)) { throw "Preview helper missing: $helperOutput" }

[pscustomobject]@{ App = $app; PreviewHelper = $helperOutput }
