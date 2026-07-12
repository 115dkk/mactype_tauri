# Control Center CI and release

The Control Center uses three independent required checks so a failure identifies the broken contract.

1. `Build and package` has two independent Windows jobs. One builds the official FreeType and IniParser forks followed by the current MacType `Rel+Detours` x86/x64 core. The other compiles the Win32 preview process, production frontend, Rust/Tauri executable, exercises the real WebView2 window states, and creates an Inno Setup installer.
2. `Frontend window gallery` opens every public view at 390, 768, and 1280 pixels. JavaScript exceptions, console errors, renderer crashes, missing readiness markers, and horizontal overflow fail the job. Screenshots and Playwright traces are retained for human review from the pull request check.
3. `Lint gates` runs frontend, Rust, and new C++ lint as blocking jobs. The legacy core boundary is documented in `docs/lint-policy.md`.

Velopack is not used. The initial installer is per-user and does not request elevation. It contains only the open Control Center and placeholder Preview Helper. It does not copy Delphi GUI programs, profiles, translations, MacType DLLs, services, or registry settings.

Tags matching `control-center-v*` build a prerelease installer. The release job uses the same artifacts and checks as pull requests.
