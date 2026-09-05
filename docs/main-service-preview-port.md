# Main extraction: service coverage, events, preview, and product copy

Base: `d949dace4a946f3a254dbbcc20e3d8c3da43e031` (`main`).
Source reviewed: `2c885ac1a2c3c603831fab2d73f960e01df1d7d9`
(`codex/alpha-plus-dll`).
Delivery branch: `codex/main-service-preview`, targeting fork `main`.

This extraction preserves main's existing renderer, settings schema, installation
boundaries, health-v1 and fixed-helper wire formats. It does not merge alpha's
history or introduce its renderer activation protocol, Unity/private-FreeType
policies, child relay, skins, or release/CI branch policy.

## Selected changes

- The ten existing locale catalogs use the reviewed user-facing copy from
  `f9922ce` and `29b763a`. Only main's existing setting keys and the extracted
  preview/event controls are included. Korean font coverage accompanies the
  strings. Internal view identifiers and service SCM identities stay unchanged.
- `15ad956`: protected runtime and profile trees give Application Packages and
  Restricted Application Packages read/execute access. The verifier requires
  these exact trustees and rights; write access remains administrator-only.
- `6d69bda` and `3529efc`: frozen processes wait in a bounded 512-identity queue.
  The host rechecks them every two seconds and verifies PID plus creation time
  before injection. A helper launch that failed before resume backs off from
  two to 32 seconds for at most five deferrals. A process that exits is skipped.
  Failure after resume never enters that retry path. Existing protected,
  critical, session-zero, installer and important-system-process exclusions
  remain in force; process mitigation queries can additionally exclude a target.
- Process observation uses `Win32_ProcessStartTrace`; access-denied or missing
  class falls back to the existing intrinsic creation query. Other WMI errors
  remain explicit failures. Main already had the bounded module-inventory retry,
  so that implementation is retained.
- `50d0f8d` and `f1f789d`: a common bounded, rotating event record covers service,
  setup, profile, helper and Control Center activity. Repeated failures are
  throttled; normal skips and frozen deferrals do not become global service
  failures. Old local activity logs remain readable. Diagnostics show localized
  event summaries, filters, sources, and expandable technical details.
- `b094521`, `1a41559`, `433f4f7`, `2fb926c`, `6bc3cf7` and their focused fixes:
  the standalone Preview Studio compares edited/saved/file profiles and plain
  Windows GDI, with size/style samples, integer zoom, loupe, difference/blink
  comparison, and PNG export. The native helper window gains its toolbar,
  sample/ladder/compare/listing modes, editing, topmost, DPI handling, and
  non-blocking PNG dialog. Native self-hide events update the main window.
  Only main's light/dark appearance is accepted; other skin implementations
  and their selectors are absent.
- `eaf7bd6` / `0771b4e`: a successful preview redraw cannot clear a failed
  document mutation or remove the requirement to recover before saving/applying.

## Main boundaries

Rust `host`, `setup`, and Control Center still forbid unsafe code. Added Win32
queries are confined to `mactype-service-platform`, with local safety proofs.
The fixed injector still accepts only the inherited target identity and its
adjacent fixed DLL. It does not accept a caller-selected DLL or executable.
A same-named DLL from a different directory remains a no-injection result.

SCM Running alone is not successful integration. The existing Ready/profile
and architecture evidence rules remain unchanged. This port improves delivery
of the existing renderer; it does not claim new rendering-engine support.

Product changes and fork CI/test documentation must be committed separately
before any later upstream-preparation step. After main CI passes, only product commits may be cherry-picked to
`codex/upstream-pr-prep` under the authorized contribution funnel. Commit, push,
PR, merge and that product-only cherry-pick were explicitly authorized. Native Windows execution remains a required merge
gate; browser-gallery renders are not native-window proof.

## Local verification

- Frontend TypeScript build, ESLint with zero warnings, Vite production build,
  i18n catalog/glyph gate (10 locales, 688 messages, 37 settings), settings
  coverage, and the existing copy-experiment contract passed.
- The desktop gallery passed 189 cases. After connecting the main diagnostics
  timeline, the affected diagnostics/Studio selection passed 63 cases across
  desktop, tablet and mobile. Twenty-one cases intentionally skip small-screen
  Studio combinations; the real Studio has an 880 × 560 minimum window size.
  A final 12-case Studio run passed after layout corrections, including all ten
  locales, profile/plain-GDI source selection, PNG export, opening failure and
  checkbox-label interaction. Korean and Arabic renders were inspected.
- Service host unit, deferred-target, process-orchestration and driver tests
  passed 47 cases. Event record, sanitization, malformed-line handling, merge,
  throttle and bounded-rotation tests passed six cases.
- Service workspace checks with all targets/features passed for Windows x86
  and x64. Windows x64 Clippy with warnings denied passed for both the service
  workspace and Tauri. Both Rust workspaces passed formatting checks.
- The new public C++ whitespace policy was checked locally against all 52
  source/header files. The renderer core, shared settings/IPC definitions and
  main workflow definitions have no changes. The existing Windows smoke script
  additionally exercises Studio hide, restore, destroy and reopen.

The full Windows Rust test suites, MSVC C++ builds, actual service injection,
installer/rollback integration and Tauri native-window smoke tests still need
Windows CI. Linux execution of Windows path-contract tests is not treated as
Windows evidence. The Linux cross-check uses LLVM's resource compiler; its
build-script GNU-toolchain warnings do not represent a native Windows build.
Vite still reports the existing large-chunk advisory; its threshold is unchanged.

Local build/test evidence is retained under `build/port-evidence/`, and rendered
screenshots under `artifacts/frontend-gallery/`. These results describe local verification before GitHub delivery; Windows CI
is the remaining integration gate.

Windows Miri cannot call `CreateDirectoryW` (run 33993243900). Eight new
file-backed log tests therefore skip only under `all(miri, windows)`. They remain
enabled in native Windows test jobs; pure sanitization and throttle contracts
still run in Miri. This CI-only annotation fix is separate from the product pick.
