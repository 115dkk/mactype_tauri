[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64')]
    [string] $Architecture = 'x64',

    [string] $BuildRoot = 'build/service-probe-asan',

    [switch] $Interactive
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
. (Join-Path $PSScriptRoot 'lib\ProbeDesktop.ps1')
if (-not $Interactive -and -not (Test-ProbeDesktopContext)) {
    exit (Invoke-ProbeDesktopScript -ScriptPath $PSCommandPath -Parameters $PSBoundParameters -TimeoutSeconds 1800)
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$source = Join-Path $root 'tools\service-probe'
$build = [IO.Path]::GetFullPath((Join-Path $root "$BuildRoot\$Architecture"))

cmake -S $source -B $build -A $Architecture -DMACTYPE_ENABLE_ASAN=ON
cmake --build $build --config RelWithDebInfo --target renderer-unit-tests

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw 'vswhere is required to locate the MSVC AddressSanitizer runtime.'
}
$visualStudio = & $vswhere -latest -products * -property installationPath
$toolset = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $toolset) { throw 'The MSVC toolset directory is missing.' }
$runtimeArchitecture = if ($Architecture -eq 'x64') { 'x64' } else { 'x86' }
$asanRuntimeDirectory = Join-Path $toolset.FullName "bin\Hostx64\$runtimeArchitecture"
$asanRuntimeName = if ($Architecture -eq 'x64') {
    'clang_rt.asan_dynamic-x86_64.dll'
} else {
    'clang_rt.asan_dynamic-i386.dll'
}
if (-not (Test-Path -LiteralPath (Join-Path $asanRuntimeDirectory $asanRuntimeName) -PathType Leaf)) {
    throw "The MSVC AddressSanitizer runtime is missing: $asanRuntimeDirectory"
}

$originalPath = $env:PATH
$originalAsanOptions = $env:ASAN_OPTIONS
$env:ASAN_OPTIONS = 'halt_on_error=1:abort_on_error=1:detect_leaks=0'
$env:PATH = "$asanRuntimeDirectory;$originalPath"
try {
    ctest --test-dir $build -C RelWithDebInfo -L renderer-unit --output-on-failure --no-tests=error
    if ($LASTEXITCODE -ne 0) {
        throw "ASan renderer tests failed for $Architecture (exit $LASTEXITCODE)"
    }
} finally {
    $env:ASAN_OPTIONS = $originalAsanOptions
    $env:PATH = $originalPath
}

Write-Host "Renderer ASan runtime tests passed for $Architecture."
