# How to build MacType Control Center

## Prerequisites

- Windows 10 or 11 x64
- Visual Studio 2022 with MSVC x86/x64 desktop tools, CMake, and the Windows SDK
- stable Rust with `rustfmt` and `clippy`
- Node.js 24 and pnpm 11.7.0
- Inno Setup 6 when producing the installer

Velopack is intentionally not used.

## Manual build

From an x64 Visual Studio developer PowerShell:

```powershell
pnpm --dir control-center install --frozen-lockfile
.github/scripts/Build-OpenCore.ps1
.github/scripts/Build-ControlCenter.ps1 -Configuration Release -SkipInstall
.github/scripts/Build-ServiceRuntime.ps1 `
  -CoreRoot artifacts/open-core `
  -OutputRoot artifacts/service-runtime `
  -Version 0.2.0
```

`Build-ControlCenter.ps1` builds the frontend, Preview Helper, and Tauri executable. The stable frontend filename is `artifacts\application\MacType Control Center.exe`. `Build-ServiceRuntime.ps1` builds the Rust host/setup and stages the fixed public x86/x64 helper and DLL payload; it does not install or start the 신식 서비스.

Before packaging, run:

```powershell
scripts/ci/Test-DistributionPolicy.ps1
scripts/ci/Test-OpenServiceContract.ps1
scripts/ci/Test-NewCppStyle.ps1
cargo test --manifest-path service-runtime/Cargo.toml --workspace --all-targets --all-features
```

The exact Inno Setup command and artifact layout are maintained in `.github/workflows/build.yml`. Use that workflow as the packaging reference instead of copying a versioned command into multiple documents.

## CI build (“one click”)

1. Open GitHub Actions.
2. Select **Build and package**.
3. Choose **Run workflow** (`workflow_dispatch`) on the desired branch.
4. Download `mactype-control-center-windows` after every job is Green.

The artifact contains the stable-name installer, checksum, Control Center executable, and staged service payload. Normal pull requests run the same build automatically; the manual dispatch exists so a maintainer can build without preparing a local Tauri/Rust/MSVC environment.

Machine-mutating reboot, AppInit, migration, and multi-session checks are deliberately separate. Use **Open service disposable VM verification** only on a disposable self-hosted runner and follow `docs/service-maintenance.md`. Never run that workflow on a maintainer workstation.
