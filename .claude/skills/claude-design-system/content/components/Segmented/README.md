# Segmented

A two-to-four way choice drawn as one control: a `radiogroup` of `segmented-option` buttons where only the selected segment is filled.

- Props: `label` (the group's accessible name), `options` (`value`, `label`), `value`, `onChange`, and `compact` for toolbars.
- The selected option has `aria-checked="true"`, `color-surface-subtle` fill (Console: `console-accent-soft`) and the foreground colour; the rest are muted.
- Use it in Preview Studio and Console toolbars for view options that apply instantly. Use a select for longer lists and radio rows for choices that need a description.
