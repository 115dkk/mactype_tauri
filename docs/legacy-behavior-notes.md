# Legacy execution-mode findings

Phase 4 treats the public source and the installed binaries as evidence, not as a license to reproduce undocumented Delphi behavior.

## Manual mode

`MacLoader.exe` and `MacLoader64.exe` are built from the public `run.cpp`. The 32-bit loader inspects the target PE, delegates a 64-bit target to `MacLoader64.exe`, and calls Detours `DetourCreateProcessWithDllEx` with `MacType.dll` or `MacType64.dll`. The Control Center therefore exposes a manual launcher that starts `MacLoader.exe` directly with an existing `.exe` path and an argument array. It never constructs a shell command.

## Tray mode

The legacy `MacTray.exe` implementation is not in the public core and is not shipped or launched by the Control Center. Tauri owns the new notification icon, show/hide/quit menu, close-to-tray behavior, and optional per-user startup value at `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. This is the supported non-admin session mode.

## Legacy service

An official installation may register a service named `MacType` whose image is `MacTray.exe -service`. The Control Center queries that service with the Service Control Manager API so users can see conflicts, but it does not start, stop, install, or remove it. Doing so would retain a dependency on the abandoned private Delphi executable and would not constitute an open replacement.

## AppInit registry mode

The official project removed registry mode from its wizard because an incorrect configuration can prevent Windows from booting. Its manual guide modifies both 64-bit and 32-bit `AppInit_DLLs`, enables `LoadAppInit_DLLs`, weakens `RequireSignedAppInit_DLLs`, and changes the system `PATH`. The Control Center detects an existing MacType AppInit entry read-only and deliberately provides no apply action. No administrator broker is built while every proposed privileged operation is rejected by policy; an executable that merely wraps the legacy Delphi service or a boot-risk registry recipe would violate the architecture rather than complete it.

Primary references:

- `run.cpp` in this repository
- <https://github.com/snowie2000/mactype/wiki/Enable-registry-mode-manually>
- <https://github.com/snowie2000/mactype/wiki/HookChildProcesses>
