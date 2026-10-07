# Security Policy

This repository is a personal project maintained by
[@115dkk](https://github.com/115dkk), who is responsible for its security and
handles every vulnerability report for it.

## Scope

- The renderer DLLs (`MacType.dll`, `MacType64.dll`). They load into other
  applications' processes, so a flaw in them is a flaw in every program they
  render.
- The x86 and x64 `mactype-injector` executables that load the renderer.
- The MacType Control Center service (`MacTypeControlCenter`), which runs as
  LocalSystem, and the setup broker.
- The Control Center app, the installer, and the workflows in this repository
  that build the release files.

The original MacType distribution, including its `MacTray.exe` service,
belongs to the upstream MacType project. Report flaws that exist only there to
that project.

## Supported versions

- The newest `MacType Control Center CI` pre-release, built from `main`.
- The newest `alpha-MacType Control Center CI` pre-release, built from
  `codex/alpha-plus-dll`. That line is experimental and gets fixes on a
  best-effort basis.

Older builds are not patched. Update to the newest build first and check
whether the problem is still there.

## Reporting a vulnerability

Report it privately through
[GitHub's private vulnerability reporting](https://github.com/115dkk/mactype_tauri/security/advisories/new).
Do not open a public issue, discussion, or pull request for a suspected
vulnerability.

A useful report names the build you tested, the Windows version, the affected
component, and the steps that reproduce the problem.

## What happens next

This project is maintained in spare time, so there is no guaranteed response
time. The maintainer reads every report and answers in the report's private
thread. A confirmed vulnerability is fixed in a new build, and the maintainer
then publishes a GitHub security advisory that credits the reporter unless the
reporter asks not to be named.

There is no bug bounty.
