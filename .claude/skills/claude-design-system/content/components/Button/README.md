# Button

Buttons are native `<button type="button">` elements with the `button` class plus one intent class; `icon-button` and `text-action` are the two lighter forms.

- `button primary` is the solid accent fill and belongs only to the action that starts or stops something (Start service, Export diagnostics). One per view.
- `button designate` writes a setting without starting anything (Set as run profile, Apply to service): accent edge, accent-tinted fill, a label mixed toward the foreground so it keeps 4.5:1.
- `button secondary` is every other action; `secondary` is a marker class and the base `.button` rule draws it. `button danger` marks a destructive action; the skins colour it with `color-destructive`.
- `icon-button` is square at `control-height` and must carry an `aria-label`. `text-action` is an inline accent link-button for disclosure and folder actions.
- Put a lucide icon (16 to 17px) before the label. A busy button swaps its icon for `LoaderCircle` with the `spin` class and sets `aria-busy`.
- The consumer supplies the label, the icon and the handler. Heights come from `control-height`, so each skin resizes buttons without new markup.
- The classic `.button.danger` rule reads `--color-danger`, which no stylesheet declares, so a classic danger button shows the inherited text colour; Fluent, Console and Cupertino set `color-destructive` explicitly.
