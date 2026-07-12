[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$files = Get-ChildItem -LiteralPath 'preview-helper' -Recurse -File -Include '*.cpp', '*.h'
$failed = $false
foreach ($file in $files) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    if ($text -match "`t") {
        Write-Error "$($file.FullName): tabs are not allowed in new C++ code" -ErrorAction Continue
        $failed = $true
    }
    if ($text -match '(?m)[ \t]+$') {
        Write-Error "$($file.FullName): trailing whitespace" -ErrorAction Continue
        $failed = $true
    }
}
if ($failed) { exit 1 }
Write-Host "New C++ style gate passed for $($files.Count) files."
