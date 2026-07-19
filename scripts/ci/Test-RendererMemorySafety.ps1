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
$commonHeader = Read-Source 'common.h'
$engineSource = Read-Source 'fteng.cpp'
$exportSource = Read-Source 'expfunc.cpp'
$hookSource = Read-Source 'hook.cpp'
$settingsHeader = Read-Source 'settings.h'
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
Reject-Pattern $settingsHeader 'memcpy\s*\(\s*\(void\s*\*\)\s*json\.c_str\s*\(\s*\)' `
    'WM_COPYDATA must not write through std::string::c_str().'
Reject-Pattern $settingsSource 'transform\s*\([^;]*::tolower\s*\)' `
    'Wide-character case conversion must not call narrow tolower.'
Reject-Pattern $commonSource 'transform\s*\([^;]*::tolower\s*\)' `
    'Wide-character case conversion must not call narrow tolower.'
Require-Pattern $settingsSource 'ret\s*>\s*\(\s*INT_MAX\s*-\s*digit\s*\)\s*/\s*10' `
    'INI integer parsing must reject signed overflow before multiplication.'

Require-Pattern $commonHeader 'BOOL\s+ChangeFileName\s*\(\s*LPWSTR\s+lpSrc\s*,\s*size_t\s+cchSrc' `
    'ChangeFileName must receive the destination capacity, not the current path length.'
Reject-Pattern $exportSource 'wcscat\s*\(\s*lpSrc\s*,' `
    'Renderer bootstrap paths must not use unbounded wcscat.'
Reject-Pattern $exportSource 'new\s+char\s*\[\s*len\s*\]' `
    'Wide-to-multibyte conversion must reserve space for the terminator.'
Require-Pattern $settingsSource 'namesz\s*=\s*nNameCapacity\s*;' `
    'RegEnumValue name capacity must be expressed in WCHARs.'
Reject-Pattern $settingsSource 'namesz\s*=\s*nBufSize\s*;' `
    'RegEnumValue must not receive a byte count for its character-count parameter.'
Require-Pattern $engineSource 'if\s*\(\s*offset\s*>\s*pThis->m_dwSize\s*\)' `
    'Mapped font reads must reject offsets beyond the mapped buffer.'
Require-Pattern $settingsSource 'GetModuleFileName\s*\(\s*NULL\s*,\s*name\s*,\s*countof\s*\(\s*name\s*\)\s*\)' `
    'GetAppDir must pass the actual module-name buffer capacity.'
Reject-Pattern $hookSource 'wcscat\s*\(\s*dllPath\s*,' `
    'EasyHook bootstrap path construction must be bounded.'
Require-Pattern $ftHeader 'new\s+char\s*\[\s*0x10000\s*\]' `
    'ControlIder must cover every possible WCHAR value, including 0xffff.'
Require-Pattern $ftSource 'if\s*\(\s*size\s*>\s*sizeof\s+m_localbuf\s*\)' `
    'Temporary glyph storage must use the heap when the request exceeds the local buffer.'
Reject-Pattern $ftSource 'LPTTPOLYGONHEADER\s+ttphpend' `
    'Native outline parsing must use byte bounds instead of scaled typed-pointer bounds.'
Require-Pattern $ftSource 'polygonRemaining\s*<\s*sizeof\s*\(\s*TTPOLYGONHEADER\s*\)' `
    'Native outline parsing must validate a complete polygon header before reading it.'
Require-Pattern $ftSource 'ttphp->cb\s*<\s*sizeof\s*\(\s*TTPOLYGONHEADER\s*\).*?ttphp->cb\s*>\s*polygonRemaining' `
    'Native outline parsing must reject undersized and out-of-buffer polygon lengths.'
Require-Pattern $ftSource '(?s)curvePrefixSize.*?curveRemaining\s*<\s*curvePrefixSize' `
    'Native outline parsing must validate the fixed curve prefix before reading cpfx.'
Require-Pattern $ftSource 'ttpcp->cpfx\s*>\s*\(\s*curveRemaining\s*-\s*curvePrefixSize\s*\)\s*/\s*sizeof\s*\(\s*POINTFX\s*\)' `
    'Native outline parsing must bound every POINTFX array by the containing polygon.'
Require-Pattern $ftSource 'pointCount\s*>\s*0x7fff\s*-\s*ttpcp->cpfx' `
    'Native outline parsing must reject point counts that overflow FT_Outline fields.'
Reject-Pattern $ftHeader 'int\s+LastPos\s*=\s*0\s*,\s*LPCurPos\s*=\s*0' `
    'Advance accumulation must not overflow a signed int before coordinate scaling.'
Require-Pattern $ftHeader 'double\s+fLPCurPos\s*=\s*0' `
    'Advance accumulation must widen each input before addition and scaling.'

Reject-Pattern $settingsHeader '\.detach\s*\(\s*\)' `
    'The control-center message thread must not outlive its owning object.'
Require-Pattern $settingsHeader 'HANDLE\s+m_msgThread\s*;' `
    'The control-center message thread must have an owned join handle.'
Require-Pattern $settingsHeader '~CControlCenter\s*\(\s*\)\s*\{\s*DestroyMessageWnd\s*\(\s*\)' `
    'CControlCenter destruction must stop and join the message thread.'
Require-Pattern $settingsHeader 'WaitForSingleObject\s*\(\s*m_msgThread\s*,\s*INFINITE\s*\)' `
    'Message-window teardown must wait for the thread before releasing CControlCenter.'

Require-Pattern $ftSource '(?s)if\s*\(\s*!axisFound\s*\)\s*\{.*?free\s*\(\s*coords\s*\).*?FT_Done_MM_Var\s*\(.*?mm_var\s*\).*?return false' `
    'Variable-font rejection must release both coordinate and MM_Var allocations.'
Require-Pattern $ftSource '(?s)FT_Set_Var_Design_Coordinates\s*\(.*?coords\s*\).*?free\s*\(\s*coords\s*\).*?FT_Done_MM_Var\s*\(.*?mm_var\s*\)' `
    'Variable-font success must release both coordinate and MM_Var allocations.'
Require-Pattern $engineSource '(?s)void FreeTypeCharData::SetGlyph.*?SubMemUsed\s*\(\s*size\s*\).*?size\s*=\s*0' `
    'Replacing a cached glyph must remove its previous memory charge.'
Require-Pattern $engineSource '(?s)ERROR_Init:\s*if\s*\(\s*m_ftFace\s*\).*?FT_Done_Face\s*\(\s*m_ftFace\s*\).*?m_ftFace\s*=\s*NULL' `
    'Font initialization failure must release any face opened before the failure.'
Require-Pattern $settingsSource '(?s)RegOpenKeyEx\(HKEY_LOCAL_MACHINE,\s*REGKEY3.*?\)\)\s*\{\s*delete\[\]\s*name;\s*delete\[\]\s*value;\s*delete\[\]\s*buf;\s*return;' `
    'Font-link initialization must release temporary arrays when REGKEY3 cannot be opened.'
Require-Pattern $settingsSource '(?s)RegOpenKeyEx\(HKEY_LOCAL_MACHINE,\s*REGKEY4.*?\)\)\s*\{\s*delete\[\]\s*name;\s*delete\[\]\s*value;\s*delete\[\]\s*buf;\s*return;' `
    'Font-link initialization must release temporary arrays when REGKEY4 cannot be opened.'

Write-Host 'Renderer memory-safety source contracts passed.'
