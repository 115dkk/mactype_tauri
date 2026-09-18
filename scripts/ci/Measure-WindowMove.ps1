[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,
    [Parameter(Mandatory)]
    [string] $PreviewHelper,
    # An installation root with MacType.dll and MacType.ini. When omitted, a
    # root is assembled from the helper build's fake MacType.dll, which is
    # enough for the helper to render specimens.
    [string] $InstallationRoot,
    [string] $OutputRoot = 'artifacts/window-move-probe',
    [string] $Skins = 'classic,console,fluent,cupertino',
    [string] $Views = 'overview',
    [int] $Steps = 240,
    [int] $IntervalMs = 8,
    [int] $Amplitude = 120,
    [int] $Rounds = 2,
    [int] $CdpPort = 9333,
    [switch] $Drag,
    [int] $TimeoutSeconds = 120
)

# One-off measurement of window-move smoothness per skin. Launches the
# Control Center with WebView2 remote debugging on, then hands the process to
# window-move-probe.mjs, which switches skins and drives the window through
# Move-Window.ps1. The app runs against a sandboxed LOCALAPPDATA so the
# machine's own preferences and event log are never touched.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedPreviewHelper = (Resolve-Path -LiteralPath $PreviewHelper).Path
$outputRoot = Join-Path $root $OutputRoot
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$probeRoot = Join-Path $env:TEMP ("mactype-window-move-" + [Guid]::NewGuid().ToString('N'))
$fixtureRoot = Join-Path $probeRoot 'installation'
$localAppData = Join-Path $probeRoot 'localappdata'
New-Item -ItemType Directory -Force -Path $fixtureRoot, (Join-Path $fixtureRoot 'ini'), $localAppData | Out-Null

if ($InstallationRoot) {
    $source = (Resolve-Path -LiteralPath $InstallationRoot).Path
    foreach ($name in @('MacLoader.exe', 'MacType.dll', 'MacLoader64.exe', 'MacType64.dll', 'MacType.ini')) {
        $file = Join-Path $source $name
        if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination (Join-Path $fixtureRoot $name) -Force }
    }
    $sourceIni = Join-Path $source 'ini'
    if (Test-Path -LiteralPath $sourceIni) {
        Get-ChildItem -LiteralPath $sourceIni -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $fixtureRoot 'ini') -Recurse -Force }
    }
} else {
    $helperDirectory = Split-Path -Parent $resolvedPreviewHelper
    $fakeCore = Join-Path $helperDirectory 'MacType.dll'
    if (-not (Test-Path -LiteralPath $fakeCore)) { throw "No installation root was given and the helper build has no fake MacType.dll at $fakeCore (build the helper with -DBUILD_TESTING=ON)." }
    Copy-Item -LiteralPath $fakeCore -Destination (Join-Path $fixtureRoot 'MacType.dll') -Force
    $fakeHook = Join-Path $helperDirectory 'EasyHK32.dll'
    if (Test-Path -LiteralPath $fakeHook) { Copy-Item -LiteralPath $fakeHook -Destination (Join-Path $fixtureRoot 'EasyHK32.dll') -Force }
}
$defaultProfile = Join-Path $fixtureRoot 'ini\Default.ini'
if (-not (Test-Path -LiteralPath $defaultProfile)) {
    "[General]`r`nDirectWrite=0`r`nFontSubstitutes=0`r`n`r`n[FreeType]`r`nNormalWeight=0`r`nGammaValue=1.0`r`n" | Set-Content -LiteralPath $defaultProfile -Encoding utf8NoBOM
}
$globalConfig = Join-Path $fixtureRoot 'MacType.ini'
if (-not (Test-Path -LiteralPath $globalConfig)) { "[General]`r`nAlternativeFile=ini\Default.ini`r`n" | Set-Content -LiteralPath $globalConfig -Encoding ascii }

$previousLocalAppData = $env:LOCALAPPDATA
$process = $null
try {
    $env:MACTYPE_HOME = $fixtureRoot
    $env:MACTYPE_PREVIEW_HELPER = $resolvedPreviewHelper
    $env:LOCALAPPDATA = $localAppData
    $env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS = "--remote-debugging-port=$CdpPort"
    Remove-Item Env:MACTYPE_CI_SMOKE_FILE -ErrorAction SilentlyContinue
    $process = Start-Process -FilePath $resolvedExecutable -ArgumentList @('--ci-view', 'overview') -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $endpoint = $null
    while ([DateTime]::UtcNow -lt $deadline -and -not $endpoint) {
        if ($process.HasExited) { throw "The Control Center exited before the probe could attach (exit code $($process.ExitCode)); is another instance running?" }
        try { $endpoint = Invoke-RestMethod -Uri "http://127.0.0.1:$CdpPort/json/version" -TimeoutSec 2 } catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $endpoint) { throw "WebView2 remote debugging did not answer on port $CdpPort within $TimeoutSeconds seconds." }
    Write-Host "Attached to $($endpoint.Browser) (pid $($process.Id))"

    $arguments = @((Join-Path $root 'scripts\ci\window-move-probe.mjs'), '--cdp', "http://127.0.0.1:$CdpPort", '--pid', $process.Id, '--out', $outputRoot, '--skins', $Skins, '--views', $Views, '--steps', $Steps, '--interval', $IntervalMs, '--amplitude', $Amplitude, '--rounds', $Rounds)
    if ($Drag) { $arguments += '--drag' }
    & node @arguments
    if ($LASTEXITCODE -ne 0) { throw "window-move-probe.mjs failed with exit code $LASTEXITCODE." }
}
finally {
    if ($process -and -not $process.HasExited) { $process.Kill($true) }
    $env:LOCALAPPDATA = $previousLocalAppData
    Remove-Item Env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_HOME -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_PREVIEW_HELPER -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $probeRoot -Recurse -Force -ErrorAction SilentlyContinue
}
