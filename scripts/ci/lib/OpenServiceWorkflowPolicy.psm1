Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'WorkflowModel.psm1') -Force

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
    if (-not (Test-Path -LiteralPath $buildWorkflowPath -PathType Leaf)) {
        $failures.Add('.github/workflows/build.yml is missing.')
    } else {
        $buildWorkflow = Read-GitHubWorkflow -Path $buildWorkflowPath
        $openCoreJob = Get-WorkflowJob -Workflow $buildWorkflow -Id 'open-core'
        if (-not $openCoreJob) {
            $failures.Add("build.yml is missing required open-service CI job 'open-core'.")
        } else {
            $openCoreUpload = @(
                Get-WorkflowStep -Job $openCoreJob -Uses 'actions/upload-artifact@v7' |
                    Where-Object {
                        $_.With.name -eq 'mactype-open-core' -and
                        $_.With.path -eq 'artifacts/open-core'
                    }
            ) | Select-Object -First 1
            if (-not $openCoreUpload) {
                $failures.Add('build.yml open-core job does not upload the mactype-open-core artifact from artifacts/open-core.')
            }
        }
        $openServiceJob = Get-WorkflowJob -Workflow $buildWorkflow -Id 'open-service-windows'
        if (-not $openServiceJob) {
            $failures.Add("build.yml is missing required open-service CI job 'open-service-windows'.")
        } elseif (-not @(Get-WorkflowStep -Job $openServiceJob `
            -RunLike '*Test-OpenServiceWindows.ps1*').Count) {
            $failures.Add("build.yml open-service-windows job is missing required command token 'Test-OpenServiceWindows.ps1'.")
        }
        $windowsBuildJob = Get-WorkflowJob -Workflow $buildWorkflow -Id 'windows-build'
        foreach ($token in @('Build-ServiceRuntime.ps1', 'ServiceRuntimeRoot')) {
            if (-not $windowsBuildJob -or
                -not @(Get-WorkflowStep -Job $windowsBuildJob -RunLike "*$token*").Count) {
                $failures.Add("build.yml windows-build job is missing required command token '$token'.")
            }
        }
        if (-not $openServiceJob -or
            -not @(Get-WorkflowStep -Job $openServiceJob `
                -NameLike '*hook x86/x64 markers*').Count) {
            $failures.Add("build.yml open-service-windows job is missing required step 'hook x86/x64 markers'.")
        }
        foreach ($token in @('BrowserLaunchGate', 'browser-launch-gate64.exe')) {
            if (-not $openServiceJob -or
                -not @(Get-WorkflowStep -Job $openServiceJob -RunLike "*$token*").Count) {
                $failures.Add("build.yml open-service-windows job is missing required browser proof token '$token'.")
            }
        }
    }

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
        -TokenMessage "disk-backed virtual font adapter is missing required token '{0}'." `
        -Tokens @(
            'BuildAliasedSfnt', 'virtual_font_cache::PersistFont',
            'CreateFontFileReference'
        )
    if ($virtualFont -and $virtualFont.Contains('CreateInMemoryFontFileLoader')) {
        $failures.Add('virtual fonts must not depend on a process-local DirectWrite loader.')
    }

    $virtualFontCachePath = Join-Path $Root 'renderer/virtual_font_cache.cpp'
    $null = Test-RequiredTokens -Failures $failures -Path $virtualFontCachePath `
        -MissingMessage 'renderer/virtual_font_cache.cpp is missing.' `
        -TokenMessage "disk-backed virtual font cache is missing required token '{0}'." `
        -Tokens @(
            'BCryptHashData', 'renderer_raii::UniqueHandle', 'MoveFileExW',
            'HRESULT PersistFont'
        )

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
    if (-not (Test-Path -LiteralPath $lintWorkflowPath -PathType Leaf)) {
        $failures.Add('.github/workflows/lint.yml is missing.')
    } else {
        $lintWorkflow = Read-GitHubWorkflow -Path $lintWorkflowPath
        $injectorJob = Get-WorkflowJob -Workflow $lintWorkflow -Id 'service-injector'
        if (-not $injectorJob) {
            $failures.Add('lint.yml does not enforce the service-injector job.')
        } else {
            foreach ($token in @('ctest --test-dir', '-DCMAKE_CXX_FLAGS=/analyze')) {
                if (-not @(Get-WorkflowStep -Job $injectorJob -RunLike "*$token*").Count) {
                    $failures.Add("lint.yml service-injector job is missing '$token'.")
                }
            }
            $injectorRaw = Get-WorkflowRawText -Workflow $lintWorkflow
            foreach ($token in @('mactype-injector32', 'mactype-injector64')) {
                if (-not $injectorRaw.Contains($token)) {
                    $failures.Add("lint.yml service-injector matrix is missing '$token'.")
                }
            }
        }
        $tauriJob = Get-WorkflowJob -Workflow $lintWorkflow -Id 'rust'
        foreach ($token in @('Test-OpenServiceTestSupport.ps1', 'Test-OpenServicePolicyModules.ps1')) {
            if (-not $tauriJob -or
                -not @(Get-WorkflowStep -Job $tauriJob -RunLike "*$token*").Count) {
                $failures.Add("lint.yml does not enforce the service-injector contract '$token'.")
            }
        }
    }

    $codeqlWorkflowPath = Join-Path $Root '.github\workflows\codeql.yml'
    if (-not (Test-Path -LiteralPath $codeqlWorkflowPath -PathType Leaf)) {
        $failures.Add('.github/workflows/codeql.yml is missing.')
    } else {
        $codeqlWorkflow = Read-GitHubWorkflow -Path $codeqlWorkflowPath
        $analysisJob = Get-WorkflowJob -Workflow $codeqlWorkflow -Id 'analyze'
        if (-not $analysisJob -or
            -not @(Get-WorkflowStep -Job $analysisJob `
                -RunLike '*.github/scripts/Build-OpenCore.ps1*').Count) {
            $failures.Add("codeql.yml does not use the verified open-core analysis build '.github/scripts/Build-OpenCore.ps1'.")
        }
    }

    $disposableWorkflowPath = Join-Path $Root '.github\workflows\open-service-disposable-vm.yml'
    $disposableScriptPath = Join-Path $Root 'scripts\ci\Test-OpenServiceDisposableVm.ps1'
    if (-not (Test-Path -LiteralPath $disposableWorkflowPath -PathType Leaf)) {
        $failures.Add('.github/workflows/open-service-disposable-vm.yml is missing.')
    } else {
        $disposableWorkflow = Read-GitHubWorkflow -Path $disposableWorkflowPath
        if ($disposableWorkflow.On -match '(?m)^  (?:push|pull_request|schedule|workflow_call):') {
            $failures.Add('open-service-disposable-vm.yml must be workflow_dispatch-only.')
        }
        if ($disposableWorkflow.On -notmatch '(?m)^  workflow_dispatch:') {
            $failures.Add("open-service-disposable-vm.yml is missing 'workflow_dispatch:'.")
        }
        $verifyJob = Get-WorkflowJob -Workflow $disposableWorkflow -Id 'verify'
        if (-not $verifyJob) {
            $failures.Add("open-service-disposable-vm.yml is missing guarded job 'verify'.")
        } else {
            if ('mactype-disposable-vm' -notin @($verifyJob.RunsOn)) {
                $failures.Add("open-service-disposable-vm.yml is missing runner label 'mactype-disposable-vm'.")
            }
            if ($verifyJob.If -match 'inputs\.confirmation\s*==') {
                $failures.Add('open-service-disposable-vm.yml must not skip the verification job on an invalid confirmation.')
            }
            $guardStep = @(Get-WorkflowStep -Job $verifyJob `
                -NameLike 'Reject invalid confirmation') | Select-Object -First 1
            $checkoutStep = @(Get-WorkflowStep -Job $verifyJob `
                -Uses 'actions/checkout@v7') | Select-Object -First 1
            $guardIndex = if ($guardStep) {
                [array]::IndexOf([object[]] $verifyJob.Steps, $guardStep)
            } else { -1 }
            $checkoutIndex = if ($checkoutStep) {
                [array]::IndexOf([object[]] $verifyJob.Steps, $checkoutStep)
            } else { -1 }
            if ($guardIndex -lt 0 -or $checkoutIndex -lt 0 -or
                $guardIndex -gt $checkoutIndex) {
                $failures.Add('open-service-disposable-vm.yml must reject an invalid confirmation in the first step before checkout or build work.')
            }
            foreach ($token in @(
                "-cne 'I_UNDERSTAND_DISPOSABLE_VM'",
                "throw 'Disposable VM confirmation must exactly match I_UNDERSTAND_DISPOSABLE_VM.'"
            )) {
                if (-not $guardStep -or -not $guardStep.Run.Contains($token)) {
                    $failures.Add("open-service-disposable-vm.yml is missing strict confirmation guard '$token'.")
                }
            }
            if (-not @(Get-WorkflowStep -Job $verifyJob `
                -RunLike '*Test-OpenServiceDisposableVm.ps1*').Count) {
                $failures.Add("open-service-disposable-vm.yml is missing 'Test-OpenServiceDisposableVm.ps1'.")
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
