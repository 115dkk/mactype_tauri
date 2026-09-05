# Alpha skin and preview regression receipt

## Scope and provenance

User-reported defects: control proportions, clipped Console specimens,
profile-aware replacement fonts, a blank Windows Studio window, and permanent
or transient bitmap-background islands in Fluent/Cupertino and theme inversion.
The existing visual system remains authoritative; no redesign is introduced.

- Baseline: `45e148c8f3f321aa2a303c72c9cb677ce8f15d3e`, `codex/alpha-plus-dll`.
- Main intake checked: `d949dace4a946f3a254dbbcc20e3d8c3da43e031`; the existing
  alpha intake ledger already classifies the applicable changes.
- Superloopy: `85ede9c958d9c11498619a8c53db66098e52158d`; frontend UX, Web,
  desktop, hybrid, layout, and motion-core references.
- Browser capture: Linux, Chromium 149.0.7827.55, Playwright 1.61.1,
  production Vite build, Korean locale, light theme, 1280x800 CSS pixels.
  The repository's Pretendard Korean subset supplied local QA glyphs.
- Gallery SVGs simulate the native preview helper. These images prove DOM
  layout and palette composition, not Windows font rasterization.

## Rendered comparison

| Journey | Before | After | Inspection |
| --- | --- | --- | --- |
| Console overview | [before](before-console-overview.png) | [after](after-console-overview.png) | All six sizes remain available; the largest specimen has adequate line height and wraps within its bitmap. The canvas scrolls instead of clipping its children. |
| Fluent Tuner | [before](before-fluent-profiles.png) | [after](after-fluent-profiles.png) | The gray text rectangle previously floated inside a white canvas. Canvas and bitmap now share the same background. |
| Cupertino Tuner | [before](before-cupertino-profiles.png) | [after](after-cupertino-profiles.png) | The same background mismatch is removed without changing the surrounding card layout. |

`rendered-baseline.json` records measured image and canvas details for both
builds. Additional regression screenshots are produced by the gallery workflow.

## Owner-scoped evidence

| Target | Affected owner | Claims and scope reason | Proof |
| --- | --- | --- | --- |
| `gallery-client`: Linux Chromium, embedded-client simulation | React/CSS | Four-skin control bounds, keyboard focus, scroll access, localized replacement selection, image/palette publication; these changed in the client | `skin-preview.spec.ts`, inspected captures, animation-frame audits with delayed image responses |
| `windows-shell`: Windows 2022 CI, packaged Tauri/WebView2 | Tauri window lifecycle | Actual Studio creation, render, hide/restore, destruction, recreation, and process exit; a browser URL cannot prove these | `Test-TauriWindows.ps1` adds `--ci-preview-studio`, with readiness sent only after decoded Studio strips render twice across a real window recreation |
| `windows-profile`: Windows Rust test runner | Read-only profile command | Reading saved substitutions must preserve an unrelated dirty editor and undo state; disabled mappings must remain disabled | `preview_substitutes_read_saved_profile_without_replacing_dirty_editor` |

Motion delta: switch hover no longer grows the thumb outside its track;
checked geometry changes directly and preserves focus and keyboard state.
Preview inversion retains the last complete palette until all replacement
images decode, then publishes the newest batch and its background together.
Animation-frame audits check visible intermediate frames, not just screenshots.

## Validation and boundaries

- Local production build, ESLint, i18n (10 locales), and generated-setting
  validation passed.
- Focused browser regressions: 34 passed, 2 skipped. The skipped cases avoid
  repeating an explicit 880x560 / 192-DPI test in unrelated viewport projects.
- Broader skin/view and Studio selection: 79 passed, 5 skipped (viewport
  duplicates guarded by existing tests). The full gallery remains an
  exact-commit CI requirement.
- Native Windows compilation and window lifetime are validated by branch CI,
  not by this Linux capture. This receipt does not claim a local Windows run.
- A user's installed fonts, their full personal profile, physical mixed-DPI
  monitors, and assistive-technology behavior are outside these captures.
- Delivery and CI outcomes bind to the commit containing this receipt via
  GitHub Actions. Verify the exact branch HEAD, not an earlier green run.
  Where the local workspace lacks `gh`, use the connected GitHub Actions API
  to inspect the same head SHA, jobs, and completion results.
