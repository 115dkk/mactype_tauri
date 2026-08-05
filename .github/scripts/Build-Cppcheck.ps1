[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$sourceRoot = Join-Path $root 'build\cppcheck-source'
$buildRoot = Join-Path $root 'build\cppcheck-tool'
$repository = 'https://github.com/cppcheck-opensource/cppcheck.git'
$commit = '502c802a69c78f3d8cfd9973aa2108ae169c73b5' # Cppcheck 2.20.0

if (-not (Test-Path -LiteralPath $sourceRoot)) {
    git clone --filter=blob:none --no-checkout $repository $sourceRoot
}

git -C $sourceRoot fetch --depth 1 origin $commit
git -C $sourceRoot checkout --detach $commit
$actualCommit = (git -C $sourceRoot rev-parse HEAD).Trim().ToLowerInvariant()
if ($actualCommit -ne $commit) {
    throw "Cppcheck source commit mismatch: expected $commit, got $actualCommit"
}

cmake `
    -S $sourceRoot `
    -B $buildRoot `
    -A x64 `
    -DBUILD_GUI=OFF `
    -DBUILD_TESTS=OFF `
    -DFILESDIR=OFF `
    -DUSE_MATCHCOMPILER=ON
cmake --build $buildRoot --config Release --parallel

$cppcheck = Join-Path $buildRoot 'bin\Release\cppcheck.exe'
if (-not (Test-Path -LiteralPath $cppcheck -PathType Leaf)) {
    throw "Pinned Cppcheck executable was not produced: $cppcheck"
}

$version = (& $cppcheck --version).Trim()
if ($version -ne 'Cppcheck 2.20.0') {
    throw "Cppcheck version mismatch: expected Cppcheck 2.20.0, got $version"
}

Write-Host "Built $version from pinned commit $actualCommit."
