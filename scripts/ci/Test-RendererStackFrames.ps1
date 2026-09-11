[CmdletBinding()]
param(
    [string[]] $Module = @(
        'artifacts/open-core/MacType.dll',
        'artifacts/open-core/MacType64.dll'
    ),

    [string[]] $Symbols = @(
        'Rel+Detours/MacType.Core.pdb',
        'x64/Rel+Detours/MacType64.Core.pdb'
    ),

    [int] $FirstPartyFrameBudget = 16384,

    [int] $DependencyFrameCeiling = 32768,

    [string] $FirstPartyObjectPattern = '[\\/]Rel\+Detours[\\/][^\\/]+\.obj$',

    [string] $EvidenceRoot = 'artifacts/stack-frames'
)

# Renderer hooks execute on whichever application, driver, or engine thread
# calls the hooked API. Field evidence from 2026-09-11 (Rebel Inc. Escalation,
# Unity 2022.3.62f3) shows such threads owning 64 KB stacks, so one frame
# above the budget is a process crash. MSVC routes every frame larger than a
# page through __chkstk with the size in EAX, which makes the frame inventory
# of a Release image recoverable without symbols. The PDB's section
# contributions then name the object file that owns each frame: first-party
# translation units keep the 16 KB budget, while the pinned FreeType fork's
# interpreter, hinter, and rasterizer frames are a documented dependency
# boundary held under a separate ceiling.

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Resolve-InputPath([string] $Relative) {
    if ([System.IO.Path]::IsPathRooted($Relative)) { return $Relative }
    return Join-Path $root $Relative
}

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

# MSF 7.0 container: fixed-size blocks, a stream directory addressed through a
# block map, and one block list per stream.
function Read-Msf([string] $Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 24)
    if ($magic -ne 'Microsoft C/C++ MSF 7.00') {
        throw "Not an MSF 7.0 program database: $Path"
    }
    $blockSize = [BitConverter]::ToInt32($bytes, 0x20)
    $directoryBytes = [BitConverter]::ToInt32($bytes, 0x2C)
    $blockMapBlock = [BitConverter]::ToInt32($bytes, 0x34)
    $directoryBlockCount = [int][Math]::Ceiling($directoryBytes / $blockSize)
    $directory = New-Object byte[] ($directoryBlockCount * $blockSize)
    for ($index = 0; $index -lt $directoryBlockCount; $index++) {
        $block = [BitConverter]::ToInt32($bytes, $blockMapBlock * $blockSize + $index * 4)
        [Array]::Copy($bytes, $block * $blockSize, $directory, $index * $blockSize, $blockSize)
    }
    $streamCount = [BitConverter]::ToInt32($directory, 0)
    $streams = New-Object System.Collections.Generic.List[object]
    $cursor = 4 + 4 * $streamCount
    for ($index = 0; $index -lt $streamCount; $index++) {
        $size = [BitConverter]::ToUInt32($directory, 4 + 4 * $index)
        # A nil stream is recorded as UInt32.MaxValue and owns no blocks.
        if ($size -eq [uint32]::MaxValue) { $size = [uint32] 0 }
        $blockCount = [int][Math]::Ceiling($size / $blockSize)
        $blocks = New-Object System.Collections.Generic.List[int]
        for ($block = 0; $block -lt $blockCount; $block++) {
            $blocks.Add([BitConverter]::ToInt32($directory, $cursor))
            $cursor += 4
        }
        $streams.Add([pscustomobject]@{ Size = [int] $size; Blocks = $blocks })
    }
    [pscustomobject]@{ Bytes = $bytes; BlockSize = $blockSize; Streams = $streams }
}

function Get-MsfStream($Msf, [int] $Index) {
    $stream = $Msf.Streams[$Index]
    $buffer = New-Object byte[] ([Math]::Max($stream.Size, 0))
    $written = 0
    foreach ($block in $stream.Blocks) {
        $length = [Math]::Min($Msf.BlockSize, $stream.Size - $written)
        if ($length -le 0) { break }
        [Array]::Copy($Msf.Bytes, $block * $Msf.BlockSize, $buffer, $written, $length)
        $written += $length
    }
    # The unary comma keeps the byte array intact; an unrolled array would be
    # re-materialized on every later call that binds it to a byte[] parameter.
    return ,$buffer
}

function Read-AsciiZ([byte[]] $Bytes, [ref] $Cursor) {
    $start = $Cursor.Value
    $end = [Array]::IndexOf($Bytes, [byte] 0, $start)
    if ($end -lt 0) { throw 'Unterminated string in the DBI module table.' }
    $Cursor.Value = $end + 1
    return [System.Text.Encoding]::UTF8.GetString($Bytes, $start, $end - $start)
}

# DBI stream: a 64-byte header, the module table, then the section
# contribution table that maps every code range to the object file that
# produced it.
function Read-DbiOwnership($Msf, $Image) {
    $dbi = Get-MsfStream $Msf 3
    $moduleTableSize = [BitConverter]::ToInt32($dbi, 24)
    $contributionSize = [BitConverter]::ToInt32($dbi, 28)
    $modules = @()
    $cursor = 64
    $moduleTableEnd = 64 + $moduleTableSize
    while ($cursor + 64 -le $moduleTableEnd) {
        $position = $cursor + 64
        $moduleName = Read-AsciiZ $dbi ([ref] $position)
        $objectName = Read-AsciiZ $dbi ([ref] $position)
        $modules += [pscustomobject]@{ Module = $moduleName; Object = $objectName }
        $cursor = ($position + 3) -band (-bnot 3)
    }
    # Ver60 (0xF12EBA2D) entries are 28 bytes; V2 (0xF13151E4) adds a 4-byte
    # COFF section index. PowerShell reads an 8-digit hex literal as a signed
    # Int32, so the constants are spelled in decimal.
    $version = [BitConverter]::ToUInt32($dbi, $moduleTableEnd)
    $entrySize = if ($version -eq [uint32] 4046371373) { 28 } elseif ($version -eq [uint32] 4046541284) { 32 } else {
        throw ('Unsupported section contribution version 0x{0:X8}' -f $version)
    }
    $contributions = New-Object System.Collections.Generic.List[object]
    $position = $moduleTableEnd + 4
    $contributionEnd = $moduleTableEnd + $contributionSize
    while ($position + $entrySize -le $contributionEnd) {
        $sectionIndex = [BitConverter]::ToUInt16($dbi, $position)
        $offset = [BitConverter]::ToInt32($dbi, $position + 4)
        $size = [BitConverter]::ToInt32($dbi, $position + 8)
        $moduleIndex = [BitConverter]::ToUInt16($dbi, $position + 16)
        if ($sectionIndex -ge 1 -and $sectionIndex -le $Image.Sections.Count -and $size -gt 0) {
            $contributions.Add([pscustomobject]@{
                Start  = [int64] $Image.Sections[$sectionIndex - 1].VirtualAddress + $offset
                Size   = [int64] $size
                Module = $moduleIndex
            })
        }
        $position += $entrySize
    }
    [pscustomobject]@{ Modules = $modules; Contributions = $contributions }
}

function Find-Owner($Ownership, [int64] $Rva) {
    foreach ($contribution in $Ownership.Contributions) {
        if ($Rva -ge $contribution.Start -and $Rva -lt $contribution.Start + $contribution.Size) {
            return $Ownership.Modules[$contribution.Module]
        }
    }
    return $null
}

# The stack-probe prologue is stable across the supported MSVC CRTs; the
# wildcard covers the small displacement the x86 CRT has changed between
# versions.
$probeSignatures = @{
    0x8664 = @(0x48, 0x83, 0xEC, 0x10, 0x4C, 0x89, 0x14, 0x24, 0x4C, 0x89, 0x5C, 0x24, 0x08, 0x4D, 0x33, 0xDB)
    0x014C = @(0x51, 0x8D, 0x4C, 0x24, -1, 0x2B, 0xC8, 0x1B, 0xC0, 0xF7, 0xD0, 0x23, 0xC8, 0x8B, 0xC4)
}

if ($Module.Count -ne $Symbols.Count) {
    throw 'Every renderer module needs exactly one program database.'
}

New-Item -ItemType Directory -Force -Path (Join-Path $root $EvidenceRoot) | Out-Null
$failed = $false
for ($moduleIndex = 0; $moduleIndex -lt $Module.Count; $moduleIndex++) {
    $path = Resolve-InputPath $Module[$moduleIndex]
    $symbolPath = Resolve-InputPath $Symbols[$moduleIndex]
    foreach ($required in @($path, $symbolPath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Renderer gate input is missing: $required"
        }
    }
    $image = Read-Image $path
    if (-not $probeSignatures.ContainsKey([int] $image.Machine)) {
        throw ("Unsupported machine type 0x{0:X4}: {1}" -f $image.Machine, $path)
    }
    $ranges = @(Get-ExecutableRanges $image)
    if ($ranges.Count -eq 0) { throw "No executable section: $path" }
    $ownership = Read-DbiOwnership (Read-Msf $symbolPath) $image
    if ($ownership.Contributions.Count -eq 0) {
        throw "The program database carries no section contributions: $symbolPath"
    }
    # A healthy first-party build has no probed frame of its own, so the
    # ownership map is validated against the module table rather than against
    # the frames it happens to own.
    if (-not ($ownership.Modules | Where-Object { $_.Object -match $FirstPartyObjectPattern })) {
        throw "No module in the program database matches the first-party object pattern; the ownership map cannot be trusted: $symbolPath"
    }

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
                    $rva = [int64] $index + $range.Delta
                    $owner = Find-Owner $ownership $rva
                    $objectName = if ($owner) { $owner.Object } else { '' }
                    $frames += [pscustomobject]@{
                        Bytes      = [int64] [BitConverter]::ToUInt32($bytes, $index - 4)
                        Rva        = $rva
                        Object     = $objectName
                        Unit       = if ($owner) { [System.IO.Path]::GetFileName($owner.Module) } else { '(unknown)' }
                        FirstParty = [bool] ($owner -and $objectName -match $FirstPartyObjectPattern)
                    }
                }
            }
            $index++
        }
    }
    if ($frames.Count -eq 0) {
        throw "No __chkstk caller was found; the frame inventory cannot be trusted: $path"
    }
    if ($frames | Where-Object { $_.Unit -eq '(unknown)' }) {
        throw "A probed frame has no owning object file in the program database: $symbolPath"
    }

    $name = [System.IO.Path]::GetFileName($path)
    $report = foreach ($frame in ($frames | Sort-Object Bytes -Descending)) {
        $kind = if ($frame.FirstParty) { 'first-party' } else { 'dependency' }
        '{0,10} bytes  call rva 0x{1:X}  {2,-12} {3} ({4})' -f $frame.Bytes, $frame.Rva, $kind, $frame.Unit, [System.IO.Path]::GetFileName($frame.Object)
    }
    $evidence = Join-Path $root (Join-Path $EvidenceRoot ("$name.txt"))
    Set-Content -LiteralPath $evidence -Value $report -Encoding utf8
    $largestFirstParty = [int64] ($frames | Where-Object FirstParty | Measure-Object Bytes -Maximum).Maximum
    $largestDependency = [int64] ($frames | Where-Object { -not $_.FirstParty } | Measure-Object Bytes -Maximum).Maximum
    Write-Host ("{0}: {1} probed frames; largest first-party {2} bytes (budget {3}); largest dependency {4} bytes (ceiling {5})" -f `
        $name, $frames.Count, $largestFirstParty, $FirstPartyFrameBudget, $largestDependency, $DependencyFrameCeiling)
    foreach ($frame in ($frames | Sort-Object Bytes -Descending)) {
        $limit = if ($frame.FirstParty) { $FirstPartyFrameBudget } else { $DependencyFrameCeiling }
        if ($frame.Bytes -le $limit) { continue }
        $kind = if ($frame.FirstParty) { 'first-party budget' } else { 'dependency ceiling' }
        Write-Error ("{0}: {1} ({2}) reserves {3} bytes of stack at call rva 0x{4:X}, above the {5}-byte {6}" -f `
            $name, $frame.Unit, [System.IO.Path]::GetFileName($frame.Object), $frame.Bytes, $frame.Rva, $limit, $kind) -ErrorAction Continue
        $failed = $true
    }
}
if ($failed) { exit 1 }
Write-Host "Renderer stack frame budget gate passed for $($Module.Count) modules."
