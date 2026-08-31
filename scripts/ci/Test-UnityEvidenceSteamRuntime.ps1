$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$testScript = Join-Path $root 'scripts\lab\Test-UnityGameCompatibility.ps1'
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryBase ("mactype-unity-steam-runtime-$([Guid]::NewGuid().ToString('N'))")

try {
    New-Item -ItemType Directory -Path $testRoot | Out-Null
    $game = Join-Path $testRoot 'steam-fixture.exe'
    $source = Join-Path $testRoot 'steam-fixture.cs'
    [IO.File]::WriteAllText($source, @'
using System;
using System.IO;
using System.Threading;

public static class SteamFixture
{
    public static int Main(string[] arguments)
    {
        for (var index = 0; index + 1 < arguments.Length; index++)
        {
            if (arguments[index] == "-logFile")
            {
                File.WriteAllText(
                    arguments[index + 1],
                    "InvalidOperationException: Steamworks is not initialized.\n");
                break;
            }
        }
        Thread.Sleep(TimeSpan.FromSeconds(20));
        return 0;
    }
}
'@, [Text.UTF8Encoding]::new($false))
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    & $compiler /nologo /target:exe "/out:$game" $source
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $game -PathType Leaf)) {
        throw 'The Steam runtime fixture executable did not compile.'
    }
    [IO.File]::WriteAllBytes(
        (Join-Path $testRoot 'UnityPlayer.dll'),
        [byte[]]@(1, 2, 3)
    )
    $output = Join-Path $testRoot 'steam-runtime-evidence.json'
    $expectedFailure = $null
    try {
        & $testScript `
            -GameExecutable $game `
            -OutputPath $output `
            -SteamAppId '42' `
            -ObserveSeconds 5
    } catch {
        $expectedFailure = $_.Exception.Message
    }
    if ($expectedFailure -notmatch 'Steam runtime initialization failed') {
        throw "The Unity evidence command accepted an explicit Steamworks failure: $expectedFailure"
    }
    $evidence = Get-Content -Raw -LiteralPath $output | ConvertFrom-Json
    if ($evidence.observation.steamRuntime -ne 'initialization-failed') {
        throw 'The Unity evidence did not preserve the explicit Steamworks failure.'
    }
    Write-Host 'Unity evidence Steam runtime contract passed.'
} finally {
    Get-Process -Name 'steam-fixture' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    if ($resolvedTestRoot.StartsWith($temporaryBase, [StringComparison]::OrdinalIgnoreCase) -and
        $resolvedTestRoot -ne $temporaryBase -and
        (Test-Path -LiteralPath $resolvedTestRoot)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
