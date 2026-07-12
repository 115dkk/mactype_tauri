# Control Center CI and release

The Control Center uses three independent required checks so a failure identifies the broken contract.

1. `Build and package` has two independent Windows jobs. One builds FreeType, IniParser, Detours, wow64ext, and the current MacType `Rel+Detours` x86/x64 core. The other compiles the Win32 preview process, production frontend, Rust/Tauri executable, exercises the real WebView2 window states, and creates an Inno Setup installer.
2. `Frontend window gallery` opens every public view at 390, 768, and 1280 pixels. It also switches profile categories, reveals advanced LCD settings, adds a font-specific entry, opens include/exclude editing, and changes the preview background. JavaScript exceptions, console errors, renderer crashes, missing readiness markers, and horizontal overflow fail the job. Lighthouse uses the median of three runs so runner noise does not weaken or randomly fail the fixed thresholds. Screenshots and Playwright traces are retained for human review.
3. `Lint gates` runs frontend, Rust, and new C++ lint as blocking jobs and rejects generated settings sources that differ from `shared/settings-schema.json`. The legacy core boundary is documented in `docs/lint-policy.md`.

The profiles window smoke is stronger than a launch check: it points the app at an x86 fixture DLL with the public `IControlCenter` ABI, waits for Helper IPC plus WIC PNG output, force-terminates the child, and requires a second PNG after automatic restart. It then duplicates the fixture profile, changes a scalar, an `[Individual]` row, and an excluded module, atomically saves and reopens the copy, and only then writes the ready marker. Frontend smoke failures write their error into the marker so CI reports the actual failed contract instead of a generic timeout.

Velopack is not used. The installer is per-user and does not request elevation. It contains the open Control Center and x86 Preview Helper. It does not copy Delphi GUI programs, profiles, translations, MacType DLLs, services, or registry settings; the first trial uses an existing installation selected by the user.

Tags matching `control-center-v*` build a prerelease installer. The release job uses the same artifacts and checks as pull requests.
