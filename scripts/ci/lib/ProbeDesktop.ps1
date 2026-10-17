#requires -Version 7.2

if (-not ('MacType.ControlCenter.Ci.ProbeDesktop' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'ProbeDesktop.cs')
}

function Test-ProbeDesktopContext {
    [CmdletBinding()]
    param()

    [MacType.ControlCenter.Ci.ProbeDesktop]::IsIsolatedContext()
}

function Invoke-ProbeDesktopCommand {
    [CmdletBinding()]
    [OutputType([int])]
    param(
        [Parameter(Mandatory)] [Alias('FilePath')] [string] $Executable,
        [AllowEmptyCollection()] [string[]] $Arguments = @(),
        [string] $WorkingDirectory = (Get-Location).ProviderPath,
        [ValidateRange(1, 1800)] [int] $TimeoutSeconds = 600,
        [ValidateRange(1, 16777216)] [int] $MaximumOutputBytes = 4194304,
        [switch] $Interactive
    )

    try {
        $result = [MacType.ControlCenter.Ci.ProbeDesktop]::Run(
            $Executable, $Arguments, $WorkingDirectory,
            $TimeoutSeconds * 1000, $MaximumOutputBytes, $Interactive.IsPresent
        )
    }
    catch {
        $failure = $_.Exception
        while ($null -ne $failure.InnerException) { $failure = $failure.InnerException }
        if ($failure.Data.Contains('ProbeStandardOutput')) {
            $outputBytes = [byte[]] $failure.Data['ProbeStandardOutput']
            $errorBytes = [byte[]] $failure.Data['ProbeStandardError']
            [Console]::OpenStandardOutput().Write($outputBytes, 0, $outputBytes.Length)
            [Console]::OpenStandardError().Write($errorBytes, 0, $errorBytes.Length)
        }
        throw
    }
    # Keep the success pipeline reserved for the exit code, even when assigned by an entry guard.
    [Console]::OpenStandardOutput().Write($result.StandardOutputBytes, 0, $result.StandardOutputBytes.Length)
    [Console]::OpenStandardError().Write($result.StandardErrorBytes, 0, $result.StandardErrorBytes.Length)
    return $result.ExitCode
}

Set-Alias -Name Invoke-ProbeDesktopProcess -Value Invoke-ProbeDesktopCommand

function Invoke-ProbeDesktopScript {
    [CmdletBinding()]
    [OutputType([int])]
    param(
        [Parameter(Mandatory)] [string] $ScriptPath,
        [System.Collections.IDictionary] $Parameters = @{},
        [ValidateRange(1, 1800)] [int] $TimeoutSeconds = 600,
        [ValidateRange(1, 16777216)] [int] $MaximumOutputBytes = 4194304,
        [string] $WorkingDirectory = (Get-Location).ProviderPath,
        [string] $ScratchDirectory = [IO.Path]::GetTempPath(),
        [switch] $Interactive
    )

    if (-not [IO.Path]::IsPathFullyQualified($ScriptPath) -or
        -not (Test-Path -LiteralPath $ScriptPath -PathType Leaf)) {
        throw 'The probe script must be an existing absolute path.'
    }
    if (-not [IO.Path]::IsPathFullyQualified($ScratchDirectory) -or
        -not (Test-Path -LiteralPath $ScratchDirectory -PathType Container)) {
        throw 'The probe scratch directory must be an existing absolute path.'
    }
    $directory = Join-Path $ScratchDirectory ('mactype-probe-' + [Guid]::NewGuid().ToString('N'))
    [void] [IO.Directory]::CreateDirectory($directory)
    try {
        $forwarded = @{}
        foreach ($key in $Parameters.Keys) {
            $value = $Parameters[$key]
            # SwitchParameter's deserialized object is not a bindable switch value.
            if ($value -is [System.Management.Automation.SwitchParameter]) { $value = $value.IsPresent }
            $forwarded[$key] = $value
        }
        $payloadPath = Join-Path $directory 'parameters.clixml'
        @{
            ScriptPath = $ScriptPath
            Parameters = $forwarded
            WorkingDirectory = $WorkingDirectory
        } | Export-Clixml -LiteralPath $payloadPath -Depth 32
        $bootstrapPath = Join-Path $directory 'invoke.ps1'
        # Values travel as serialized data, not executable PowerShell source or shell arguments.
        @'
param([Parameter(Mandatory)] [string] $PayloadPath)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Console]::OutputEncoding
try {
    $payload = Import-Clixml -LiteralPath $PayloadPath
    Set-Location -LiteralPath $payload.WorkingDirectory
    $parameters = $payload.Parameters
    $global:LASTEXITCODE = 0
    & $payload.ScriptPath @parameters
    $succeeded = $?
    # A handled native failure leaves LASTEXITCODE set after a successful script.
    if ($succeeded) { exit 0 }
    if ($global:LASTEXITCODE -is [int] -and $global:LASTEXITCODE -ne 0) {
        exit $global:LASTEXITCODE
    }
    exit 1
}
catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 1
}
'@ | Set-Content -LiteralPath $bootstrapPath -Encoding utf8
        Invoke-ProbeDesktopCommand -Executable (Join-Path $PSHOME 'pwsh.exe') -Arguments @(
            '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $bootstrapPath, '-PayloadPath', $payloadPath
        ) -WorkingDirectory $WorkingDirectory -TimeoutSeconds $TimeoutSeconds `
            -MaximumOutputBytes $MaximumOutputBytes -Interactive:$Interactive
    }
    finally {
        Remove-Item -LiteralPath $directory -Recurse -Force
    }
}
