# Icons

Every glyph the Control Center imports from **lucide-react** 0.468, as the unmodified lucide-static source SVG (ISC licence). Each is a 24px outline drawn with `stroke="currentColor"`, stroke width 2, round caps and joins.

- In the app, render the React component and let it inherit the text colour; pass `size` and `strokeWidth` (navigation 18 or 17 at 1.8, inline 15 to 17 at the default 2, window controls 13 to 16 at 1.5 to 1.7).
- Shown through `<img>` these files draw in black, because `currentColor` cannot inherit into an image. Inline the SVG or use the React component whenever the icon must follow `color-foreground`, `color-muted` or `color-primary`.
- Some React names are lucide aliases of a differently named file (`AlertTriangle` is `triangle-alert.svg`, `Home` is `house.svg`); the table below maps each one.
