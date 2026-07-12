[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,
    [int] $TimeoutSeconds = 25
)

$ErrorActionPreference = 'Stop'
$views = @('overview', 'profiles', 'diagnostics')
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$markerRoot = Join-Path $env:TEMP ("mactype-window-smoke-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $markerRoot | Out-Null

try {
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
    Remove-Item -LiteralPath $markerRoot -Recurse -Force -ErrorAction SilentlyContinue
}
