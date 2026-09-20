[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib\WorkflowModel.psm1') -Force

function Test-ReleaseGatePolicy {
    [CmdletBinding()]
    param([Parameter(Mandatory)] $Workflow)

    $failures = [System.Collections.Generic.List[string]]::new()

    $requiredRuns = [ordered]@{
        'release-frontend-quality' = @(
            'pnpm generate:settings',
            'pnpm test:i18n',
            'pnpm test:settings',
            'pnpm lint'
        )
        'release-tauri-quality' = @(
            'cargo fmt --all -- --check',
            'cargo clippy --all-targets --all-features -- -D warnings',
            'cargo test --all-targets'
        )
        'release-cpp-quality' = @(
            'ctest --test-dir build/preview-helper -C Release --output-on-failure'
        )
        'release-static-quality' = @(
            'scripts/ci/Test-DistributionPolicy.ps1',
            'scripts/ci/Test-ReleaseGatePolicy.ps1',
            'scripts/ci/Test-InstallerRollbackPolicy.ps1'
        )
        'release-gallery' = @('pnpm test:gallery')
    }
    foreach ($entry in $requiredRuns.GetEnumerator()) {
        $job = Get-WorkflowJob -Workflow $Workflow -Id $entry.Key
        if (-not $job) {
            $failures.Add("Release workflow omits required quality job '$($entry.Key)'.")
            continue
        }
        foreach ($run in $entry.Value) {
            if (-not @(Get-WorkflowStep -Job $job -RunLike "*$run*").Count) {
                $failures.Add("Release quality job '$($entry.Key)' omits required command '$run'.")
            }
        }
    }
    if (-not (Get-WorkflowJob -Workflow $Workflow -Id 'release-injector-quality')) {
        $failures.Add("Release workflow omits required quality job 'release-injector-quality'.")
    }

    $qualityNeeds = @(
        'release-frontend-quality',
        'release-tauri-quality',
        'release-cpp-quality',
        'release-injector-quality',
        'release-static-quality',
        'release-gallery'
    )
    $qualityGate = Get-WorkflowJob -Workflow $Workflow -Id 'release-quality-gate'
    if (-not $qualityGate) {
        $failures.Add('Release quality gate job is missing.')
    } else {
        foreach ($job in $qualityNeeds) {
            if ($job -notin $qualityGate.Needs) {
                $failures.Add("Release quality gate does not depend on $job.")
            }
        }
    }

    $releaseGalleryJob = Get-WorkflowJob -Workflow $Workflow -Id 'release-gallery'
    if ($releaseGalleryJob) {
        $galleryUpload = @(
            Get-WorkflowStep -Job $releaseGalleryJob -Uses 'actions/upload-artifact@v7' |
                Where-Object { $_.With.name -eq 'release-frontend-window-gallery' }
        ) | Select-Object -First 1
        if (-not $galleryUpload -or
            $galleryUpload.With.path -ne 'artifacts/frontend-gallery') {
            $failures.Add('Release gallery job no longer preserves the complete validated gallery artifact.')
        }
    }

    $releaseJob = Get-WorkflowJob -Workflow $Workflow -Id 'release-main-snapshot'
    if (-not $releaseJob) {
        $failures.Add('Main snapshot release job is missing.')
        return $failures.ToArray()
    }
    foreach ($job in @('windows-build', 'open-service-windows', 'release-quality-gate', 'release-gallery')) {
        if ($job -notin $releaseJob.Needs) {
            $failures.Add("Main snapshot publication can run without $job.")
        }
    }
    if ($releaseJob.If -ne "success() && github.event_name == 'push' && (github.ref == 'refs/heads/main' || github.ref == 'refs/heads/codex/alpha-plus-dll')") {
        $failures.Add('Snapshot publication is not restricted to successful main or alpha-plus-dll pushes.')
    }

    $galleryDownload = @(
        Get-WorkflowStep -Job $releaseJob -Uses 'actions/download-artifact@v8' |
            Where-Object { $_.With.name -eq 'release-frontend-window-gallery' }
    ) | Select-Object -First 1
    if (-not $galleryDownload) {
        $failures.Add('Main snapshot publication does not download the validated frontend gallery artifact.')
    } elseif ($galleryDownload.With.path -ne 'gallery-artifact') {
        $failures.Add('Validated frontend gallery is not downloaded to the bounded selection input.')
    }

    $releaseAssets = @(
        'desktop-1280-overview-en.png',
        'desktop-1280-execution-ready-en.png',
        'desktop-1280-execution-migration-available-en.png',
        'desktop-1280-profiles-zh-CN.png',
        'mobile-390-overview-ar.png'
    )
    $selectionStep = @(Get-WorkflowStep -Job $releaseJob -NameLike 'Select the bounded release gallery') |
        Select-Object -First 1
    if (-not $selectionStep) {
        $failures.Add('Bounded release gallery selection step is missing.')
        $selectedAssets = @()
    } else {
        $selectedAssets = @(
            [regex]::Matches($selectionStep.Run, "'(?<asset>[^'`r`n]+\.png)'") |
                ForEach-Object { $_.Groups['asset'].Value }
        )
    }

    $publicationStep = @(Get-WorkflowStep -Job $releaseJob -NameLike 'Publish automatic pre-release') |
        Select-Object -First 1
    if (-not $publicationStep) {
        $failures.Add('Automatic pre-release publication step is missing.')
        $publishedAssets = @()
    } else {
        $publishedAssets = @(
            ([string] $publicationStep.With.files).Split("`n") |
                Where-Object { $_ -like 'release/gallery/*.png' } |
                ForEach-Object { [IO.Path]::GetFileName($_) }
        )
    }

    foreach ($assetSet in @(
        @{ Name = 'selected'; Values = $selectedAssets },
        @{ Name = 'published'; Values = $publishedAssets }
    )) {
        $difference = @(Compare-Object -ReferenceObject $releaseAssets `
            -DifferenceObject $assetSet.Values -CaseSensitive)
        if ($assetSet.Values.Count -ne $releaseAssets.Count -or $difference.Count -ne 0) {
            $failures.Add("Release gallery $($assetSet.Name) asset allowlist does not exactly match the required five files.")
        }
    }

    return $failures.ToArray()
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workflow = Read-GitHubWorkflow -Path (Join-Path $root '.github\workflows\build.yml')
$failures = @(Test-ReleaseGatePolicy -Workflow $workflow)
if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host "ERROR: $_" -ForegroundColor Red }
    throw "Release publication quality-gate policy failed with $($failures.Count) violation(s)."
}

Write-Host 'Release publication quality-gate policy passed.'
