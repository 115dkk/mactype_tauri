# Skin designs

One design record per skin, so a page, a control or the native preview window can be added to a skin without rediscovering its rules. `DESIGN.md` fixes what every skin shares; this file fixes what each skin owns. Skin ids (`classic`, `fluent`, `console`, `cupertino`) are interface.

## What every skin shares

- The page models in `control-center/src/features/` own every action, message and IPC call. A skin arranges; it never re-implements.
- The six navigation entries and their groups come from `src/app/shell.ts` in a fixed order: 개요, then 위자드 (프로필, 서비스), then 튜너 (단계별 설정, 전체 설정), then 도구 (진단).
- The language, skin and theme controls sit at the bottom of the navigation in every skin, built on `PreferenceMenu`, in that order.
- Preview canvases take the window theme's polarity by default and offer one invert control. They never ask for a background separately.
- Bitmaps rendered by the helper are shown at device pixels or at integer nearest-neighbour zoom, never scaled by CSS.
- The event timeline (`EventTimeline`) is one component; a skin styles it and chooses where the filter chips sit.
- One accent hue per skin. Success, warning and danger are reserved for meaning. No pure `#000000`/`#FFFFFF` surfaces except the light preview canvas.
- The native preview window (`mactype-preview32.exe`) draws its own chrome from a `chrome` object the Control Center computes from the active skin (`src/features/preview/nativeChrome.ts`), so the window matches the skin that opened it without the helper knowing skins.
- Shared switch/selection metrics and atomic bitmap/palette publication are defined in `DESIGN.md`; every skin follows `frontend-skin-validation.md` for evidence. Console's locale-specific specimen choice displays and requests the enabled profile replacement (including localized source aliases), with the source choice retained when profiles change. The overview reads saved substitution metadata without replacing the Tuner document or its undo history.

## Classic

The layout the Control Center shipped with: a labelled navigation pane with group headings, one work area, section blocks with a heading and a description, 40px controls, square corners with a 4px radius.

Palette (light / dark): canvas `#F3F5F7` / `#11161B`, surface `#FFFFFF` / `#192027`, subtle `#E9EDF1` / `#222B33`, border `#C9D1D8` / `#34414C`, text `#17212B` / `#E8EDF2`, muted `#5A6773` / `#9AA8B5`, accent `#0067C0` / `#4CA6E8`, on-accent `#FFFFFF` / `#07131C`, preview `#EEF1F4` / `#0D1115`.

Metrics: title bar 40, navigation 220, control 40, radius 4, settings index 196, settings control column 232. Body 14px/1.5, h1 24/600, h2 16/600.

Pages: overview is a service card with four key/value cells and a collapsible recent-activity block; profiles is a card grid with rendered thumbnails and a "current file" block; service is a summary grid, a notice band and details rows that expand in place; the Tuner is an index column, a settings form and the docked preview; diagnostics is three section blocks and a log view.

Native preview window: toolbar 44, controls 32, radius 4, canvas inset 18 with a 1px border, status strip 28 in the UI font. Colours from the palette above.

## Fluent

Windows 11 Settings grammar. One Mica-like canvas shared by the title bar and the navigation pane; hierarchy from size and colour, never from weight; every setting is a card.

Palette (light / dark): canvas `#F0F2F5` / `#202124`, surface `#FBFBFC` / `#2B2C30`, subtle `#F4F5F7` / `#323337`, border `rgba(0,0,0,.062)` / `rgba(255,255,255,.08)` (solid stand-ins `#E6E8EB` / `#3A3B40` where alpha is unavailable), text `#1B1B1B` / `#FFFFFF`, muted `#5F646B` / `#C4C8CE`, accent `#005FB8` / `#60CDFF`, on-accent `#FFFFFF` / `#06202C`, success `#0F7B0F` / `#6CCB5F`, preview `#FCFCFD` / `#16171A`.

Metrics: title bar 40 without a divider, navigation 248, navigation items 36 with a 3×16 accent pill, section headings 12/600 muted, page title 28/600 (Segoe UI Variable Display), subtitle muted, content column 1000, controls 32 with a darker bottom edge, radius 4, cards 62 minimum with a 24px icon column, sub-rows on the subtle surface indented 56.

Components: `FluentCard` (icon, regular-weight title, muted description, trailing control), hero variant (28px icon, 20px title), expander variant (chevron, sub-rows), `FluentState` (muted value, tone colours), badge (accent pill), profile card gallery with a 64px thumbnail.

Pages: overview is a hero card and one card per fact plus an activity expander; profiles is the card gallery and three file cards with a footer action row; service is a hero card and expander cards (system mode expands to the injection row, profile generation, AppInit, maintenance controls and the UAC note); the Tuner is an index of numbered circles or check marks, setting cards and the preview card; diagnostics is an installation hero, finding cards, component cards and a timeline card.

Native preview window: toolbar 48 on the canvas colour with no divider, controls 32 with the darker bottom edge, radius 4, toggles filled with the accent, canvas inset 18 inside a 1px border with a 3px radius on the surface colour, status strip 28 in 12px muted text.

## Console

A rendering tool's workbench. Dense, panel-based, monospace values, one accent for every change mark, LED status dots. Dark is the native palette; light is the same structure in paper tones.

Palette (dark / light): canvas `#15181C` / `#E4E8EC`, surface `#1C2025` / `#F4F6F8`, subtle `#23282E` / `#FFFFFF`, border `#2B3138` / `#D2D8DF`, strong border `#3B434C` / `#B4BDC7`, text `#DDE2E7` / `#1B2129`, muted `#8C97A3` / `#5A6673`, faint `#5F6974` / `#8A95A1`, accent `#3FC8D8` / `#0B8E9F`, on-accent `#062A30` / `#FFFFFF`, ok `#48CF82` / `#1E8A4E`, warn `#E6B54A` / `#9A6700`, bad `#F26A57` / `#C6382A`, preview `#0D1013` / `#FFFFFF`.

Metrics: title bar 32 with a dim 11px title, rail 64 with 56px items (20px icon over a 10px label, 2px accent bar on the selection), command bar 40, panels with a 30px title strip in 11/600 muted, key/value tables 120px label column, rows 28, controls 26, radius 4, status bar 24 in 11px, body 12px/16px with tabular numbers, values in Cascadia Mono 11px.

Components: `ConsoleFrame` (command bar with breadcrumb, context line, actions; body; status bar), `ConsolePanel` (title strip, body, footer), `ConsoleKv`, `StatusDot`, `Segmented`, tags (`.console-tag`), the `.console-table` grid, mode rows with a trailing switch or chevron and an in-place detail region.

Pages: overview is a dashboard (active-profile specimen at six sizes with a font toggle and invert, service panel with a big LED line and key/value rows, timestamped activity log); profiles is a searchable table with a selected-profile panel (specimen, key/value, save-as, actions); service pairs a status panel (big line, key/value, notices, maintenance disclosure, UAC note) with mode rows that expand in place; the Tuner is three panels with two-digit step numbers, a group title strip and the preview panel; diagnostics is installation, component and log-file panels beside one event panel with filter chips.

Native preview window: title bar text dim; toolbar 36 on the surface colour with a 1px border below, controls 26 with 1px strong borders and 4px corners, toggles filled with the soft accent, segmented mode switch, canvas inset 10 inside a 1px border, ladder gutter numbers in Cascadia Mono 10px faint, status strip 24 in 11px with tabular numbers and LED dots.

## Cupertino

macOS System Settings grammar borrowed because MacType borrows Mac rendering. A tinted sidebar that runs to the top edge, muted icon tiles, rounded groups of rows, controls at the trailing edge, 13px body.

Palette (light / dark): window `#F5F5F7` / `#1E1E1E`, sidebar `#EBEBED` / `#262628`, group `#FFFFFF` / `#2C2C2E`, subtle `#F2F2F4` / `#3A3A3C`, group line `rgba(0,0,0,.09)` / `rgba(255,255,255,.10)` (solid stand-ins `#E3E3E6` / `#3F3F42`), separator `rgba(0,0,0,.08)` / `rgba(255,255,255,.08)`, text `#1D1D1F` / `#F5F5F7`, muted `#6E6E73` / `#98989D`, faint `#A1A1A6` / `#6C6C70`, accent `#3B74D8` / `#4C8DF5`, on-accent `#FFFFFF`, ok `#4F9A6B` / `#6DBB88`, warn `#B98A3A` / `#D3A659`, tiles gray `#8E8E93`, blue `#5F86C6`, green `#5E9B78`, violet `#8C7FB8`, slate `#6E6E73`, amber `#C39461` (dark variants lighter). Tiles are deliberately desaturated.

Metrics: sidebar 224 with a 40px pad under the title bar, search field 28 with a 7px radius, items 28 with 20px tiles (5px radius) and a filled accent pill for the selection, title bar 40 named after the page and without the product icon, page title 24/700, content column 700, groups with a 10px radius, rows 44 minimum with hairlines inset 16, hero rows 64, controls 26 with a 6px radius and a faint drop shadow, switches 26×15, popup buttons with an accent chevron block.

Components: `CupertinoPage` (title, subtitle, actions, optional back control), `CupertinoGroup`, `CupertinoRow` (leading slot, title, description, trailing value, disclosure chevron), `CupertinoStatusCircle`, badge, footnote, toolbar; the sidebar search filters navigation entries.

Pages: overview is one group (hero row, disclosure rows for the profile and the preview connection, plain value rows) and an activity group with a "show earlier" link; profiles is a group of radio rows with an 84×44 thumbnail and a "current file" group plus a bottom toolbar; service is a hero group and a modes group whose disclosure rows open detail pages with a back control; the Tuner is three rounded groups (list, settings, preview); diagnostics is an installation group with finding rows, a components group, an events group and a log-file group.

Native preview window: toolbar 40 on the window colour, controls 26 with a 6px radius and the faint shadow, mode switch as a segmented control, canvas inset 14 inside a group (10px radius, 1px group line), ladder gutter numbers 10px faint, status strip 26 in 12px muted.
