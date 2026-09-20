[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\WorkflowModel.psm1') -Force

function Test-InstallerRollbackPolicy {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)] $Workflow,
        [Parameter(Mandatory)] [string] $Repo
    )

    $failures = [System.Collections.Generic.List[string]]::new()
    $root = $Repo
    $installerTest = Get-Content -LiteralPath (Join-Path $root 'scripts\ci\Test-InstallerWindows.ps1') -Raw
    $installerDefinitionPath = Join-Path $root 'installer\mactype-control-center.iss'
    $rootCleanupPath = Join-Path $root 'installer\application-root-cleanup.iss'
    $innoFailureContractPath = Join-Path $root 'scripts\ci\Test-InnoInstallerFailureContract.ps1'
    $installerHelperPath = Join-Path $root 'scripts\ci\lib\InstallerWindowsAssertions.ps1'
    $snapshotContractPath = Join-Path $root 'scripts\ci\Test-InstallerSnapshotContract.ps1'
    $fixturePath = Join-Path $root '.github\scripts\Build-FailingServiceRuntimeFixture.ps1'

    $windowsBuild = Get-WorkflowJob -Workflow $Workflow -Id 'windows-build'
    if (-not $windowsBuild) {
        $failures.Add("Hosted installer CI omits the guarded job 'windows-build'.")
    } else {
        foreach ($contract in @(
            @{ Name = 'failing runtime version'; Tokens = @('$failingRuntimeVersion') },
            @{ Name = 'failing runtime payload'; Tokens = @('Build-FailingServiceRuntimeFixture.ps1', 'artifacts/service-runtime-failing-upgrade') },
            @{ Name = 'failing installer output'; Tokens = @('artifacts/installer-failing-upgrade') },
            @{ Name = 'failed-upgrade verification'; Tokens = @('-FailingUpgradeInstaller') }
        )) {
            $matchingStep = @($windowsBuild.Steps | Where-Object {
                $run = $_.Run
                @($contract.Tokens | Where-Object { $run.Contains($_) }).Count -eq $contract.Tokens.Count
            }) | Select-Object -First 1
            if (-not $matchingStep) {
                $failures.Add("Hosted installer CI omits the rollback fixture contract: $($contract.Name)")
            }
        }
    }

    if (-not (Test-Path -LiteralPath $installerHelperPath -PathType Leaf)) {
        $failures.Add('Installer E2E bounded-process helper is missing.')
    }

    $installerDefinition = Get-Content -LiteralPath $installerDefinitionPath -Raw
    $rootCleanup = Get-Content -LiteralPath $rootCleanupPath -Raw
    foreach ($token in @(
        'ExecAndLogOutput',
        'ExtractTemporaryFiles',
        'ExtractedCount <> 7',
        'BootstrapBeforeFileInstall',
        'PrepareToInstall',
        'service-runtime.setup-backup',
        'RestoreApplicationBroker',
        'RestoreLegacyTrayStartupAfterBootstrapFailure',
        'BrokerFatalLegacyTrayBlocked',
        'ExecAsOriginalUser',
        '--restore-current-user-legacy-tray-autostart',
        '--control-center-service-broker restore-legacy-tray-autostart',
        '"outcome":"applied"',
        '"reason":"legacy-service"',
        '"reason":"appinit"',
        '"reason":"foreign-open-service"'
    )) {
        if (-not $installerDefinition.Contains($token)) {
            $failures.Add("Installer fatal-bootstrap classification/rollback contract is missing: $token")
        }
    }
    $bootstrapCapture = [regex]::Match(
        $installerDefinition,
        '(?ms)^procedure\s+CaptureBrokerOutput\b.*?^end;'
    )
    if (-not $bootstrapCapture.Success -or
        $bootstrapCapture.Value -notmatch '(?s)"reason":"legacy-tray-mode".*BrokerFatalLegacyTrayBlocked\s*:=\s*True') {
        $failures.Add('Installer does not classify a legacy tray-mode bootstrap blocker as fatal.')
    }
    $stagedBootstrap = [regex]::Match(
        $installerDefinition,
        '(?ms)^function\s+RunStagedBootstrap\b.*?^end;'
    )
    if (-not $stagedBootstrap.Success -or
        $stagedBootstrap.Value -notmatch 'if\s+BrokerFatalLegacyTrayBlocked\s+then' -or
        $stagedBootstrap.Value -notmatch 'MacTray tray mode') {
        $failures.Add('Installer does not propagate the legacy tray-mode blocker as an installation failure.')
    }
    $fixedBrokerCall = [regex]::Match(
        $installerDefinition,
        '(?ms)^procedure\s+RunFixedBrokerOrFail\b.*?^end;'
    )
    if (-not $fixedBrokerCall.Success) {
        $failures.Add('Installer fixed-broker failure propagation procedure is missing.')
    }
    foreach ($token in @(
        'ExecAndLogOutput',
        '@CaptureBrokerOutput',
        'BrokerFailure'
    )) {
        if (-not $fixedBrokerCall.Value.Contains($token)) {
            $failures.Add("Owned uninstall broker failures omit bounded diagnostics: $token")
        }
    }
    if ($installerDefinition -match 'AfterInstall:\s*BootstrapMachineService' -or
        $installerDefinition -match '(?s)procedure\s+CurStepChanged\b.*?ssPostInstall.*?RunFixedBrokerOrFail') {
        $failures.Add('Installer must complete required bootstrap before the Files phase begins.')
    }
    $installDeleteSection = [regex]::Match(
        $installerDefinition,
        '(?ms)^\[InstallDelete\]\s*(?<body>.*?)(?=^\[[^]]+\])'
    )
    if (-not $installDeleteSection.Success -or
        $installDeleteSection.Groups['body'].Value -notmatch '(?m)^Type:\s*filesandordirs;\s*Name:\s*"\{app\}\\service-runtime"\s*$') {
        $failures.Add('Installer must remove the prior app-side runtime only after PrepareToInstall succeeds.')
    }
    if ($installDeleteSection.Groups['body'].Value -notmatch
        '(?m)^Type:\s*files;\s*Name:\s*"\{app\}\\\.setup-root-cleanup-trigger";\s*BeforeInstall:\s*BootstrapAndPurgeApplicationRootBeforeInstall\s*$') {
        $failures.Add('Installer must run protected bootstrap and root cleanup only after CloseApplications completes.')
    }
    $prepareToInstall = [regex]::Match(
        $installerDefinition,
        '(?ms)^function\s+PrepareToInstall\b.*?^end;'
    )
    if (-not $prepareToInstall.Success -or
        -not $prepareToInstall.Value.Contains('ValidateApplicationRootCleanup') -or
        $prepareToInstall.Value.Contains('BootstrapBeforeFileInstall')) {
        $failures.Add('PrepareToInstall must validate only; protected bootstrap runs after CloseApplications.')
    }
    $bootstrapCleanupTransaction = [regex]::Match(
        $installerDefinition,
        '(?ms)^procedure\s+BootstrapAndPurgeApplicationRootBeforeInstall\b.*?^end;'
    )
    if (-not $bootstrapCleanupTransaction.Success) {
        $failures.Add('Installer post-CloseApplications bootstrap/cleanup transaction is missing.')
    }
    $stageIndex = $bootstrapCleanupTransaction.Value.IndexOf('StageApplicationRootCleanup')
    $bootstrapIndex = $bootstrapCleanupTransaction.Value.IndexOf('BootstrapBeforeFileInstall')
    $commitIndex = $bootstrapCleanupTransaction.Value.IndexOf('CommitStagedRootCleanup')
    if ($stageIndex -lt 0 -or $bootstrapIndex -le $stageIndex -or $commitIndex -le $bootstrapIndex) {
        $failures.Add('Installer must stage rollback, bootstrap, then commit cleanup in that order.')
    }
    foreach ($token in @(
        'RegisterExtraCloseApplicationsResource',
        'RootCleanupProtectedDirectoryName',
        'FILE_ATTRIBUTE_REPARSE_POINT',
        'FileFlagOpenReparsePoint',
        'RestoreStagedRootCleanup',
        'SuppressibleMsgBox',
        'ExitRootCleanupSetup(4)',
        'Application-root cleanup refuses a reparse-point application root.'
    )) {
        if (-not $rootCleanup.Contains($token)) {
            $failures.Add("Application-root cleanup omits required close/rollback/path safety: $token")
        }
    }
    $uninstallDeleteSection = [regex]::Match(
        $installerDefinition,
        '(?ms)^\[UninstallDelete\]\s*(?<body>.*?)(?=^\[[^]]+\]|\z)'
    )
    if (-not $uninstallDeleteSection.Success -or
        $uninstallDeleteSection.Groups['body'].Value -notmatch '(?m)^Type:\s*dirifempty;\s*Name:\s*"\{app\}"\s*$') {
        $failures.Add('Installer must remove the exact application root when the protected bootstrap made it pre-exist and uninstall leaves it empty.')
    }
    if ($uninstallDeleteSection.Groups['body'].Value -match '(?im)^Type:\s*filesandordirs;\s*Name:\s*"\{app\}(?:[\\/]|"|\*)') {
        $failures.Add('Installer must never recursively delete the application root or its descendants during final cleanup.')
    }
    $innoContractStep = if ($windowsBuild) {
        @(Get-WorkflowStep -Job $windowsBuild -RunLike '*scripts/ci/Test-InnoInstallerFailureContract.ps1*') |
            Select-Object -First 1
    }
    if (-not (Test-Path -LiteralPath $innoFailureContractPath -PathType Leaf) -or
        -not $innoContractStep) {
        $failures.Add('Hosted Windows CI does not execute the real Inno required-failure rollback contract.')
    }

    $innoFailureContract = Get-Content -LiteralPath $innoFailureContractPath -Raw
    foreach ($token in @(
        'RaiseException',
        'ExtractTemporaryFiles',
        'service-runtime.setup-backup',
        'staged broker exit code 23',
        'Compile product Inno staging contract',
        'Installation process succeeded.',
        'PrepareToInstall failed:',
        'ExitCode -ne 7',
        'baseline-payload',
        'MacTypeInnoEmptyRootCleanupContract',
        'foreign-marker.txt',
        '/NOCLOSEAPPLICATIONS',
        '/FORCECLOSEAPPLICATIONS',
        'New-Item -ItemType Junction',
        'Rejected reparse-point application root changed its target tree.',
        'Rejected application-root cleanup did not restore the exact prior tree.',
        'Installer cleanup did not close the running foreign process.',
        'Empty-root cleanup fixture left its pre-existing application root behind.',
        'Application-root cleanup fixture left the foreign application root behind.'
    )) {
        if (-not $innoFailureContract.Contains($token)) {
            $failures.Add("Real Inno regression fixture is missing required RED/GREEN evidence: $token")
        }
    }
    $installerHelper = Get-Content -LiteralPath $installerHelperPath -Raw
    foreach ($token in @(
        'BoundedProcessRunner',
        'InstallerProcessTimeoutMilliseconds',
        'StandardOutput',
        'StandardError',
        'DiagnosticLogPath',
        'Read-InstallerDiagnosticLog'
    )) {
        if (-not $installerHelper.Contains($token)) {
            $failures.Add("Installer E2E process execution is not bounded with diagnostic capture: $token")
        }
    }
    if ($installerHelper -match '(?is)Start-Process\b.*?-Wait\b') {
        $failures.Add('Installer E2E must not wait indefinitely with Start-Process -Wait.')
    }

    if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
        $failures.Add('The deliberate failing-upgrade payload builder is missing.')
    }
    $fixture = Get-Content -LiteralPath $fixturePath -Raw
    foreach ($token in @('test-only', 'mactype-service-setup.exe', 'mactype-service.exe', 'Get-FileHash')) {
        if (-not $fixture.Contains($token)) {
            $failures.Add("Failing-upgrade fixture is not a valid manifest-preserving start failure: $token")
        }
    }

    foreach ($token in @(
        '[string] $FailingUpgradeInstaller',
        'Deliberately failing protected upgrade',
        'Assert-BaselineRestoredAfterFailedUpgrade',
        '$baselineApplicationSnapshot',
        '$baselineServiceSnapshot',
        'obsolete-from-prior-version.bin',
        '-ExcludedRoot $serviceRoot',
        'Invoke-InstallerExpectedFailure',
        'Installer diagnostic logs:'
    )) {
        if (-not $installerTest.Contains($token)) {
            $failures.Add("Installer E2E does not prove automatic rollback at the installer boundary: $token")
        }
    }

    if (-not (Test-Path -LiteralPath $snapshotContractPath -PathType Leaf)) {
        $failures.Add('Installer immutable app-side snapshot contract test is missing.')
    }
    foreach ($token in @(
        '-ExcludedRoot $ServiceRoot',
        'Get-TreeSnapshotDifference',
        'Get-BoundedTreeInventory',
        'Wait-PathAbsent'
    )) {
        if (-not $installerHelper.Contains($token)) {
            $failures.Add("Installer app-side rollback diagnostics omit: $token")
        }
    }

    foreach ($token in @(
        "-Label 'Owned uninstall'",
        'PendingFileRenameOperations',
        'Owned uninstall left non-runtime application files behind',
        'bounded reboot cleanup registrations'
    )) {
        if (-not $installerTest.Contains($token)) {
            $failures.Add("Installer E2E does not prove bounded immediate-or-reboot cleanup after the detached Inno uninstall phase: $token")
        }
    }
    foreach ($token in @('DirectorySeparatorChar', 'AltDirectorySeparatorChar', 'GetRelativePath')) {
        if (-not $installerHelper.Contains($token)) {
            $failures.Add("Installer snapshot exclusion is not host-platform path aware: $token")
        }
    }
    if ($installerHelper -match "TrimEnd\('\\\\'\)" -or
        $installerHelper -match "StartsWith\('\.\.\\\\'") {
        $failures.Add('Installer snapshot exclusion hard-codes a Windows path boundary in cross-platform policy code.')
    }

    foreach ($job in $Workflow.Jobs.Values) {
        foreach ($step in @(Get-WorkflowStep -Job $job -Uses 'actions/upload-artifact@v7')) {
            $uploadText = (($step.With.Keys | ForEach-Object { [string] $step.With[$_] }) -join "`n")
            if ($uploadText.Contains('failing-upgrade')) {
                $failures.Add('The deliberate failing-upgrade fixture must never be uploaded as a release artifact.')
            }
        }
    }

    return $failures.ToArray()
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workflow = Read-GitHubWorkflow -Path (Join-Path $root '.github\workflows\build.yml')
$failures = @(Test-InstallerRollbackPolicy -Workflow $workflow -Repo $root)
if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "ERROR: $_" -ForegroundColor Red }
    throw "Installer failed-upgrade rollback policy failed with $($failures.Count) violation(s)."
}

& (Join-Path $root 'scripts\ci\Test-InstallerSnapshotContract.ps1')
Write-Host 'Installer failed-upgrade rollback policy passed.'
