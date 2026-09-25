# ConsolePanel

The Console skin's unit, pinned to Console in this card: a bordered panel with a 30px muted title strip, a dense body and an optional footer, inside a frame that ends in the 24px status bar.

- `ConsoleFrame` props: `crumb`, `title`, `summary`, `notice`, `actions`, `status`, `statusRight` and the body. Every Console page fills the command bar and the status bar.
- `ConsolePanel` props: `title`, `icon`, `right` (tags or controls in the title strip), `footer`, `scroll`.
- `console-big` is the LED line: a `StatusDot`, the state sentence and a small muted detail. `ConsoleKv` is a key/value table with a 120px label column and 28px rows; values are monospace.
- Tags are short words in a 16px chip: `console-tag` on `console-accent-soft`, `console-tag ok` (the run-profile badge) on `console-ok-soft`. Controls are 26px; numbers are tabular everywhere. Dark is the native palette; light moves the same structure to paper tones.
