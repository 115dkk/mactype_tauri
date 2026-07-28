[CmdletBinding()]
param(
    [string] $Compiler = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\InstallerWindowsAssertions.ps1')

function Read-BoundedFixtureLog {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [int] $MaximumBytes = 256 * 1024
    )

    $stream = [IO.FileStream]::new(
        $Path,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite
    )
    try {
        $bytes = [byte[]]::new($MaximumBytes + 1)
        $total = 0
        while ($total -lt $bytes.Length) {
            $read = $stream.Read($bytes, $total, $bytes.Length - $total)
            if ($read -eq 0) { break }
            $total += $read
        }
        if ($total -gt $MaximumBytes) { throw 'Inno fixture log exceeded its diagnostic bound.' }
        return [Text.UTF8Encoding]::new($false, $true).GetString($bytes, 0, $total)
    }
    finally {
        $stream.Dispose()
    }
}

function Invoke-FixtureInstaller {
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string] $LogPath
    )

    Invoke-ProcessExit -File $Executable -Arguments @(
        '/VERYSILENT',
        '/SUPPRESSMSGBOXES',
        '/NORESTART',
        '/SP-',
        "/LOG=$LogPath"
    ) -TimeoutMilliseconds 60000
}

function Get-FixtureTreeSnapshot {
    param([Parameter(Mandatory)] [string] $Path)

    @(
        Get-ChildItem -LiteralPath $Path -Recurse -Force | Sort-Object FullName | ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($Path, $_.FullName)
            if ($_.PSIsContainer) {
                "directory|$relative"
            }
            else {
                $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                "file|$relative|$($_.Length)|$hash"
            }
        }
    ) -join "`n"
}

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) `
    "mactype-inno-failure-contract-$PID-$([Guid]::NewGuid().ToString('N'))"
$outputRoot = Join-Path $temporaryRoot 'output'
$legacyAppRoot = Join-Path $temporaryRoot 'legacy-app'
$fatalAppRoot = Join-Path $temporaryRoot 'fatal-app'
$payloadPath = Join-Path $temporaryRoot 'payload.txt'
$legacyScriptPath = Join-Path $temporaryRoot 'legacy.iss'
$fatalScriptPath = Join-Path $temporaryRoot 'fatal.iss'
$legacyLogPath = Join-Path $temporaryRoot 'legacy.log'
$fatalLogPath = Join-Path $temporaryRoot 'fatal.log'
$fatalUpgradeLogPath = Join-Path $temporaryRoot 'fatal-upgrade.log'
$emptyRootScriptPath = Join-Path $temporaryRoot 'empty-root-cleanup.iss'
$emptyRootInstallLogPath = Join-Path $temporaryRoot 'empty-root-install.log'
$emptyRootUninstallLogPath = Join-Path $temporaryRoot 'empty-root-uninstall.log'
$foreignRootInstallLogPath = Join-Path $temporaryRoot 'foreign-root-install.log'
$foreignRootUninstallLogPath = Join-Path $temporaryRoot 'foreign-root-uninstall.log'
$lockedRootInstallLogPath = Join-Path $temporaryRoot 'locked-root-install.log'
$autoCloseRootInstallLogPath = Join-Path $temporaryRoot 'auto-close-root-install.log'
$autoCloseRootUninstallLogPath = Join-Path $temporaryRoot 'auto-close-root-uninstall.log'
$reparseRootInstallLogPath = Join-Path $temporaryRoot 'reparse-root-install.log'
$emptyRootAppRoot = Join-Path $temporaryRoot 'pre-existing-empty-app'
$foreignRootAppRoot = Join-Path $temporaryRoot 'pre-existing-foreign-app'
$lockedRootAppRoot = Join-Path $temporaryRoot 'pre-existing-locked-app'
$autoCloseRootAppRoot = Join-Path $temporaryRoot 'pre-existing-running-app'
$reparseRootAppRoot = Join-Path $temporaryRoot 'reparse-app-root'
$reparseRootTarget = Join-Path $temporaryRoot 'reparse-app-target'
$commandProcessorPath = Join-Path $env:SystemRoot 'System32\cmd.exe'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

try {
    if (-not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw "Inno Setup compiler is missing: $Compiler"
    }
    if (-not (Test-Path -LiteralPath $commandProcessorPath -PathType Leaf)) {
        throw "Windows command processor is missing: $commandProcessorPath"
    }
    New-Item -ItemType Directory -Path $temporaryRoot, $outputRoot -Force | Out-Null
    [IO.File]::WriteAllText($payloadPath, 'payload', [Text.UTF8Encoding]::new($false))

    $legacyScript = @"
[Setup]
AppId=MacTypeInnoLegacyFailureContract
AppName=MacType Inno legacy failure contract
AppVersion=1.0
DefaultDirName=$legacyAppRoot
PrivilegesRequired=lowest
OutputDir=$outputRoot
OutputBaseFilename=legacy
Uninstallable=no
DisableProgramGroupPage=yes

[Files]
Source: "$payloadPath"; DestDir: "{app}"; AfterInstall: LegacyFailure

[Code]
procedure LegacyFailure;
var
  ResultCode: Integer;
begin
  if (not Exec(ExpandConstant('{cmd}'), '/d /c exit 23', '', SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    RaiseException('required helper failed');
end;
"@
    [IO.File]::WriteAllText($legacyScriptPath, $legacyScript, [Text.UTF8Encoding]::new($false))
    Invoke-ExpectedSuccess -File $Compiler -Arguments @('/Qp', $legacyScriptPath) `
        -Label 'Compile legacy Inno failure fixture'
    $legacyResult = Invoke-FixtureInstaller `
        -Executable (Join-Path $outputRoot 'legacy.exe') -LogPath $legacyLogPath
    if ($legacyResult.ExitCode -ne 0 -or
        -not (Test-Path -LiteralPath (Join-Path $legacyAppRoot 'payload.txt') -PathType Leaf)) {
        throw 'The fixture no longer reproduces the final AfterInstall exception success trap.'
    }
    $legacyLog = Read-BoundedFixtureLog -Path $legacyLogPath
    if (-not $legacyLog.Contains('Installation process succeeded.')) {
        throw 'Legacy failure fixture did not capture the misleading successful terminal state.'
    }

    $fatalScript = @"
[Setup]
AppId=MacTypeInnoFatalFailureContract
AppName=MacType Inno fatal failure contract
AppVersion=1.0
DefaultDirName=$fatalAppRoot
PrivilegesRequired=lowest
OutputDir=$outputRoot
OutputBaseFilename=fatal
Uninstallable=no
DisableProgramGroupPage=yes

[InstallDelete]
Type: filesandordirs; Name: "{app}\service-runtime"

[Files]
Source: "$payloadPath"; DestDir: "{app}"
Source: "$commandProcessorPath"; DestDir: "{app}\service-runtime"; DestName: "broker.exe"; Flags: ignoreversion

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ApplicationRoot: String;
  RuntimeRoot: String;
  BackupRoot: String;
  ExtractedBroker: String;
  StagedBroker: String;
  ApplicationRootExisted: Boolean;
  HadRuntime: Boolean;
  ExtractedCount: Integer;
  ResultCode: Integer;
  OperationError: String;
  RestoreError: String;
begin
  ApplicationRoot := ExpandConstant('{app}');
  RuntimeRoot := AddBackslash(ApplicationRoot) + 'service-runtime';
  BackupRoot := AddBackslash(ApplicationRoot) + 'service-runtime.setup-backup';
  ApplicationRootExisted := DirExists(ApplicationRoot);
  if DirExists(BackupRoot) then
  begin
    Result := 'required helper failed: unresolved backup collision';
    Exit;
  end;
  HadRuntime := DirExists(RuntimeRoot);
  if HadRuntime and not RenameFile(RuntimeRoot, BackupRoot) then
  begin
    Result := 'required helper failed: could not preserve baseline runtime';
    Exit;
  end;

  OperationError := '';
  RestoreError := '';
  try
    try
      ExtractedCount := ExtractTemporaryFiles('{app}\service-runtime\*');
      if ExtractedCount <> 1 then
        RaiseException('expected one fixed broker file, extracted ' + IntToStr(ExtractedCount));
      if not ForceDirectories(RuntimeRoot) then
        RaiseException('could not create exact app-side staging path');
      ExtractedBroker := AddBackslash(ExpandConstant('{tmp}')) +
        '{app}\service-runtime\broker.exe';
      StagedBroker := AddBackslash(RuntimeRoot) + 'broker.exe';
      if not FileCopy(ExtractedBroker, StagedBroker, False) then
        RaiseException('could not copy broker to exact app-side staging path');
      ResultCode := -1;
      if not Exec(StagedBroker, '/d /c exit 23', RuntimeRoot, SW_HIDE, ewWaitUntilTerminated, ResultCode) then
        OperationError := 'required helper failed: staged broker could not start'
      else if ResultCode <> 0 then
      begin
        Log('staged broker exit code ' + IntToStr(ResultCode));
        OperationError := 'required helper failed: staged broker exit code ' + IntToStr(ResultCode);
      end;
    except
      OperationError := 'required helper failed: ' + GetExceptionMessage;
    end;
  finally
    if DirExists(RuntimeRoot) and not DelTree(RuntimeRoot, True, True, True) then
      RestoreError := 'could not remove staging runtime';
    if (RestoreError = '') and HadRuntime and not RenameFile(BackupRoot, RuntimeRoot) then
      RestoreError := 'could not restore baseline runtime';
    if (RestoreError = '') and (not HadRuntime) and DirExists(BackupRoot) then
      RestoreError := 'unexpected backup remains';
    if (RestoreError = '') and (not ApplicationRootExisted) then
      RemoveDir(ApplicationRoot);
  end;

  if RestoreError <> '' then
    Result := OperationError + #13#10 + 'restore failed: ' + RestoreError
  else
    Result := OperationError;
end;
"@
    [IO.File]::WriteAllText($fatalScriptPath, $fatalScript, [Text.UTF8Encoding]::new($false))
    Invoke-ExpectedSuccess -File $Compiler -Arguments @('/Qp', $fatalScriptPath) `
        -Label 'Compile fatal Inno failure fixture'
    $fatalResult = Invoke-FixtureInstaller `
        -Executable (Join-Path $outputRoot 'fatal.exe') -LogPath $fatalLogPath
    if ($fatalResult.ExitCode -ne 7) {
        throw "Required-operation failure fixture returned $($fatalResult.ExitCode) instead of Preparing to Install failure 7."
    }
    if (Test-Path -LiteralPath $fatalAppRoot) {
        throw 'Required-operation failure fixture left installed files after rollback.'
    }
    $fatalLog = Read-BoundedFixtureLog -Path $fatalLogPath
    foreach ($token in @('required helper failed', 'staged broker exit code 23', 'PrepareToInstall failed:')) {
        if (-not $fatalLog.Contains($token)) {
            throw "Required-operation failure fixture log omits rollback evidence: $token`n$fatalLog"
        }
    }

    $baselineRuntimeRoot = Join-Path $fatalAppRoot 'service-runtime'
    $baselinePayloadRoot = Join-Path $baselineRuntimeRoot 'payload\files'
    New-Item -ItemType Directory -Path $baselinePayloadRoot -Force | Out-Null
    $baselinePayloadPath = Join-Path $fatalAppRoot 'payload.txt'
    [IO.File]::WriteAllText(
        $baselinePayloadPath,
        'baseline-payload',
        [Text.UTF8Encoding]::new($false)
    )
    [IO.File]::WriteAllText(
        (Join-Path $baselineRuntimeRoot 'baseline-broker.exe'),
        'baseline-broker',
        [Text.UTF8Encoding]::new($false)
    )
    [IO.File]::WriteAllText(
        (Join-Path $baselinePayloadRoot 'obsolete-runtime-file.bin'),
        'obsolete-runtime-payload',
        [Text.UTF8Encoding]::new($false)
    )
    $baselineSnapshot = Get-FixtureTreeSnapshot -Path $fatalAppRoot
    $fatalUpgradeResult = Invoke-FixtureInstaller `
        -Executable (Join-Path $outputRoot 'fatal.exe') -LogPath $fatalUpgradeLogPath
    if ($fatalUpgradeResult.ExitCode -ne 7) {
        throw "Required-operation upgrade failure fixture returned $($fatalUpgradeResult.ExitCode) instead of 7."
    }
    if (-not (Test-Path -LiteralPath $baselinePayloadPath -PathType Leaf) -or
        [IO.File]::ReadAllText($baselinePayloadPath) -cne 'baseline-payload' -or
        (Get-FixtureTreeSnapshot -Path $fatalAppRoot) -cne $baselineSnapshot -or
        (Test-Path -LiteralPath (Join-Path $fatalAppRoot 'service-runtime.setup-backup'))) {
        throw 'Required-operation upgrade failure did not restore the exact baseline payload.'
    }
    $fatalUpgradeLog = Read-BoundedFixtureLog -Path $fatalUpgradeLogPath
    foreach ($token in @('required helper failed', 'staged broker exit code 23', 'PrepareToInstall failed:')) {
        if (-not $fatalUpgradeLog.Contains($token)) {
            throw "Required-operation upgrade fixture log omits rollback evidence: $token`n$fatalUpgradeLog"
        }
    }

    $productInstallerDefinition = Get-Content -LiteralPath `
        (Join-Path $sourceRoot 'installer\mactype-control-center.iss') -Raw
    $rootCleanupEntry = [regex]::Match(
        $productInstallerDefinition,
        '(?m)^Type:\s*dirifempty;\s*Name:\s*"\{app\}"\s*$'
    )
    if (-not $rootCleanupEntry.Success) {
        throw 'Product installer does not define exact empty application-root cleanup.'
    }
    $emptyRootCleanupScript = @"
[Setup]
AppId=MacTypeInnoEmptyRootCleanupContract-$PID
AppName=MacType Inno empty root cleanup contract
AppVersion=1.0
DefaultDirName=$emptyRootAppRoot
PrivilegesRequired=lowest
UsePreviousAppDir=no
CreateUninstallRegKey=no
OutputDir=$outputRoot
OutputBaseFilename=empty-root-cleanup
DisableProgramGroupPage=yes
Compression=none

[Files]
Source: "$payloadPath"; DestDir: "{app}"

[InstallDelete]
Type: files; Name: "{app}\.setup-root-cleanup-trigger"; BeforeInstall: PurgeApplicationRootBeforeInstall

[UninstallDelete]
$($rootCleanupEntry.Value)

[Code]
#include "$sourceRoot\installer\application-root-cleanup.iss"
"@
    [IO.File]::WriteAllText(
        $emptyRootScriptPath,
        $emptyRootCleanupScript,
        [Text.UTF8Encoding]::new($false)
    )
    Invoke-ExpectedSuccess -File $Compiler -Arguments @('/Qp', $emptyRootScriptPath) `
        -Label 'Compile exact empty application-root cleanup fixture'
    $emptyRootInstaller = Join-Path $outputRoot 'empty-root-cleanup.exe'
    $silentArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-')

    New-Item -ItemType Directory -Path $emptyRootAppRoot -Force | Out-Null
    Invoke-ExpectedSuccess -File $emptyRootInstaller `
        -Arguments ($silentArguments + "/DIR=$emptyRootAppRoot") `
        -Label 'Install into pre-existing empty application root' `
        -DiagnosticLogPath $emptyRootInstallLogPath
    $emptyRootUninstaller = Get-ChildItem -LiteralPath $emptyRootAppRoot `
        -File -Filter 'unins*.exe' | Select-Object -First 1
    if (-not $emptyRootUninstaller) {
        throw 'Empty-root cleanup fixture did not create an uninstaller.'
    }
    Invoke-ExpectedSuccess -File $emptyRootUninstaller.FullName `
        -Arguments $silentArguments `
        -Label 'Uninstall from pre-existing empty application root' `
        -DiagnosticLogPath $emptyRootUninstallLogPath
    Wait-PathAbsent -Path $emptyRootAppRoot -TimeoutMilliseconds 15000
    if (Test-Path -LiteralPath $emptyRootAppRoot) {
        throw 'Empty-root cleanup fixture left its pre-existing application root behind.'
    }

    New-Item -ItemType Directory -Path $foreignRootAppRoot -Force | Out-Null
    $foreignMarkerPath = Join-Path $foreignRootAppRoot 'foreign-marker.txt'
    [IO.File]::WriteAllText(
        $foreignMarkerPath,
        'foreign-user-file',
        [Text.UTF8Encoding]::new($false)
    )
    Invoke-ExpectedSuccess -File $emptyRootInstaller `
        -Arguments ($silentArguments + "/DIR=$foreignRootAppRoot") `
        -Label 'Install beside foreign application-root file' `
        -DiagnosticLogPath $foreignRootInstallLogPath
    if (Test-Path -LiteralPath $foreignMarkerPath) {
        throw 'Application-root cleanup fixture left a foreign file beside the installed payload.'
    }
    $foreignRootUninstaller = Get-ChildItem -LiteralPath $foreignRootAppRoot `
        -File -Filter 'unins*.exe' | Select-Object -First 1
    if (-not $foreignRootUninstaller) {
        throw 'Foreign-root cleanup fixture did not create an uninstaller.'
    }
    Invoke-ExpectedSuccess -File $foreignRootUninstaller.FullName `
        -Arguments $silentArguments `
        -Label 'Uninstall beside foreign application-root file' `
        -DiagnosticLogPath $foreignRootUninstallLogPath
    Wait-PathAbsent -Path (Join-Path $foreignRootAppRoot 'payload.txt') `
        -TimeoutMilliseconds 15000
    Wait-PathAbsent -Path $foreignRootUninstaller.FullName -TimeoutMilliseconds 15000
    if (Test-Path -LiteralPath $foreignRootAppRoot) {
        $remainingTree = Get-BoundedTreeInventory -Path $foreignRootAppRoot
        throw "Application-root cleanup fixture left the foreign application root behind.`nRemaining tree:`n$remainingTree"
    }

    New-Item -ItemType Directory -Path $reparseRootTarget -Force | Out-Null
    $reparseTargetMarker = Join-Path $reparseRootTarget 'must-not-be-touched.txt'
    [IO.File]::WriteAllText(
        $reparseTargetMarker,
        'reparse-target-baseline',
        [Text.UTF8Encoding]::new($false)
    )
    $reparseTargetSnapshot = Get-FixtureTreeSnapshot -Path $reparseRootTarget
    New-Item -ItemType Junction -Path $reparseRootAppRoot -Target $reparseRootTarget | Out-Null
    Invoke-ExpectedFailure -File $emptyRootInstaller `
        -Arguments ($silentArguments + "/DIR=$reparseRootAppRoot") `
        -Label 'Reject a reparse-point application root before cleanup' `
        -DiagnosticLogPath $reparseRootInstallLogPath
    if ((Get-FixtureTreeSnapshot -Path $reparseRootTarget) -cne $reparseTargetSnapshot) {
        throw 'Rejected reparse-point application root changed its target tree.'
    }
    if (-not (Test-Path -LiteralPath $reparseRootAppRoot -PathType Container)) {
        throw 'Rejected reparse-point application root removed the junction itself.'
    }

    New-Item -ItemType Directory -Path $lockedRootAppRoot -Force | Out-Null
    $lockedMarkerPath = Join-Path $lockedRootAppRoot 'locked-foreign-marker.txt'
    $siblingMarkerPath = Join-Path $lockedRootAppRoot 'sibling-foreign-marker.txt'
    [IO.File]::WriteAllText(
        $lockedMarkerPath,
        'locked-foreign-user-file',
        [Text.UTF8Encoding]::new($false)
    )
    [IO.File]::WriteAllText(
        $siblingMarkerPath,
        'sibling-foreign-user-file',
        [Text.UTF8Encoding]::new($false)
    )
    $lockedRootSnapshot = Get-FixtureTreeSnapshot -Path $lockedRootAppRoot
    $lockedMarkerStream = [IO.File]::Open(
        $lockedMarkerPath,
        [IO.FileMode]::Open,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None
    )
    try {
        Invoke-ExpectedFailure -File $emptyRootInstaller `
            -Arguments ($silentArguments + @('/NOCLOSEAPPLICATIONS', "/DIR=$lockedRootAppRoot")) `
            -Label 'Reject cleanup when a foreign file remains in use' `
            -DiagnosticLogPath $lockedRootInstallLogPath
    }
    finally {
        $lockedMarkerStream.Dispose()
    }
    $lockedRootAfterFailure = Get-FixtureTreeSnapshot -Path $lockedRootAppRoot
    if ($lockedRootAfterFailure -cne $lockedRootSnapshot) {
        throw "Rejected application-root cleanup did not restore the exact prior tree.`nBefore:`n$lockedRootSnapshot`nAfter:`n$lockedRootAfterFailure"
    }
    if (Test-Path -LiteralPath (Join-Path $lockedRootAppRoot '.setup-root-rollback')) {
        throw 'Rejected application-root cleanup left its rollback directory behind.'
    }

    New-Item -ItemType Directory -Path $autoCloseRootAppRoot -Force | Out-Null
    $runningForeignExecutable = Join-Path $autoCloseRootAppRoot 'running-foreign-process.exe'
    Copy-Item -LiteralPath $commandProcessorPath -Destination $runningForeignExecutable
    $runningForeignStartInfo = [Diagnostics.ProcessStartInfo]::new()
    $runningForeignStartInfo.FileName = $runningForeignExecutable
    $runningForeignStartInfo.Arguments = '/d /c ping -n 120 127.0.0.1 >nul'
    $runningForeignStartInfo.UseShellExecute = $false
    $runningForeignStartInfo.CreateNoWindow = $true
    $runningForeignProcess = [Diagnostics.Process]::Start($runningForeignStartInfo)
    try {
        if ($runningForeignProcess.WaitForExit(250)) {
            throw 'Foreign process fixture exited before installer close-applications testing.'
        }
        Invoke-ExpectedSuccess -File $emptyRootInstaller `
            -Arguments ($silentArguments + @('/FORCECLOSEAPPLICATIONS', "/DIR=$autoCloseRootAppRoot")) `
            -Label 'Close a running foreign process before application-root cleanup' `
            -DiagnosticLogPath $autoCloseRootInstallLogPath
        if (-not $runningForeignProcess.WaitForExit(5000)) {
            throw 'Installer cleanup did not close the running foreign process.'
        }
    }
    finally {
        if (-not $runningForeignProcess.HasExited) {
            $runningForeignProcess.Kill($true)
            [void] $runningForeignProcess.WaitForExit(5000)
        }
        $runningForeignProcess.Dispose()
    }
    if (Test-Path -LiteralPath $runningForeignExecutable) {
        throw 'Installer cleanup left the previously running foreign executable behind.'
    }
    $autoCloseRootUninstaller = Get-ChildItem -LiteralPath $autoCloseRootAppRoot `
        -File -Filter 'unins*.exe' | Select-Object -First 1
    if (-not $autoCloseRootUninstaller) {
        throw 'Auto-close cleanup fixture did not create an uninstaller.'
    }
    Invoke-ExpectedSuccess -File $autoCloseRootUninstaller.FullName `
        -Arguments $silentArguments `
        -Label 'Uninstall after closing a foreign process' `
        -DiagnosticLogPath $autoCloseRootUninstallLogPath
    Wait-PathAbsent -Path $autoCloseRootAppRoot -TimeoutMilliseconds 15000
    if (Test-Path -LiteralPath $autoCloseRootAppRoot) {
        throw 'Auto-close cleanup fixture left its application root behind.'
    }

    $productFixtureRoot = Join-Path $temporaryRoot 'product-fixture'
    $productCoreRoot = Join-Path $productFixtureRoot 'core'
    $productServiceRoot = Join-Path $productFixtureRoot 'service-runtime'
    $productPayloadRoot = Join-Path $productServiceRoot 'payload\files'
    $productOutputRoot = Join-Path $temporaryRoot 'product-output'
    New-Item -ItemType Directory -Path $productCoreRoot, $productPayloadRoot, $productOutputRoot -Force | Out-Null
    $productApp = Join-Path $productFixtureRoot 'MacType Control Center.exe'
    $productPreview = Join-Path $productFixtureRoot 'mactype-preview32.exe'
    foreach ($path in @(
        $productApp,
        $productPreview,
        (Join-Path $productCoreRoot 'MacType.dll'),
        (Join-Path $productCoreRoot 'MacType64.dll'),
        (Join-Path $productCoreRoot 'MacType.Core.dll'),
        (Join-Path $productCoreRoot 'MacType64.Core.dll'),
        (Join-Path $productCoreRoot 'MacLoader.exe'),
        (Join-Path $productCoreRoot 'MacLoader64.exe'),
        (Join-Path $productServiceRoot 'mactype-service-setup.exe'),
        (Join-Path $productServiceRoot 'payload\manifest.json'),
        (Join-Path $productPayloadRoot 'mactype-service.exe'),
        (Join-Path $productPayloadRoot 'mactype-injector32.exe'),
        (Join-Path $productPayloadRoot 'mactype-injector64.exe'),
        (Join-Path $productPayloadRoot 'MacType.dll'),
        (Join-Path $productPayloadRoot 'MacType64.dll')
    )) {
        [IO.File]::WriteAllText($path, 'compile-fixture', [Text.UTF8Encoding]::new($false))
    }
    Invoke-ExpectedSuccess -File $Compiler -Arguments @(
        '/Qp',
        '/DAppVersion=0.0.0',
        "/DSourceRoot=$sourceRoot",
        "/DOutputRoot=$productOutputRoot",
        "/DAppExe=$productApp",
        "/DPreviewExe=$productPreview",
        "/DCoreRoot=$productCoreRoot",
        "/DServiceRuntimeRoot=$productServiceRoot",
        (Join-Path $sourceRoot 'installer\mactype-control-center.iss')
    ) -Label 'Compile product Inno staging contract'
    if (-not (Test-Path -LiteralPath (Join-Path $productOutputRoot 'MacType-Control-Center-Installer.exe') -PathType Leaf)) {
        throw 'Product Inno staging contract did not produce an installer.'
    }

    Write-Host 'Inno required-operation failure propagation and rollback contract passed.'
}
finally {
    $resolvedTemporaryRoot = [IO.Path]::GetFullPath($temporaryRoot)
    $resolvedSystemTemporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedTemporaryRoot.StartsWith(
        $resolvedSystemTemporaryRoot,
        [StringComparison]::OrdinalIgnoreCase
    ) -and (Test-Path -LiteralPath $resolvedTemporaryRoot)) {
        Remove-Item -LiteralPath $resolvedTemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
