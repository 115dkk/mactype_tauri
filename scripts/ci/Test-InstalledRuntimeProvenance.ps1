$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$diagnostic = Join-Path $root 'scripts\lab\Get-InstalledRuntimeProvenance.ps1'
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ("mactype-runtime-provenance-$([Guid]::NewGuid().ToString('N'))")

function Write-Utf8File([string] $Path, [string] $Text) {
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent | Out-Null
    }
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Get-Sha256([string] $Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

try {
    $serviceRoot = Join-Path $testRoot 'service'
    $profileRoot = Join-Path $testRoot 'profiles'
    $version = '0.2.0+ci.42.abcdef123456.current'
    $runtimeRoot = Join-Path (Join-Path $serviceRoot 'bin') $version
    New-Item -ItemType Directory -Path $runtimeRoot | Out-Null

    $runtimeFiles = [ordered]@{
        'mactype-service.exe' = 'service-fixture'
        'mactype-injector32.exe' = 'injector-32-fixture'
        'mactype-injector64.exe' = 'injector-64-fixture'
        'MacType.dll' = 'renderer-32-fixture'
        'MacType64.dll' = 'renderer-64-fixture'
    }
    foreach ($entry in $runtimeFiles.GetEnumerator()) {
        Write-Utf8File (Join-Path $runtimeRoot $entry.Key) $entry.Value
    }

    Write-Utf8File (Join-Path $serviceRoot 'current.json') (
        [ordered]@{ schema = 1; version = $version } | ConvertTo-Json -Compress
    )
    $receiptFiles = [ordered]@{}
    foreach ($name in $runtimeFiles.Keys) {
        $receiptFiles[$name] = "sha256:$(Get-Sha256 (Join-Path $runtimeRoot $name))"
    }
    $receipt = [ordered]@{
        schema = 1
        version = $version
        files = $receiptFiles
    }
    Write-Utf8File (
        Join-Path (Join-Path $serviceRoot 'runtime-receipts') "$version.json"
    ) ($receipt | ConvertTo-Json -Depth 4 -Compress)

    $profileText = "[General]`r`nUnityFontHook=2`r`nHookChildProcesses=1`r`n"
    $profileFixture = Join-Path $testRoot 'profile-fixture.ini'
    Write-Utf8File $profileFixture $profileText
    $profileDigest = Get-Sha256 $profileFixture
    $generationRoot = Join-Path (Join-Path $profileRoot 'generations') $profileDigest
    New-Item -ItemType Directory -Path $generationRoot | Out-Null
    Copy-Item -LiteralPath $profileFixture -Destination (Join-Path $generationRoot 'profile.ini')
    Write-Utf8File (Join-Path $profileRoot 'active.json') (
        [ordered]@{ schema = 1; generation = "sha256:$profileDigest" } |
            ConvertTo-Json -Compress
    )

    $expectedManifest = Join-Path $testRoot 'expected-manifest.json'
    Write-Utf8File $expectedManifest ($receipt | ConvertTo-Json -Depth 4 -Compress)
    $output = Join-Path $testRoot 'provenance.json'

    & $diagnostic `
        -ServiceRoot $serviceRoot `
        -ProfileRoot $profileRoot `
        -ExpectedManifest $expectedManifest `
        -OutputPath $output | Out-Null

    $result = Get-Content -Raw -LiteralPath $output | ConvertFrom-Json
    if ($result.kind -ne 'mactype-installed-runtime-provenance') {
        throw 'The diagnostic did not emit the installed-runtime provenance schema.'
    }
    if ($result.service.version -ne $version -or -not $result.service.receiptVerified) {
        throw 'The diagnostic did not verify the selected protected runtime receipt.'
    }
    if (-not $result.release.requested -or -not $result.release.matches) {
        throw 'The diagnostic did not bind the installed runtime to the expected release manifest.'
    }
    if (-not $result.profile.digestVerified -or
        $result.profile.unityFontHook -ne 2 -or
        -not $result.profile.hookChildProcesses) {
        throw 'The diagnostic did not verify the active Unity profile generation.'
    }
    if (@($result.issues).Count -ne 0) {
        throw "A valid fixture produced provenance issues: $($result.issues -join '; ')"
    }

    Write-Host 'Installed runtime provenance contract passed.'
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if ($resolvedTestRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        $resolvedTestRoot -ne $temporaryBase -and
        (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
