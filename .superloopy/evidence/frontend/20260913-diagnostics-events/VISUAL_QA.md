# Diagnostics event view validation

Base commit: `8014cbd` on `codex/alpha-design-diagnostics`.

Environment: Windows 11 Education 10.0.22621, Chromium 149.0.7827.55, 1340 × 760 CSS pixels, device scale factor 1 (96 DPI), Korean locale. The browser gallery simulates backend event records; no native Tauri, WebView2, renderer injection, or Windows font rendering claim is made.

The Classic raw-log behavior was identified in the existing page and gallery assertion before modification; no before-change browser capture was taken. Classic now keeps a localized timeline visible and places three switches in its section heading. Console places them below the chips, while Fluent and Cupertino retain them inside the shared timeline.

The initial after-change capture exposed the Console and Cupertino skin rules hiding all switch labels. Scoped event-option rules now display those labels without altering switch metrics. A regression assertion verifies every visible label in all four skins, and clicking the first label toggles its switch. Final captures show readable labels, consistent row columns, and no horizontal document overflow in any skin. Classic's hidden-summary capture retains other event lines and the checked option.

The five requested gates passed after correction, including nine focused gallery tests. Adjacent regression coverage includes installation actions, overview events without parameters, warning-chip filtering, raw detail disclosure, persistence after reload, repeat counts, routine hiding, and absent versus unreadable source files.

Captures are under `E:/mactype_tauri/.worktrees/alpha-design-diagnostics/tmp/after/`: `diagnostics-classic.png`, `diagnostics-fluent.png`, `diagnostics-console.png`, `diagnostics-cupertino.png`, and `diagnostics-classic-hide-summaries.png`. Capture metrics and the Chromium version are in `capture-metrics.json`. Screenshots are full-page browser captures; application-owned scroll containers retain their normal viewport behavior.

The full multi-viewport, all-locale gallery and native packaged application tests were not run for this frontend-only task. No hosted-CI result is claimed.
