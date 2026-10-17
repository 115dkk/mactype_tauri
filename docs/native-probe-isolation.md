# Native probe isolation

Automated tests must not show windows on the developer's input desktop or take
keyboard focus. Hiding a window after launch does not satisfy this requirement.
Tests that need a visible HWND still need real activation and paint behavior;
replacing those assertions with hidden-window behavior is not equivalent.

## Default execution

`scripts/ci/lib/ProbeDesktop.ps1` launches a process tree on a unique non-input
Windows desktop. It never switches the input desktop. The child starts suspended,
joins a kill-on-close job, then resumes with its desktop explicitly selected.
Console processes use `CREATE_NO_WINDOW`. GUI windows can remain visible and
paint normally on the test desktop without appearing on the user's screen.

The default guards apply to:

- `tools/service-probe/tests/Test-ProbeContract.ps1`
- `scripts/ci/Test-RendererAsan.ps1`
- `scripts/ci/Test-RendererProfileContract.ps1`
- The `renderer-unit` CTest entries in `tools/service-probe/CMakeLists.txt`

Both normal and ASan builds use the same explicit CMake unit-test list. Field
probes that need an adapter/evidence/input file are not registered as argumentless
unit tests. The injector integration test's console fixtures use `-NoNewWindow`.

For another native test command, use `scripts/ci/Invoke-ProbeDesktop.ps1` with an
absolute `-Executable`, an argument array, and a finite `-TimeoutSeconds`. For a
PowerShell test script, import the library and call `Invoke-ProbeDesktopScript`
with an absolute `-ScriptPath` and a `-Parameters` dictionary. Typed values are
serialized as data, not interpolated into executable PowerShell source.

Already-isolated callers reuse their actual desktop. An environment variable
alone never proves isolation. Failure to create the desktop or assign the job
fails the test; there is no fallback to the input desktop.

## Results and cleanup

The runner returns the child's exit code unchanged. It bounds stdout and stderr
separately and forwards their bytes after completion; their interleaving is not
preserved. Timeouts and output-limit failures terminate the entire test job.
The desktop remains owned until all descendants have exited, including children
that outlive their parent. Unconfirmed cleanup is an error, not a passing test.

`Test-ProbeDesktopIsolation.ps1` verifies argument and parameter round-trips,
nonzero exits, real window activation/painting, desktop identity, no console
window, nested execution, descendant lifetime, timeout cleanup, bounded output,
and failed process creation. It does not exercise interactive mode.

## Boundaries

This is desktop isolation, not a VM or an injection sandbox. A production service
can still discover these processes. Do not stop the developer's service, change
machine policy, or suppress a failure to make a test pass. Report possible
interference separately and retain the original diagnostics.

Shell/tray integration, real pointer input, and external application field tests
have additional requirements. Do not assume that a private desktop provides an
Explorer shell or safely redirects `SendInput`. Keep those tests on a disposable
runner, or obtain explicit approval before an interactive local run. `-Interactive`
is an explicit diagnostic opt-in where supported, never an automatic retry.
Raw probe executables can still create windows; use the guarded entrypoints or
the runner rather than launching them directly during automated development.
