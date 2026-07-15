# Control Center design maintenance

This is a file map and editing checklist for maintainers. It explains where to change the interface; it is not a description of the visual concept.

## Keep design work out of Rust

Routine visual work belongs under `control-center/src` and can be previewed in a browser. Do not edit these paths for a color, spacing, typography, layout, copy, icon, or page-composition change:

- `control-center/src-tauri/**`
- `preview-helper/**`
- `shared/settings-schema.json`
- generated settings files

Those paths define native behavior, IPC, packaging, or the MacType setting model. If a proposed design change needs a new native command or new data from Windows, split that behavior into a separate change. The visual part can then consume the new TypeScript adapter method without mixing Rust into design review.

## Where to make each change

| Change | Primary file | Also check |
| --- | --- | --- |
| Colors, spacing, control height, radii, animation timing, navigation width | `control-center/src/styles/tokens.css` | Both `:root` and `:root[data-theme="dark"]` |
| Shared controls, cards, grids, page spacing, responsive behavior, RTL rules | `control-center/src/styles/app.css` | The mobile and tablet media queries at the end of the file |
| Application shell, navigation order, navigation icons, theme switch | `control-center/src/app/App.tsx` | `ViewId` in `control-center/src/app/model.ts` when adding or removing a page |
| Custom title bar layout, icon, and window-control icons | `control-center/src/components/WindowTitleBar.tsx` | `.window-titlebar`, `.window-title`, and `.window-controls` in `app.css` |
| Language picker layout, option order, and scrollable menu | `control-center/src/components/LanguagePicker.tsx` and `control-center/src/i18n/i18n.ts` | `.language-*` selectors in `app.css`, including dark and mobile states |
| Overview layout | `control-center/src/pages/OverviewPage.tsx` | Overview interaction assertions in `tests/frontend-gallery/gallery.spec.ts` |
| Settings-file and migration layout | `control-center/src/pages/FileSettingsPage.tsx` | `.legacy-import-*` and `.file-*` selectors in `app.css` |
| Profile editor shell and preview placement | `control-center/src/pages/ProfilesPage.tsx` | `control-center/src/pages/profiles/` for individual editor sections |
| Basic, shape, and LCD setting rows | `control-center/src/pages/profiles/SchemaSettings.tsx` | Shared `.setting-row`, `.range-control`, and `.number-control` selectors |
| Advanced, per-font, and list editors | `control-center/src/pages/profiles/AdvancedSettings.tsx`, `IndividualSettings.tsx`, and `ListsEditor.tsx` | Font picker and collection selectors in `app.css` |
| Execution and service-control layout | `control-center/src/pages/ExecutionPage.tsx` | `.manual-*`, `.service-*`, and `.system-mode-*` selectors |
| Diagnostics layout | `control-center/src/pages/DiagnosticsPage.tsx` | `.diagnostic-*` and `.log-view` selectors |
| User-facing text | Every JSON catalog under `control-center/src/i18n/` | All ten catalogs must contain the same keys and placeholders |
| Locale order, language detection, RTL selection | `control-center/src/i18n/i18n.ts` and `I18nProvider.tsx` | `tests/frontend-gallery/windows.ts` |
| Browser-only sample data used during design work | `control-center/src/app/runtimeAdapters/browserGalleryAdapter.ts` | Keep the adapter result shapes equal to `runtimeAdapter.ts` |
| In-app MacType logo | `control-center/public/mactype-icon.png` | Preserve the existing filename to avoid code changes |
| Packaged EXE and installer icon | `control-center/src-tauri/icons/icon.ico` and `assets/mactype.ico` | These are assets; no Rust source change is required |

## Common maintenance tasks

### Change the palette or density

Start in `tokens.css`. Prefer changing a semantic token instead of replacing individual hex values or pixel sizes throughout `app.css`. Update the light and dark values together, then inspect status colors, disabled controls, focus rings, and preview backgrounds.

Spacing follows the `--space-*` scale. Controls use `--control-height`; navigation and profile-editor widths have dedicated tokens. A density change should normally be possible without editing a page component.

### Change a shared control or card

Edit the shared selector in `app.css` before adding a page-specific override. The main reusable patterns are:

- `.button`, `.icon-button`, and `.text-action`
- `.section-block` and `.section-heading`
- `.detail-list`
- `.setting-row`, `.range-control`, and `.number-control`
- `.success-message`, `.inline-error`, and `.warning-text`

Keep focus-visible styling and disabled-state contrast. Do not remove visible focus just to match a screenshot.

### Change a page layout

Edit the relevant component in `control-center/src/pages` and its selectors in `app.css`. Page components should decide structure and accessibility; CSS should decide placement and presentation. Keep user-facing copy in the locale catalogs rather than embedding it in TSX.

For grid children that contain paths, translations, or font names, use `minmax(0, 1fr)` and `min-width: 0`. Test long German, Chinese, and Arabic content instead of relying on English width.

### Change the custom title bar

The visible title bar is ordinary React and CSS. Change its markup, logo, text, or Lucide icons in `WindowTitleBar.tsx`; change its height with `--titlebar-height` in `tokens.css`; and change colors, borders, hover states, or control widths in the `.window-titlebar`, `.window-title`, and `.window-controls` rules in `app.css`. None of those changes requires Rust.

Keep `data-tauri-drag-region` on the non-interactive title area so the real window remains draggable. Keep minimize, maximize/restore, and close as separate buttons with translated `aria-label` values. `WindowTitleBar.tsx` calls the narrow Tauri window API only after detecting the native runtime, so the same component remains visible and safe in browser gallery mode.

The native frame is disabled once in `control-center/src-tauri/tauri.conf.json`, and the matching window-control permissions live in `control-center/src-tauri/capabilities/default.json`. Treat those as platform wiring, not design files. Ordinary title-bar redesign must leave them alone. Only restoring the operating-system title bar or adding a new native window operation should require a separate native configuration change.

### Add a navigation page without native behavior

For a frontend-only page:

1. Add the `ViewId` in `control-center/src/app/model.ts`.
2. Add the page component under `control-center/src/pages`.
3. Register its icon, order, and render branch in `App.tsx`.
4. Add `nav.<id>` to every locale catalog.
5. Add the view and localized titles to `tests/frontend-gallery/windows.ts`.
6. Add a gallery interaction test if the page has controls.

This does not require Rust. Only native command-line launch support for the new view would require a separate change to the Tauri launch parser.

### Change copy or add a locale

Never put translated text directly in TSX. Existing messages live in ten JSON files under `control-center/src/i18n`. Keys and `{placeholder}` names must match exactly across catalogs.

When adding a locale, update `localeOptions`, `catalogs`, and locale detection in `i18n.ts`, then add its script and direction to `tests/frontend-gallery/windows.ts`. RTL is selected in `I18nProvider.tsx`; use logical CSS properties such as `padding-inline-start` when possible and add `[dir="rtl"]` only when the visual direction genuinely changes.

### Change icons

Interface icons come from `lucide-react` and are selected in TSX. Decorative icons need `aria-hidden="true"`. An icon-only button needs a translated `aria-label`. Keep icon size and stroke weight consistent with neighboring controls.

## Browser preview without Rust

The browser gallery adapter supplies installation, profile, font, diagnostics, and service sample data. This makes visual work possible without compiling or launching Tauri.

```powershell
cd control-center
pnpm install --frozen-lockfile
pnpm dev
```

Open a specific state with query parameters, for example:

```text
http://localhost:1420/?gallery=1&view=files&lang=en
http://localhost:1420/?gallery=1&view=profiles&lang=zh-CN
http://localhost:1420/?gallery=1&view=execution&lang=ar
```

Use the in-app theme button to inspect dark mode. Browser mode is for design and interaction review; native Windows behavior remains behind the runtime adapter.

## Before sending a design change

Run the frontend-only checks from `control-center`:

```powershell
pnpm test:i18n
pnpm test:settings
pnpm lint
pnpm build
pnpm test:gallery
```

The gallery covers every public page in all supported locales at 390, 768, and 1280 pixels. It fails on JavaScript errors, horizontal overflow, broken RTL, missing translations, or inaccessible interaction results. Review the generated images under `artifacts/frontend-gallery`, including at least one mobile layout, one dark state, and Arabic RTL.

## Stop conditions

A design-only change has crossed into native scope if it requires any of the following:

- a new Tauri command;
- a different Rust DTO;
- filesystem, registry, service, tray, or process behavior;
- a new MacType INI setting;
- a Preview Helper protocol change.

Stop there and separate the native behavior from the design change. Maintainers should be able to review and revise the interface without rebuilding Rust for ordinary visual decisions.
