[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$fixtureRoot = Join-Path $PSScriptRoot 'fixtures\workflow-model'
Import-Module (Join-Path $PSScriptRoot 'lib\WorkflowModel.psm1') -Force

function Assert-Equal {
    param(
        [Parameter(Mandatory)] $Actual,
        [Parameter(Mandatory)] $Expected,
        [Parameter(Mandatory)] [string] $Message
    )
    if ($Actual -cne $Expected) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function Assert-FailuresContain {
    param(
        [Parameter(Mandatory)] [string[]] $Failures,
        [Parameter(Mandatory)] [string] $Expected,
        [Parameter(Mandatory)] [string] $Label
    )
    if (-not ($Failures -match [regex]::Escape($Expected))) {
        throw "$Label failed for the wrong reason. Failures: $($Failures -join '; ')"
    }
}

function Import-PolicyScript {
    param([Parameter(Mandatory)] [string] $Name)
    $scriptPath = Join-Path $PSScriptRoot $Name
    $definition = Get-Content -LiteralPath $scriptPath -Raw
    $functionName = switch ($Name) {
        'Test-ReleaseGatePolicy.ps1' { 'Test-ReleaseGatePolicy' }
        'Test-InstallerRollbackPolicy.ps1' { 'Test-InstallerRollbackPolicy' }
        'Test-ServicePayloadVersionPolicy.ps1' { 'Test-ServicePayloadVersionPolicy' }
        'Test-DistributionPolicy.ps1' { 'Test-DistributionPolicy' }
        'Test-IntegrationDeveloperBundle.ps1' { 'Test-IntegrationDeveloperBundlePolicy' }
    }
    $functionStart = $definition.IndexOf("function $functionName")
    if ($functionStart -lt 0) {
        throw "$Name does not expose $functionName."
    }
    $functionAst = [System.Management.Automation.Language.Parser]::ParseInput(
        $definition.Substring($functionStart),
        [ref] $null,
        [ref] $null
    ).Find({
        param($ast)
        $ast -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $ast.Name -eq $functionName
    }, $false)
    if (-not $functionAst) {
        throw "$Name does not contain a parseable $functionName definition."
    }
    Set-Item -Path "function:global:$functionName" `
        -Value ([scriptblock]::Create($functionAst.Body.Extent.Text.Trim('{', '}')))
}

$subset = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'subset.yml')
Assert-Equal -Actual $subset.Name -Expected 'Workflow # model' -Message 'Quoted workflow name changed.'
if ($subset.On -notmatch '(?m)^  workflow_dispatch:$') {
    throw 'The model did not retain the raw on block.'
}
Assert-Equal -Actual $subset.Env.PLAIN -Expected 'value' -Message 'Plain scalar comment stripping failed.'
Assert-Equal -Actual $subset.Env.HASH -Expected 'keep # text' -Message 'Quoted hash handling failed.'
Assert-Equal -Actual $subset.Env.QUOTED -Expected "line`nvalue" -Message 'Double-quoted escape handling failed.'
$build = Get-WorkflowJob -Workflow $subset -Id 'build'
if (-not $build) {
    throw 'Get-WorkflowJob did not find the build fixture job.'
}
Assert-Equal -Actual ($build.RunsOn -join ',') -Expected 'self-hosted,Windows' `
    -Message 'Inline runs-on sequence parsing failed.'
Assert-Equal -Actual ($build.Needs -join ',') -Expected 'prepare,quote-job' `
    -Message 'Inline needs sequence parsing failed.'
Assert-Equal -Actual $build.If -Expected "`${{ github.event_name == 'push' }}" `
    -Message 'Expression scalar parsing failed.'
Assert-Equal -Actual $build.Environment -Expected 'production' `
    -Message 'Environment parsing failed.'
$literal = @(Get-WorkflowStep -Job $build -NameLike 'Literal') | Select-Object -First 1
Assert-Equal -Actual $literal.Id -Expected 'literal' -Message 'Step id parsing failed.'
Assert-Equal -Actual $literal.Shell -Expected 'pwsh' -Message 'Step shell parsing failed.'
Assert-Equal -Actual $literal.WorkingDirectory -Expected 'src' `
    -Message 'Step working-directory parsing failed.'
Assert-Equal -Actual $literal.If -Expected 'always()' -Message 'Step if parsing failed.'
Assert-Equal -Actual $literal.Env.STEP_VALUE -Expected 'one' -Message 'Step env parsing failed.'
Assert-Equal -Actual $literal.With.name -Expected 'artifact' -Message 'Step with parsing failed.'
# Joined rather than written as a here-string: a here-string takes the line
# endings of the file it sits in, so on a CRLF checkout it produced CRLF while
# the parser under test yields LF, and this gate could not be run locally at
# all. The parser's own output is the thing being asserted, so the expectation
# has to be built with the line ending the parser uses.
$expectedLiteral = @(
    "Write-Host '# literal comment'",
    'Write-Host "${{ github.sha }}"'
) -join "`n"
Assert-Equal -Actual $literal.Run -Expected $expectedLiteral `
    -Message 'Literal block scalar parsing failed.'
$folded = @(Get-WorkflowStep -Job $build -NameLike 'Folded') | Select-Object -First 1
Assert-Equal -Actual $folded.Run -Expected 'first second' `
    -Message 'Folded strip block scalar parsing failed.'
$keep = @(Get-WorkflowStep -Job $build -NameLike 'Keep') | Select-Object -First 1
Assert-Equal -Actual $keep.Run -Expected "one`ntwo`n" `
    -Message 'Literal keep block scalar parsing failed.'
Assert-Equal -Actual $folded.Uses -Expected 'actions/upload-artifact@v7' `
    -Message 'Step uses parsing failed.'
if (-not (Get-WorkflowRawText -Workflow $subset).Contains('# ignored comment')) {
    throw 'Get-WorkflowRawText did not preserve original comments.'
}

try {
    $null = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'rejected.yml')
    throw 'The workflow model accepted an anchor and alias.'
} catch {
    if ($_.Exception.Message -notmatch 'line 4' -or
        $_.Exception.Message -notmatch 'anchors, aliases, and tags') {
        throw "The rejected construct failed without its line-numbered reason: $($_.Exception.Message)"
    }
}

Import-PolicyScript -Name 'Test-ReleaseGatePolicy.ps1'
$positive = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'release-positive.yml')
$failures = @(Test-ReleaseGatePolicy -Workflow $positive)
if ($failures.Count -ne 0) {
    throw "Release policy rejected its positive fixture: $($failures -join '; ')"
}
$negative = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'release-negative.yml')
Assert-FailuresContain -Failures @(Test-ReleaseGatePolicy -Workflow $negative) `
    -Expected "quality job 'release-gallery'" -Label 'Release negative fixture'

Import-PolicyScript -Name 'Test-InstallerRollbackPolicy.ps1'
$positive = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'installer-positive.yml')
$failures = @(Test-InstallerRollbackPolicy -Workflow $positive -Repo $root)
if ($failures.Count -ne 0) {
    throw "Installer policy rejected its positive fixture: $($failures -join '; ')"
}
$negative = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'installer-negative.yml')
Assert-FailuresContain `
    -Failures @(Test-InstallerRollbackPolicy -Workflow $negative -Repo $root) `
    -Expected "guarded job 'windows-build'" -Label 'Installer negative fixture'

Import-PolicyScript -Name 'Test-ServicePayloadVersionPolicy.ps1'
$positive = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'service-positive.yml')
$failures = @(Test-ServicePayloadVersionPolicy -Workflow $positive -Repo $root)
if ($failures.Count -ne 0) {
    throw "Service payload policy rejected its positive fixture: $($failures -join '; ')"
}
$negative = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'service-negative.yml')
Assert-FailuresContain `
    -Failures @(Test-ServicePayloadVersionPolicy -Workflow $negative -Repo $root) `
    -Expected "guarded job 'windows-build'" -Label 'Service payload negative fixture'

Import-PolicyScript -Name 'Test-DistributionPolicy.ps1'
$positive = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'distribution-positive.yml')
$failures = @(Test-DistributionPolicy -Workflow $positive -Repo $root)
if ($failures.Count -ne 0) {
    throw "Distribution policy rejected its positive fixture: $($failures -join '; ')"
}
$negative = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'distribution-negative.yml')
Assert-FailuresContain `
    -Failures @(Test-DistributionPolicy -Workflow $negative -Repo $root) `
    -Expected "guarded job 'windows-build'" -Label 'Distribution negative fixture'

Import-PolicyScript -Name 'Test-IntegrationDeveloperBundle.ps1'
$positive = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'integration-positive.yml')
$failures = @(Test-IntegrationDeveloperBundlePolicy -Workflow $positive -Repo $root)
if ($failures.Count -ne 0) {
    throw "Integration bundle policy rejected its positive fixture: $($failures -join '; ')"
}
$negative = Read-GitHubWorkflow -Path (Join-Path $fixtureRoot 'integration-negative.yml')
Assert-FailuresContain `
    -Failures @(Test-IntegrationDeveloperBundlePolicy -Workflow $negative -Repo $root) `
    -Expected "job 'release-main-snapshot' is missing" -Label 'Integration bundle negative fixture'

Write-Host 'Workflow model and policy function contracts passed.'
