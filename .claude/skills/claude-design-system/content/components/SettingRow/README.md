# SettingRow

One setting in the Tuner: label and help in a flexible column, the control in a stable `settings-control-width` column, a hairline below. Rows never sit inside a card in the classic skin.

- Markup: `div.setting-row` > `div.setting-label` (a `label` wrapped in the `Hint` term, the `dirty-mark` "Changed", optional `p` help) + the control.
- Controls are a `select` (discrete), a `range-control` with its exact number input and the revert and restore-default icon buttons, or a `switch-control`.
- The hint popover carries the description plus "Default {default}, allowed range {min}–{max}" and, when needed, "Reopen apps to see changes".
- Show dirty state as the `dirty-mark` word in `color-primary`; never by colour alone.
- Rows live inside `settings-form`, whose container query stacks collection editors when the column is narrow.
