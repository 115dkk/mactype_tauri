# StatusDot

A filled 8px dot with a soft halo: a status glyph that cannot be misread as a character. Console calls it the LED.

- Props: `tone` (`ok`, `warn`, `bad`, `accent`, `neutral`) and an optional `label`. Without a label the dot is `aria-hidden`, so the word beside it must carry the state.
- Tones map to `color-success`, `color-warning`, `color-destructive`, `color-primary` and `color-border-strong`; the halo is the same colour at 18%.
- Always pair a dot with a state word. Never use a dot as decoration or as a bullet.
