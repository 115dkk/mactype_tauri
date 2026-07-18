[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$policy = Join-Path $root 'scripts\ci\Test-GitHubActionsRuntimePolicy.ps1'
$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) "mactype-actions-policy-$([guid]::NewGuid().ToString('N'))"

function Set-WorkflowFixture([string] $Uses) {
    $workflow = @"
name: policy fixture
on: workflow_dispatch
jobs:
  verify:
    runs-on: ubuntu-latest
    steps:
      - uses: $Uses
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureRoot 'fixture.yml'),
        $workflow,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-PolicyRejects([string] $Uses, [string] $ExpectedMessage) {
    Set-WorkflowFixture -Uses $Uses
    $messages = @()
    try {
        $messages = @(& $policy -WorkflowRoot $fixtureRoot *>&1)
    } catch {
        $messages += $_
        $combined = ($messages | Out-String) + $_.Exception.Message
        if ($combined -match [regex]::Escape($ExpectedMessage)) {
            return
        }
        throw "Policy rejected '$Uses' for the wrong reason: $combined"
    }
    throw "Policy accepted forbidden remote action '$Uses'."
}

function Set-CodeqlWorkflowFixture {
    $workflow = @"
name: CodeQL
on: workflow_dispatch
jobs:
  analyze:
    runs-on: windows-latest
    steps:
      - shell: cmd
        run: |
          call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" x64
          msbuild gdipp.sln
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureRoot 'codeql.yml'),
        $workflow,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-PolicyRejectsHardcodedCodeqlToolchain {
    Set-CodeqlWorkflowFixture
    $messages = @()
    try {
        $messages = @(& $policy -WorkflowRoot $fixtureRoot *>&1)
    } catch {
        $messages += $_
        $combined = ($messages | Out-String) + $_.Exception.Message
        if ($combined -match 'must discover MSBuild through microsoft/setup-msbuild@v3') {
            return
        }
        throw "Policy rejected the CodeQL fixture for the wrong reason: $combined"
    }
    throw 'Policy accepted a hardcoded CodeQL Visual Studio toolchain path.'
}

function Set-CodeqlUnsupportedRunnerFixture {
    $workflow = @"
name: CodeQL
on: workflow_dispatch
jobs:
  analyze:
    strategy:
      matrix:
        include:
          - language: c-cpp
            os: windows-latest
            build-mode: manual
    runs-on: `${{ matrix.os }}
    steps:
      - uses: microsoft/setup-msbuild@v3
"@
    [System.IO.File]::WriteAllText(
        (Join-Path $fixtureRoot 'codeql.yml'),
        $workflow,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-PolicyRejectsCodeqlLatestRunner {
    Set-CodeqlUnsupportedRunnerFixture
    $messages = @()
    try {
        $messages = @(& $policy -WorkflowRoot $fixtureRoot *>&1)
    } catch {
        $messages += $_
        $combined = ($messages | Out-String) + $_.Exception.Message
        if ($combined -match 'CodeQL C/C\+\+ must run on windows-2022') {
            return
        }
        throw "Policy rejected the CodeQL runner fixture for the wrong reason: $combined"
    }
    throw 'Policy accepted windows-latest for the ATL-dependent CodeQL C++ build.'
}

try {
    New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
    Assert-PolicyRejects -Uses 'example/old-node-action@v1' -ExpectedMessage 'closed allowlist'
    Assert-PolicyRejects -Uses 'actions/checkout@v6' -ExpectedMessage 'audited Node.js 24 release'
    Set-WorkflowFixture -Uses 'dtolnay/rust-toolchain@stable'
    Assert-PolicyRejectsHardcodedCodeqlToolchain
    Assert-PolicyRejectsCodeqlLatestRunner
    Remove-Item -LiteralPath (Join-Path $fixtureRoot 'codeql.yml') -Force

    & $policy -WorkflowRoot $fixtureRoot
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) {
        Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
    }
}

Write-Host 'GitHub Actions runtime policy contract passed.'
