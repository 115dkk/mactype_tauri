[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$required = @(
    'distribution\MacType.ini',
    'distribution\ini\Default.ini',
    'distribution\languages\en.json',
    'distribution\languages\ko.json',
    'distribution\THIRD_PARTY_NOTICES.md',
    'distribution\INTEGRATION_DEVELOPER_README.md',
    'LICENSE'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
        throw "Distribution source file is missing: $relative"
    }
}

$binary = Get-ChildItem -LiteralPath (Join-Path $root 'distribution') -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') }
if ($binary) { throw "Prebuilt binary is forbidden in distribution/: $($binary.FullName)" }

$english = Get-Content -LiteralPath (Join-Path $root 'distribution\languages\en.json') -Raw | ConvertFrom-Json -AsHashtable
$korean = Get-Content -LiteralPath (Join-Path $root 'distribution\languages\ko.json') -Raw | ConvertFrom-Json -AsHashtable
if (Compare-Object ($english.Keys | Sort-Object) ($korean.Keys | Sort-Object)) {
    throw 'English and Korean distribution translation keys differ.'
}

$profile = Get-Content -LiteralPath (Join-Path $root 'distribution\ini\Default.ini') -Raw
foreach ($section in @('[General]', '[DirectWrite]', '[Individual]', '[Exclude]', '[ExcludeModule]')) {
    if (-not $profile.Contains($section)) { throw "Default profile is missing section $section" }
}

$buildScript = Get-Content -LiteralPath (Join-Path $root '.github\scripts\Build-OpenCore.ps1') -Raw
foreach ($commit in @(
    'ef771574d04721baf45a1b66bfb4692193603088',
    'a457397ffa9d20e8df43e2c143c60da78c16c059',
    'd644ce94e8c7f7f5a31591577c78134ea3ac1fae',
    '667359c7967249dd9d28d8f8cef65b60e7e2d963'
)) {
    if (-not $buildScript.Contains($commit)) { throw "Core dependency is not pinned: $commit" }
}

$installer = Get-Content -LiteralPath (Join-Path $root 'installer\mactype-control-center.iss') -Raw
foreach ($legacy in @('MacTray.exe', 'MacTuner.exe', 'MacWiz.exe', 'VisTuner.exe', 'EasyHK32.dll', 'EasyHK64.dll')) {
    if ($installer.Contains($legacy)) { throw "Installer references forbidden legacy binary: $legacy" }
}
if (-not $installer.Contains('MacType64.dll') -or -not $installer.Contains('MacLoader64.exe')) {
    throw 'Installer does not contain the independent x86/x64 core set.'
}

foreach ($machinePayloadToken in @(
    '{#ServiceRuntimeRoot}',
    '{app}\service-runtime',
    'mactype-service-setup.exe',
    'payload\manifest.json',
    'payload\files\mactype-service.exe',
    'payload\files\mactype-injector32.exe',
    'payload\files\mactype-injector64.exe',
    'payload\files\MacType.dll',
    'payload\files\MacType64.dll'
)) {
    if (-not $installer.Contains($machinePayloadToken)) {
        throw "Installer does not stage the fixed open-service payload token: $machinePayloadToken"
    }
}
foreach ($protectedInstallerToken in @(
    'DefaultDirName={autopf}\MacType Control Center',
    'DisableDirPage=yes',
    'OutputBaseFilename=MacType-Control-Center-Installer',
    'Root: HKLM64; Subkey: "SOFTWARE\MacType\ControlCenter"; ValueType: string; ValueName: "InstallLocation"; ValueData: "{app}"',
    'PrivilegesRequired=admin',
    'UsePreviousAppDir=no',
    'bootstrap-install',
    'uninstall-owned',
    'UninstallNeedRestart',
    'DeferredRuntimeCleanup',
    'PrepareToInstall',
    'ewWaitUntilTerminated',
    'runasoriginaluser',
    '{cm:LaunchProgram,MacType Control Center}',
    '{autodesktop}\MacType Control Center'
)) {
    if (-not $installer.Contains($protectedInstallerToken)) {
        throw "Installer does not enforce protected machine bootstrap token: $protectedInstallerToken"
    }
}

$buildWorkflow = Get-Content -LiteralPath (Join-Path $root '.github\workflows\build.yml') -Raw
foreach ($installerArtifactToken in @(
    'name: mactype-control-center-installer-windows',
    'artifacts/installer/MacType-Control-Center-Installer.exe',
    'artifacts/installer/SHA256SUMS.txt',
    'name: mactype-control-center-integration-developer-bundle-windows',
    '.github/scripts/Build-IntegrationDeveloperBundle.ps1',
    'MacType-Control-Center-Integration-Developer-Bundle.zip',
    'INTEGRATION_SHA256SUMS.txt'
)) {
    if (-not $buildWorkflow.Contains($installerArtifactToken)) {
        throw "Windows workflow is missing a separated distribution contract token: $installerArtifactToken"
    }
}
$bundleBuilder = Get-Content -LiteralPath (
    Join-Path $root '.github\scripts\Build-IntegrationDeveloperBundle.ps1'
) -Raw
foreach ($bundleToken in @(
    'installation-tree',
    'MacType Control Center.exe',
    'mactype-preview32.exe',
    'service-runtime',
    'manifest.json',
    'INTEGRATION_DEVELOPER_README.md'
)) {
    if (-not $bundleBuilder.Contains($bundleToken)) {
        throw "Integration/Developer bundle does not reproduce the installed tree: $bundleToken"
    }
}
foreach ($installerUpgradePromptToken in @(
    'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{AF6B9697-3DF2-46C4-B203-79194967AE7A}_is1',
    'RegQueryStringValue(HKLM64',
    "'InstallLocation'",
    "'DisplayVersion'",
    "'UninstallString'",
    'CreateOutputMsgPage',
    'NextButton.Caption',
    'korean.VerifiedUpdateTitle=MacType Control Center를 업데이트하시겠습니까?',
    'korean.VerifiedUpdateMessage=기존 프로필과 설정을 유지한 채 새 버전으로 업데이트합니다.',
    'korean.VerifiedUpdateButton=업데이트',
    'korean.VerifiedReinstallTitle=MacType Control Center를 다시 설치하시겠습니까?',
    'korean.VerifiedReinstallButton=다시 설치',
    'korean.ForeignContentsTitle=설치 폴더의 기존 내용을 정리하시겠습니까?',
    'korean.ForeignContentsButton=계속'
)) {
    if (-not $installer.Contains($installerUpgradePromptToken)) {
        throw "Installer does not distinguish verified update/reinstall from foreign contents: $installerUpgradePromptToken"
    }
}
$initializeWizard = [regex]::Match(
    $installer,
    '(?ms)^procedure\s+InitializeWizard\b.*?^end;'
)
if (-not $initializeWizard.Success -or
    $initializeWizard.Value.Contains('ClassifyExistingInstall') -or
    $initializeWizard.Value.Contains("ExpandConstant('{app}')")) {
    throw 'InitializeWizard must not inspect {app} before Inno initializes the application directory.'
}
$skipExistingInstallPage = [regex]::Match(
    $installer,
    '(?ms)^function\s+SkipExistingInstallPage\b.*?^end;'
)
if (-not $skipExistingInstallPage.Success -or
    -not $skipExistingInstallPage.Value.Contains('EnsureExistingInstallClassified')) {
    throw 'Existing-install classification must run only when the post-Ready prompt is evaluated.'
}
foreach ($forbiddenInstallerToken in @(
    'PrivilegesRequired=lowest',
    '{localappdata}',
    'Root: HKCU',
    "ShellExec('runas'",
    'Description: "MacType Control Center 실행"'
)) {
    if ($installer.Contains($forbiddenInstallerToken)) {
        throw "Admin installer retains a user-writable/elevated broker hazard: $forbiddenInstallerToken"
    }
}
$uninstallDeleteSection = [regex]::Match(
    $installer,
    '(?ms)^\[UninstallDelete\]\s*(?<body>.*?)(?=^\[[^]]+\]|\z)'
)
if (-not $uninstallDeleteSection.Success -or
    $uninstallDeleteSection.Groups['body'].Value -notmatch '(?m)^Type:\s*dirifempty;\s*Name:\s*"\{app\}"\s*$') {
    throw 'Installer does not safely remove the exact application root when final cleanup leaves it empty.'
}
if ($uninstallDeleteSection.Groups['body'].Value -match '(?im)^Type:\s*filesandordirs;\s*Name:\s*"\{app\}\\Service(?:\\|"|\*)') {
    throw 'Installer must not recursively delete the protected Service tree without broker receipts.'
}
if ($installer -match '(?im)\bsc(?:\.exe)?\s+(?:create|config|start|stop|delete)\b') {
    throw 'Installer must mutate SCM only through the fixed protected setup broker.'
}

$installedPackage = Get-Content -LiteralPath (
    Join-Path $root 'control-center\src-tauri\src\machine_integration\open_service\broker\installed_package.rs'
) -Raw
foreach ($installedPackageToken in @(
    'HKEY_LOCAL_MACHINE',
    'RRF_SUBKEY_WOW6464KEY',
    'SOFTWARE\MacType\ControlCenter',
    'InstallLocation',
    'FOLDERID_ProgramFiles',
    'reject_reparse_ancestors',
    'service-runtime\mactype-service-setup.exe',
    'service-runtime\payload\manifest.json',
    'INSTALLATION_REQUIRED_PREFIX',
    'INSTALLATION_INCOMPLETE_PREFIX',
    'INSTALLATION_UNTRUSTED_PREFIX'
)) {
    if (-not $installedPackage.Contains($installedPackageToken)) {
        throw "Installed-package preflight is missing trust contract token: $installedPackageToken"
    }
}
if ($installedPackage.Contains('Program Files\MacType Control Center') -or
    $installedPackage.Contains('join("MacType Control Center")')) {
    throw 'Installed-package discovery hardcodes the product folder instead of trusting installer registration.'
}

$operationLog = Get-Content -LiteralPath (
    Join-Path $root 'control-center\src-tauri\src\diagnostics\operation_log.rs'
) -Raw
foreach ($diagnosticToken in @(
    'Expected installed Control Center:',
    'Current executable:',
    'Expected executable exists:',
    'Installed Control Center:',
    'Setup broker:',
    'Runtime manifest:',
    'Runtime payload:',
    'Elevation attempted:',
    'Machine state changed:',
    'Rollback required:'
)) {
    if (-not $operationLog.Contains($diagnosticToken)) {
        throw "Installation preflight diagnostics are missing field: $diagnosticToken"
    }
}

$installerTest = Get-Content -LiteralPath (Join-Path $root 'scripts\ci\Test-InstallerWindows.ps1') -Raw
foreach ($installerTestToken in @(
    'CommonDesktopDirectory',
    'Arbitrary-directory install',
    'must survive rejected /DIR install',
    'Rejected /DIR attempt changed the existing target tree',
    'Assert-ReadyOpenService',
    'Assert-BaselineRestoredAfterFailedUpgrade',
    'Deliberately failing protected upgrade',
    'Upgrade reused an immutable runtime version',
    'Uninstall with missing protected broker',
    'CI foreign fixed-name service',
    'foreign application-root contents',
    'Install retained foreign application-root contents',
    'CI legacy MacTray service',
    'Assert-UserMarkers'
)) {
    if (-not $installerTest.Contains($installerTestToken)) {
        throw "Installer E2E omits required machine integration scenario: $installerTestToken"
    }
}

$distributionDocs = @{
    'docs\control-center-ci.md' = @(
        'PrivilegesRequired=admin',
        'fixed Program Files',
        'test-only failing upgrade',
        'LocalAppData theme, locale, recent-profile, applied-profile, and profile files'
    )
    'docs\independent-distribution.md' = @(
        'administrator-elevated installer',
        'bootstrap-install',
        'Auto/LocalSystem/Running',
        '%LOCALAPPDATA%'
    )
}
$staleInstallerClaims = @(
    'PrivilegesRequired=lowest',
    'installer is per-user',
    'per-user installer only stages',
    'never registers or starts SCM',
    'request no elevation'
)
foreach ($entry in $distributionDocs.GetEnumerator()) {
    $text = Get-Content -LiteralPath (Join-Path $root $entry.Key) -Raw
    foreach ($token in $entry.Value) {
        if (-not $text.Contains($token)) {
            throw "Distribution documentation is missing current installer contract '$token': $($entry.Key)"
        }
    }
    foreach ($claim in $staleInstallerClaims) {
        if ($text.Contains($claim)) {
            throw "Distribution documentation retains stale installer behavior '$claim': $($entry.Key)"
        }
    }
}

$trackedFiles = @(& git -C $root ls-files)
if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate tracked files for desktop-only distribution policy.' }

$forbiddenSiteArtifacts = $trackedFiles | Where-Object {
    (Test-Path -LiteralPath (Join-Path $root $_)) -and (
        $_ -match '(?i)(?:^|/)(?:robots\.txt|sitemap(?:[-._][^/]*)?|seo(?:[-._][^/]*)?)$' -or
        $_ -match '(?i)(?:^|/)lighthouse(?:/|[-._])' -or
        $_ -eq 'scripts/ci/Assert-Lighthouse.mjs'
    )
}
if ($forbiddenSiteArtifacts) {
    throw "Desktop distribution contains website-only artifacts: $($forbiddenSiteArtifacts -join ', ')"
}

$workflowTokens = @('light' + 'house', 'robots' + '.txt', 'site' + 'map', 'S' + 'EO')
foreach ($workflow in Get-ChildItem -LiteralPath (Join-Path $root '.github\workflows') -File | Where-Object Extension -in @('.yml', '.yaml')) {
    $workflowText = Get-Content -LiteralPath $workflow.FullName -Raw
    foreach ($token in $workflowTokens) {
        if ($workflowText -match "(?i)\b$([regex]::Escape($token))\b") {
            throw "Desktop CI workflow contains website-only token '$token': $($workflow.Name)"
        }
    }
}

$frontendEvidenceRoot = Join-Path $root '.superloopy\evidence\frontend\2026-07-12-control-center'
$frontendEvidenceContracts = @{
    'PERF.md' = @('Design-system compliance', 'React Doctor')
    'SUPERLOOPY_EVIDENCE.md' = @('VISUAL_QA.md', 'DESIGN_TOKENS.md', 'TARGET_SPEC.md', 'screenshots/*.png')
}
foreach ($entry in $frontendEvidenceContracts.GetEnumerator()) {
    $path = Join-Path $frontendEvidenceRoot $entry.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Desktop frontend evidence is missing: $($entry.Key)"
    }
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -match '(?i)lighthouse') {
        throw "Desktop frontend evidence contains a website-only Lighthouse reference: $($entry.Key)"
    }
    foreach ($token in $entry.Value) {
        if (-not $text.Contains($token)) {
            throw "Desktop frontend evidence is missing '$token': $($entry.Key)"
        }
    }
}

Write-Host 'Independent distribution policy passed.'
