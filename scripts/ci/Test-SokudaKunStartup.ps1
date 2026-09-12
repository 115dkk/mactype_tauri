[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ApplicationRoot,
    [Parameter(Mandatory)][string] $Probe,
    [Parameter(Mandatory)][string] $ReportedCore,
    [Parameter(Mandatory)][string] $CurrentCore,
    [Parameter(Mandatory)][ValidateSet('x86', 'x64')][string] $Architecture,
    [Parameter(Mandatory)][string] $OutputRoot
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'This launch-and-debug experiment requires a disposable GitHub-hosted Windows runner.'
}
if (Get-Service -Name MacTypeControlCenter -ErrorAction SilentlyContinue |
    Where-Object Status -ne Stopped) {
    throw 'An existing MacType service would contaminate the isolated startup comparison.'
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$output = [IO.Path]::GetFullPath($OutputRoot)
if (-not $output.StartsWith((Join-Path $root 'artifacts') + [IO.Path]::DirectorySeparatorChar,
    [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $output)) {
    throw 'Use a new evidence directory strictly under this checkout artifacts/ directory.'
}
$app = (Resolve-Path -LiteralPath $ApplicationRoot).Path
$debugger = (Resolve-Path -LiteralPath $Probe).Path
$reported = (Resolve-Path -LiteralPath $ReportedCore).Path
$current = (Resolve-Path -LiteralPath $CurrentCore).Path
$coreName = if ($Architecture -eq 'x86') { 'MacType.dll' } else { 'MacType64.dll' }
$loaderName = if ($Architecture -eq 'x86') { 'MacLoader.exe' } else { 'MacLoader64.exe' }
$reportedHash = (Get-FileHash (Join-Path $reported 'MacType.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
if ($reportedHash -ne 'be79ce9c8c2864aa943700d9a073a75fcb72b676278ebb16cd2c42b12158db30') {
    throw 'Reported artifact does not match the investigated x86 DLL.'
}
$profile = Get-Content -LiteralPath (Join-Path $root 'distribution/ini/Default.ini') -Raw
$cases = @(
    @{ Name = 'stock'; Core = $null; DirectWrite = 0; Aliases = $false },
    @{ Name = 'reported-dw-off'; Core = $reported; DirectWrite = 0; Aliases = $false },
    @{ Name = 'reported-dw-on'; Core = $reported; DirectWrite = 1; Aliases = $false },
    @{ Name = 'reported-aliases'; Core = $reported; DirectWrite = 1; Aliases = $true },
    @{ Name = 'current-dw-off'; Core = $current; DirectWrite = 0; Aliases = $false },
    @{ Name = 'current-dw-on'; Core = $current; DirectWrite = 1; Aliases = $false },
    @{ Name = 'current-aliases'; Core = $current; DirectWrite = 1; Aliases = $true }
)
New-Item -ItemType Directory -Path $output | Out-Null
$results = [Collections.Generic.List[object]]::new()
$errors = [Collections.Generic.List[string]]::new()
$previousLocalAppData = $env:LOCALAPPDATA
$previousDiagnostics = $env:MACTYPE_DIRECTWRITE_DIAGNOSTICS
$metadata = [ordered]@{
    sourceCommit = '864a624091fa3d7a66d2f65b26473190bd427f5c'
    targetFramework = 'net6.0-windows'
    runtimeVersion = '6.0.36'
    architecture = $Architecture
    runnerImage = $env:ImageOS
    runnerImageVersion = $env:ImageVersion
    operatingSystem = (Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber)
    testedCommit = $env:GITHUB_SHA
    debuggerSha256 = (Get-FileHash $debugger -Algorithm SHA256).Hash
    applicationSha256 = (Get-FileHash (Join-Path $app 'SokudaKun.exe') -Algorithm SHA256).Hash
    managedApplicationSha256 = (Get-FileHash (Join-Path $app 'SokudaKun.dll') -Algorithm SHA256).Hash
    reportedCoreSha256 = (Get-FileHash (Join-Path $reported $coreName) -Algorithm SHA256).Hash
    currentCoreSha256 = (Get-FileHash (Join-Path $current $coreName) -Algorithm SHA256).Hash
}
$metadata | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $output 'metadata.json') -Encoding utf8
try {
    foreach ($case in $cases) {
        $caseRoot = Join-Path $output $case.Name
        New-Item -ItemType Directory -Path $caseRoot | Out-Null
        Copy-Item -LiteralPath $app -Destination (Join-Path $caseRoot 'application') -Recurse
        $caseApp = Join-Path $caseRoot 'application/SokudaKun.exe'
        $env:LOCALAPPDATA = Join-Path $caseRoot 'local-app-data'
        New-Item -ItemType Directory -Path $env:LOCALAPPDATA | Out-Null
        $loader = 'stock'
        $expectedCore = $null
        if ($case.Core) {
            $runtime = Join-Path $caseRoot 'runtime'
            New-Item -ItemType Directory -Path $runtime | Out-Null
            foreach ($name in @('MacType.dll', 'MacType64.dll', 'MacLoader.exe', 'MacLoader64.exe')) {
                Copy-Item -LiteralPath (Join-Path $case.Core $name) -Destination (Join-Path $runtime $name)
            }
            $caseProfile = $profile -replace '(?m)^DirectWrite=\d+', "DirectWrite=$($case.DirectWrite)"
            $caseProfile = $caseProfile -replace '(?m)^HookChildProcesses=\d+', 'HookChildProcesses=0'
            if ($case.Aliases) {
                $caseProfile = $caseProfile -replace '(?m)^FontSubstitutes=\d+', 'FontSubstitutes=1'
                $caseProfile += "`r`n[FontSubstitutes]`r`nSegoe UI=Courier New`r`nArial=Courier New`r`n"
            }
            [IO.File]::WriteAllText((Join-Path $runtime 'MacType.ini'), $caseProfile, [Text.UTF8Encoding]::new($false))
            $loader = Join-Path $runtime $loaderName
            $expectedCore = Join-Path $runtime $coreName
        }
        foreach ($temperature in @('cold', 'warm')) {
            $attempt = Join-Path $caseRoot $temperature
            $env:MACTYPE_DIRECTWRITE_DIAGNOSTICS = 'sokuda-' + [guid]::NewGuid().ToString('N')
            & $debugger $caseApp $loader $attempt
            $probeExit = $LASTEXITCODE
            $observationPath = Join-Path $attempt 'observation.json'
            if (-not (Test-Path -LiteralPath $observationPath)) {
                $errors.Add("$($case.Name)/$temperature produced no debugger evidence (exit $probeExit)")
                continue
            }
            $observation = Get-Content -LiteralPath $observationPath -Raw | ConvertFrom-Json
            $targets = @($observation.processes | Where-Object target)
            $modules = @($targets | ForEach-Object modules | Where-Object {
                [IO.Path]::GetFileName($_.path) -in @('MacType.dll', 'MacType64.dll')
            } | Select-Object -ExpandProperty path -Unique)
            $faults = @($targets | ForEach-Object exceptions)
            $overflow = @($faults | Where-Object code -eq 3221225725).Count -gt 0
            $identityValid = if ($case.Core) {
                $modules.Count -eq 1 -and $modules[0] -ieq $expectedCore
            } else { $modules.Count -eq 0 }
            $ready = $targets.Count -eq 1 -and $targets[0].hookReady
            $aliasApplied = $targets.Count -eq 1 -and $targets[0].aliasApplied
            $entry = [ordered]@{
                case = $case.Name; temperature = $temperature
                healthy = $observation.healthy; stackOverflow = $overflow
                moduleIdentityValid = $identityValid; hookReady = $ready; aliasApplied = $aliasApplied
                probeExit = $probeExit; exceptions = $faults
                profileSha256 = if ($case.Core) { (Get-FileHash (Join-Path $runtime 'MacType.ini')).Hash } else { $null }
                loaderSha256 = if ($case.Core) { (Get-FileHash $loader).Hash } else { $null }
            }
            $results.Add($entry)
            if ($probeExit -ne 0 -or $targets.Count -ne 1 -or -not $observation.targetObserved -or -not $identityValid) {
                $errors.Add("$($case.Name)/$temperature failed launch/debugger/module identity checks")
            }
            if ($case.Name -eq 'stock' -or $case.Name.StartsWith('current-') -or $case.DirectWrite -eq 0) {
                if (-not $observation.healthy) { $errors.Add("$($case.Name)/$temperature did not remain responsive") }
            }
            if ($observation.healthy -and $case.DirectWrite -eq 1 -and -not $ready) {
                $errors.Add("$($case.Name)/$temperature never armed DirectWrite hooks")
            }
            if ($observation.healthy -and $case.Aliases -and -not $aliasApplied) {
                $errors.Add("$($case.Name)/$temperature never built the alias collection")
            }
            if ($case.Name.StartsWith('reported-') -and -not $observation.healthy -and -not $overflow) {
                $errors.Add("$($case.Name)/$temperature failed without the reported stack overflow")
            }
            $results | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $output 'results.json') -Encoding utf8
        }
    }
} finally {
    $env:LOCALAPPDATA = $previousLocalAppData
    $env:MACTYPE_DIRECTWRITE_DIAGNOSTICS = $previousDiagnostics
}
$reproduced = @($results | Where-Object { $_.case.StartsWith('reported-') -and $_.stackOverflow }).Count -gt 0
$summary = [ordered]@{
    reportedStackOverflowReproduced = $reproduced
    comparison = if ($reproduced -and $errors.Count -eq 0) { 'reproduced-and-current-passes' }
        elseif ($errors.Count -eq 0) { 'baseline-not-reproduced-on-this-runner' } else { 'failed-or-inconclusive' }
    errors = @($errors)
}
$summary | ConvertTo-Json -Depth 5 | Tee-Object -FilePath (Join-Path $output 'summary.json')
if ($errors.Count -gt 0) { throw ($errors -join '; ') }
