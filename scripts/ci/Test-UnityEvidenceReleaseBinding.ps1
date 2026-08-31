$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$testScript = Join-Path $root 'scripts\lab\Test-UnityGameCompatibility.ps1'
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ("mactype-unity-release-binding-$([Guid]::NewGuid().ToString('N'))")

try {
    $gameRoot = Join-Path $testRoot 'game'
    $runtimeRoot = Join-Path $testRoot 'runtime'
    New-Item -ItemType Directory -Path $gameRoot, $runtimeRoot | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $gameRoot 'fixture.exe'), [byte[]]@(1, 2, 3))
    [IO.File]::WriteAllBytes((Join-Path $gameRoot 'UnityPlayer.dll'), [byte[]]@(4, 5, 6))
    [IO.File]::WriteAllBytes((Join-Path $runtimeRoot 'MacLoader64.exe'), [byte[]]@(7, 8, 9))
    [IO.File]::WriteAllBytes((Join-Path $runtimeRoot 'MacType64.dll'), [byte[]]@(10, 11, 12))
    [IO.File]::WriteAllText(
        (Join-Path $runtimeRoot 'MacType.ini'),
        "[General]`r`nUnityFontHook=2`r`n",
        [Text.UTF8Encoding]::new($false)
    )
    $output = Join-Path $testRoot 'must-not-exist.json'
    $expectedFailure = $null
    try {
        & $testScript `
            -GameExecutable (Join-Path $gameRoot 'fixture.exe') `
            -OutputPath $output `
            -MacLoader (Join-Path $runtimeRoot 'MacLoader64.exe') `
            -ExpectedCoreSha256 ('0' * 64)
    } catch {
        $expectedFailure = $_.Exception.Message
    }
    if ($expectedFailure -notmatch 'does not match the expected release SHA-256') {
        throw "The Unity evidence command did not reject a mismatched release core: $expectedFailure"
    }
    if (Test-Path -LiteralPath $output) {
        throw 'The Unity evidence command wrote evidence for a mismatched release core.'
    }
    Write-Host 'Unity evidence release binding contract passed.'
} finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if ($resolvedTestRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        $resolvedTestRoot -ne $temporaryBase -and
        (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
