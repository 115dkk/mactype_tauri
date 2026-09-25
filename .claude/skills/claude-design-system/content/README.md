MacType Control Center is the Windows settings surface for MacType, a system-wide text renderer. Its readers care about glyph edges, so the interface stays engineered, calm and inspectable: dense rows, hairline separators, stable alignment and one cyan-blue accent used only for selection and action. It never resembles a marketing dashboard: no cards, gradients, decorative statistics, oversized headings or ornamental status dots.

This system is the fork's `main` branch: one presentation (the classic layout) with a `light` and a `dark` theme. Design dials: variance 4, motion 3, density 8.

## Content fundamentals

- Write for the person at the screen, in their words: what a setting changes and what they can do next. "MacType is running", "Open profiles", "The original file remains unchanged until you save."
- Sentence case everywhere. No marketing copy, no exclamation marks, no emoji, no em dashes and no decorative uppercase eyebrows in visible copy.
- Show defaults, current values, dirty state and apply requirements as text. Say when an app must restart, when administrator approval is needed and when a change only reaches apps opened next.
- Keep a technical option name when it identifies a real compatibility choice, and explain its visible effect beside it.
- The product's Korean terms are fixed: **위자드** (never 마법사), **튜너**, **실행 프로필** (never 활성 프로필 or 적용 프로필), **신식 서비스**, **단계별 설정**, **전체 설정**. Korean wraps between words (`word-break: keep-all`), so a sentence never leaves one syllable alone on a line.
- All ten locales (ar, de, en, es, fr, ja, ko, pt, zh-CN, zh-TW) keep identical keys; Arabic runs right to left, so every rule uses logical properties or a mirrored `[dir="rtl"]` twin.
- Navigation order is fixed: Overview; Wizard (Profiles, Service); Tuner (Guided setup, All settings); Diagnostics. The language picker and the theme toggle sit at the bottom of the navigation.

## Colour

- Every colour is a token. Build with `color-canvas` for the window, `color-surface` for work surfaces, `color-surface-subtle` for hover, selection and secondary fills.
- Set text in `color-foreground`; secondary text in `color-muted`. Both read on `color-canvas`, `color-surface` and `color-surface-subtle` in both themes.
- `color-primary` is the only accent. Use it for selection markers, the filled primary action, checked switches and text actions. Never set body text in it on `color-canvas`; text on it is `color-on-primary`.
- The solid accent fill (`.button.primary`) belongs to the action that starts or stops something. An action that writes a setting, such as designating the run profile, wears `.button.designate`: accent edge, accent-tinted fill and a label mixed toward the foreground to hold 4.5:1.
- `color-success`, `color-warning` and `color-destructive` carry meaning only: running or verified, needs attention, failed or destructive. Pair each with a word or an icon; never use them for decoration.
- Hairlines use `color-border`; control edges use `color-border-strong`. Focus is always a 2px `color-focus` outline offset 2px.
- No gradients, glass, pure `#000000` surfaces or saturated primaries in decorative positions.

## Type

- The UI face is the Windows system stack: `"Segoe UI Variable Text", "Segoe UI", sans-serif`. It is deliberate, so the utility matches platform metrics without downloading fonts. Outside Windows the stack falls back to the platform sans.
- Korean bold comes from the bundled **Pretendard KO UI** subset (`fonts/pretendard-ko-ui.woff2`, SIL OFL 1.1) because WebView2 never selects Malgun Gothic's bold cut; use the `ui-ko` family. Japanese and Chinese name their own UI faces (`ui-ja`, `ui-zh-cn`, `ui-zh-tw`). Synthetic bold stays off (`font-synthesis: none`).
- Use `type-title` for the view title, `type-section` for section titles, `type-body` for body and controls, `type-label` for labels, `type-caption` for metadata and help, `type-mono` for versions and paths.
- Use `font-variant-numeric: tabular-nums` wherever digits line up: timestamps, step numbers, sizes.

## Space, size and shape

- Space on the 4px grid: `space-1` (4) through `space-10` (40). Layout dimensions use a named size token, never an unexplained literal.
- The window shell is a `nav-width` navigation rail, a 1px divider and a flexible work area under a `titlebar-height` title bar. Controls are at least `control-height`; a setting row keeps a stable `settings-control-width` control column.
- The window is responsive. From 768px to 1023px the metrics tighten (`nav-width` 176px, `settings-index-width` 168px, `settings-control-width` 200px, page padding `space-6`); below 768px the navigation becomes a sticky bar under the title bar. No width may cause horizontal scrolling.
- Corners are `radius-control` (4px). Nothing is fully rounded except switch tracks and step circles.
- Depth is borders plus tonal shifts. `shadow-window` belongs only to layers above the window: the language menu, hint popovers and dialogs. No other shadows.

## Motion

Animate only opacity, transform and filter, with `--motion-fast` (120ms) or `--motion-normal` (180ms) and `--ease-standard` (`cubic-bezier(0.2, 0, 0, 1)`). A view enters with a 4px vertical settle plus fade; a pressed button scales to 0.96. `prefers-reduced-motion: reduce` sets every animation and transition to 1ms and turns off smooth scrolling.

## Previews of rendered text

- A preview canvas takes the window theme's polarity (dark window, dark canvas, light text) and offers one invert control; it never asks for a background separately.
- Helper bitmaps are shown at device pixels or at integer nearest-neighbour zoom. Never scale a preview image with CSS transforms, and never let a surface colour replace the bitmap's own palette.

## Iconography

- One icon family: **lucide-react** 0.468, outline glyphs on a 24px grid with round caps and joins, drawn in `currentColor` so they take the text colour. The Icons group holds every glyph the Control Center imports, as its source SVG.
- Navigation icons are 18px (17px for sub-items) at stroke 1.8; the language and theme icons 17px; inline icons 15 to 17px; title bar window controls 16px (maximize 13px) at stroke 1.5 to 1.7.
- Icons sit beside a word; an icon alone needs an accessible name. Never use emoji or glyph characters as icons.
- The product mark is the MacType icon (Logos group): 18px in the title bar, 32px in the product lockup beside "MacType" and "Control Center". It is a raster file; never redraw or recolour it.

## Using this system

- The components here are static renditions of the real Control Center markup, styled by `components/bundle.css`, which is the application's own stylesheets in import order with the token declarations moved to `tokens.css`. Build a screen by composing the same class names the previews use (`button primary`, `switch-control`, `setting-row`, `section-block`, `detail-list`, `nav-item`).
- Show state with words plus colour and keep every control keyboard reachable with visible focus, in both themes and in RTL.

## Not synced

Motion values (`--motion-fast`, `--motion-normal`, `--ease-standard`) stay in `bundle.css`, because this format has no motion family. Segoe UI Variable, Consolas, Cascadia Mono, Malgun Gothic, Yu Gothic UI, Meiryo UI, Microsoft YaHei UI and Microsoft JhengHei UI are Windows system fonts and are not copied. The stylesheet still carries a `.status-band` rule that no screen renders, so it has no component card. The React pages were not built into a bundle; each component card is a static rendition of its markup.
