# Skin and preview validation

This maps the Superloopy frontend contract to the existing Windows application.
Reference: `beefiker/superloopy` commit
`85ede9c958d9c11498619a8c53db66098e52158d`,
`skills/superloopy-frontend/SKILL.md`, and its `ux`, `web`, `desktop`, `hybrid`,
`layout`, and `motion-core` references. It adds no runtime dependency or competing design
system. `DESIGN.md`, `docs/skin-designs.md`, and their CSS tokens own visuals.

## Target and ownership

The production target is the Windows Tauri 2 shell with the installed
WebView2 provider. React owns DOM rendering, focus, controls, and the preview
batch; Tauri owns windows and IPC; the x86 preview helper owns GDI/MacType
bitmaps. Browser-gallery SVGs simulate that last owner. Browser screenshots
cannot establish Windows GDI font metrics, WebView2 lifetime, or installed
renderer compatibility. SEO is not applicable to this embedded desktop UI.

## Invariants and evidence

| Invariant | Owner | Acceptance and evidence |
| --- | --- | --- |
| A switch thumb remains inside its track when on/off, hovered, disabled, or RTL | shared CSS primitive and skin tokens | `skin-preview.spec.ts`: all four skins, LTR/RTL, state bounds, keyboard Space and focus |
| Native selection inputs retain square metrics and ranges retain keyboard editing | shared CSS and browser controls | gallery control captures and Tuner keyboard regression |
| Console's large text fits the bitmap without CSS scaling; every size stays reachable | `SpecimenBoard`, `wrapSample`, `specimenStripHeight` | 880x560 at 192 DPI, six strips, text bounds and scrolling; desktop/mobile gallery |
| Enabled localized font substitutions select the replacement family; disabled mappings do not | profile read command and `previewFonts` | Korean alias-to-Pretendard gallery fixture, switch back to Segoe UI, disabled-mode case; Rust dirty-document isolation test |
| Canvas gaps and raster backgrounds match before, during, and after inversion | decoded preview batch and its palette | animation-frame palette audit with delayed helper simulation, skin theme changes, Console and Studio inversion |
| Opening Studio never blocks the Windows UI thread, reuses a live window, and releases a closed window | asynchronous Tauri lifecycle owner | `Test-TauriWindows.ps1`: actual secondary-window render, hide/restore, destroy/recreate, second render and clean process exit |
| An open failure is visible and supports retry/dismissal | shared app shell | gallery failed-IPC fixture; no unhandled promise rejection |

## Required workflow

Before modifying a skin, reproduce the affected journey and find its existing
model and CSS authority. Record the delta, adjacent regression, exact base
commit, actual browser/OS, DPI/window bounds, and native capabilities available
for validation in a run receipt under `.superloopy/evidence/frontend/`.

Run from `control-center`:

```sh
pnpm build
pnpm lint
pnpm test:i18n
pnpm test:settings
pnpm test:gallery
```

The gallery includes all skin/theme/view combinations and the focused
`skin-preview.spec.ts` regressions. Inspect affected captures, not only exit
status, and record findings in `VISUAL_QA.md`. The existing Windows build job
runs `scripts/ci/Test-TauriWindows.ps1` against the packaged executable/helper;
its Studio case must report readiness from rendered secondary-window content,
not from the main view or successful invocation alone.

For new capabilities maintain per-skin reachability, error/retry, editable
state, keyboard/focus, and return-path evidence. Add IME, assistive technology,
distribution, motion, or performance proof only when the changed claim needs
it. Keep simulation and unavailable native evidence explicit. Deliver through
the exact-commit CI rules in `AGENTS.md`.
