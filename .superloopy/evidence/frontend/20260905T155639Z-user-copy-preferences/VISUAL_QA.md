# User copy and preference tiles

Base: `7f8c2fe8081ddee7954b60743234cc7d29c94de2` on
`codex/alpha-plus-dll`. Main intake remains `d949dace4a946f3a254dbbcc20e3d8c3da43e031`;
no new intake was required. Superloopy uses the frontend UX, embedded-Web,
desktop, hybrid, and layout contracts mapped in `docs/frontend-skin-validation.md`.
`DESIGN.md`, `docs/skin-designs.md`, and existing skin metrics remain authoritative.

## Scope and ownership

| Target | Owner | Claims | Evidence | Scope reason |
| --- | --- | --- | --- | --- |
| Windows desktop / Tauri 2 / embedded WebView2 DOM | React preferences and localized catalogs | Console icon-over-label layout, whole-tile activation, usable menu selection and dismissal, understandable setting effects | Chromium rendered captures; `skin-preview.spec.ts` interaction and bounds assertions; ten-locale catalog gate | Only client text, markup, focus, and CSS changed |
| Linux client simulation / Chromium 149 / 1280×800 CSS pixels / DPR 1 | Gallery fixture renderer | Before/after layout comparison, Korean and RTL menu placement, service copy | `before-*` and `after-*` PNGs | Client evidence only; no claim of local Windows execution |

The existing branch Windows smoke and package workflows remain delivery gates.
No native window, injection, profile persistence, or elevation behavior changes.
SEO and native renderer claims are outside this patch.

## Review

- Audited user-facing message usage in service management, settings, profile
  editing, preview, and events. Reworked 135 message keys in all ten languages;
  catalog gates validate the full set of 737 keys in every language.
  Necessary troubleshooting values remain; implementation/provenance prose
  is removed from ordinary help. The service introduction explains automatic
  profile application and the available management actions.
- Settings were checked against `renderer/settings.cpp`, `override.cpp`,
  `fteng.cpp`, `ft.cpp`, and the DirectWrite enum reference. In particular,
  `UseInclude` controls the application list; font inclusion is independent.
  The experimental inversion option does not promise whole-screen inversion.
- Console's inherited two-column grid and 12px trigger caused the original
  horizontal layout and tiny activation area. The icon and label now belong
  to one 56px button (52px at narrow widths). Fluent and Cupertino retain their existing
  computed 40px tiles and fill them with the button. Fluent padding moves
  into the button so its edges and icon are also clickable.
- Menus focus the selected option on open, navigate with arrows/Home/End,
  select with Enter/Space, dismiss with Escape/outside click/Tab, and restore
  focus after replacing a skin shell. The light/dark toggle remains a
  full-tile action. Narrow Console menus use viewport positioning to avoid
  clipping inside the horizontally scrolling rail.
- Shared service rows now shrink to their available width, and translated
  action labels and status values wrap. This fixes the narrow French/German
  and unknown-runtime overflows observed in the initial full gallery.
- Before/after captures cover Console Korean and Arabic overview/menu,
  Fluent Korean service, and Classic English settings. Capture-only
  Fontconfig includes Noto Sans CJK for language autonyms missing from the
  Linux image. The app still ships only its existing Korean subset, regenerated
  from the pinned Pretendard source for the revised text.
- Inspected icon alignment, visible label placement, popup placement, service
  copy wrapping, and the adjacent profile/preview layout. Browser gallery
  raster data are fixtures, not Windows font-rendering proof.

## Validation and limits

Build, ESLint, i18n, settings coverage, and copy-experiment checks pass.
Focused preference interaction tests pass at 390, 768, and 1280 CSS pixels.
The initial full gallery completed 606 passing cases, 20 viewport-specific
skips, and 46 failures. The failures covered stale text assertions and the
shared service-row overflow. All 46 pass after the copy assertions and layout
fixes. Final preference/all-skin/locale checks had 48 passes and exposed three
Fluent target-size failures; all three pass after moving its padding inside
the button. This covers the six new Console keyboard journeys and twelve
all-skin tile cases. The exact-HEAD CI gallery remains the final complete run. The 390px gallery width is below the Windows minimum.
Local Linux font metrics differ from the hosted Windows and gallery runners.

All applicable Actions workflows must complete for the exact delivery SHA.
The connected GitHub Actions API is used where `gh` is unavailable; a prior
commit's result is never substituted. CI results attach to this receipt's
commit through GitHub Actions.

Workspace maintenance removed the first uncommitted checkout during this
session. The original remote base was re-cloned, the edits were reconstructed,
and the replacement build and evidence were produced in this run. Product
commits were also saved as GitHub commit objects before final delivery.
