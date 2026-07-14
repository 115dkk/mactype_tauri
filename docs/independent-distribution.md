# Independent distribution

The Phase 5 installer does not require an existing MacType installation. CI builds the rendering core and manual loaders from pinned public source revisions, builds the open Tauri Control Center and Win32 Preview Helper, and packages them with a newly authored default profile.

## Installed manifest

- `MacType Control Center.exe`
- `mactype-preview32.exe`
- source-built `MacType.dll`, `MacType64.dll`, `MacType.Core.dll`, and `MacType64.Core.dll`
- source-built `MacLoader.exe` and `MacLoader64.exe`
- `MacType.ini` and `ini\Default.ini` authored in this repository
- English and Korean distribution translation catalogs
- GPL and third-party notices

The `Rel+Detours` core DLL is copied under the public runtime names `MacType.dll` and `MacType64.dll`. This build has no external EasyHook runtime dependency, so `EasyHK32.dll` and `EasyHK64.dll` are not redistributed. Delphi GUI files, legacy updater files, existing profiles, and existing language resources are forbidden by CI.

## Installation lifecycle gate

CI builds a `0.0.9` baseline installer and the current `0.1.0` installer with the same stable AppId. It silently installs the baseline into an isolated user directory, verifies the manifest and forbidden-file list, runs all four Tauri windows against the installed source-built core, verifies hidden tray startup, and launches an injected x86 marker target through the installed MacLoader. It then upgrades to `0.1.0`, runs the uninstaller, and fails if files remain.

The installer and normal Control Center are per-user and request no elevation. The installer does not install a service and never changes AppInit or IFEO registry keys. When an already installed official `MacTray.exe` is verified, a user may explicitly invoke a short-lived UAC broker to add, remove, start, or stop that legacy service; the broker is not a general-purpose elevated copy of the GUI and the independent package does not ship `MacTray.exe`. User-created and imported profiles under `%LOCALAPPDATA%\MacType\ControlCenter\profiles` are intentionally retained by uninstall.
