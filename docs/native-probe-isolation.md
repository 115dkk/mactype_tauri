# Native probe isolation

Automated tests must not show windows on the developer's input desktop or take
keyboard focus. Tests that need a visible HWND must still exercise activation
and painting; hiding their window is not an equivalent test.

`tools/service-probe/tests/Test-ProbeContract.ps1` and the control-message and
settings-snapshot CTest entries use `scripts/ci/lib/ProbeDesktop.ps1` by default. The runner creates a unique
non-input Windows desktop without switching the user's desktop. It starts the
child suspended, assigns a kill-on-close job, and only then resumes the child.
Console processes use `CREATE_NO_WINDOW`; GUI windows can display and paint on
the test desktop. Injector integration console fixtures use `-NoNewWindow`.

For other native tests, use `scripts/ci/Invoke-ProbeDesktop.ps1` with an absolute
`-Executable`, an argument array, and a finite `-TimeoutSeconds`. The library's
`Invoke-ProbeDesktopScript` accepts an absolute `-ScriptPath` and a typed
`-Parameters` dictionary. Parameters are serialized as data rather than inserted
into executable script text. Already-isolated callers reuse their actual desktop;
an environment marker alone does not prove isolation.

Child exit codes remain unchanged. Output is bounded per stream and forwarded
after completion without preserving stdout/stderr interleaving. Timeouts and
output-limit failures terminate the whole job. The desktop remains owned until
all descendants exit. Isolation or cleanup failures are errors, never permission
to retry on the input desktop.

`Test-ProbeDesktopIsolation.ps1` verifies arguments, typed parameters, nonzero
exits, actual window activation/painting, no console window, desktop identity,
nested calls, descendant lifetime, timeout cleanup, output bounds and failed
process creation. Interactive mode is not part of the automatic contract.

Desktop separation does not isolate machine services or production injection.
Do not stop the developer's service or alter machine policy to obtain a passing
test. Preserve failure evidence and distinguish possible external interference.
Shell/tray, real-input and external-application tests need their own disposable
runner or explicit approval for an interactive local run. A private desktop does
not supply an Explorer shell or safely redirect `SendInput`. `-Interactive`, where
supported, is an explicit diagnostic opt-in, not an automatic fallback. Raw probe
executables may still create windows; automated development uses the guarded
scripts or runner instead.
