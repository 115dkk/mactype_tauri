[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $CoreRoot,

    [Parameter(Mandatory)]
    [string] $ProbeRoot,

    [Parameter(Mandatory)]
    [string] $OutputRoot,

    [string[]] $Sources = @('맑은 고딕', '굴림', 'Arial'),

    [string] $Replacement = 'Pretendard Medium',

    [string] $PairFamily = 'Pretendard ExtraBold',

    [ValidateRange(0, 3)]
    [int[]] $Modes = @(0, 1, 2, 3),

    [ValidateSet('x64', 'Win32')]
    [string[]] $Architectures = @('x64', 'Win32'),

    [switch] $IncludeStock = $true,

    [ValidateRange(0, 60000)]
    [int] $WaitMs = 5000,

    [ValidateRange(5, 600)]
    [int] $ProbeTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'

$openService = Get-Service -Name MacTypeControlCenter -ErrorAction SilentlyContinue
if ($openService -and $openService.Status -ne 'Stopped') {
    throw 'Refusing an isolated bold substitution run while MacTypeControlCenter can inject another renderer generation.'
}

$resolvedCoreRoot = (Resolve-Path -LiteralPath $CoreRoot).Path
$resolvedProbeRoot = (Resolve-Path -LiteralPath $ProbeRoot).Path
$resolvedOutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ((Test-Path -LiteralPath $resolvedOutputRoot) -and
    @(Get-ChildItem -LiteralPath $resolvedOutputRoot -Force).Count -ne 0) {
    throw "Bold substitution evidence already exists: $resolvedOutputRoot"
}
New-Item -ItemType Directory -Force -Path $resolvedOutputRoot | Out-Null

$layout = @{
    x64 = @{ Core = 'MacType64.dll'; Probe = 'bold-substitution-probe64.exe' }
    Win32 = @{ Core = 'MacType.dll'; Probe = 'bold-substitution-probe32.exe' }
}
foreach ($architecture in $Architectures) {
    foreach ($required in @(
            (Join-Path $resolvedCoreRoot $layout[$architecture].Core),
            (Join-Path $resolvedProbeRoot $layout[$architecture].Probe))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Bold substitution field input is missing: $required"
        }
    }
}

$englishTwins = @{
    '맑은 고딕' = 'Malgun Gothic'
    '굴림' = 'Gulim'
}

function Get-SourceSlug([string] $Source, [int] $Index) {
    if ($englishTwins.ContainsKey($Source)) {
        $Source = $englishTwins[$Source]
    }
    $slug = ($Source.ToLowerInvariant() -replace '[^a-z0-9]+', '-').Trim('-')
    if ([string]::IsNullOrEmpty($slug)) {
        $slug = "source-$Index"
    }
    return $slug
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-Profile([string] $Path, [string] $Source, [int] $Mode) {
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('[General]')
    $lines.Add('FontSubstitutes=1')
    $lines.Add('DirectWrite=1')
    $lines.Add('HookChildProcesses=0')
    $lines.Add("FontSubstitutesBold=$Mode")
    $lines.Add('')
    $lines.Add('[FontSubstitutes]')
    $lines.Add("$Source=$Replacement")
    if ($englishTwins.ContainsKey($Source)) {
        $lines.Add("$($englishTwins[$Source])=$Replacement")
    }
    foreach ($fallbackSource in @('맑은 고딕', 'Malgun Gothic')) {
        if (-not ($lines -contains "$fallbackSource=$Replacement")) {
            $lines.Add("$fallbackSource=$Replacement")
        }
    }
    if ($Mode -eq 3) {
        $lines.Add('')
        $lines.Add('[FontSubstitutesBold]')
        $lines.Add("$Replacement=$PairFamily")
    }
    # The renderer's INI parser reads non-ASCII names as UTF-8 only after a BOM;
    # without one it decodes the file in the ANSI code page.
    [IO.File]::WriteAllText(
        $Path,
        (($lines -join "`r`n") + "`r`n"),
        [Text.UTF8Encoding]::new($true))
}

function ConvertTo-CommandLineArgument([string] $Value) {
    if ($Value -match '"') {
        throw "Probe argument must not contain a double quote: $Value"
    }
    return '"' + $Value + '"'
}

function Invoke-ProbeThroughWmi([string] $Probe, [string[]] $Arguments, [string] $WorkingDirectory) {
    $commandLine = (@($Probe) + $Arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ }) -join ' '
    $launch = 'created'
    $processId = $null
    try {
        $created = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -ErrorAction Stop -Arguments @{
            CommandLine = $commandLine
            CurrentDirectory = $WorkingDirectory
        }
        if ($created.ReturnValue -ne 0) {
            return [ordered]@{ commandLine = $commandLine; pid = $null; launch = "Win32_Process.Create returned $($created.ReturnValue)"; exited = $false }
        }
        $processId = [int] $created.ProcessId
    }
    catch {
        # WMI can refuse Create outright (E_FAIL) in some sessions. A direct
        # launch is acceptable because the probe records every MacType module
        # it carries and the summary rejects any run that is not isolated.
        $launch = "start-process-fallback ($($_.Exception.Message.Trim()))"
        $arguments = @($Arguments | ForEach-Object { ConvertTo-CommandLineArgument $_ })
        $process = Start-Process -FilePath $Probe -ArgumentList $arguments -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -PassThru
        $processId = [int] $process.Id
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($ProbeTimeoutSeconds)
    $exited = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Get-Process -Id $processId -ErrorAction SilentlyContinue)) {
            $exited = $true
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $exited) {
        Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
    }
    return [ordered]@{ commandLine = $commandLine; pid = $processId; launch = $launch; exited = $exited }
}

$expectations = @{
    0 = @{ Face = 'medium'; Synthetic = $false; Simulations = 'none'; Pixels = @('pretendardMedium400') }
    1 = @{ Face = 'medium'; Synthetic = $true; Simulations = 'bold'; Pixels = @('pretendardMedium700') }
    2 = @{ Face = 'bold'; Synthetic = $false; Simulations = 'none'; Pixels = @('pretendard700') }
    3 = @{ Face = 'extrabold'; Synthetic = $false; Simulations = 'none'; Pixels = @('pretendardExtraBold700', 'pretendardExtraBold800') }
}

function Get-Mark([object] $Expected, [bool] $Pass) {
    if ($null -eq $Expected) { return '' }
    if ($Pass) { return ' PASS' }
    return ' FAIL'
}

function Get-FallbackVerdicts([object] $State, [object] $Mode) {
    $result = [Collections.Generic.List[object]]::new()
    foreach ($fallback in @($State.directWrite.fallback)) {
        $families = @($fallback.familyNames | ForEach-Object { $_.name })
        $runFamilies = @($fallback.layoutRuns | ForEach-Object { $_.face.familyName })
        $fallbackPaths = @($fallback.face.files | ForEach-Object { $_.path })
        $replacement = @($fallbackPaths | Where-Object {
            $_ -match '[\\/]MacType[\\/]FontCache[\\/]'
        }).Count -ne 0
        $source = @($fallbackPaths | Where-Object {
            [IO.Path]::GetFileName($_) -match '^malgun'
        }).Count -ne 0
        $result.Add([ordered]@{
            primaryFamily = $fallback.primaryFamily
            mappedLength = $fallback.mappedLength
            familyNames = $families
            weight = $fallback.weight
            simulations = $fallback.simulations
            files = @($fallback.face.files)
            layoutRunFamilies = $runFamilies
            replacementObserved = $replacement
            sourceObserved = $source
            pass = if ($null -eq $Mode) { $null } else { $replacement -and -not $source }
        })
    }
    return @($result)
}

function Get-RunVerdict([object] $Json, [string] $StateName, [object] $Mode) {
    $state = $Json.states.$StateName
    $gdi = $state.verdicts.gdi.source700
    $collection = $state.verdicts.dwrite.collection700
    $textFormat = $state.verdicts.dwrite.textFormat700
    $expected = if ($null -ne $Mode) { $expectations[[int] $Mode] } else { $null }
    $gdiPass = $expected -and $gdi.face -eq $expected.Face -and $gdi.synthetic -eq $expected.Synthetic
    $collectionPass = $expected -and $collection.backing -eq $expected.Face -and $collection.simulations -eq $expected.Simulations
    # A text format created against the native collection cannot ask for a
    # simulation, so mode 1 legitimately lands on the family's real bold there.
    $textFormatPass = $expected -and (
        ($textFormat.backing -eq $expected.Face -and $textFormat.simulations -eq $expected.Simulations) -or
        ([int] $Mode -eq 1 -and $textFormat.backing -eq 'bold' -and $textFormat.simulations -eq 'none'))
    $pixelsPass = $expected -and $gdi.pixelsMatch -and ($expected.Pixels -contains $gdi.pixelsMatch)
    return [ordered]@{
        state = $StateName
        gdi700 = [ordered]@{ face = $gdi.face; synthetic = $gdi.synthetic; selectedFace = $gdi.selectedFace; tmWeight = $gdi.tmWeight; usWeightClass = $gdi.usWeightClass; windowDcFace = $gdi.windowDc.face; windowDcSynthetic = $gdi.windowDc.synthetic; pass = if ($expected) { [bool] $gdiPass } else { $null } }
        dwriteCollection700 = [ordered]@{ backing = $collection.backing; simulations = $collection.simulations; weight = $collection.weight; pass = if ($expected) { [bool] $collectionPass } else { $null } }
        dwriteTextFormat700 = [ordered]@{ backing = $textFormat.backing; simulations = $textFormat.simulations; runBackings = $textFormat.runBackings; pass = if ($expected) { [bool] $textFormatPass } else { $null } }
        pixels = [ordered]@{ match = $gdi.pixelsMatch; expected = if ($expected) { $expected.Pixels } else { $null }; pass = if ($expected) { [bool] $pixelsPass } else { $null } }
        fallback = Get-FallbackVerdicts $state $Mode
        expected = $expected
    }
}

$runs = [Collections.Generic.List[object]]::new()
$failedRuns = 0
foreach ($architecture in $Architectures) {
    $probe = Join-Path $resolvedProbeRoot $layout[$architecture].Probe
    $sourceCore = Join-Path $resolvedCoreRoot $layout[$architecture].Core
    $plan = [Collections.Generic.List[object]]::new()
    if ($IncludeStock) {
        for ($index = 0; $index -lt $Sources.Count; ++$index) {
            $plan.Add([ordered]@{ Mode = $null; Source = $Sources[$index]; Name = "stock-$(Get-SourceSlug $Sources[$index] $index)" })
        }
    }
    foreach ($mode in $Modes) {
        for ($index = 0; $index -lt $Sources.Count; ++$index) {
            $plan.Add([ordered]@{ Mode = $mode; Source = $Sources[$index]; Name = "mode-$mode-$(Get-SourceSlug $Sources[$index] $index)" })
        }
    }

    foreach ($entry in $plan) {
        $runRoot = Join-Path (Join-Path $resolvedOutputRoot $architecture) $entry.Name
        New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
        $output = Join-Path $runRoot 'probe.json'
        $probeArguments = [Collections.Generic.List[string]]::new()
        $coreSha256 = $null
        $profileSha256 = $null
        $expectedCore = $null
        if ($null -ne $entry.Mode) {
            $expectedCore = Join-Path $runRoot $layout[$architecture].Core
            Copy-Item -LiteralPath $sourceCore -Destination $expectedCore
            $profilePath = Join-Path $runRoot 'MacType.ini'
            Write-Profile $profilePath $entry.Source $entry.Mode
            $coreSha256 = Get-Sha256 $expectedCore
            $profileSha256 = Get-Sha256 $profilePath
            $probeArguments.Add('--core')
            $probeArguments.Add($expectedCore)
        }
        foreach ($argument in @(
                '--out', $output,
                '--source', $entry.Source,
                '--replacement', $Replacement,
                '--pair', $PairFamily,
                '--wait-ms', [string] $WaitMs)) {
            $probeArguments.Add($argument)
        }

        Write-Host "[$architecture] $($entry.Name): launching probe"
        $launch = Invoke-ProbeThroughWmi $probe $probeArguments.ToArray() $runRoot

        $json = $null
        $problem = $null
        if (Test-Path -LiteralPath $output -PathType Leaf) {
            try {
                $json = [IO.File]::ReadAllText($output, [Text.Encoding]::UTF8) | ConvertFrom-Json
            } catch {
                $problem = "probe JSON is unreadable: $($_.Exception.Message)"
            }
        } else {
            $problem = "probe produced no JSON (launch: $($launch.launch), exited: $($launch.exited))"
        }
        $isolated = $null
        $verdict = $null
        if ($json) {
            $isolated = $json.isolated
            $stateName = if ($null -ne $entry.Mode) { 'active' } else { 'stock' }
            $verdict = Get-RunVerdict $json $stateName $entry.Mode
            if ($null -ne $entry.Mode -and $isolated -ne $true) {
                $problem = 'run was not isolated: ' +
                    ((@($json.modules) | ForEach-Object { $_.path }) -join ', ')
            }
        }
        if ($problem) {
            $failedRuns += 1
            Write-Warning "[$architecture] $($entry.Name): $problem"
        }
        $disabledVerdict = if ($json -and $null -ne $entry.Mode) { Get-RunVerdict $json 'disabled' $null } else { $null }
        $runs.Add([ordered]@{
            architecture = $architecture
            name = $entry.Name
            mode = $entry.Mode
            source = $entry.Source
            directory = $runRoot
            probeJson = if ($json) { $output } else { $null }
            launch = $launch
            core = $expectedCore
            coreSha256 = $coreSha256
            profileSha256 = $profileSha256
            isolated = $isolated
            modules = if ($json) { @($json.modules | ForEach-Object { $_.path }) } else { @() }
            readiness = if ($json -and $json.core) { $json.core.readiness } else { $null }
            problem = $problem
            verdict = $verdict
            disabled = $disabledVerdict
        })
    }
}

function Format-Cell([object] $Part, [string] $Text) {
    if ($null -eq $Part) { return 'no data' }
    if ($null -eq $Part.pass) { return $Text }
    return $Text + (Get-Mark $true ([bool] $Part.pass))
}

$markdown = [Collections.Generic.List[string]]::new()
$markdown.Add('# Bold substitution field run')
$markdown.Add('')
$markdown.Add("Core root: ``$resolvedCoreRoot``; replacement ``$Replacement``; pair ``$PairFamily``; captured $((Get-Date).ToUniversalTime().ToString('O')).")
$markdown.Add('')
$markdown.Add('Expected for bold-class requests: mode 0 medium without synthetic bold, mode 1 medium with synthetic bold, mode 2 bold, mode 3 extrabold. Stock rows carry no expectation.')
foreach ($architecture in $Architectures) {
    $markdown.Add('')
    $markdown.Add("## $architecture")
    $markdown.Add('')
    $markdown.Add('| Mode | Source | Isolated | GDI 700 face / synthetic | DWrite collection 700 backing / simulations | DWrite text format 700 backing / simulations | Segoe UI fallback | Segoe UI Variable fallback | Pixel match |')
    $markdown.Add('|---|---|---|---|---|---|---|---|---|')
    foreach ($run in @($runs | Where-Object { $_.architecture -eq $architecture })) {
        $modeText = if ($null -eq $run.mode) { 'stock' } else { [string] $run.mode }
        $isolatedText = if ($null -eq $run.isolated) { 'n/a' } else { ([string] $run.isolated).ToLowerInvariant() }
        if ($null -eq $run.verdict) {
            $markdown.Add("| $modeText | $($run.source) | $isolatedText | $($run.problem) | | | | | |")
            continue
        }
        $v = $run.verdict
        $gdiText = Format-Cell $v.gdi700 "$($v.gdi700.face) / $(([string] $v.gdi700.synthetic).ToLowerInvariant())"
        $collectionText = Format-Cell $v.dwriteCollection700 "$($v.dwriteCollection700.backing) / $($v.dwriteCollection700.simulations)"
        $textFormatText = Format-Cell $v.dwriteTextFormat700 "$($v.dwriteTextFormat700.backing) / $($v.dwriteTextFormat700.simulations)"
        $fallbackCells = @('Segoe UI', 'Segoe UI Variable') | ForEach-Object {
            $fallback = @($v.fallback | Where-Object primaryFamily -eq $_ | Select-Object -First 1)
            if ($fallback.Count -eq 0) { 'no data' }
            elseif ($fallback[0].replacementObserved) { 'Pretendard' + (Get-Mark $fallback[0].pass ([bool] $fallback[0].pass)) }
            elseif ($fallback[0].sourceObserved) { 'Malgun Gothic' + (Get-Mark $fallback[0].pass ([bool] $fallback[0].pass)) }
            else { 'other' + (Get-Mark $fallback[0].pass ([bool] $fallback[0].pass)) }
        }
        $pixelText = Format-Cell $v.pixels $(if ($v.pixels.match) { $v.pixels.match } else { 'none' })
        $markdown.Add("| $modeText | $($run.source) | $isolatedText | $gdiText | $collectionText | $textFormatText | $($fallbackCells[0]) | $($fallbackCells[1]) | $pixelText |")
    }
}

$summaryMarkdown = Join-Path $resolvedOutputRoot 'summary.md'
$summaryJson = Join-Path $resolvedOutputRoot 'summary.json'
[IO.File]::WriteAllText($summaryMarkdown, (($markdown -join "`r`n") + "`r`n"), [Text.UTF8Encoding]::new($false))
$summary = [ordered]@{
    schema = 1
    kind = 'mactype-bold-substitution-field-summary'
    capturedAt = (Get-Date).ToUniversalTime().ToString('O')
    coreRoot = $resolvedCoreRoot
    probeRoot = $resolvedProbeRoot
    replacement = $Replacement
    pairFamily = $PairFamily
    runs = $runs
}
[IO.File]::WriteAllText($summaryJson, ($summary | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))

$markdown | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host "Summary: $summaryMarkdown"
if ($failedRuns -ne 0) {
    Write-Warning "$failedRuns run(s) produced no JSON or were not isolated."
    exit 1
}
exit 0
