[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\WorkflowModel.psm1') -Force

function Test-ServicePayloadVersionPolicy {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Workflow,
        [Parameter(Mandatory)] [string] $Repo
    )

    $failures = [System.Collections.Generic.List[string]]::new()
    $builder = Get-Content -LiteralPath (Join-Path $Repo '.github\scripts\Build-ServiceRuntime.ps1') -Raw
    $hostBuilder = Get-Content -LiteralPath (Join-Path $Repo 'service-runtime\host\build.rs') -Raw
    $hostScm = Get-Content -LiteralPath (Join-Path $Repo 'service-runtime\host\src\scm.rs') -Raw

    foreach ($token in @(
        "-split '[+-]', 2",
        '$packageBaseVersion',
        'runtime package base version',
        '(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?'
    )) {
        if (-not $builder.Contains($token)) {
            $failures.Add("Service runtime builder does not preserve immutable SemVer generations safely: $token")
        }
    }
    foreach ($token in @(
        'MACTYPE_SERVICE_RUNTIME_VERSION',
        '$env:MACTYPE_SERVICE_RUNTIME_VERSION = $Version',
        'finally'
    )) {
        if (-not $builder.Contains($token)) {
            $failures.Add("Service runtime builder does not bind the host binary to the payload generation: $token")
        }
    }
    foreach ($token in @(
        'cargo:rerun-if-env-changed=MACTYPE_SERVICE_RUNTIME_VERSION',
        'MACTYPE_COMPILED_SERVICE_RUNTIME_VERSION',
        'CARGO_PKG_VERSION',
        '!matches!(version, "." | "..")'
    )) {
        if (-not $hostBuilder.Contains($token)) {
            $failures.Add("Service host build contract does not preserve the requested generation or development fallback: $token")
        }
    }
    if (($hostScm | Select-String -Pattern 'env!\("CARGO_PKG_VERSION"\)' -AllMatches).Matches.Count -ne 0 -or
        ($hostScm | Select-String -Pattern 'service_runtime_version\(\)' -AllMatches).Matches.Count -lt 2) {
        $failures.Add('Service health does not consistently report the compiled payload generation.')
    }

    $windowsBuild = Get-WorkflowJob -Workflow $Workflow -Id 'windows-build'
    if (-not $windowsBuild) {
        $failures.Add("Main build omits the guarded job 'windows-build'.")
        return $failures.ToArray()
    }
    $payloadStep = @(Get-WorkflowStep -Job $windowsBuild `
        -NameLike 'Build distinct immutable service payloads*') | Select-Object -First 1
    if (-not $payloadStep) {
        $failures.Add('Main build omits the distinct immutable service payload build step.')
        return $failures.ToArray()
    }
    foreach ($token in @(
        '${{ github.run_id }}',
        '${{ github.sha }}',
        '0.2.0+ci.',
        'artifacts/service-runtime-baseline',
        'artifacts/service-runtime-failing-upgrade',
        'artifacts/service-runtime-current',
        '$baselineRuntimeVersion',
        '$failingRuntimeVersion',
        '$currentRuntimeVersion'
    )) {
        if (-not $payloadStep.Run.Contains($token)) {
            $failures.Add("Main build does not bind service payload generations to run and commit identity: $token")
        }
    }
    foreach ($generation in @(
        @{ Path = 'artifacts/service-runtime-baseline'; Version = '$baselineRuntimeVersion' },
        @{ Path = 'artifacts/service-runtime-current'; Version = '$currentRuntimeVersion' }
    )) {
        $matchingLine = @($payloadStep.Run.Split("`n") | Where-Object {
            $_.Contains('Build-ServiceRuntime.ps1') -and
            $_.Contains($generation.Path) -and
            $_.Contains($generation.Version)
        })
        if ($matchingLine.Count -eq 0) {
            $failures.Add('Installer baseline and current payloads are not built as distinct immutable generations.')
            break
        }
    }

    return $failures.ToArray()
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workflow = Read-GitHubWorkflow -Path (Join-Path $root '.github\workflows\build.yml')
$failures = @(Test-ServicePayloadVersionPolicy -Workflow $workflow -Repo $root)
if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "ERROR: $_" -ForegroundColor Red }
    throw "Service payload immutable-version policy failed with $($failures.Count) violation(s)."
}

Write-Host 'Service payload immutable-version policy passed.'
