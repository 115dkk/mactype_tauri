# Control Center architecture

The x64 Tauri process never loads `MacType.dll`. It owns profile files, validation, the child-process lifecycle, and the WebView. `mactype-preview32.exe` is the only process allowed to load the selected installation's x86 DLL.

## Machine integration terminology

`MachineIntegration` is the deep module behind the frontend system-integration seam. Its frontend adapter produces `ExecutionViewModel`; SCM flags, protected storage, migration receipts, helpers, and registry inspection remain implementation details. **신식 서비스** means the open-source `MacTypeControlCenter` runtime. **레거시 서비스** means the original `MacType` service hosted by `MacTray.exe`; it is a migration subject and fallback only.

## Preview boundary

The parent starts the Helper directly with `std::process::Command`, redirected stdin/stdout/stderr, and no shell. MTPC v1 frames have fixed little-endian headers and bounded JSON/PNG lengths. A reader thread consumes responses while stderr is retained in a 100-line diagnostic buffer. Requests time out after two seconds; the parent terminates and restarts the Helper once before returning an error.

The Helper validates an x86 PE image, `MacType.ini`, and `CreateControlCenter`. `EasyHK32.dll` is optional because the independent `Rel+Detours` package has no external EasyHook runtime dependency. Current public installers do not all export `DllGetVersion`, so that export is reported as an optional capability and `IControlCenter::GetVersion` is the authoritative core version. All `IControlCenter` mutation and GDI rendering remains in the x86 process.

Preview pixels are rendered into a top-down 32-bit DIB and encoded through WIC. PNG bytes cross only the binary frame section. Tauri writes them under app-local data and the WebView reads the narrowly scoped asset URL; no base64 image is retained in application state.

The plain preview engine never loads MacType or reads its DLL, INI, profile, or setting overrides,
and reports `coreVersion` 0. The parent keeps a second lazy Helper slot for this engine while
installation checks and reconnect remain bound to the MacType slot. Preview Studio uses the
`preview-studio` window label and loads `index.html?window=preview-studio`; closing the main window
destroys it. `write_preview_export` accepts a strictly padded standard-base64 PNG and writes it
atomically to a validated absolute `.png` path.

## Profile boundary

Rust owns a line-preserving INI document. It retains BOM, encoding, line endings, blank lines, comments, unknown entries, and ordering. Only the value slice of a changed key is rewritten. Save compares the original SHA-256, flushes a same-directory temporary file, keeps one backup, and uses `ReplaceFileW` on Windows.

The editor keeps the selected source profile as the editing identity. A writable profile in the installation `ini` directory or the user-owned `%LOCALAPPDATA%\MacType\ControlCenter\profiles` directory is saved back to that same file. Installed read-only profiles and external files require an explicit Import or Save As operation; both create a user-owned copy without elevation. A profile selected by an existing installation's `MacType.ini` is opened directly when it already belongs to a known profile directory, so the UI does not ask the user to import a profile it can already edit.

User-facing and persisted source identities are portable (`ini\Default.ini` or `Profiles\Foo.ini`) for files in those profile directories. Absolute paths are retained only for actual file I/O and reveal-in-Explorer actions. Applying is allowed only from a saved document and publishes the exact saved bytes into the service's protected generation; the generated `profile.ini`, digest, and runtime path remain internal implementation details. Imports are strictly decoded as INI documents, copied byte-for-byte, and receive a collision-safe name.

When no profile has been applied, only a `start` or `publish-profile` action may
select the validated bundled `ini\Default.ini`; every other profile remains an
explicit user choice. The frontend announces that default before the action
and names it after success. A stopped service has no live generation, so its
summary does not invent a profile mismatch or repeat stale live-health text;
only a persisted degraded or failed record remains diagnostically relevant.

Scalar settings use the public core's `[General]` keys. Structured `[Individual]`, font include/exclude, and module include/exclude sections retain their surrounding comments while edited entries are validated and replaced. The legacy-codec gate vendors a pinned, licensed 70-profile community corpus and requires correct encoding detection, byte-identical no-edit round trips, edit/save/reopen behavior, and line-ending/BOM preservation without network access.

`shared/settings-schema.json` is the source for generated Rust, TypeScript, and C++ setting definitions. CI regenerates and rejects drift.

## Localization boundary

The React frontend owns ten complete runtime catalogs: Korean, English, Simplified Chinese, Traditional Chinese, Japanese, French, German, Spanish, Portuguese, and Arabic. An explicit `?lang=` value takes precedence and is persisted per user; otherwise the stored preference or the browser language selects the initial locale. Chinese script and regional subtags are normalized separately (`zh-Hant`, Taiwan, Hong Kong, and Macao select Traditional Chinese), while unsupported locales fall back to English.

Changing the language updates visible text, the document title, accessibility labels, the HTML language and direction, and the native Tauri tray menu without restarting. Arabic sets native right-to-left document direction and direction-aware navigation and editor borders. CI requires exact catalog key and placeholder parity, coverage for all generated settings, native tray-menu tests, and real browser rendering of every view, viewport, and locale.

## Skin boundary

The React tree renders one DOM for every skin. `skinPreference.ts` resolves the skin like the locale (`?skin=` wins and is persisted, then the stored choice, then `classic`) and applies it as `html[data-skin]`; `themePreference.ts` accepts `?theme=` the same way. Each skin lives in `src/styles/skins/<id>.css`, scoped to its `data-skin` value, and overrides the token custom properties and the shared class names only. The `PreferenceMenu` component backs both the language and the skin pickers so a skin restyles one popup. The status bar element exists in every skin's DOM and is shown only by skins that draw one. The frontend gallery renders every skin in both themes at every viewport and asserts no horizontal overflow, that the pickers stay reachable, and that a chosen skin survives a reload.

## Execution boundary

The open Tauri executable owns the notification icon and close-to-tray lifecycle. Its optional login startup entry is user-scoped. The GUI remains `asInvoker`; machine mutation is isolated in a one-shot fixed-verb broker dispatched before Tauri starts. AppInit registry mode remains read-only in normal UI flows. Manual mode invokes the public `MacLoader.exe` directly with an executable path and argument vector; no shell string is accepted.

The 신식 서비스 is Tauri-free. Rust owns protected runtime/profile generations, SCM state, Ready health, WMI process observation, retry and deduplication policy, repair, and rollback. The Windows inspector collects typed per-process facts; `ProcessTargetValidator` alone classifies them and returns only a verified eligible identity or an explicit process-local skip. Unknown mitigation evidence never becomes an image-wide ban, and a known CIG/ACG refusal does not launch the helper or change global health. `InjectionOrchestrator` binds an eligible identity to one generation and owns deduplication, retry, cancellation, bounded result history, telemetry, and terminal health classification. Service stop cancels an in-flight helper through its private Job Object instead of waiting for the normal 20-second bound. A fixed x86/x64 helper receives one inherited process handle and loads only its adjacent public MacType DLL. Normal target skips, pre-injection rejection, and confirmed service-stop cancellation preserve global Ready. Unknown post-injection cleanup or an invalid helper response degrades the generation until a later verified success; observer or protected-runtime failures may fail it.

The 레거시 서비스 is detected through a strict official-layout adapter. Its configuration and profile can be backed up and restored only by the explicit migration flow. Normal startup, login startup, tray actions, profile application, and 신식 서비스 repair never execute or require MacTray. The normative safety contract is `docs/open-service-contract.md`.

The official single-instance plugin is registered before every other Tauri plugin. On Windows, a pre-Tauri per-session startup mutex serializes cold starts until the first process has created its IPC window and completed setup, closing the plugin's mutex-to-window race. Later launches send their arguments to the existing process, which shows, unminimizes, and focuses the main window. The privileged service broker exits before this gate and therefore never participates in GUI instance arbitration.

## Maintenance notes

Cross-module contracts belong in this architecture document, `docs/control-center-ci.md`, or `docs/legacy-behavior-notes.md` rather than being repeated beside each implementation. Source comments follow `docs/source-comment-policy.md`: local invariants and platform or compatibility traps remain beside the code, while routine control flow and temporary implementation history remain uncommented.
