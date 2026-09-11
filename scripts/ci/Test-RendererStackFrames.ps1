[CmdletBinding()]
param(
    [string[]] $Module = @(
        'artifacts/open-core/MacType.dll',
        'artifacts/open-core/MacType64.dll'
    ),

    [int] $MaximumFrameBytes = 16384,

    [string] $EvidenceRoot = 'artifacts/stack-frames'
)

# Renderer hooks execute on whichever application, driver, or engine thread
# calls the hooked API. Field evidence from 2026-09-11 (Rebel Inc. Escalation,
# Unity 2022.3.62f3) shows such threads owning 64 KB stacks, so one frame
# above the budget is a process crash. MSVC routes every frame larger than a
# page through __chkstk with the size in EAX, which makes the frame inventory
# of a Release image recoverable without symbols.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Find-Pattern([byte[]] $Bytes, [int] $Start, [int] $End, [int[]] $Pattern) {
    # A negative pattern element is a wildcard byte.
    $limit = $End - $Pattern.Length
    for ($index = $Start; $index -le $limit; $index++) {
        $matched = $true
        for ($offset = 0; $offset -lt $Pattern.Length; $offset++) {
            $expected = $Pattern[$offset]
            if ($expected -ge 0 -and $Bytes[$index + $offset] -ne $expected) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $index }
    }
    return -1
}

function Read-Image([string] $Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a PE image: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -le 0 -or $peOffset + 24 -gt $bytes.Length -or
        [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "Missing PE signature: $Path"
    }
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $optionalSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $sectionTable = $peOffset + 24 + $optionalSize
    $sections = @()
    for ($index = 0; $index -lt $sectionCount; $index++) {
        $entry = $sectionTable + $index * 40
        $sections += [pscustomobject]@{
            VirtualSize     = [BitConverter]::ToUInt32($bytes, $entry + 8)
            VirtualAddress  = [BitConverter]::ToUInt32($bytes, $entry + 12)
            RawSize         = [BitConverter]::ToUInt32($bytes, $entry + 16)
            RawPointer      = [BitConverter]::ToUInt32($bytes, $entry + 20)
            Characteristics = [BitConverter]::ToUInt32($bytes, $entry + 36)
        }
    }
    [pscustomobject]@{
        Bytes    = $bytes
        Machine  = $machine
        Sections = $sections
    }
}

function Get-ExecutableRanges($Image) {
    foreach ($section in $Image.Sections) {
        if (($section.Characteristics -band 0x20000000) -eq 0) { continue }
        $length = [Math]::Min([int64] $section.RawSize, [int64] ($Image.Bytes.Length - $section.RawPointer))
        if ($length -le 0) { continue }
        [pscustomobject]@{
            FileStart = [int] $section.RawPointer
            FileEnd   = [int] ($section.RawPointer + $length)
            Delta     = [int64] $section.VirtualAddress - [int64] $section.RawPointer
        }
    }
}

# The stack-probe prologue is stable across the supported MSVC CRTs; the
# wildcard covers the small displacement the x86 CRT has changed between
# versions.
$probeSignatures = @{
    0x8664 = @(0x48, 0x83, 0xEC, 0x10, 0x4C, 0x89, 0x14, 0x24, 0x4C, 0x89, 0x5C, 0x24, 0x08, 0x4D, 0x33, 0xDB)
    0x014C = @(0x51, 0x8D, 0x4C, 0x24, -1, 0x2B, 0xC8, 0x1B, 0xC0, 0xF7, 0xD0, 0x23, 0xC8, 0x8B, 0xC4)
}

New-Item -ItemType Directory -Force -Path (Join-Path $root $EvidenceRoot) | Out-Null
$failed = $false
foreach ($relative in $Module) {
    $path = if ([System.IO.Path]::IsPathRooted($relative)) { $relative } else { Join-Path $root $relative }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Renderer module is missing: $path"
    }
    $image = Read-Image $path
    if (-not $probeSignatures.ContainsKey([int] $image.Machine)) {
        throw ("Unsupported machine type 0x{0:X4}: {1}" -f $image.Machine, $path)
    }
    $ranges = @(Get-ExecutableRanges $image)
    if ($ranges.Count -eq 0) { throw "No executable section: $path" }

    $probeRva = -1
    foreach ($range in $ranges) {
        $hit = Find-Pattern $image.Bytes $range.FileStart $range.FileEnd $probeSignatures[[int] $image.Machine]
        if ($hit -ge 0) {
            $probeRva = [int64] $hit + $range.Delta
            break
        }
    }
    if ($probeRva -lt 0) {
        throw "The __chkstk stack probe was not found; the frame inventory cannot be trusted: $path"
    }

    $frames = @()
    $bytes = $image.Bytes
    foreach ($range in $ranges) {
        $index = $range.FileStart + 5
        $end = $range.FileEnd - 5
        while ($index -lt $end) {
            if ($bytes[$index] -eq 0xE8) {
                $target = [int64] $index + 5 + [BitConverter]::ToInt32($bytes, $index + 1) + $range.Delta
                if ($target -eq $probeRva -and $bytes[$index - 5] -eq 0xB8) {
                    $frames += [pscustomobject]@{
                        Bytes = [int64] [BitConverter]::ToUInt32($bytes, $index - 4)
                        Rva   = [int64] $index + $range.Delta
                    }
                }
            }
            $index++
        }
    }
    if ($frames.Count -eq 0) {
        throw "No __chkstk caller was found; the frame inventory cannot be trusted: $path"
    }

    $name = [System.IO.Path]::GetFileName($path)
    $report = foreach ($frame in ($frames | Sort-Object Bytes -Descending)) {
        '{0,10} bytes  call rva 0x{1:X}' -f $frame.Bytes, $frame.Rva
    }
    $evidence = Join-Path $root (Join-Path $EvidenceRoot ("$name.txt"))
    Set-Content -LiteralPath $evidence -Value $report -Encoding utf8
    $oversized = @($frames | Where-Object { $_.Bytes -gt $MaximumFrameBytes })
    Write-Host ("{0}: {1} probed frames, largest {2} bytes, budget {3} bytes" -f `
        $name, $frames.Count, ($frames | Measure-Object Bytes -Maximum).Maximum, $MaximumFrameBytes)
    foreach ($frame in ($oversized | Sort-Object Bytes -Descending)) {
        Write-Error ("{0}: stack frame of {1} bytes at call rva 0x{2:X} exceeds the {3}-byte foreign-thread budget" -f `
            $name, $frame.Bytes, $frame.Rva, $MaximumFrameBytes) -ErrorAction Continue
        $failed = $true
    }
}
if ($failed) { exit 1 }
Write-Host "Renderer stack frame budget gate passed for $($Module.Count) modules."
