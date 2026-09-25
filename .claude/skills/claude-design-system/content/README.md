MacType Control Center is the Windows settings surface for MacType, a system-wide text renderer. Its readers care about glyph edges, so the interface stays engineered, calm and inspectable: dense rows, hairline separators, stable alignment and one accent used only for selection and action. It never resembles a marketing dashboard.

This system is the alpha distribution branch. It ships **four skins over one application model**, each with a light and a dark theme. The eight themes of this system are those pairs: `classic-light`, `classic-dark`, `fluent-light`, `fluent-dark`, `console-light`, `console-dark`, `cupertino-light` and `cupertino-dark`. Classic is the default and the baseline every other skin is compared against.

## Skins

A skin arranges the shared page models in its own paradigm and restyles the shared controls; it never owns an action, a message or an IPC call. Skin ids (`classic`, `fluent`, `console`, `cupertino`) are interface and never change; labels may.

- **Classic:** a labelled navigation pane with group headings, one work area, section blocks with a heading and a description, 40px controls, 4px corners.
- **Fluent:** the Windows 11 Settings grammar. One Mica-like canvas shared by the title bar and the navigation, a 28px display title, and a settings card for every setting (icon, regular-weight title, muted description, control at the trailing edge). Hierarchy comes from size and colour, never from bold.
- **Console:** a rendering tool's workbench. A 64px icon rail, a command bar over each page, bordered panels with a muted title strip, 26px controls, monospace values, LED status dots and a 24px status bar. Dark is its native palette.
- **Cupertino:** the macOS System Settings grammar, borrowed because MacType borrows Mac rendering. A tinted sidebar that runs to the top edge, muted icon tiles, rounded groups of 44px rows with inset hairlines, controls at the trailing edge and a 13px body.

The Skin designs section records each skin's palette, metrics, components, page paradigms and native preview window chrome. Add a page or a control to a skin from that record.

## Content fundamentals

- Write for the person at the screen, in their words: what a setting changes and what they can do next. "MacType is running", "Open profiles", "The original file remains unchanged until you save."
- Sentence case everywhere. No marketing copy, no exclamation marks, no emoji, no em dashes and no decorative uppercase eyebrows in visible copy.
- Show defaults, current values, dirty state and apply requirements as text. Say when an app must restart, when administrator approval is needed and when a change only reaches apps opened next.
- Keep a technical option name when it identifies a real compatibility choice, and explain its visible effect beside it. DirectWrite rendering choices describe spacing and smoothing, never "Mode N".
- The product's Korean terms are fixed: **위자드** (never 마법사), **튜너**, **실행 프로필** (never 활성 프로필 or 적용 프로필), **신식 서비스**, **단계별 설정**, **전체 설정**.
- All ten locales (ar, de, en, es, fr, ja, ko, pt, zh-CN, zh-TW) keep identical keys; Arabic runs right to left, so every rule uses logical properties or a mirrored `[dir="rtl"]` twin.
- Navigation order is fixed in every skin: Overview; Wizard (Profiles, Service); Tuner (Guided setup, All settings); Tools (Diagnostics). Language, skin and theme controls sit at the bottom of the navigation, in that order.

## Colour

- Every colour is a token, and each skin redefines the same names. Build with `color-canvas` for the window, `color-surface` for work surfaces, `color-surface-subtle` for hover, selection and secondary fills.
- Set text in `color-foreground`; secondary text in `color-muted`. Both read on `color-canvas`, `color-surface` and `color-surface-subtle` in every theme except where a token's note says otherwise.
- `color-primary` is the only accent. Use it for selection markers, the filled primary action, checked switches and text actions. Never set body text in it on `color-canvas`; text on it is `color-on-primary`.
- The solid accent fill (`.button.primary`) belongs to the action that starts or stops something. An action that writes a setting, such as designating the run profile, wears `.button.designate`: accent edge, accent-tinted fill and a label mixed toward the foreground to hold 4.5:1.
- `color-success`, `color-warning` and `color-destructive` carry meaning only: running or verified, needs attention, failed or destructive. Pair each with a word or an icon; never use them for decoration.
- Hairlines use `color-border`; control edges use `color-border-strong`. Focus is always a 2px `color-focus` outline offset 2px.
- No gradients, glass, pure `#000000` surfaces or saturated primaries in decorative positions. The light preview canvas is the one pure white surface. Cupertino's sidebar tiles are the only coloured navigation, and they are deliberately desaturated.
- Skin-only tokens carry the skin's prefix (`fluent-hover`, `console-accent-soft`, `cupertino-sep`). Use them only inside that skin.

## Type

- The UI face is the Windows system stack: `"Segoe UI Variable Text", "Segoe UI", sans-serif`. It is deliberate, so the utility matches platform metrics without downloading fonts. Outside Windows the stack falls back to the platform sans.
- Korean bold comes from the bundled **Pretendard KO UI** subset (`fonts/pretendard-ko-ui.woff2`, SIL OFL 1.1) because WebView2 never selects Malgun Gothic's bold cut; use the `ui-ko` family. Japanese and Chinese name their own UI faces (`ui-ja`, `ui-zh-cn`, `ui-zh-tw`). Synthetic bold stays off (`font-synthesis: none`).
- Classic: `type-title` for the view title, `type-section` for section titles, `type-body` for body and controls, `type-label` for labels, `type-caption` for metadata and help, `type-mono` for versions and paths.
- Fluent titles use `type-fluent-display` (Segoe UI Variable Display) and never bold a card title. Console sets everything at 12px with tabular numbers and values in `type-console-value` (Cascadia Mono). Cupertino uses a bold 24px title and a 13px body.
- Use `font-variant-numeric: tabular-nums` wherever digits line up: timestamps, step numbers, sizes.

## Space, size and shape

- Space on the 4px grid: `space-1` (4) through `space-10` (40). Layout dimensions use a named size token, never an unexplained literal.
- Read metrics from their variables so each skin can supply its own: `nav-width`, `titlebar-height`, `control-height`, `settings-index-width`, `settings-control-width`. Classic values are the tokens; the Skin designs section lists every override.
- The window is responsive. From 768px to 1023px the classic metrics tighten (`nav-width` 176px, `settings-index-width` 168px, `settings-control-width` 200px, page padding `space-6`); below 768px the navigation becomes a sticky bar under the title bar. No width may cause horizontal scrolling.
- Corners are `radius-control` (4px; Cupertino 6px, with 10px groups). Nothing is fully rounded except switch tracks, status dots and step circles.
- Switches, checkboxes, radios and closed selects share one primitive driven by `switch-width`, `switch-height`, `switch-thumb`, `switch-inset`, `switch-border`, `selection-size`, `select-marker-size`, `select-marker-inset` and `select-badge-size`. A skin changes those metrics and never adds its own transform or offset. The thumb stays inside its track when on, off, hovered, disabled and in RTL.
- Depth is borders plus tonal shifts. `shadow-window` belongs only to layers above the window: the preference menu, hint popovers and dialogs. Fluent replaces control shadows with a darker bottom edge; Cupertino controls carry a faint 1px drop shadow.

## Motion

Animate only opacity, transform and filter, with `--motion-fast` (120ms) or `--motion-normal` (180ms) and `--ease-standard` (`cubic-bezier(0.2, 0, 0, 1)`). A view enters with a 4px vertical settle plus fade; a pressed button scales to 0.96. `prefers-reduced-motion: reduce` sets every animation and transition to 1ms and turns off smooth scrolling.

## Previews of rendered text

- A preview canvas takes the window theme's polarity (dark window, dark canvas, light text) and offers one invert control; it never asks for a background separately.
- Helper bitmaps are shown at device pixels or at integer nearest-neighbour zoom. Never scale, stretch, fade or CSS-invert a renderer sample, and never let a skin surface colour replace the bitmap's own palette.
- Keep the displayed batch and its palette together until every replacement image has decoded, then publish them together. A placeholder keeps the strip's eventual height.

## Iconography

- One icon family: **lucide-react** 0.468, outline glyphs on a 24px grid with round caps and joins, drawn in `currentColor` so they take the text colour. The Icons group holds every glyph the Control Center imports, as its source SVG.
- Navigation icons are 18px (17px for sub-items) at stroke 1.8; preference and inline icons 16 to 17px; title bar window controls 16px (maximize 13px) at stroke 1.5 to 1.7. Console uses 20px rail icons over a 10px label.
- Icons sit beside a word; an icon alone needs an accessible name. Never use emoji or glyph characters as icons.
- The product mark is the MacType icon (Logos group): 18px in the title bar, 32px in the classic product lockup beside "MacType" and "Control Center". It is a raster file; never redraw or recolour it.

## Using this system

- The components here are static renditions of the real Control Center markup, styled by `components/bundle.css`, which is the application's own stylesheets in import order with the token declarations moved to `tokens.css`. Build a screen by composing the same class names the previews use (`button primary`, `switch-control`, `setting-row`, `status-band`, `fluent-card`, `console-panel`, `cupertino-group`).
- The preview frame's theme id selects the skin. To show one skin regardless of the chosen theme, wrap the markup in `<div data-theme="fluent-dark">` (any `<skin>-<mode>` id): every skin rule matches that wrapper as it matches the root.
- Every skin must pass in both themes at every width without horizontal overflow, with keyboard reach, visible focus and RTL mirroring.

## Not synced

Motion values (`--motion-fast`, `--motion-normal`, `--ease-standard`) and keyword values (`--select-marker-color: currentColor`, `--select-badge-color: transparent`) stay in `bundle.css`, because this format has no motion family and no keyword colours. Skin-only metrics that have no classic default (`--statusbar-height`, `--console-mono`) stay on their skin's root rule in `bundle.css`. Segoe UI Variable, Cascadia Mono, Consolas, Malgun Gothic, Yu Gothic UI, Meiryo UI, Microsoft YaHei UI and Microsoft JhengHei UI are Windows system fonts and are not copied. The React components were not built into a bundle; each component card is a static rendition of its markup.
