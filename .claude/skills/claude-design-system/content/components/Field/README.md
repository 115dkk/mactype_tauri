# Field

Native form controls styled by the shared rules: text and number inputs, `search-field`, `select`, checkboxes, radios and the `range-control` row.

- Every text field, number field and select is at least `control-height` tall with a `color-border-strong` edge, `radius-control` corners and a `color-surface` fill; focus adds the 2px `color-focus` outline and keeps the border.
- `label.search-field` wraps a 16px `Search` icon, a visually hidden label and a borderless `type="search"` input.
- Selects, checkboxes and radios keep the browser's own glyphs and keyboard model; only their box and colours follow the tokens.
- `range-control` pairs a range with an exact number input (64px) and, in All settings, the revert and restore-default icon buttons. The range keeps browser keyboard stepping.
- The consumer supplies the `id`, the label (visible or `aria-label`) and `aria-describedby` for help text.
