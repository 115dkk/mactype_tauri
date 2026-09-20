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
        -Tokens @('ExpectedRuntimeRoot', 'resolvedModuleRoot', 'OrdinalIgnoreCase', 'pid = [uint32]', 'sessionId = [uint32]')

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
