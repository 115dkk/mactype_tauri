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
$detoursRoot = Join-Path $dependencyRoot 'Detours'
$wow64extRoot = Join-Path $dependencyRoot 'wow64ext'

New-Item -ItemType Directory -Force -Path $dependencyRoot, $libraryRoot, $artifactRoot | Out-Null

if (-not (Test-Path -LiteralPath $freetypeRoot)) {
    git clone --depth 1 https://github.com/snowie2000/freetype.git $freetypeRoot
}
if (-not (Test-Path -LiteralPath $iniParserRoot)) {
    git clone --depth 1 https://github.com/snowie2000/IniParser.git $iniParserRoot
}
if (-not (Test-Path -LiteralPath $detoursRoot)) {
    git clone --depth 1 https://github.com/microsoft/Detours.git $detoursRoot
}
if (-not (Test-Path -LiteralPath $wow64extRoot)) {
    git clone --depth 1 https://github.com/snowie2000/rewolf-wow64ext.git $wow64extRoot
}

function Build-Freetype([string] $Platform, [string] $BuildName, [string] $OutputName) {
    $buildPath = Join-Path $dependencyRoot $BuildName
    cmake -S $freetypeRoot -B $buildPath -A $Platform -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
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

$detoursSolution = Join-Path $detoursRoot 'vc\Detours.sln'
msbuild $detoursSolution /m /t:Rebuild /p:Configuration=ReleaseMD /p:Platform=x86 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
$detours32 = Get-ChildItem -LiteralPath $detoursRoot -Recurse -File -Filter 'detours.lib' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $detours32) { throw 'Detours x86 library was not produced.' }
Copy-Item -LiteralPath $detours32.FullName -Destination (Join-Path $libraryRoot 'detours.lib') -Force
Copy-Item -LiteralPath $detours32.FullName -Destination (Join-Path $root 'detours.lib') -Force

msbuild $detoursSolution /m /t:Rebuild /p:Configuration=ReleaseMD /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
$detours64 = Get-ChildItem -LiteralPath $detoursRoot -Recurse -File -Filter 'detours.lib' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $detours64) { throw 'Detours x64 library was not produced.' }
Copy-Item -LiteralPath $detours64.FullName -Destination (Join-Path $libraryRoot 'detours64.lib') -Force
Copy-Item -LiteralPath $detours64.FullName -Destination (Join-Path $root 'detours64.lib') -Force

$wow64Solution = Join-Path $wow64extRoot 'src\wow64ext.sln'
msbuild $wow64Solution /m /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
$wow64Library = Get-ChildItem -LiteralPath $wow64extRoot -Recurse -File -Filter 'wow64ext.lib' | Sort-Object Length -Descending | Select-Object -First 1
if (-not $wow64Library) { throw 'wow64ext x86 library was not produced.' }
Copy-Item -LiteralPath $wow64Library.FullName -Destination (Join-Path $libraryRoot 'wow64ext.lib') -Force

$env:FREETYPE_PATH = $freetypeRoot
$env:INI_PARSER_PATH = $iniParserRoot
$solution = Join-Path $root 'gdipp.sln'
msbuild $solution /m /t:Rebuild '/p:Configuration=Rel+Detours' /p:Platform=Win32 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
msbuild $solution /m /t:Rebuild '/p:Configuration=Rel+Detours' /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0

$expected = @(
    (Join-Path $root 'Rel+Detours\MacType.Core.dll'),
    (Join-Path $root 'Release\macloader.exe'),
    (Join-Path $root 'x64\Rel+Detours\MacType64.Core.dll'),
    (Join-Path $root 'x64\Release\macloader64.exe')
)
foreach ($file in $expected) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Expected core artifact missing: $file" }
    Copy-Item -LiteralPath $file -Destination $artifactRoot -Force
}

Write-Host "Legacy core source build produced $($expected.Count) verified artifacts."
