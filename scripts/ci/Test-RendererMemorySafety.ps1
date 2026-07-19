[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Read-Source([string] $relativePath) {
    Get-Content -LiteralPath (Join-Path $root $relativePath) -Raw
}

function Require-Pattern([string] $text, [string] $pattern, [string] $message) {
    if ($text -notmatch $pattern) {
        throw $message
    }
}

function Reject-Pattern([string] $text, [string] $pattern, [string] $message) {
    if ($text -match $pattern) {
        throw $message
    }
}

$ftHeader = Read-Source 'ft.h'
$ftSource = Read-Source 'ft.cpp'
$commonSource = Read-Source 'common.cpp'
$settingsSource = Read-Source 'settings.cpp'

Reject-Pattern $ftHeader 'ZeroMemory\s*\(\s*this\s*,\s*sizeof\s*\(\s*\*this\s*\)\s*\)' `
    'FREETYPE_PARAMS must not byte-zero an object containing std::wstring members.'
Require-Pattern $ftHeader 'delete\[\]\s+unicode\s*;' `
    'ControlIder must release its array with delete[].'
Require-Pattern $ftHeader 'delete\[\]\s+Dx\s*;' `
    'FreeTypeDrawInfo must release Dx with delete[].'
Require-Pattern $ftHeader 'delete\[\]\s+Dy\s*;' `
    'FreeTypeDrawInfo must release Dy with delete[].'
Reject-Pattern $ftSource '(?m)^\s*delete\s+lpfontlink(?:\[i\])?\s*;' `
    'Font-link arrays must not be released with scalar delete.'
Reject-Pattern $settingsSource 'memcpy\s*\(\s*\(void\s*\*\)\s*json\.c_str\s*\(\s*\)' `
    'WM_COPYDATA must not write through std::string::c_str().'
Reject-Pattern $settingsSource 'transform\s*\([^;]*::tolower\s*\)' `
    'Wide-character case conversion must not call narrow tolower.'
Reject-Pattern $commonSource 'transform\s*\([^;]*::tolower\s*\)' `
    'Wide-character case conversion must not call narrow tolower.'
Require-Pattern $settingsSource 'ret\s*>\s*\(\s*INT_MAX\s*-\s*digit\s*\)\s*/\s*10' `
    'INI integer parsing must reject signed overflow before multiplication.'

Write-Host 'Renderer memory-safety source contracts passed.'
