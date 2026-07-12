[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dependencyRoot = Join-Path $root 'build\core-dependencies'
$libraryRoot = Join-Path $root 'deps\lib'
$artifactRoot = Join-Path $root 'artifacts\legacy-core'
$freetypeRoot = Join-Path $dependencyRoot 'freetype'
$iniParserRoot = Join-Path $dependencyRoot 'IniParser'

New-Item -ItemType Directory -Force -Path $dependencyRoot, $libraryRoot, $artifactRoot | Out-Null

if (-not (Test-Path -LiteralPath $freetypeRoot)) {
    git clone --depth 1 https://github.com/snowie2000/freetype.git $freetypeRoot
}
if (-not (Test-Path -LiteralPath $iniParserRoot)) {
    git clone --depth 1 https://github.com/snowie2000/IniParser.git $iniParserRoot
}

function Build-Freetype([string] $Platform, [string] $BuildName, [string] $OutputName) {
    $buildPath = Join-Path $dependencyRoot $BuildName
    cmake -S $freetypeRoot -B $buildPath -A $Platform -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
    cmake --build $buildPath --config Release --parallel
    $library = Get-ChildItem -LiteralPath $buildPath -Recurse -File -Filter 'freetype.lib' | Select-Object -First 1
    if (-not $library) { throw "FreeType library was not produced for $Platform" }
    Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $libraryRoot $OutputName) -Force
}

Build-Freetype -Platform Win32 -BuildName 'freetype-x86' -OutputName 'freetype.lib'
Build-Freetype -Platform x64 -BuildName 'freetype-x64' -OutputName 'freetype64.lib'

$iniSolution = Join-Path $iniParserRoot 'IniParser.sln'
msbuild $iniSolution /m /t:Rebuild /p:Configuration=Release /p:Platform=x86 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
msbuild $iniSolution /m /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0

$ini32 = Get-ChildItem -LiteralPath $iniParserRoot -Recurse -File -Filter 'IniParser.lib' | Where-Object { $_.FullName -notmatch '[\\/]x64[\\/]' } | Select-Object -First 1
$ini64 = Get-ChildItem -LiteralPath $iniParserRoot -Recurse -File -Filter 'IniParser64.lib' | Select-Object -First 1
if (-not $ini32 -or -not $ini64) { throw 'IniParser x86/x64 libraries were not produced.' }
Copy-Item -LiteralPath $ini32.FullName -Destination (Join-Path $libraryRoot 'iniparser.lib') -Force
Copy-Item -LiteralPath $ini64.FullName -Destination (Join-Path $libraryRoot 'iniparser64.lib') -Force

$env:FREETYPE_PATH = $freetypeRoot
$env:INI_PARSER_PATH = Join-Path $iniParserRoot 'IniParser'
$solution = Join-Path $root 'gdipp.sln'
msbuild $solution /m /t:Rebuild '/p:Configuration=Rel+Detours' /p:Platform=Win32 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
msbuild $solution /m /t:Rebuild '/p:Configuration=Rel+Detours' /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0

$expected = @(
    (Join-Path $root 'Rel+Detours\MacType.Core.dll'),
    (Join-Path $root 'Rel+Detours\macloader.exe'),
    (Join-Path $root 'x64\Rel+Detours\MacType64.Core.dll'),
    (Join-Path $root 'x64\Rel+Detours\macloader64.exe')
)
foreach ($file in $expected) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Expected core artifact missing: $file" }
    Copy-Item -LiteralPath $file -Destination $artifactRoot -Force
}

Write-Host "Legacy core source build produced $($expected.Count) verified artifacts."
