[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $GameExecutable,

    [Parameter(Mandatory)]
    [string] $OutputPath,

    [string] $MacLoader,

    [string] $SteamAppId,

    [ValidateRange(5, 300)]
    [int] $ObserveSeconds = 30
)

$ErrorActionPreference = 'Stop'

function Get-ExactGameProcesses([string] $Executable) {
    $result = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
    foreach ($process in Get-Process -ErrorAction SilentlyContinue) {
        try {
            if ($process.Path -and [System.IO.Path]::GetFullPath($process.Path).Equals(
                $Executable,
                [StringComparison]::OrdinalIgnoreCase
            )) {
                $result.Add($process)
            }
        } catch {
            continue
        }
    }
    return $result.ToArray()
}

function Get-UnityMarkers([string] $UnityPlayer) {
    $text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($UnityPlayer))
    $markers = [ordered]@{}
    foreach ($marker in @(
        'FT_Init_FreeType',
        'TextRenderingPrivate',
        'Gulim',
        'Malgun Gothic',
        'GetFontData',
        'CreateFontIndirectW',
        'DWriteCreateFactory'
    )) {
        $markers[$marker] = $text.IndexOf($marker, [StringComparison]::Ordinal) -ge 0
    }
    return $markers
}

function Get-RelevantModules([System.Diagnostics.Process] $Process) {
    $names = @(
        'MacType.dll',
        'MacType64.dll',
        'UnityPlayer.dll',
        'GameAssembly.dll',
        'mono-2.0-bdwgc.dll',
        'gameoverlayrenderer.dll',
        'gameoverlayrenderer64.dll'
    )
    $modules = [System.Collections.Generic.List[object]]::new()
    try {
        foreach ($module in $Process.Modules) {
            if ($module.ModuleName -iin $names) {
                $modules.Add([ordered]@{
                    name = $module.ModuleName
                    path = $module.FileName
                })
            }
        }
    } catch {
        $modules.Add([ordered]@{
            name = 'module-inventory-error'
            path = $_.Exception.Message
        })
    }
    return $modules.ToArray()
}

$game = (Resolve-Path -LiteralPath $GameExecutable).Path
$unityPlayer = Join-Path (Split-Path -Parent $game) 'UnityPlayer.dll'
if (-not (Test-Path -LiteralPath $unityPlayer -PathType Leaf)) {
    throw "UnityPlayer.dll is not adjacent to the game executable: $unityPlayer"
}

$output = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $output) {
    throw "Unity compatibility evidence already exists: $output"
}
$outputRoot = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $outputRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $outputRoot | Out-Null
}
$playerLog = [IO.Path]::ChangeExtension($output, '.player.log')

$loader = $null
$expectedCore = $null
if (-not [string]::IsNullOrWhiteSpace($MacLoader)) {
    $loader = (Resolve-Path -LiteralPath $MacLoader).Path
    $loaderRoot = Split-Path -Parent $loader
    $expectedCore = Join-Path $loaderRoot 'MacType64.dll'
    foreach ($required in @($expectedCore, (Join-Path $loaderRoot 'MacType.ini'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Injected Unity test input is missing: $required"
        }
    }
}

if (@(Get-ExactGameProcesses $game).Count -ne 0) {
    throw "Refusing to mix evidence with an already running game: $game"
}

$markers = Get-UnityMarkers $unityPlayer
$privateRendererBoundary =
    $markers['FT_Init_FreeType'] -and
    $markers['TextRenderingPrivate'] -and
    -not $markers['GetFontData'] -and
    -not $markers['CreateFontIndirectW'] -and
    -not $markers['DWriteCreateFactory']

$previousSteamAppId = $env:SteamAppId
$previousSteamGameId = $env:SteamGameId
$startedAt = Get-Date
$gameProcess = $null
$launcherProcess = $null
$cleanup = 'not-required'
$observationError = $null

try {
    if (-not [string]::IsNullOrWhiteSpace($SteamAppId)) {
        $env:SteamAppId = $SteamAppId
        $env:SteamGameId = $SteamAppId
    }
    $launchArguments = @("`"$game`"", '-logFile', "`"$playerLog`"")
    if ($loader) {
        $launcherProcess = Start-Process -FilePath $loader -ArgumentList $launchArguments -PassThru
    } else {
        $launcherProcess = Start-Process -FilePath $game `
            -ArgumentList @('-logFile', "`"$playerLog`"") -PassThru
    }

    $startupDeadline = (Get-Date).AddSeconds(25)
    do {
        Start-Sleep -Milliseconds 250
        $gameProcess = @(Get-ExactGameProcesses $game |
            Where-Object { $_.StartTime -ge $startedAt } |
            Sort-Object StartTime -Descending |
            Select-Object -First 1)
        if ($gameProcess.Count -ne 0) {
            $gameProcess = $gameProcess[0]
            break
        }
    } while ((Get-Date) -lt $startupDeadline)
    if (-not $gameProcess) {
        throw 'The Unity game process did not appear before the startup deadline.'
    }

    $observationDeadline = (Get-Date).AddSeconds($ObserveSeconds)
    do {
        if ($gameProcess.HasExited) {
            break
        }
        Start-Sleep -Milliseconds 250
        $gameProcess.Refresh()
    } while ((Get-Date) -lt $observationDeadline)
} catch {
    $observationError = $_.Exception.Message
} finally {
    $env:SteamAppId = $previousSteamAppId
    $env:SteamGameId = $previousSteamGameId
}

$exited = $null
$exitCode = $null
$responding = $null
$mainWindow = $null
$modules = @()
$pidValue = $null
if ($gameProcess) {
    $pidValue = $gameProcess.Id
    try {
        $exited = $gameProcess.HasExited
        if ($exited) {
            $exitCode = $gameProcess.ExitCode
        } else {
            $gameProcess.Refresh()
            $responding = $gameProcess.Responding
            $mainWindow = $gameProcess.MainWindowHandle.ToInt64()
            $modules = @(Get-RelevantModules $gameProcess)
        }
    } catch {
        $observationError = if ($observationError) {
            "$observationError; $($_.Exception.Message)"
        } else {
            $_.Exception.Message
        }
    }
}

$werReports = @()
try {
    $werReports = @(Get-WinEvent -FilterHashtable @{
        LogName = 'Application'
        StartTime = $startedAt
    } -ErrorAction SilentlyContinue | Where-Object {
        $_.ProviderName -in @('Application Error', 'Windows Error Reporting') -and
        $_.Message -match [regex]::Escape([IO.Path]::GetFileName($game))
    } | Select-Object -First 8 | ForEach-Object {
        [ordered]@{
            time = $_.TimeCreated.ToUniversalTime().ToString('O')
            provider = $_.ProviderName
            eventId = $_.Id
            message = $_.Message
        }
    })
} catch {
    $werReports = @([ordered]@{
        time = $null
        provider = 'event-log-unavailable'
        eventId = $null
        message = $_.Exception.Message
    })
}

if ($gameProcess -and -not $gameProcess.HasExited) {
    Stop-Process -Id $gameProcess.Id -ErrorAction Stop
    $null = $gameProcess.WaitForExit(5000)
    $cleanup = 'terminated-exact-test-process'
}

$mactypeModules = @($modules | Where-Object { $_.name -iin @('MacType.dll', 'MacType64.dll') })
$exactCoreLoaded = if ($expectedCore) {
    @($mactypeModules | Where-Object {
        [IO.Path]::GetFullPath($_.path).Equals(
            [IO.Path]::GetFullPath($expectedCore),
            [StringComparison]::OrdinalIgnoreCase
        )
    }).Count -eq 1
} else {
    $mactypeModules.Count -eq 0
}

$evidence = [ordered]@{
    schema = 1
    kind = 'mactype-unity-compatibility-evidence'
    capturedAt = (Get-Date).ToUniversalTime().ToString('O')
    game = [ordered]@{
        path = $game
        sha256 = (Get-FileHash -LiteralPath $game -Algorithm SHA256).Hash.ToLowerInvariant()
        version = [Diagnostics.FileVersionInfo]::GetVersionInfo($game).FileVersion
        steamAppId = if ($SteamAppId) { $SteamAppId } else { $null }
    }
    unity = [ordered]@{
        path = $unityPlayer
        sha256 = (Get-FileHash -LiteralPath $unityPlayer -Algorithm SHA256).Hash.ToLowerInvariant()
        version = [Diagnostics.FileVersionInfo]::GetVersionInfo($unityPlayer).FileVersion
        binaryMarkers = $markers
        rendererBoundaryInference = if ($privateRendererBoundary) {
            'application-private-freetype'
        } else {
            'mixed-or-unknown'
        }
    }
    launch = [ordered]@{
        mode = if ($loader) { 'product-macloader' } else { 'stock' }
        loader = $loader
        expectedCore = $expectedCore
        launcherExitCode = if ($launcherProcess -and $launcherProcess.HasExited) {
            $launcherProcess.ExitCode
        } else {
            $null
        }
    }
    observation = [ordered]@{
        pid = $pidValue
        durationSeconds = $ObserveSeconds
        exitedBeforeDeadline = $exited
        exitCode = $exitCode
        respondingAtDeadline = $responding
        mainWindow = $mainWindow
        relevantModules = $modules
        expectedCoreStateObserved = $exactCoreLoaded
        error = $observationError
        werReports = $werReports
    }
    cleanup = $cleanup
}

[IO.File]::WriteAllText(
    $output,
    ($evidence | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false)
)

if ($observationError -or $exited -or -not $exactCoreLoaded -or $werReports.Count -ne 0) {
    throw "Unity compatibility evidence did not satisfy the survival contract: $output"
}

Write-Host "Unity compatibility evidence passed: $output"
