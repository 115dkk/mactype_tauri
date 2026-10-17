[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,

    [string[]] $Arguments = @(),

    [string] $WorkingDirectory = (Get-Location).Path,

    [ValidateRange(1, 1800)]
    [int] $TimeoutSeconds = 120,

    [switch] $Interactive
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\ProbeDesktop.ps1')
exit (Invoke-ProbeDesktopCommand -Executable $Executable -Arguments $Arguments `
    -WorkingDirectory $WorkingDirectory -TimeoutSeconds $TimeoutSeconds -Interactive:$Interactive)
