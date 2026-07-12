[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,
    [Parameter(Mandatory)]
    [string] $PreviewHelper,
    [string] $InstallationRoot,
    [int] $TimeoutSeconds = 25
)

$ErrorActionPreference = 'Stop'
$views = @('overview', 'profiles', 'execution', 'diagnostics')
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedPreviewHelper = (Resolve-Path -LiteralPath $PreviewHelper).Path
$fixtureRoot = if ($InstallationRoot) {
    (Resolve-Path -LiteralPath $InstallationRoot).Path
} else {
    Split-Path -Parent $resolvedPreviewHelper
}
$markerRoot = Join-Path $env:TEMP ("mactype-window-smoke-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $markerRoot | Out-Null

try {
    $env:MACTYPE_HOME = $fixtureRoot
    $env:MACTYPE_PREVIEW_HELPER = $resolvedPreviewHelper
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
    Remove-Item -LiteralPath $markerRoot -Recurse -Force -ErrorAction SilentlyContinue
}
