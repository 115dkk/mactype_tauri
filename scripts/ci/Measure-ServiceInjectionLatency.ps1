[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Marker32,

    [Parameter(Mandatory)]
    [string] $Marker64,

    [int] $SingleRounds = 6,

    [int] $Burst64 = 12,

    [int] $Burst32 = 4
)

# Reports how long the installed service takes to reach a program that no
# injected parent created, which is every program started before the shell is
# injected. It never gates on the numbers: a hosted runner's timing is not a
# product property. It fails only when it cannot measure.

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

# WMI delivery on its own: the same trace class the service subscribes to,
# received by this shell, against the creation time each marker reports.
$sourceId = "mactype-latency-trace-$PID"
Register-CimIndicationEvent -Query 'SELECT ProcessID FROM Win32_ProcessStartTrace' -SourceIdentifier $sourceId | Out-Null
try {
    Start-Sleep -Milliseconds 1500

    $single = @()
    for ($round = 0; $round -lt $SingleRounds; $round++) {
        $single += Read-Latency (Start-Marker -Executable $Marker64 -Name "single-$round")
        Start-Sleep -Milliseconds 750
    }

    $launches = @()
    for ($index = 0; $index -lt $Burst64; $index++) {
        $launches += Start-Marker -Executable $Marker64 -Name "burst64-$index"
    }
    for ($index = 0; $index -lt $Burst32; $index++) {
        $launches += Start-Marker -Executable $Marker32 -Name "burst32-$index"
    }
    $burst = @($launches | ForEach-Object { Read-Latency $_ })

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

$wmi = @(@($single) + @($burst) | ForEach-Object {
    $arrival = $arrivals[[uint32]$_.pid]
    $delivery = $null
    if ($null -ne $arrival) { $delivery = $arrival - $_.startedMs }
    [pscustomobject]@{ latencyMs = $delivery }
})

$rows = @(
    [pscustomobject]@{ series = 'birth to MacType loaded, one program at a time'; stats = Get-Stats $single }
    [pscustomobject]@{ series = "birth to MacType loaded, $($Burst64 + $Burst32) programs at once"; stats = Get-Stats $burst }
    [pscustomobject]@{ series = 'birth to Win32_ProcessStartTrace delivery (this shell)'; stats = Get-Stats $wmi }
)

$lines = @(
    '### Service injection latency (ms)',
    '',
    '| series | n | reached | min | p50 | p90 | max |',
    '| --- | ---: | ---: | ---: | ---: | ---: | ---: |'
)
foreach ($row in $rows) {
    $s = $row.stats
    $lines += "| $($row.series) | $($s.n) | $($s.hooked) | $($s.min) | $($s.p50) | $($s.p90) | $($s.max) |"
}
$lines += ''
$lines += 'Marker load times are polled every 25 ms. Reported, never gated.'
$lines | ForEach-Object { Write-Host $_ }
if ($env:GITHUB_STEP_SUMMARY) {
    $lines | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Encoding utf8
}
