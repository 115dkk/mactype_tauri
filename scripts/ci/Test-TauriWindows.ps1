[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,
    [Parameter(Mandatory)]
    [string] $PreviewHelper,
    [Parameter(Mandatory)]
    [string] $InstallationRoot,
    [int] $TimeoutSeconds = 25
)

$ErrorActionPreference = 'Stop'
$views = @('overview', 'files', 'profiles', 'execution', 'diagnostics')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedPreviewHelper = (Resolve-Path -LiteralPath $PreviewHelper).Path
$resolvedInstallation = (Resolve-Path -LiteralPath $InstallationRoot).Path
$manualTarget = Join-Path (Split-Path -Parent $resolvedPreviewHelper) 'manual-launch-target.exe'
if (-not (Test-Path -LiteralPath $manualTarget)) {
    $manualTarget = Join-Path $root 'build\preview-helper\Release\manual-launch-target.exe'
}
if (-not (Test-Path -LiteralPath $manualTarget)) { throw "Manual launch smoke target is missing: $manualTarget" }
$markerRoot = Join-Path $env:TEMP ("mactype-window-smoke-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $markerRoot | Out-Null
$fixtureRoot = Join-Path $markerRoot 'installation'
$localAppData = Join-Path $markerRoot 'localappdata'
New-Item -ItemType Directory -Force -Path $fixtureRoot, (Join-Path $fixtureRoot 'ini'), $localAppData | Out-Null
foreach ($name in @('MacLoader.exe', 'MacType.dll', 'MacLoader64.exe', 'MacType64.dll')) {
    $source = Join-Path $resolvedInstallation $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $fixtureRoot $name) -Force }
}
foreach ($required in @('MacLoader.exe', 'MacType.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $fixtureRoot $required))) { throw "Installation smoke fixture is missing $required." }
}
$sourceIni = Join-Path $resolvedInstallation 'ini'
if (Test-Path -LiteralPath $sourceIni) {
    Get-ChildItem -LiteralPath $sourceIni -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $fixtureRoot 'ini') -Recurse -Force
    }
}
$defaultProfile = Join-Path $fixtureRoot 'ini\Default.ini'
if (-not (Test-Path -LiteralPath $defaultProfile)) { "[FreeType]`r`nNormalWeight=0`r`nGammaValue=1.0`r`n" | Set-Content -LiteralPath $defaultProfile -Encoding utf8NoBOM }
$globalConfig = Join-Path $fixtureRoot 'MacType.ini'
if (-not (Test-Path -LiteralPath $globalConfig)) { "[General]`r`nAlternativeFile=ini\Default.ini`r`n" | Set-Content -LiteralPath $globalConfig -Encoding ascii }
$previousLocalAppData = $env:LOCALAPPDATA

try {
    $env:MACTYPE_HOME = $fixtureRoot
    $env:MACTYPE_PREVIEW_HELPER = $resolvedPreviewHelper
    $env:MACTYPE_CI_MANUAL_TARGET = $manualTarget
    $env:LOCALAPPDATA = $localAppData
    foreach ($view in $views) {
        $marker = Join-Path $markerRoot "$view.ready"
        $env:MACTYPE_CI_SMOKE_FILE = $marker
        $process = Start-Process -FilePath $resolvedExecutable -ArgumentList @('--ci-view', $view) -PassThru -WindowStyle Hidden
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while (-not $process.HasExited -and -not (Test-Path -LiteralPath $marker) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 200
            $process.Refresh()
        }
        if (-not (Test-Path -LiteralPath $marker)) {
            if (-not $process.HasExited) { $process.Kill($true) }
            throw "Window '$view' did not report frontend readiness within $TimeoutSeconds seconds."
        }
        $content = Get-Content -LiteralPath $marker -Raw
        if ($content.Trim() -ne "ready:$view") {
            throw "Window '$view' wrote an invalid readiness marker: $content"
        }
        if (-not $process.WaitForExit(5000)) {
            $process.Kill($true)
            throw "Window '$view' did not close after its smoke test."
        }
        if ($process.ExitCode -ne 0) {
            throw "Window '$view' exited with code $($process.ExitCode)."
        }
        Write-Host "PASS: $view"
    }
}
finally {
    Remove-Item Env:MACTYPE_CI_SMOKE_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_HOME -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_PREVIEW_HELPER -ErrorAction SilentlyContinue
    Remove-Item Env:MACTYPE_CI_MANUAL_TARGET -ErrorAction SilentlyContinue
    if ($null -eq $previousLocalAppData) { Remove-Item Env:LOCALAPPDATA -ErrorAction SilentlyContinue } else { $env:LOCALAPPDATA = $previousLocalAppData }
    Remove-Item -LiteralPath $markerRoot -Recurse -Force -ErrorAction SilentlyContinue
}
