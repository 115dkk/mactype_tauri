# Control Center architecture

The x64 Tauri process never loads `MacType.dll`. It owns profile files, validation, the child-process lifecycle, and the WebView. `mactype-preview32.exe` is the only process allowed to load the selected installation's x86 DLL.

## Preview boundary

The parent starts the Helper directly with `std::process::Command`, redirected stdin/stdout/stderr, and no shell. MTPC v1 frames have fixed little-endian headers and bounded JSON/PNG lengths. A reader thread consumes responses while stderr is retained in a 100-line diagnostic buffer. Requests time out after two seconds; the parent terminates and restarts the Helper once before returning an error.

The Helper validates an x86 PE image, `MacType.ini`, and `CreateControlCenter`. `EasyHK32.dll` is optional because the independent `Rel+Detours` package has no external EasyHook runtime dependency. Current public installers do not all export `DllGetVersion`, so that export is reported as an optional capability and `IControlCenter::GetVersion` is the authoritative core version. All `IControlCenter` mutation and GDI rendering remains in the x86 process.

Preview pixels are rendered into a top-down 32-bit DIB and encoded through WIC. PNG bytes cross only the binary frame section. Tauri writes them under app-local data and the WebView reads the narrowly scoped asset URL; no base64 image is retained in application state.

## Profile boundary

Rust owns a line-preserving INI document. It retains BOM, encoding, line endings, blank lines, comments, unknown entries, and ordering. Only the value slice of a changed key is rewritten. Save compares the original SHA-256, flushes a same-directory temporary file, keeps one backup, and uses `ReplaceFileW` on Windows.

The editor reads installed profiles but creates user-owned copies under `%LOCALAPPDATA%\MacType\ControlCenter\profiles`; it never needs elevation to duplicate a profile from `Program Files`. File selection, native `.ini` import, duplication, save, and apply live in a dedicated settings-file view instead of being mixed into the setting editor. The view discovers the profile selected by an existing installation's `MacType.ini`, explains the source, and imports it only after explicit user consent. Imports are strictly decoded as INI documents, copied byte-for-byte, and receive a collision-safe name.

Scalar settings use the public core's `[General]` keys. Structured `[Individual]`, font include/exclude, and module include/exclude sections retain their surrounding comments while edited entries are validated and replaced. The legacy-codec gate vendors a pinned, licensed 70-profile community corpus and requires correct encoding detection, byte-identical no-edit round trips, edit/save/reopen behavior, and line-ending/BOM preservation without network access.

`shared/settings-schema.json` is the source for generated Rust, TypeScript, and C++ setting definitions. CI regenerates and rejects drift.

## Localization boundary

The React frontend owns ten complete runtime catalogs: Korean, English, Simplified Chinese, Traditional Chinese, Japanese, French, German, Spanish, Portuguese, and Arabic. An explicit `?lang=` value takes precedence and is persisted per user; otherwise the stored preference or the browser language selects the initial locale. Chinese script and regional subtags are normalized separately (`zh-Hant`, Taiwan, Hong Kong, and Macao select Traditional Chinese), while unsupported locales fall back to English.

Changing the language updates visible text, the document title, accessibility labels, the HTML language and direction, and the native Tauri tray menu without restarting. Arabic sets native right-to-left document direction and direction-aware navigation and editor borders. CI requires exact catalog key and placeholder parity, coverage for all generated settings, native tray-menu tests, and real browser rendering of every view, viewport, and locale.

## Execution boundary

The open Tauri executable owns the notification icon and close-to-tray lifecycle. Its optional login startup entry is user-scoped. The GUI remains `asInvoker`; privileged legacy-service operations are isolated in a one-shot verb allowlisted broker dispatched before Tauri starts. AppInit registry mode remains read-only. Manual mode invokes the public `MacLoader.exe` directly with an executable path and argument vector; no shell string is accepted. The evidence and safety decisions are recorded in `docs/legacy-behavior-notes.md`.

The official single-instance plugin is registered before every other Tauri plugin. On Windows, a pre-Tauri per-session startup mutex serializes cold starts until the first process has created its IPC window and completed setup, closing the plugin's mutex-to-window race. Later launches send their arguments to the existing process, which shows, unminimizes, and focuses the main window. The privileged service broker exits before this gate and therefore never participates in GUI instance arbitration.

## Maintenance notes

Cross-module contracts belong in this architecture document, `docs/control-center-ci.md`, or `docs/legacy-behavior-notes.md` rather than being repeated beside each implementation. Source comments are reserved for local invariants and platform or compatibility traps that are easy to violate while editing. Generated files retain only their generated-file warning; routine control flow and temporary implementation history should remain uncommented.
