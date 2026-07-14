[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $BaselineInstaller,
    [Parameter(Mandatory)]
    [string] $Installer
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resolvedBaselineInstaller = (Resolve-Path -LiteralPath $BaselineInstaller).Path
$resolvedInstaller = (Resolve-Path -LiteralPath $Installer).Path
$installRoot = Join-Path $env:TEMP ("mactype-independent-" + [Guid]::NewGuid().ToString('N'))
$resolvedTempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
$resolvedInstallRoot = [IO.Path]::GetFullPath($installRoot)
if (-not $resolvedInstallRoot.StartsWith($resolvedTempRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.Path]::GetFileName($resolvedInstallRoot).StartsWith('mactype-independent-', [StringComparison]::Ordinal)) {
    throw "Refusing to use unsafe installer test directory: $resolvedInstallRoot"
}
$installerArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=$installRoot")
$manualMarker = $null

function Invoke-Installer([string] $File, [string[]] $Arguments, [string] $Label) {
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -PassThru -Wait -WindowStyle Hidden
    if ($process.ExitCode -ne 0) {
        throw "$Label exited with code $($process.ExitCode)."
    }
}

try {
    Invoke-Installer -File $resolvedBaselineInstaller -Arguments $installerArguments -Label 'Baseline installer'

    $expected = @(
        'MacType Control Center.exe',
        'mactype-preview32.exe',
        'MacType.dll',
        'MacType64.dll',
        'MacType.Core.dll',
        'MacType64.Core.dll',
        'MacLoader.exe',
        'MacLoader64.exe',
        'MacType.ini',
        'ini\Default.ini',
        'languages\en.json',
        'languages\ko.json',
        'THIRD_PARTY_NOTICES.md',
        'LICENSE.txt'
    )
    foreach ($relative in $expected) {
        $path = Join-Path $installRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Independent installer omitted required file: $relative"
        }
    }

    $forbidden = @('MacTray.exe', 'MacTuner.exe', 'MacWiz.exe', 'VisTuner.exe', 'EasyHK32.dll', 'EasyHK64.dll', 'updater.exe')
    foreach ($name in $forbidden) {
        if (Get-ChildItem -LiteralPath $installRoot -Recurse -File -Filter $name) {
            throw "Independent installer contains forbidden legacy file: $name"
        }
    }

    $globalConfig = Get-Content -LiteralPath (Join-Path $installRoot 'MacType.ini') -Raw
    if ($globalConfig -notmatch 'AlternativeFile=ini\\Default.ini') {
        throw 'Independent MacType.ini does not select the new public default profile.'
    }

    & (Join-Path $root 'scripts\ci\Test-TauriWindows.ps1') `
        -Executable (Join-Path $installRoot 'MacType Control Center.exe') `
        -PreviewHelper (Join-Path $installRoot 'mactype-preview32.exe') `
        -InstallationRoot $installRoot

    & (Join-Path $root 'scripts\ci\Test-TrayWindows.ps1') `
        -Executable (Join-Path $installRoot 'MacType Control Center.exe')

    $manualTarget = Join-Path $root 'build\preview-helper\Release\manual-launch-target.exe'
    $manualMarker = Join-Path $env:TEMP ("mactype-manual-launch-" + [Guid]::NewGuid().ToString('N') + '.ready')
    $loader = Start-Process -FilePath (Join-Path $installRoot 'MacLoader.exe') -ArgumentList @($manualTarget, $manualMarker) -PassThru -WindowStyle Hidden
    if (-not $loader.WaitForExit(10000)) {
        $loader.Kill($true)
        throw 'Independent MacLoader did not exit after launching the x86 target.'
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $manualMarker) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not (Test-Path -LiteralPath $manualMarker)) {
        throw 'Independent MacLoader did not start the injected x86 target.'
    }
    $manualContent = (Get-Content -LiteralPath $manualMarker -Raw).Trim()
    Remove-Item -LiteralPath $manualMarker -Force
    if ($manualContent -ne 'mactype-manual-launch-ready') {
        throw "Manual launch target wrote an invalid marker: $manualContent"
    }

    Invoke-Installer -File $resolvedInstaller -Arguments $installerArguments -Label 'Upgrade installer'
    if (-not (Test-Path -LiteralPath (Join-Path $installRoot 'MacType Control Center.exe'))) {
        throw 'Upgrade removed the installed application.'
    }

    $uninstaller = Get-ChildItem -LiteralPath $installRoot -File -Filter 'unins*.exe' | Select-Object -First 1
    if (-not $uninstaller) { throw 'Uninstaller was not created.' }
    Invoke-Installer -File $uninstaller.FullName -Arguments @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Label 'Uninstaller'
    if (Test-Path -LiteralPath $installRoot) {
        $remaining = Get-ChildItem -LiteralPath $installRoot -Force -ErrorAction SilentlyContinue
        if ($remaining) { throw "Uninstall left files behind in $installRoot." }
    }
    Write-Host 'PASS: independent install, upgrade, launch, and uninstall'
}
finally {
    if ($manualMarker -and (Test-Path -LiteralPath $manualMarker)) {
        Remove-Item -LiteralPath $manualMarker -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $installRoot) {
        $uninstaller = Get-ChildItem -LiteralPath $installRoot -File -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($uninstaller) {
            Start-Process -FilePath $uninstaller.FullName -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Wait -WindowStyle Hidden | Out-Null
        }
        Remove-Item -LiteralPath $installRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
