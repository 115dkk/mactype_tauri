# WindowTitleBar

The frameless window's own title bar: the 18px MacType icon and the title on the left, 46px minimize, maximize and close buttons on the right, `titlebar-height` tall.

- Props: `title` (defaults to "MacType Control Center"; standalone windows pass their own), `icon` and `className`.
- The whole bar is a drag region; double-click toggles maximize. Buttons carry accessible names.
- Hover fills a control with `color-surface-subtle`; the close button fills with `color-destructive` and a white glyph.
