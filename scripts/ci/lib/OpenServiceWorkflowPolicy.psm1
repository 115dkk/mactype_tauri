Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-RequiredTokens {
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]] $Failures,
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $MissingMessage,
        [Parameter(Mandatory)] [string] $TokenMessage,
        [Parameter(Mandatory)] [string[]] $Tokens
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $Failures.Add($MissingMessage)
        return $null
    }
    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($token in $Tokens) {
        if (-not $text.Contains($token)) {
            $Failures.Add(($TokenMessage -f $token))
        }
    }
    return $text
}

function Test-OpenServiceWorkflowPolicy {
    [CmdletBinding()]
    param([Parameter(Mandatory)] [string] $Root)

    $failures = [System.Collections.Generic.List[string]]::new()
    $markerVerifierPath = Join-Path $Root 'scripts\ci\Test-OpenServiceMarkersWindows.ps1'
    $null = Test-RequiredTokens -Failures $failures -Path $markerVerifierPath `
        -MissingMessage 'scripts/ci/Test-OpenServiceMarkersWindows.ps1 is missing.' `
        -TokenMessage "hosted marker verification is missing generation-binding token '{0}'." `
        -Tokens @(
            'ExpectedRuntimeRoot', 'resolvedModuleRoot', 'OrdinalIgnoreCase',
            'pid = [uint32]', 'sessionId = [uint32]',
            'directWriteFontSetCollection.replacementObserved',
            'activeIdentityCoherent',
            'virtualNameTableCoherent',
            'retainedGenerationObjectStable',
            'resourceRoundTripCoherent',
            'replacementGeometryCoherent'
        )

    $buildWorkflowPath = Join-Path $Root '.github\workflows\build.yml'
    $null = Test-RequiredTokens -Failures $failures -Path $buildWorkflowPath `
        -MissingMessage '.github/workflows/build.yml is missing.' `
        -TokenMessage "build.yml is missing required open-service CI token '{0}'." `
        -Tokens @(
            'open-core:', 'mactype-open-core', 'artifacts/open-core',
            'open-service-windows:', 'Test-OpenServiceWindows.ps1',
            'Build-ServiceRuntime.ps1', 'ServiceRuntimeRoot', 'hook x86/x64 markers',
            'BrowserLaunchGate', 'browser-launch-gate64.exe'
        )

    $hostedLifecyclePath = Join-Path $Root 'scripts\ci\Test-OpenServiceWindows.ps1'
    $hostedLifecycle = Test-RequiredTokens -Failures $failures -Path $hostedLifecyclePath `
        -MissingMessage 'scripts/ci/Test-OpenServiceWindows.ps1 is missing.' `
        -TokenMessage "hosted lifecycle verification is missing required contract token '{0}'." `
        -Tokens @(
            'Assert-GenerationBoundMarkerTelemetry', 'runtimeGenerationId',
            'profileDigest', '$MarkerResults', 'successCount', 'lastSuccess',
            'x86 and x64 marker telemetry is not bound to the same runtime generation',
            'OpenServiceAclFixture.psm1', 'Invoke-OpenServiceAclRepairFixture',
            '-RepairContext $stagedSetup', 'param($setupExecutable)',
            "`$sourceFamily = 'Cambria'",
            '--chromium-loader $chromiumLoader',
            "'product-macloader'", 'productLoaderBoundaryObserved',
            "'product-loader-runtime'", 'AlternativeFile=profile.ini',
            "-Phase 'post-MacLoader service restart'",
            '--firefox-launch-gate $BrowserLaunchGate',
            '--expect unsupported-late-collection', 'unsupportedLateCollectionObserved',
            'replacementObserved', 'aliasGenerationConsumedObserved',
            'aliasCollectionReturnedObserved', 'retainedStockGenerationObserved',
            "-Verb 'publish-profile' -InputBytes `$profileA",
            "Assert-ActiveRuntimeProfile -ExpectedBytes `$profileA"
        )
    if ($hostedLifecycle) {
        $profilePublishToken = "-Verb 'publish-profile' -InputBytes `$profileA"
        $profilePublishIndex = $hostedLifecycle.IndexOf($profilePublishToken)
        $profileVerificationIndex = $hostedLifecycle.IndexOf(
            "Assert-ActiveRuntimeProfile -ExpectedBytes `$profileA"
        )
        $aclRepairIndex = $hostedLifecycle.IndexOf('Invoke-OpenServiceAclRepairFixture')
        if ([regex]::Matches(
                $hostedLifecycle,
                [regex]::Escape($profilePublishToken)
            ).Count -ne 1) {
            $failures.Add('hosted lifecycle must publish profile A exactly once.')
        }
        if ($profilePublishIndex -gt $aclRepairIndex -or
            $profileVerificationIndex -gt $aclRepairIndex) {
            $failures.Add(
                'hosted lifecycle must publish and verify profile A before the exact ACL repair fixture.'
            )
        }
    }

    $browserProbePath = Join-Path $Root 'tools\service-probe\browser_font_probe.mjs'
    $null = Test-RequiredTokens -Failures $failures -Path $browserProbePath `
        -MissingMessage 'tools/service-probe/browser_font_probe.mjs is missing.' `
        -TokenMessage "browser font proof is missing coherent rendering contract '{0}'." `
        -Tokens @(
            'schemaVersion: 2',
            'classifyDirectWriteGeneration',
            'targetTreeHooked', 'initialSuccessCount',
            'browserPidInjectionObserved', 'metricSamples',
            'rasterComparison', 'replacementMetricsObserved',
            'replacementRasterObserved', 'aliasGenerationConsumedObserved',
            'retainedStockGenerationObserved',
            'unsupportedLateCollectionObserved',
            'legacy-system-collection-alias-returned',
            'system-font-set-alias-returned',
            'modern-system-collection-alias-returned', 'firefoxLaunchGate',
            'MACTYPE_BROWSER_GATE_TARGET', 'MACTYPE_BROWSER_GATE_PID_FILE',
            'chromiumLoader', 'launchChromiumWithProductLoader',
            'productLoaderBoundaryObserved'
        )
    $browserProbe = Get-Content -LiteralPath $browserProbePath -Raw

    $browserEvidencePath = Join-Path $Root 'tools\service-probe\browser_font_evidence.mjs'
    $browserEvidence = Test-RequiredTokens -Failures $failures -Path $browserEvidencePath `
        -MissingMessage 'tools/service-probe/browser_font_evidence.mjs is missing.' `
        -TokenMessage "browser generation evidence is missing classification token '{0}'." `
        -Tokens @(
            'classifyDirectWriteGeneration', 'ALIAS_RETURN_STAGES',
            'aliasSnapshotPreparedObserved', 'aliasCollectionReturnedObserved',
            'aliasGenerationConsumedObserved', 'retainedStockGenerationObserved',
            'legacy-system-collection-alias-returned',
            'system-font-set-alias-returned',
            'modern-system-collection-alias-returned'
        )

    $browserEvidenceTestPath = Join-Path $Root 'tools\service-probe\tests\browser_font_evidence.test.mjs'
    $null = Test-RequiredTokens -Failures $failures -Path $browserEvidenceTestPath `
        -MissingMessage 'browser generation evidence tests are missing.' `
        -TokenMessage "browser generation evidence tests are missing scenario '{0}'." `
        -Tokens @(
            'ordinary late Chromium retains the stock generation',
            'MacLoader Chromium consumes the returned alias generation',
            'a later Firefox alias return does not mutate its retained stock list'
        )

    $productLoaderPath = Join-Path $Root 'tools\service-probe\chromium_product_loader.mjs'
    $productLoader = Test-RequiredTokens -Failures $failures -Path $productLoaderPath `
        -MissingMessage 'tools/service-probe/chromium_product_loader.mjs is missing.' `
        -TokenMessage "Chromium product-loader adapter is missing lifecycle token '{0}'." `
        -Tokens @(
            'spawn', 'connectOverCDP', 'DevToolsActivePort',
            "session.send('Browser.close')", 'process.kill(pid)',
            'removeUserDataDirectory', 'mactype-chromium-loader-'
        )
    foreach ($forbiddenToken in @(
        'MOZ_DEBUG_CHILD_PAUSE',
        'dom.ipc.processPrelaunch.enabled', 'waitForBrowserRoleInjection',
        'FontDataServiceAllWebContents'
    )) {
        if ($browserProbe.Contains($forbiddenToken) -or
            ($browserEvidence -and $browserEvidence.Contains($forbiddenToken)) -or
            ($productLoader -and $productLoader.Contains($forbiddenToken))) {
            $failures.Add("browser proof must not restore launch/injection tape '$forbiddenToken'.")
        }
    }

    $browserGatePath = Join-Path $Root 'tools\service-probe\browser_launch_gate.cpp'
    $null = Test-RequiredTokens -Failures $failures -Path $browserGatePath `
        -MissingMessage 'tools/service-probe/browser_launch_gate.cpp is missing.' `
        -TokenMessage "browser launch gate is missing entry-point injection contract '{0}'." `
        -Tokens @(
            'DEBUG_ONLY_THIS_PROCESS', 'AddressOfEntryPoint',
            'RestoreAndRewind', 'DebugActiveProcessStop',
            'pid-%lu.hook-ready', 'SuspendMainThread',
            'ScopedInheritableDescriptor', '_get_osfhandle',
            'GetStartupInfoW', '-juggler-pipe'
        )

    $directWritePath = Join-Path $Root 'renderer\directwrite.cpp'
    $directWrite = Test-RequiredTokens -Failures $failures -Path $directWritePath `
        -MissingMessage 'renderer/directwrite.cpp is missing.' `
        -TokenMessage "DirectWrite launch readiness is missing race guard '{0}'." `
        -Tokens @(
            'static DWORD WINAPI HookExistingDirectWriteFactory',
            'static void ScheduleExistingDirectWriteFactoryHook',
            'static bool HookDirectWriteAliasCollection',
            'kFactoryGetSystemFontCollectionSlot = 3',
            'kFactory3GetSystemFontSetSlot = 35',
            'kFactory3GetSystemFontCollectionSlot = 38',
            'renderer_raii::PageProtection::TrySet',
            'PatchFactoryAliasVtables',
            'legacy-system-collection-alias-returned',
            'SignalDirectWriteDiagnostic(readyStage);',
            'DWRITE_FACTORY_TYPE_SHARED',
            'DWRITE_FACTORY_TYPE_ISOLATED',
            'HookKnownDirectWriteFactories(ORIG_DWriteCreateFactory, L"hook-ready");'
        )
    if ($directWrite) {
        $readyCall = 'HookKnownDirectWriteFactories(ORIG_DWriteCreateFactory, L"hook-ready");'
        $readyPublisher = 'SignalDirectWriteDiagnostic(readyStage);'
        $workerStart = $directWrite.IndexOf(
            'static DWORD WINAPI HookExistingDirectWriteFactory'
        )
        $workerEnd = $directWrite.IndexOf(
            'static void ScheduleExistingDirectWriteFactoryHook',
            $workerStart
        )
        $readyIndex = $directWrite.IndexOf($readyCall)
        if ($workerStart -lt 0 -or $workerEnd -le $workerStart -or
            $readyIndex -le $workerStart -or $readyIndex -ge $workerEnd) {
            $failures.Add(
                'hook-ready must be requested by the completed existing-factory worker.'
            )
        }
        if ([regex]::Matches(
                $directWrite,
                [regex]::Escape($readyCall)
            ).Count -ne 1) {
            $failures.Add('DirectWrite hook-ready must have exactly one worker request.')
        }
        if ([regex]::Matches(
                $directWrite,
                [regex]::Escape($readyPublisher)
            ).Count -ne 1) {
            $failures.Add('The existing-factory helper must have exactly one readiness publisher.')
        }
        foreach ($forbiddenToken in @(
            'AliasedDWriteFont', 'AliasedDWriteFontFace',
            'AliasedLocalizedStrings', 'thread_local',
            'HookCollectionFontCreation', 'FontFace_GetFiles',
            'FontFace_GetIndex', 'Factory_CreateFontFace',
            'PatchRetainedCollectionAlias',
            'IMPL_Factory_CreateCustomFontCollection',
            'IMPL_Factory3_CreateFontCollectionFromFontSet',
            'IMPL_Factory_CreateFontFace', 'IMPL_FontFamily_GetFont',
            'IMPL_Collection_GetFontFamilyCount',
            'IMPL_Collection1_GetFontSet'
        )) {
            if ($directWrite.Contains($forbiddenToken)) {
                $failures.Add(
                    "DirectWrite must not restore mixed-identity hook '$forbiddenToken'."
                )
            }
        }
    }

    $directWriteAliasPath = Join-Path $Root 'renderer\directwrite_alias.cpp'
    $null = Test-RequiredTokens -Failures $failures -Path $directWriteAliasPath `
        -MissingMessage 'renderer/directwrite_alias.cpp is missing.' `
        -TokenMessage "DirectWrite alias collection is missing coherent object-graph token '{0}'." `
        -Tokens @(
            'IDWriteFontSetBuilder', 'FindReplacementReference',
            'directwrite_virtual_font::CreateAliasedReference',
            'builder->AddFontFaceReference(virtualReference)',
            'CreateFontCollectionFromFontSet',
            'DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME'
        )

    $virtualFontPath = Join-Path $Root 'renderer\directwrite_virtual_font.cpp'
    $virtualFont = Test-RequiredTokens -Failures $failures -Path $virtualFontPath `
        -MissingMessage 'renderer/directwrite_virtual_font.cpp is missing.' `
        -TokenMessage "disk-backed virtual font is missing required token '{0}'." `
        -Tokens @(
            'BuildAliasedSfnt', 'PersistFont', 'BCryptHashData',
            'renderer_raii::UniqueHandle', 'MoveFileExW',
            'CreateFontFileReference'
        )
    if ($virtualFont -and $virtualFont.Contains('CreateInMemoryFontFileLoader')) {
        $failures.Add('virtual fonts must not depend on a process-local DirectWrite loader.')
    }

    $aclFixtureModulePath = Join-Path $Root 'scripts\ci\lib\OpenServiceAclFixture.psm1'
    $null = Test-RequiredTokens -Failures $failures -Path $aclFixtureModulePath `
        -MissingMessage 'scripts/ci/lib/OpenServiceAclFixture.psm1 is missing.' `
        -TokenMessage "exact ACL repair diagnostics are missing required token '{0}'." `
        -Tokens @(
            'S-1-5-32-545', 'exact-users-modify-repair',
            'post-repair-verification', 'targetAclSddl', 'innerError',
            'scQueryex', 'scQfailure', "-Name 'icacls'", 'RepairContext'
        )

    $supportTestPath = Join-Path $Root 'scripts\ci\Test-OpenServiceTestSupport.ps1'
    $null = Test-RequiredTokens -Failures $failures -Path $supportTestPath `
        -MissingMessage 'scripts/ci/Test-OpenServiceTestSupport.ps1 is missing.' `
        -TokenMessage "open-service CI support tests do not execute required test '{0}'." `
        -Tokens @('Test-OpenServiceAclFixture.ps1')

    $lintWorkflowPath = Join-Path $Root '.github\workflows\lint.yml'
    $null = Test-RequiredTokens -Failures $failures -Path $lintWorkflowPath `
        -MissingMessage '.github/workflows/lint.yml is missing.' `
        -TokenMessage "lint.yml does not enforce the service-injector contract '{0}'." `
        -Tokens @(
            'service-injector:', 'service-injector-x86', 'service-injector-x64',
            'ctest --test-dir', '-DCMAKE_CXX_FLAGS=/analyze',
            'mactype-injector32', 'mactype-injector64',
            'Test-OpenServiceTestSupport.ps1', 'Test-OpenServicePolicyModules.ps1'
        )

    $codeqlWorkflowPath = Join-Path $Root '.github\workflows\codeql.yml'
    $null = Test-RequiredTokens -Failures $failures -Path $codeqlWorkflowPath `
        -MissingMessage '.github/workflows/codeql.yml is missing.' `
        -TokenMessage "codeql.yml does not use the verified open-core analysis build '{0}'." `
        -Tokens @('.github/scripts/Build-OpenCore.ps1')

    $disposableWorkflowPath = Join-Path $Root '.github\workflows\open-service-disposable-vm.yml'
    $disposableScriptPath = Join-Path $Root 'scripts\ci\Test-OpenServiceDisposableVm.ps1'
    if (-not (Test-Path -LiteralPath $disposableWorkflowPath -PathType Leaf)) {
        $failures.Add('.github/workflows/open-service-disposable-vm.yml is missing.')
    } else {
        $disposableWorkflow = Get-Content -LiteralPath $disposableWorkflowPath -Raw
        if ($disposableWorkflow -match '(?m)^\s{2}(?:push|pull_request|schedule|workflow_call):') {
            $failures.Add('open-service-disposable-vm.yml must be workflow_dispatch-only.')
        }
        foreach ($requiredToken in @(
            'workflow_dispatch:', 'mactype-disposable-vm',
            'I_UNDERSTAND_DISPOSABLE_VM', 'Test-OpenServiceDisposableVm.ps1'
        )) {
            if (-not $disposableWorkflow.Contains($requiredToken)) {
                $failures.Add("open-service-disposable-vm.yml is missing '$requiredToken'.")
            }
        }
        if ($disposableWorkflow -match '(?m)^\s{4}if:\s*inputs\.confirmation\s*==') {
            $failures.Add('open-service-disposable-vm.yml must not skip the verification job on an invalid confirmation.')
        }
        $confirmationGuardIndex = $disposableWorkflow.IndexOf('Reject invalid confirmation')
        $checkoutIndex = $disposableWorkflow.IndexOf('actions/checkout@')
        if ($confirmationGuardIndex -lt 0 -or $checkoutIndex -lt 0 -or
            $confirmationGuardIndex -gt $checkoutIndex) {
            $failures.Add('open-service-disposable-vm.yml must reject an invalid confirmation in the first step before checkout or build work.')
        }
        foreach ($token in @(
            "-cne 'I_UNDERSTAND_DISPOSABLE_VM'",
            "throw 'Disposable VM confirmation must exactly match I_UNDERSTAND_DISPOSABLE_VM.'"
        )) {
            if (-not $disposableWorkflow.Contains($token)) {
                $failures.Add("open-service-disposable-vm.yml is missing strict confirmation guard '$token'.")
            }
        }
    }

    $null = Test-RequiredTokens -Failures $failures -Path $disposableScriptPath `
        -MissingMessage 'scripts/ci/Test-OpenServiceDisposableVm.ps1 is missing.' `
        -TokenMessage "disposable VM verifier is missing scenario contract '{0}'." `
        -Tokens @(
            'lifecycle', 'prepare-reboot', 'verify-after-reboot',
            'verify-migration', 'verify-multi-session', 'AppInit_DLLs',
            'MacTypeControlCenterTest'
        )

    return $failures.ToArray()
}

Export-ModuleMember -Function 'Test-OpenServiceWorkflowPolicy'
