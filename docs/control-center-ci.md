# Manual Control Center build

The upstream-oriented workflow is build automation, not a merge gate. It does not run for pull requests, pushes, tags, or a schedule, and it does not create a GitHub Release.

## Run the build

1. Open **Actions** in the repository.
2. Select **Build MacType Control Center**.
3. Choose **Run workflow**.
4. Select the branch or commit to build.
5. Enter the installer version, such as `0.1.0` or `0.1.0-preview.1`.
6. Start the workflow and download its artifact when both jobs finish.

The only workflow file is `.github/workflows/build.yml`, and its only trigger is `workflow_dispatch`. This keeps ordinary upstream merges independent of the project's previous CI matrix while still giving maintainers a reproducible Windows build.

## What it builds

The workflow uses a GitHub-hosted Windows runner to:

- build the x86 and x64 MacType legacy core from source;
- build and test the Win32 Preview Helper;
- install the locked frontend dependencies and produce the production frontend;
- compile the Rust/Tauri Control Center executable;
- create the per-user Inno Setup installer; and
- write a SHA-256 checksum for the installer.

The downloadable artifact is named `mactype-control-center-<version>`. It contains the installer, `SHA256SUMS.txt`, the Control Center executable, and the Preview Helper. GitHub retains the artifact for 14 days.

Velopack is not used. The installer continues to use the existing MacType icon and the repository's Inno Setup definition.

## Scope and local checks

This workflow deliberately does not publish a release and is not a required pull-request check. A maintainer can download and test the artifact before deciding whether to merge or publish it.

Frontend-only changes can be checked locally without compiling Rust:

```powershell
cd control-center
pnpm install --frozen-lockfile
pnpm test:i18n
pnpm test:settings
pnpm lint
pnpm build
pnpm test:gallery
```

The repository's build and test scripts remain available for deeper local or downstream validation; they are no longer attached to upstream pull requests automatically.
