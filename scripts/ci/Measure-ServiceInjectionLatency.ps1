[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Marker32,

    [Parameter(Mandatory)]
    [string] $Marker64,

    [string] $WindowMarker32,

    [string] $WindowMarker64,

    [int] $SingleRounds = 6,

    [int] $Burst64 = 12,

    [int] $Burst32 = 4
)

# Reports how long the installed service takes to reach a program that no
# injected parent created, which is every program started before the shell is
# injected. It never gates on the numbers: a hosted runner's timing is not a
# product property. It fails only when it cannot measure.
#
# Console and windowed programs are measured apart because a branch may hold a
# console program back on purpose. The alpha branch waits out a grace period
# before reaching a console program that has only just started, so its console
# rows carry that wait on top of the latency while its windowed rows carry the
# latency alone. Reading one number for both kinds would credit the wait to the
# observer and hide what the observer actually costs.

$ErrorActionPreference = 'Stop'
$markerWaitMilliseconds = 6000
$resultRoot = Join-Path $env:RUNNER_TEMP "mactype-latency-$PID"
New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null

function Start-Marker([string] $Executable, [string] $Name) {
    $out = Join-Path $resultRoot "$Name.json"
    $process = Start-Process -FilePath $Executable -PassThru -WindowStyle Hidden `
        -ArgumentList @('--out', "`"$out`"", '--wait-ms', "$markerWaitMilliseconds")
    [pscustomobject]@{ name = $Name; out = $out; process = $process }
}

function ConvertTo-UnixMilliseconds([object] $Value) {
    # ConvertFrom-Json turns ISO timestamps into DateTime values; casting the
    # value keeps its milliseconds, while going through [string] would not.
    if ($Value -is [DateTime]) {
        return ([DateTimeOffset]$Value.ToUniversalTime()).ToUnixTimeMilliseconds()
    }
    return [DateTimeOffset]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture).ToUnixTimeMilliseconds()
}

function Read-Latency([object] $Launch) {
    if (-not $Launch.process.WaitForExit($markerWaitMilliseconds + 20000)) {
        throw "Latency marker $($Launch.name) did not exit."
    }
    if (-not (Test-Path -LiteralPath $Launch.out -PathType Leaf)) {
        throw "Latency marker $($Launch.name) wrote no result."
    }
    $result = Get-Content -LiteralPath $Launch.out -Raw | ConvertFrom-Json
    $started = ConvertTo-UnixMilliseconds $result.startedAt
    $latency = if ($result.mactypeModuleLoaded -and $result.loadObservedAt) {
        (ConvertTo-UnixMilliseconds $result.loadObservedAt) - $started
    } else {
        $null
    }
    [pscustomobject]@{
        name = $Launch.name
        pid = [uint32]$result.pid
        startedMs = $started
        latencyMs = $latency
    }
}

function Get-Stats([object[]] $Samples) {
    $hooked = @($Samples | Where-Object { $null -ne $_.latencyMs } | ForEach-Object { [double]$_.latencyMs } | Sort-Object)
    if ($hooked.Count -eq 0) {
        return [pscustomobject]@{ n = $Samples.Count; hooked = 0; min = '-'; p50 = '-'; p90 = '-'; max = '-' }
    }
    $pick = { param($q) $hooked[[Math]::Min($hooked.Count - 1, [int][Math]::Floor($q * ($hooked.Count - 1) + 0.5))] }
    [pscustomobject]@{
        n = $Samples.Count
        hooked = $hooked.Count
        min = [int]$hooked[0]
        p50 = [int](& $pick 0.5)
        p90 = [int](& $pick 0.9)
        max = [int]$hooked[-1]
    }
}

# One kind of program, measured twice: alone, then against a burst that makes
# the service drain a queue instead of handling one target at a time.
function Measure-Kind([string] $Executable32, [string] $Executable64, [string] $Tag) {
    $single = @()
    for ($round = 0; $round -lt $SingleRounds; $round++) {
        $single += Read-Latency (Start-Marker -Executable $Executable64 -Name "$Tag-single-$round")
        Start-Sleep -Milliseconds 750
    }

    $launches = @()
    for ($index = 0; $index -lt $Burst64; $index++) {
        $launches += Start-Marker -Executable $Executable64 -Name "$Tag-burst64-$index"
    }
    for ($index = 0; $index -lt $Burst32; $index++) {
        $launches += Start-Marker -Executable $Executable32 -Name "$Tag-burst32-$index"
    }
    $burst = @($launches | ForEach-Object { Read-Latency $_ })

    [pscustomobject]@{ tag = $Tag; single = $single; burst = $burst }
}

$kinds = @(
    [pscustomobject]@{ tag = 'console'; x86 = $Marker32; x64 = $Marker64 }
)
if ($WindowMarker32 -and $WindowMarker64 -and
    (Test-Path -LiteralPath $WindowMarker32 -PathType Leaf) -and
    (Test-Path -LiteralPath $WindowMarker64 -PathType Leaf)) {
    $kinds += [pscustomobject]@{ tag = 'windowed'; x86 = $WindowMarker32; x64 = $WindowMarker64 }
}

# WMI delivery on its own: the same trace class the service subscribes to,
# received by this shell, against the creation time each marker reports.
$sourceId = "mactype-latency-trace-$PID"
Register-CimIndicationEvent -Query 'SELECT ProcessID FROM Win32_ProcessStartTrace' -SourceIdentifier $sourceId | Out-Null
try {
    Start-Sleep -Milliseconds 1500

    $measured = @()
    foreach ($kind in $kinds) {
        $measured += Measure-Kind -Executable32 $kind.x86 -Executable64 $kind.x64 -Tag $kind.tag
    }

    Start-Sleep -Milliseconds 2500
    $arrivals = @{}
    foreach ($record in @(Get-Event -SourceIdentifier $sourceId -ErrorAction SilentlyContinue)) {
        $processId = [uint32]$record.SourceEventArgs.NewEvent.ProcessID
        if (-not $arrivals.ContainsKey($processId)) {
            $arrivals[$processId] = ([DateTimeOffset]$record.TimeGenerated.ToUniversalTime()).ToUnixTimeMilliseconds()
        }
    }
} finally {
    Unregister-Event -SourceIdentifier $sourceId -ErrorAction SilentlyContinue
    Get-Event -SourceIdentifier $sourceId -ErrorAction SilentlyContinue | Remove-Event
}

$allSamples = @($measured | ForEach-Object { @($_.single) + @($_.burst) } | ForEach-Object { $_ })
$wmi = @($allSamples | ForEach-Object {
    $arrival = $arrivals[[uint32]$_.pid]
    $delivery = $null
    if ($null -ne $arrival) { $delivery = $arrival - $_.startedMs }
    [pscustomobject]@{ latencyMs = $delivery }
})

$burstSize = $Burst64 + $Burst32
$rows = @()
foreach ($kind in $measured) {
    $rows += [pscustomobject]@{ series = "$($kind.tag) program, one at a time"; stats = Get-Stats $kind.single }
    $rows += [pscustomobject]@{ series = "$($kind.tag) program, $burstSize at once"; stats = Get-Stats $kind.burst }
}
$rows += [pscustomobject]@{ series = 'Win32_ProcessStartTrace delivery (this shell)'; stats = Get-Stats $wmi }

$lines = @(
    '### Birth to MacType loaded (ms)',
    '',
    '| series | n | reached | min | p50 | p90 | max |',
    '| --- | ---: | ---: | ---: | ---: | ---: | ---: |'
)
foreach ($row in $rows) {
    $s = $row.stats
    $lines += "| $($row.series) | $($s.n) | $($s.hooked) | $($s.min) | $($s.p50) | $($s.p90) | $($s.max) |"
}
$lines += ''
$lines += 'Marker load times are polled every 25 ms. Reported, never gated. A branch that holds young console programs back on purpose carries that wait in its console rows; read the windowed rows for the latency alone.'
$lines | ForEach-Object { Write-Host $_ }
if ($env:GITHUB_STEP_SUMMARY) {
    $lines | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8
}
