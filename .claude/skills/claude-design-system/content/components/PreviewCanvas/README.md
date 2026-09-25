# PreviewCanvas

The ground on which the preview helper's GDI bitmaps appear: a bordered `color-preview` panel whose placeholder text keeps the strip's eventual height until the images arrive.

- The canvas follows the window theme's polarity by default and offers one invert control (`data-dark="true"`); it never asks for a separate background.
- Helper bitmaps sit at device pixels or integer nearest-neighbour zoom inside it. Never scale, fade or CSS-invert a rendered sample, and never replace its palette with a surface colour.
- A stacked canvas (`data-stack="true"`) scrolls when its specimen strips are taller than the panel; each strip's caption is 11px muted.
- The placeholder text here is `type-specimen`; the real content is always the helper's bitmap.
