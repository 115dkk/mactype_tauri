[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $GameExecutable,

    [Parameter(Mandatory)]
    [string] $OutputPath,

    [string] $MacLoader,

    [string] $ServiceCore,

    [string] $UnityEvidenceProbe,

    [string] $ScreenshotPath,

    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $ExpectedCoreSha256,

    [switch] $RequireUnityRedirect,

    [string] $SteamAppId,

    [ValidateRange(5, 300)]
    [int] $ObserveSeconds = 30,

    [ValidateRange(0, 5)]
    [int] $EscapeBeforeCaptureCount = 0
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

function Get-SteamRuntimeState([string] $LogPath, [bool] $Requested) {
    if (-not $Requested) {
        return 'not-requested'
    }
    if (-not (Test-Path -LiteralPath $LogPath -PathType Leaf)) {
        return 'not-observed'
    }
    $stream = [IO.File]::Open(
        $LogPath,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite
    )
    try {
        $maximumTailBytes = 1MB
        if ($stream.Length -gt $maximumTailBytes) {
            $null = $stream.Seek(-$maximumTailBytes, [IO.SeekOrigin]::End)
        }
        $reader = [IO.StreamReader]::new(
            $stream,
            [Text.Encoding]::UTF8,
            $true,
            4096,
            $true
        )
        try {
            $text = $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
    $positive = $text.LastIndexOf(
        'SteamManager.Initialized:True',
        [StringComparison]::OrdinalIgnoreCase
    )
    $negative = @(
        $text.LastIndexOf(
            'Steamworks is not initialized',
            [StringComparison]::OrdinalIgnoreCase
        ),
        $text.LastIndexOf(
            'SteamManager.Initialized:False',
            [StringComparison]::OrdinalIgnoreCase
        ),
        $text.LastIndexOf(
            'SteamAPI_Init() failed',
            [StringComparison]::OrdinalIgnoreCase
        )
    ) | Measure-Object -Maximum | Select-Object -ExpandProperty Maximum
    if ($negative -ge 0 -and $negative -gt $positive) {
        return 'initialization-failed'
    }
    if ($positive -ge 0) {
        return 'initialized'
    }
    return 'not-observed'
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

function Save-ExactWindowCapture(
    [System.Diagnostics.Process] $Process,
    [string] $Path,
    [int] $EscapeCount
) {
    Add-Type -AssemblyName System.Drawing
    if (-not ('MacType.UnityWindowCapture' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace MacType
{
    public static class UnityWindowCapture
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct Rect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetWindowRect(IntPtr window, out Rect rect);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool SetForegroundWindow(IntPtr window);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool ShowWindow(IntPtr window, int command);

        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PostMessageW(
            IntPtr window,
            uint message,
            IntPtr wParam,
            IntPtr lParam
        );

        [DllImport("user32.dll")]
        public static extern IntPtr SetThreadDpiAwarenessContext(
            IntPtr dpiContext
        );

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PrintWindow(
            IntPtr window,
            IntPtr destination,
            uint flags
        );
    }
}
'@
    }

    $Process.Refresh()
    $window = $Process.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) {
        throw 'The exact Unity test process has no capturable main window.'
    }
    $null = [MacType.UnityWindowCapture]::ShowWindow($window, 9)
    $null = [MacType.UnityWindowCapture]::SetForegroundWindow($window)
    Start-Sleep -Milliseconds 750
    for ($index = 0; $index -lt $EscapeCount; $index++) {
        $null = [MacType.UnityWindowCapture]::PostMessageW(
            $window,
            0x0100,
            [IntPtr]::new(0x1B),
            [IntPtr]::Zero
        )
        $null = [MacType.UnityWindowCapture]::PostMessageW(
            $window,
            0x0101,
            [IntPtr]::new(0x1B),
            [IntPtr]::Zero
        )
        Start-Sleep -Milliseconds 750
    }
    $null = [MacType.UnityWindowCapture]::SetThreadDpiAwarenessContext(
        [IntPtr]::new(-4)
    )
    $rect = [MacType.UnityWindowCapture+Rect]::new()
    if (-not [MacType.UnityWindowCapture]::GetWindowRect($window, [ref] $rect)) {
        throw 'GetWindowRect failed for the exact Unity test process.'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0 -or
        $width -gt 16384 -or $height -gt 16384) {
        throw "The exact Unity window has an invalid capture size: ${width}x${height}"
    }

    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $device = $graphics.GetHdc()
        try {
            $printed = [MacType.UnityWindowCapture]::PrintWindow(
                $window,
                $device,
                2
            )
        } finally {
            $graphics.ReleaseHdc($device)
        }
        if (-not $printed) {
            if ([MacType.UnityWindowCapture]::GetForegroundWindow() -ne $window) {
                throw 'PrintWindow failed and the exact Unity window did not become foreground.'
            }
            $graphics.CopyFromScreen(
                $rect.Left,
                $rect.Top,
                0,
                0,
                [Drawing.Size]::new($width, $height),
                [Drawing.CopyPixelOperation]::SourceCopy
            )
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
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
$screenshot = $null
if (-not [string]::IsNullOrWhiteSpace($ScreenshotPath)) {
    $screenshot = [IO.Path]::GetFullPath($ScreenshotPath)
    if (Test-Path -LiteralPath $screenshot) {
        throw "Unity compatibility screenshot already exists: $screenshot"
    }
    $screenshotRoot = Split-Path -Parent $screenshot
    if (-not (Test-Path -LiteralPath $screenshotRoot -PathType Container)) {
        New-Item -ItemType Directory -Path $screenshotRoot | Out-Null
    }
}

$loader = $null
$expectedCore = $null
$profilePath = $null
if (-not [string]::IsNullOrWhiteSpace($MacLoader)) {
    if (-not [string]::IsNullOrWhiteSpace($ServiceCore)) {
        throw 'MacLoader and ServiceCore are mutually exclusive launch modes.'
    }
    $loader = (Resolve-Path -LiteralPath $MacLoader).Path
    $loaderRoot = Split-Path -Parent $loader
    $expectedCore = Join-Path $loaderRoot 'MacType64.dll'
    $profilePath = Join-Path $loaderRoot 'MacType.ini'
    foreach ($required in @($expectedCore, $profilePath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Injected Unity test input is missing: $required"
        }
    }
    $openService = Get-Service -Name MacTypeControlCenter -ErrorAction SilentlyContinue
    if ($openService -and $openService.Status -ne 'Stopped') {
        throw 'Refusing an isolated MacLoader run while MacTypeControlCenter can inject another renderer generation.'
    }
} elseif (-not [string]::IsNullOrWhiteSpace($ServiceCore)) {
    $expectedCore = (Resolve-Path -LiteralPath $ServiceCore).Path
    $profilePath = Join-Path (Split-Path -Parent $expectedCore) 'MacType.ini'
    $openService = Get-Service -Name MacTypeControlCenter -ErrorAction Stop
    if ($openService.Status -ne 'Running') {
        throw 'ServiceCore evidence requires MacTypeControlCenter to be running.'
    }
}
$loaderSha256 = if ($loader) {
    (Get-FileHash -LiteralPath $loader -Algorithm SHA256).Hash.ToLowerInvariant()
} else {
    $null
}
$coreSha256 = if ($expectedCore) {
    (Get-FileHash -LiteralPath $expectedCore -Algorithm SHA256).Hash.ToLowerInvariant()
} else {
    $null
}
$profileSha256 = if ($profilePath -and
    (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    (Get-FileHash -LiteralPath $profilePath -Algorithm SHA256).Hash.ToLowerInvariant()
} else {
    $null
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedCoreSha256)) {
    if (-not $coreSha256) {
        throw 'ExpectedCoreSha256 requires an explicit MacLoader or ServiceCore runtime.'
    }
    if (-not $coreSha256.Equals(
        $ExpectedCoreSha256,
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw "The selected MacType core SHA-256 $coreSha256 does not match the expected release SHA-256 $($ExpectedCoreSha256.ToLowerInvariant())."
    }
}
if (-not [string]::IsNullOrWhiteSpace($UnityEvidenceProbe)) {
    $UnityEvidenceProbe = (Resolve-Path -LiteralPath $UnityEvidenceProbe).Path
    if (-not $expectedCore) {
        throw 'Unity font evidence requires an explicit MacLoader or ServiceCore runtime.'
    }
} elseif ($RequireUnityRedirect) {
    throw 'RequireUnityRedirect requires UnityEvidenceProbe.'
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
$previousUnityEvidence = $env:MACTYPE_UNITY_EVIDENCE
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
    if ($UnityEvidenceProbe) {
        $env:MACTYPE_UNITY_EVIDENCE = '1'
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
    if ($screenshot -and -not $gameProcess.HasExited) {
        Save-ExactWindowCapture -Process $gameProcess -Path $screenshot `
            -EscapeCount $EscapeBeforeCaptureCount
    }
} catch {
    $observationError = $_.Exception.Message
} finally {
    $env:SteamAppId = $previousSteamAppId
    $env:SteamGameId = $previousSteamGameId
    $env:MACTYPE_UNITY_EVIDENCE = $previousUnityEvidence
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

$unityFontEvidence = $null
if ($UnityEvidenceProbe -and $gameProcess -and -not $gameProcess.HasExited) {
    $evidenceOutput = @(& $UnityEvidenceProbe --evidence $gameProcess.Id 2>&1)
    $evidenceExitCode = $LASTEXITCODE
    $evidenceText = ($evidenceOutput | ForEach-Object { [string] $_ }) -join "`n"
    $parsed = [regex]::Match(
        $evidenceText,
        '^pid=(?<pid>\d+) observed=(?<observed>\d+) attempts=(?<attempts>\d+) successes=(?<successes>\d+) fallbacks=(?<fallbacks>\d+) renders=(?<renders>\d+) render-successes=(?<renderSuccesses>\d+) bitmaps=(?<bitmaps>\d+) last-error=(?<lastError>-?\d+) last-glyph=(?<lastGlyph>\d+) last-width=(?<lastWidth>\d+) last-rows=(?<lastRows>\d+) charmap=(?<charmap>\d+) face-glyphs=(?<faceGlyphs>-?\d+) sample-glyph=(?<sampleGlyph>\d+) lookups=(?<lookups>\d+) lookup-hits=(?<lookupHits>\d+) last-character=(?<lastCharacter>\d+) last-lookup-glyph=(?<lastLookupGlyph>\d+) face-resolutions=(?<faceResolutions>\d+) face-resolution-hits=(?<faceResolutionHits>\d+) face-glyph-hits=(?<faceGlyphHits>\d+) last-face-glyph=(?<lastFaceGlyph>\d+) last-face-sample=(?<lastFaceSample>\d+) family=(?<family>.*?) mapped-lookups=(?<mappedLookups>\d+) mapped-hits=(?<mappedHits>\d+) mapped-character=(?<mappedCharacter>\d+) mapped-glyph=(?<mappedGlyph>\d+) mapped-sample=(?<mappedSample>\d+) mapped-family=(?<mappedFamily>.*?) mapped-face-family=(?<mappedFaceFamily>.*?) os-resolutions=(?<osResolutions>\d+) os-hits=(?<osHits>\d+) os-sample=(?<osSample>\d+) os-family=(?<osFamily>.*?) os-face-family=(?<osFaceFamily>.*?) mapped-os-resolutions=(?<mappedOsResolutions>\d+) mapped-os-hits=(?<mappedOsHits>\d+) mapped-os-sample=(?<mappedOsSample>\d+) mapped-os-family=(?<mappedOsFamily>.*?) mapped-os-face-family=(?<mappedOsFaceFamily>.*?) observed-path=(?<observedPath>.*?) source=(?<source>.*?) replacement=(?<replacement>.*)$'
    )
    $unityFontEvidence = [ordered]@{
        exitCode = $evidenceExitCode
        raw = $evidenceText
        pid = if ($parsed.Success) { [int] $parsed.Groups['pid'].Value } else { $null }
        observedFontOpens = if ($parsed.Success) { [int] $parsed.Groups['observed'].Value } else { $null }
        redirectAttempts = if ($parsed.Success) { [int] $parsed.Groups['attempts'].Value } else { $null }
        redirectSuccesses = if ($parsed.Success) { [int] $parsed.Groups['successes'].Value } else { $null }
        redirectFallbacks = if ($parsed.Success) { [int] $parsed.Groups['fallbacks'].Value } else { $null }
        renderCalls = if ($parsed.Success) { [int] $parsed.Groups['renders'].Value } else { $null }
        renderSuccesses = if ($parsed.Success) { [int] $parsed.Groups['renderSuccesses'].Value } else { $null }
        nonEmptyBitmaps = if ($parsed.Success) { [int] $parsed.Groups['bitmaps'].Value } else { $null }
        lastRenderError = if ($parsed.Success) { [int] $parsed.Groups['lastError'].Value } else { $null }
        lastGlyphIndex = if ($parsed.Success) { [int] $parsed.Groups['lastGlyph'].Value } else { $null }
        lastBitmapWidth = if ($parsed.Success) { [int] $parsed.Groups['lastWidth'].Value } else { $null }
        lastBitmapRows = if ($parsed.Success) { [int] $parsed.Groups['lastRows'].Value } else { $null }
        redirectedFaceHasCharmap = if ($parsed.Success) { [int] $parsed.Groups['charmap'].Value } else { $null }
        redirectedFaceGlyphs = if ($parsed.Success) { [int] $parsed.Groups['faceGlyphs'].Value } else { $null }
        sampleKoreanGlyph = if ($parsed.Success) { [int] $parsed.Groups['sampleGlyph'].Value } else { $null }
        characterLookups = if ($parsed.Success) { [int] $parsed.Groups['lookups'].Value } else { $null }
        characterLookupHits = if ($parsed.Success) { [int] $parsed.Groups['lookupHits'].Value } else { $null }
        lastCharacter = if ($parsed.Success) { [int] $parsed.Groups['lastCharacter'].Value } else { $null }
        lastLookupGlyph = if ($parsed.Success) { [int] $parsed.Groups['lastLookupGlyph'].Value } else { $null }
        faceResolutions = if ($parsed.Success) { [int] $parsed.Groups['faceResolutions'].Value } else { $null }
        faceResolutionHits = if ($parsed.Success) { [int] $parsed.Groups['faceResolutionHits'].Value } else { $null }
        faceResolutionGlyphHits = if ($parsed.Success) { [int] $parsed.Groups['faceGlyphHits'].Value } else { $null }
        lastFaceResolutionGlyph = if ($parsed.Success) { [int] $parsed.Groups['lastFaceGlyph'].Value } else { $null }
        lastFaceResolutionSampleKoreanGlyph = if ($parsed.Success) { [int] $parsed.Groups['lastFaceSample'].Value } else { $null }
        lastLookupFamily = if ($parsed.Success) { $parsed.Groups['family'].Value } else { $null }
        mappedCharacterLookups = if ($parsed.Success) { [int] $parsed.Groups['mappedLookups'].Value } else { $null }
        mappedCharacterLookupHits = if ($parsed.Success) { [int] $parsed.Groups['mappedHits'].Value } else { $null }
        lastMappedCharacter = if ($parsed.Success) { [int] $parsed.Groups['mappedCharacter'].Value } else { $null }
        lastMappedGlyph = if ($parsed.Success) { [int] $parsed.Groups['mappedGlyph'].Value } else { $null }
        lastMappedSampleKoreanGlyph = if ($parsed.Success) { [int] $parsed.Groups['mappedSample'].Value } else { $null }
        lastMappedFamily = if ($parsed.Success) { $parsed.Groups['mappedFamily'].Value } else { $null }
        lastMappedResolvedFaceFamily = if ($parsed.Success) { $parsed.Groups['mappedFaceFamily'].Value } else { $null }
        osFaceResolutions = if ($parsed.Success) { [int] $parsed.Groups['osResolutions'].Value } else { $null }
        osFaceResolutionHits = if ($parsed.Success) { [int] $parsed.Groups['osHits'].Value } else { $null }
        lastOsFaceSampleKoreanGlyph = if ($parsed.Success) { [int] $parsed.Groups['osSample'].Value } else { $null }
        lastOsFaceFamily = if ($parsed.Success) { $parsed.Groups['osFamily'].Value } else { $null }
        lastOsResolvedFaceFamily = if ($parsed.Success) { $parsed.Groups['osFaceFamily'].Value } else { $null }
        mappedOsFaceResolutions = if ($parsed.Success) { [int] $parsed.Groups['mappedOsResolutions'].Value } else { $null }
        mappedOsFaceResolutionHits = if ($parsed.Success) { [int] $parsed.Groups['mappedOsHits'].Value } else { $null }
        lastMappedOsFaceSampleKoreanGlyph = if ($parsed.Success) { [int] $parsed.Groups['mappedOsSample'].Value } else { $null }
        lastMappedOsFaceFamily = if ($parsed.Success) { $parsed.Groups['mappedOsFamily'].Value } else { $null }
        lastMappedOsResolvedFaceFamily = if ($parsed.Success) { $parsed.Groups['mappedOsFaceFamily'].Value } else { $null }
        observedPath = if ($parsed.Success) { $parsed.Groups['observedPath'].Value } else { $null }
        sourcePath = if ($parsed.Success) { $parsed.Groups['source'].Value } else { $null }
        replacementPath = if ($parsed.Success) { $parsed.Groups['replacement'].Value } else { $null }
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

$steamRuntime = Get-SteamRuntimeState `
    -LogPath $playerLog `
    -Requested (-not [string]::IsNullOrWhiteSpace($SteamAppId))

$mactypeModules = @($modules | Where-Object { $_.name -iin @('MacType.dll', 'MacType64.dll') })
$exactCoreLoaded = if ($expectedCore) {
    $mactypeModules.Count -eq 1 -and @($mactypeModules | Where-Object {
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
        mode = if ($loader) {
            'product-macloader'
        } elseif ($ServiceCore) {
            'product-service'
        } else {
            'stock'
        }
        loader = $loader
        loaderSha256 = $loaderSha256
        expectedCore = $expectedCore
        coreSha256 = $coreSha256
        expectedCoreSha256 = if ($ExpectedCoreSha256) {
            $ExpectedCoreSha256.ToLowerInvariant()
        } else {
            $null
        }
        profile = $profilePath
        profileSha256 = $profileSha256
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
        playerLogPath = $playerLog
        steamRuntime = $steamRuntime
        werReports = $werReports
        unityFontEvidence = $unityFontEvidence
        screenshotPath = $screenshot
    }
    cleanup = $cleanup
}

[IO.File]::WriteAllText(
    $output,
    ($evidence | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false)
)

if ($steamRuntime -eq 'initialization-failed') {
    throw "Steam runtime initialization failed during Unity compatibility evidence: $output"
}
if ($observationError -or $exited -or -not $responding -or
    -not $exactCoreLoaded -or
    $werReports.Count -ne 0 -or
    ($RequireUnityRedirect -and
        (-not $unityFontEvidence -or $unityFontEvidence.exitCode -ne 0 -or
            $unityFontEvidence.redirectSuccesses -le 0 -or
            $unityFontEvidence.redirectFallbacks -ne 0 -or
            ($unityFontEvidence.characterLookups -gt 0 -and
                $unityFontEvidence.characterLookupHits -le 0)))) {
    throw "Unity compatibility evidence did not satisfy the survival contract: $output"
}

Write-Host "Unity compatibility evidence passed: $output"
