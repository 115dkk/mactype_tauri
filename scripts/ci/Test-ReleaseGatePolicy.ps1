[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workflowPath = Join-Path $root '.github\workflows\build.yml'
$workflow = Get-Content -LiteralPath $workflowPath -Raw

$requiredTokens = @(
    'release-frontend-quality:',
    'pnpm generate:settings',
    'pnpm test:i18n',
    'pnpm test:settings',
    'pnpm lint',
    'release-tauri-quality:',
    'cargo fmt --all -- --check',
    'cargo clippy --all-targets --all-features -- -D warnings',
    'cargo test --all-targets',
    'release-cpp-quality:',
    'ctest --test-dir build/preview-helper -C Release --output-on-failure',
    'release-injector-quality:',
    'release-static-quality:',
    'scripts/ci/Test-DistributionPolicy.ps1',
    'scripts/ci/Test-ReleaseGatePolicy.ps1',
    'scripts/ci/Test-InstallerRollbackPolicy.ps1',
    'release-gallery:',
    'pnpm test:gallery',
    'release-quality-gate:'
)
foreach ($token in $requiredTokens) {
    if (-not $workflow.Contains($token)) {
        throw "Release workflow omits required quality token: $token"
    }
}

$qualityNeeds = @(
    'release-frontend-quality',
    'release-tauri-quality',
    'release-cpp-quality',
    'release-injector-quality',
    'release-static-quality',
    'release-gallery'
)
$qualityGate = [regex]::Match(
    $workflow,
    '(?ms)^  release-quality-gate:\s*.*?(?=^  [a-zA-Z0-9_-]+:|\z)'
).Value
if (-not $qualityGate) { throw 'Release quality gate job is missing.' }
foreach ($job in $qualityNeeds) {
    if ($qualityGate -notmatch "(?m)^\s+needs:\s*\[[^\]]*\b$([regex]::Escape($job))\b") {
        throw "Release quality gate does not depend on $job."
    }
}

$releaseJob = [regex]::Match(
    $workflow,
    '(?ms)^  release-main-snapshot:\s*.*?(?=^  [a-zA-Z0-9_-]+:|\z)'
).Value
if (-not $releaseJob) { throw 'Main snapshot release job is missing.' }
foreach ($job in @('windows-build', 'open-service-windows', 'release-quality-gate')) {
    if ($releaseJob -notmatch "(?m)^\s+needs:\s*\[[^\]]*\b$([regex]::Escape($job))\b") {
        throw "Main snapshot publication can run without $job."
    }
}
if ($releaseJob -notmatch "(?m)^\s+if:\s*github\.event_name == 'push' && github\.ref == 'refs/heads/main'") {
    throw 'Main snapshot publication is not restricted to main pushes.'
}

Write-Host 'Release publication quality-gate policy passed.'
