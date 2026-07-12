# Control Center architecture

The x64 Tauri process never loads `MacType.dll`. It owns profile files, validation, the child-process lifecycle, and the WebView. `mactype-preview32.exe` is the only process allowed to load the selected installation's x86 DLL.

## Preview boundary

The parent starts the Helper directly with `std::process::Command`, redirected stdin/stdout/stderr, and no shell. MTPC v1 frames have fixed little-endian headers and bounded JSON/PNG lengths. A reader thread consumes responses while stderr is retained in a 100-line diagnostic buffer. Requests time out after two seconds; the parent terminates and restarts the Helper once before returning an error.

The Helper validates an x86 PE image, `EasyHK32.dll`, `MacType.ini`, and `CreateControlCenter`. Current public installers do not all export `DllGetVersion`, so that export is reported as an optional capability and `IControlCenter::GetVersion` is the authoritative core version. All `IControlCenter` mutation and GDI rendering remains in the x86 process.

Preview pixels are rendered into a top-down 32-bit DIB and encoded through WIC. PNG bytes cross only the binary frame section. Tauri writes them under app-local data and the WebView reads the narrowly scoped asset URL; no base64 image is retained in application state.

## Profile boundary

Rust owns a line-preserving INI document. It retains BOM, encoding, line endings, blank lines, comments, unknown entries, and ordering. Only the value slice of a changed key is rewritten. Save compares the original SHA-256, flushes a same-directory temporary file, keeps one backup, and uses `ReplaceFileW` on Windows.

The editor reads installed profiles but creates user-owned copies under `%LOCALAPPDATA%\MacType\ControlCenter\profiles`; it never needs elevation to duplicate a profile from `Program Files`. Scalar settings use the public core's `[General]` keys. Structured `[Individual]`, font include/exclude, and module include/exclude sections retain their surrounding comments while edited entries are validated and replaced.

`shared/settings-schema.json` is the source for generated Rust, TypeScript, and C++ setting definitions. CI regenerates and rejects drift.

## Execution boundary

The open Tauri executable owns the notification icon and close-to-tray lifecycle. Its optional login startup entry is user-scoped. Manual mode invokes the public `MacLoader.exe` directly with an executable path and argument vector; no shell string is accepted. Legacy `MacTray.exe -service` and AppInit registry mode are detected read-only and never controlled. The evidence and safety decision are recorded in `docs/legacy-behavior-notes.md`.
